#include "expensesync.h"
#include "drive_server.h"
#include <set>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static int fd=-1;
static int io_read(uint8_t *p,size_t n,uint32_t ms) { pollfd f={fd,POLLIN,0}; if (poll(&f,1,ms)<=0) return 0; int r=::read(fd,p,n); return r>0 ? r : -1; }
static bool io_write(const uint8_t *p,size_t n) { return ::write(fd,p,n)==(ssize_t)n; }
static uint32_t ticks() { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000; }
using Cells=std::array<std::string,12>;
class SheetsServer : public Server {
public:
    struct Sheet { bool tab=false; std::string title="Expenses"; std::vector<Cells> cells; };
    std::map<std::string,Sheet> sheets;
    std::set<std::string> hidden;
    unsigned mutations=0,sheet_creates=0,reads=0,race_read=0;
    bool lose_create=false,fail_create=false,lose_batch=false,fail_batch=false,deny_sheets=false,formula=false,numeric_amount=false;
    bool race_after_batch=false;
    size_t row_by_id(const std::string &sheet,const std::string &id) {
        auto &v=sheets[sheet].cells;
        for (size_t i=1;i<v.size();i++) if (v[i][0]==id) return i;
        assert(false); return 0;
    }
    int request(const std::string &url,const char *method,const std::string &auth,const char *type,const std::string &body,std::string &reply) override {
        assert(auth=="Bearer token");
        if (!strcmp(method,"POST") && url.find("/drive/v3/files?")!=std::string::npos) {
            if (fail_create) return 503;
            auto *j=cJSON_Parse(body.c_str()); assert(j && field(j,"mimeType")=="application/vnd.google-apps.spreadsheet");
            File f; f.id="sheet-"+std::to_string(next_id++); f.modified=when("2026-10-15T07:00:00Z"); metadata(f,j); cJSON_Delete(j); files[f.id]=f; sheets[f.id]=Sheet(); sheet_creates++; mutations++;
            reply="{\"id\":\""+f.id+"\"}"; if (lose_create) { lose_create=false; return -1; } return 200;
        }
        const std::string prefix="https://sheets.googleapis.com/v4/spreadsheets/";
        if (url.rfind(prefix,0)!=0) {
            size_t at=0; GoogleHeaders h,out;
            return transfer(url,method,auth,type,body.size(),[&](uint8_t *p,size_t n) { memcpy(p,body.data()+at,n); at+=n; return true; },h,reply,out);
        }
        if (deny_sheets) { reply="{\"error\":{\"message\":\"Sheets API is disabled\"}}"; return 403; }
        std::string id=url.substr(prefix.size()); id=id.substr(0,id.find_first_of("/?:"));
        if (!sheets.count(id)) return 404;
        auto &s=sheets[id];
        if (!strcmp(method,"GET") && url.find("/values/")!=std::string::npos) {
            if (!s.tab || s.title!="Expenses") return 400;
            assert(param(url,"valueRenderOption")=="FORMULA" && decode(url).find("'Expenses'!A:L")!=std::string::npos);
            auto *j=cJSON_CreateObject(),*values=cJSON_AddArrayToObject(j,"values");
            for (unsigned r=0;r<s.cells.size();r++) {
                if (r && hidden.count(s.cells[r][0])) continue;
                auto *row=cJSON_CreateArray(); cJSON_AddItemToArray(values,row);
                for (unsigned c=0;c<12;c++) {
                    if (numeric_amount && r==1 && c==5) cJSON_AddItemToArray(row,cJSON_CreateNumber(12.34));
                    else cJSON_AddItemToArray(row,cJSON_CreateString(s.cells[r][c].c_str()));
                }
            }
            reply=print(j); reads++;
            if (race_read==reads && s.cells.size()>1) s.cells[1][6]="Concurrent vendor "+std::to_string(reads);
            return 200;
        }
        if (!strcmp(method,"GET") && !param(url,"ranges").empty()) {
            // Formula-sensitive inspection of a single suspicious cell.
            auto range=param(url,"ranges"); auto at=range.find('!'); assert(at!=std::string::npos);
            unsigned col=range[at+1]-'A',row=std::stoul(range.substr(at+2))-1;
            auto *j=cJSON_CreateObject(),*a=cJSON_AddArrayToObject(j,"sheets"),*sh=cJSON_CreateObject(); cJSON_AddItemToArray(a,sh);
            auto *data=cJSON_AddArrayToObject(sh,"data"),*grid=cJSON_CreateObject(); cJSON_AddItemToArray(data,grid);
            auto *rs=cJSON_AddArrayToObject(grid,"rowData"),*r=cJSON_CreateObject(); cJSON_AddItemToArray(rs,r);
            auto *vs=cJSON_AddArrayToObject(r,"values"),*v=cJSON_CreateObject(); cJSON_AddItemToArray(vs,v);
            cJSON_AddStringToObject(cJSON_AddObjectToObject(v,"userEnteredValue"),formula ? "formulaValue" : "stringValue",s.cells[row][col].c_str()); reply=print(j); return 200;
        }
        if (!strcmp(method,"GET")) {
            auto *j=cJSON_CreateObject(); cJSON_AddStringToObject(j,"spreadsheetId",id.c_str()); auto *a=cJSON_AddArrayToObject(j,"sheets");
            if (s.tab) { auto *sh=cJSON_CreateObject(); cJSON_AddItemToArray(a,sh); auto *p=cJSON_AddObjectToObject(sh,"properties"); cJSON_AddNumberToObject(p,"sheetId",830016); cJSON_AddStringToObject(p,"title",s.title.c_str()); }
            reply=print(j); return 200;
        }
        assert(!strcmp(method,"POST") && url.find(":batchUpdate")!=std::string::npos);
        if (fail_batch) return 503;
        auto *j=cJSON_Parse(body.c_str()); assert(j);
        if (url.find("/values:batchUpdate")!=std::string::npos) {
            assert(field(j,"valueInputOption")=="RAW"); auto *data=cJSON_GetObjectItemCaseSensitive(j,"data"); assert(cJSON_IsArray(data)); const cJSON *item; unsigned updated_row=0;
            cJSON_ArrayForEach(item,data) {
                unsigned first,last; assert(sscanf(field(const_cast<cJSON *>(item),"range").c_str(),"'Expenses'!A%u:L%u",&first,&last)==2 && first==last && first>1);
                unsigned row=first-1; if (!updated_row) updated_row=row; if (s.cells.size()<=row) s.cells.resize(row+1);
                auto *rows=cJSON_GetObjectItemCaseSensitive(item,"values"); assert(cJSON_GetArraySize(rows)==1);
                auto *values=cJSON_GetArrayItem(rows,0); assert(cJSON_GetArraySize(values)==12);
                for (unsigned col=0;col<12;col++) { auto *v=cJSON_GetArrayItem(values,col); if (cJSON_IsNull(v)) continue; assert(cJSON_IsString(v)); s.cells[row][col]=v->valuestring; }
            }
            cJSON_Delete(j); mutations++; reply="{}";
            if (race_after_batch && updated_row) { race_after_batch=false; s.cells[updated_row][6]="Edit after batch"; }
            if (lose_batch) { lose_batch=false; return -1; } return 200;
        }
        auto *requests=cJSON_GetObjectItemCaseSensitive(j,"requests"); assert(cJSON_IsArray(requests)); const cJSON *req;
        cJSON_ArrayForEach(req,requests) {
            auto *add=cJSON_GetObjectItemCaseSensitive(req,"addSheet"); if (add) { assert(!s.tab); s.tab=true; continue; }
            if (cJSON_GetObjectItemCaseSensitive(req,"repeatCell")) continue;
            auto *update=cJSON_GetObjectItemCaseSensitive(req,"updateCells"),*append=cJSON_GetObjectItemCaseSensitive(req,"appendCells");
            assert(update || append); auto *u=update ? update : append; assert(field(u,"fields")=="userEnteredValue");
            auto *start=cJSON_GetObjectItemCaseSensitive(u,"start"); unsigned row=update ? cJSON_GetObjectItemCaseSensitive(start,"rowIndex")->valueint : s.cells.size();
            unsigned col=update ? cJSON_GetObjectItemCaseSensitive(start,"columnIndex")->valueint : 0;
            auto *rs=cJSON_GetObjectItemCaseSensitive(u,"rows"); assert(cJSON_GetArraySize(rs)==1);
            auto *vs=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rs,0),"values");
            if (s.cells.size()<=row) s.cells.resize(row+1);
            for (int c=0;c<cJSON_GetArraySize(vs);c++) {
                auto *v=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(vs,c),"userEnteredValue");
                assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(v,"stringValue"))); s.cells[row][col+c]=field(v,"stringValue");
            }
        }
        cJSON_Delete(j); mutations++; reply="{}";
        if (race_after_batch && s.cells.size()>1) { race_after_batch=false; s.cells[1][6]="Edit after batch"; }
        if (lose_batch) { lose_batch=false; return -1; } return 200;
    }
    int transfer(const std::string &url,const char *method,const std::string &auth,const char *type,size_t size,const GoogleRead &source,const GoogleHeaders &headers,std::string &reply,GoogleHeaders &out) override {
        if (!strcmp(method,"GET") && url.find("/files?")!=std::string::npos && !hidden.empty()) {
            std::map<std::string,File> removed; for (const auto &id:hidden) if (files.count(id)) { removed[id]=files[id]; files.erase(id); }
            int code=Server::transfer(url,method,auth,type,size,source,headers,reply,out); for (const auto &p:removed) files[p.first]=p.second; return code;
        }
        return Server::transfer(url,method,auth,type,size,source,headers,reply,out);
    }
};
class ExpenseHost : public ExpenseSyncHost {
public:
    SheetsServer &server; ExpenseSyncState state;
    bool writable=true,connected=true,fail_import_commit=false;
    unsigned entropy=800;
    explicit ExpenseHost(SheetsServer &s) : server(s) {}
    GoogleHttp &http() override { return server; }
    bool online() override { return connected; }
    uint32_t random() override { return ++entropy; }
    bool access_token(std::string &t,std::string &) override { t="token"; return true; }
    bool load(ExpenseSyncState &s) override { s=state; return true; }
    bool store(const ExpenseSyncState &s) override {
        if (!writable) return false;
        if (fail_import_commit) for (const auto &old:state.map) if (old.pending==3) for (const auto &m:s.map) if (m.palm_id==old.palm_id && !m.pending) { fail_import_commit=false; return false; }
        std::string text; assert(expensesync_encode(s,text)); ExpenseSyncState decoded; assert(expensesync_decode(text,64,decoded)); state=decoded; return true;
    }
};
static ExpenseSyncResult sync(ExpenseHost &host,ExpenseSyncSetup setup,bool success=true) {
    ExpenseSyncResult r; expensesync_run(setup,host,r); printf("Expenses: %s\n",expensesync_summary(r).c_str()); fflush(stdout); assert(r.problem.empty()==success && !r.link_lost); return r;
}
static uint32_t put(uint32_t id,const PalmExpense &e,uint8_t attr=HOTSYNC_ATTR_DIRTY,const std::string &raw="") {
    uint8_t db; assert(hotsync_open_db("ExpenseDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE|HOTSYNC_MODE_SECRET,&db)); size_t room; auto *p=hotsync_record_buffer(&room);
    size_t n=raw.empty() ? expense_pack(e,p,room) : raw.size(); if (!raw.empty()) memcpy(p,raw.data(),n);
    uint32_t saved; assert(n && hotsync_write_record(db,id,attr,e.category,n,&saved) && hotsync_close_db(db)); return saved;
}
static bool get(uint32_t id,PalmExpense &e) {
    uint8_t db; assert(hotsync_open_db("ExpenseDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_SECRET,&db)); HotSync_Record rec; bool found=hotsync_read_record(db,id,&rec); std::string problem;
    if (found) assert(!rec.truncated && expense_unpack(rec.data,rec.len,rec.category,e,problem));
    assert(hotsync_close_db(db)); return found;
}
static void remove(uint32_t id) { uint8_t db; assert(hotsync_open_db("ExpenseDB",HOTSYNC_MODE_WRITE,&db) && hotsync_delete_record(db,id) && hotsync_close_db(db)); }
static Cells cloud_row(const std::string &vendor) { return {"","2026-10-15","Taxi","Cash","23","0099.9900",vendor,"Auckland","Alice","Cloud note","2","FALSE"}; }
int main(int argc,char **argv) {
    // Independent wire fixture: reserved byte and trailing zero padding accepted,
    // but date, terminators, nonzero garbage and truncation remain validated.
    const uint8_t fixture[]={0xf5,0x4f,23,1,23,0xa5,'1','2','.','3','4',0,'V',0,'C',0,'A',0,'N',0,0,0};
    PalmExpense e; std::string problem; assert(expense_unpack(fixture,sizeof(fixture),2,e,problem));
    assert(expense_fields(e,PALM_WESTERN)[0]=="2026-10-15" && e.text[0]=="12.34");
    // Independent byte-level check: a hex dump showing 17 means 0x17,
    // decimal 23 (USD). Decimal Sheet value 17 would be byte 0x11 (NZD).
    assert(e.currency==23 && expense_fields(e,PALM_WESTERN)[3]=="USD ($)");
    auto nz_fixture=std::string((const char *)fixture,sizeof(fixture)); nz_fixture[4]=0x11;
    PalmExpense nz; assert(expense_unpack((const uint8_t *)nz_fixture.data(),nz_fixture.size(),2,nz,problem));
    assert(nz.currency==17 && expense_fields(nz,PALM_WESTERN)[3]=="NZD ($)");
    assert(!expense_unpack(fixture,10,2,e,problem)); auto bad=std::string((const char *)fixture,sizeof(fixture)); bad.back()='x'; assert(!expense_unpack((const uint8_t *)bad.data(),bad.size(),2,e,problem));
    auto fields=ExpenseFields{"2024-02-29","#255","#254","#255","000.0100","Vendor","City","A","Note","15"};
    assert(expense_project(fields,PALM_WESTERN,e,problem) && expense_fields(e,PALM_WESTERN)==fields);
    fields[0]="2025-02-29"; assert(!expense_project(fields,PALM_WESTERN,e,problem)); fields[0]="2032-01-01"; assert(!expense_project(fields,PALM_WESTERN,e,problem));
    for (unsigned code=0;code<256;code++) {
        uint8_t decoded=0; auto label=expense_currency_label(code);
        assert(expense_currency_code(label,decoded) && decoded==code);
        assert(expense_currency_code(std::to_string(code),decoded) && decoded==code);
        auto labelled=fields,legacy=fields; labelled[3]=label; legacy[3]=std::to_string(code);
        assert(expense_hashes(labelled)==expense_hashes(legacy));
    }
    uint8_t currency=0;
    assert(expense_currency_code("NZD",currency) && currency==17 && expense_currency_label(17)=="NZD ($)");
    assert(expense_currency_code("USD ($)",currency) && currency==23);
    for (const char *invalid:{"$","256","#256","-1","12x","","NOTACURRENCY"}) assert(!expense_currency_code(invalid,currency));
    assert(argc==2); fd=open(argv[1],O_RDWR|O_NOCTTY); assert(fd>=0); std::vector<uint8_t> memory(HOTSYNC_BUFFER_SIZE(9000)); HotSync_Io io={io_read,io_write,ticks}; assert(hotsync_accept(&io,memory.data(),memory.size(),10000));
    uint32_t rom; assert(hotsync_read_sys_info(&rom)); SheetsServer server; ExpenseHost host(server); ExpenseSyncSetup setup;
    // Lost spreadsheet creation is recovered by the persisted Drive marker.
    server.lose_create=true; sync(host,setup,false); auto sheet=server.sheets.begin()->first; server.hidden.insert(sheet);
    sync(host,setup,false); assert(server.sheet_creates==1); server.hidden.clear();
    server.lose_batch=true; sync(host,setup,false); // initialized tab, lost reply
    auto r=sync(host,setup); assert(r.uploaded==1 && r.skipped==2 && host.state.map.size()==1 && server.sheet_creates==1);
    sheet=host.state.spreadsheet; auto linked=host.state.map.front().row_id; auto &table=server.sheets[sheet].cells;
    assert(table.size()==2 && table[1][5]=="0012.3400" && table[1][6]=="Palm taxi" && table[1][4]=="NZD ($)");
    // Existing numeric Sheets cells are upgraded without interpreting the
    // presentation change as a Palm currency edit or changing row linkage.
    table[1][4]="17"; r=sync(host,setup);
    assert(r.uploaded==1 && !r.downloaded && table[1][4]=="NZD ($)" && host.state.map.front().row_id==linked);
    assert(get(101,e) && e.currency==17);
    auto mutations=server.mutations; r=sync(host,setup); assert(!r.uploaded && !r.downloaded && server.mutations==mutations);
    table.push_back(cloud_row("From Sheets")); r=sync(host,setup); assert(r.downloaded==1);
    auto imported=host.state.map.back().palm_id; auto imported_id=host.state.map.back().row_id;
    assert(get(imported,e) && e.text[0]=="0099.9900" && e.text[1]=="From Sheets" && e.category==2);
    // Reordering complete rows preserves linkage; independent field edits merge.
    std::swap(table[1],table[2]); assert(get(101,e)); e.text[1]="Palm vendor"; put(101,e);
    table[server.row_by_id(sheet,linked)][9]="Google note"; r=sync(host,setup);
    assert(r.uploaded==1 && get(101,e) && e.text[1]=="Palm vendor" && e.text[4]=="Google note");
    auto row=server.row_by_id(sheet,linked); e.text[1]="Conflicting Palm"; put(101,e); table[row][6]="Conflicting Sheets"; mutations=server.mutations;
    sync(host,setup,false); assert(server.mutations==mutations && table[row][6]=="Conflicting Sheets"); setup.conflicts=2; sync(host,setup); setup.conflicts=0;
    assert(get(101,e) && e.text[1]=="Conflicting Sheets");
    // Google Unicode unsupported by Palm survives unrelated Palm edits.
    table[row][6]="Cafe \xf0\x9f\x98\x80"; sync(host,setup); assert(get(101,e)); e.text[4]="Palm note"; put(101,e); sync(host,setup); assert(table[row][6]=="Cafe \xf0\x9f\x98\x80");
    // Formula cells are rejected; literal text beginning '=' remains text.
    table[row][7]="=SUM(1,2)"; server.formula=true; mutations=server.mutations; sync(host,setup,false); assert(server.mutations==mutations);
    server.formula=false; sync(host,setup); assert(get(101,e) && e.text[2]=="=SUM(1,2)");
    server.numeric_amount=true; sync(host,setup,false); server.numeric_amount=false;
    // Linked row removal is an error, not an instruction to erase Palm data.
    auto saved=table[row]; table.erase(table.begin()+row); sync(host,setup,false); assert(get(101,e)); table.push_back(saved); row=server.row_by_id(sheet,linked);
    table.push_back(table[row]); sync(host,setup,false); table.pop_back();
    // Explicit Sheets and Palm deletions, including edit/delete conflicts.
    table[server.row_by_id(sheet,imported_id)][11]="TRUE"; r=sync(host,setup); assert(r.deleted==1 && !get(imported,e));
    assert(get(101,e)); put(101,e,HOTSYNC_ATTR_DELETED); table[row][9]="New cloud edit"; sync(host,setup,false);
    setup.conflicts=2; sync(host,setup); setup.conflicts=0; assert(get(101,e) && e.text[4]=="New cloud edit");
    put(101,e,HOTSYNC_ATTR_DELETED); r=sync(host,setup); assert(r.deleted==1 && table[row][11]=="TRUE" && !get(101,e));
    // Native app metadata and excluded records must remain untouched.
    auto fresh_row=cloud_row("Journal import"); table.push_back(fresh_row); host.fail_import_commit=true; sync(host,setup,false);
    auto reserved=host.state.map.back().palm_id; assert(host.state.map.back().pending==3 && get(reserved,e)); mutations=server.mutations;
    r=sync(host,setup); assert(!r.downloaded && server.mutations==mutations && !host.state.map.back().pending);
    // Lost append reply: discover the existing stable ID, never append twice.
    e.text[1]="Lost append"; auto outgoing=put(0,e); auto count=table.size(); server.lose_batch=true; sync(host,setup,false); assert(table.size()==count+1);
    auto unconfirmed=host.state.map.back().row_id; server.hidden.insert(unconfirmed); sync(host,setup,false); assert(table.size()==count+1);
    server.hidden.clear(); sync(host,setup); assert(table.size()==count+1 && get(outgoing,e));
    // Lost assignment of a new cloud row's ID uses the same reserved Palm ID.
    table.push_back(cloud_row("Interrupted row link")); server.lose_batch=true; sync(host,setup,false);
    auto linked_import=host.state.map.back().palm_id; assert(host.state.map.back().pending==3 && !get(linked_import,e));
    sync(host,setup); assert(get(linked_import,e) && e.text[1]=="Interrupted row link");
    // No transfers on malformed Palm or invalid remote snapshots.
    auto malformed=put(0,e,HOTSYNC_ATTR_DIRTY,"invalid fixture"); mutations=server.mutations; sync(host,setup,false); assert(server.mutations==mutations); remove(malformed);
    auto invalid=cloud_row("Bad date"); invalid[1]="2026-02-30"; table.push_back(invalid); mutations=server.mutations; sync(host,setup,false); assert(server.mutations==mutations); table.pop_back();
    auto row_count=table.size(); while (table.size()<66) table.push_back(cloud_row("Beyond snapshot limit"));
    mutations=server.mutations; sync(host,setup,false); assert(server.mutations==mutations); table.resize(row_count);
    // Reserve room for the resulting mirror, not just today's input snapshot.
    assert(get(outgoing,e)); e.text[4]=std::string(1000,'x'); auto growth=put(0,e);
    size_t current_bytes=0,export_bytes=0;
    for (const auto &m:host.state.map) { PalmExpense item; if (get(m.palm_id,item)) for (const auto &s:item.text) current_bytes+=s.size(); }
    for (const auto &s:e.text) current_bytes+=s.size();
    for (size_t i=1;i<table.size();i++) for (unsigned c=1;c<=10;c++) current_bytes+=table[i][c].size();
    for (const auto &s:expense_fields(e,PALM_WESTERN)) export_bytes+=s.size();
    auto bytes_setup=setup; bytes_setup.content_memory=current_bytes+export_bytes-1; mutations=server.mutations;
    auto growth_result=sync(host,bytes_setup,false); assert(growth_result.problem.find("Resulting")!=std::string::npos && server.mutations==mutations); remove(growth);
    auto tiny=setup; tiny.content_memory=1; sync(host,tiny,false); tiny=setup; tiny.max_expenses=1; sync(host,tiny,false);
    host.connected=false; sync(host,setup,false); host.connected=true; server.deny_sheets=true; sync(host,setup,false); server.deny_sheets=false;
    auto state=host.state; host.state.account="other"; sync(host,setup,false); host.state=state;
    ExpenseHost no_space(server); no_space.writable=false; mutations=server.mutations; sync(no_space,setup,false); assert(server.mutations==mutations);
    // A complete second snapshot detects concurrent edits before upload.
    assert(get(outgoing,e)); e.text[4]="Preflight update"; put(outgoing,e); server.race_read=server.reads+1; mutations=server.mutations;
    sync(host,setup,false); assert(server.mutations==mutations); server.race_read=0; setup.conflicts=1; sync(host,setup); setup.conflicts=0;
    // Edits after upload are detected before any Palm data or baseline commits.
    assert(get(outgoing,e)); e.text[4]="Postflight update"; put(outgoing,e); server.race_after_batch=true;
    sync(host,setup,false); assert(get(outgoing,e) && e.text[4]=="Postflight update"); setup.conflicts=2; sync(host,setup); setup.conflicts=0;
    // A preflight failure has not sent a creation, so it needs no explicit retry.
    e.text[1]="Preflight append"; put(0,e); server.race_read=server.reads+1; mutations=server.mutations;
    auto links=host.state.map.size(); sync(host,setup,false); assert(server.mutations==mutations && host.state.map.size()==links);
    server.race_read=0; setup.conflicts=1; sync(host,setup); setup.conflicts=0;
    // Explicit retries affect only already-pending appends, not future failures.
    e.text[1]="Rejected append"; put(0,e); server.fail_batch=true; sync(host,setup,false); assert(expensesync_retry_unconfirmed(host.state)); server.fail_batch=false; sync(host,setup);
    assert(!expensesync_retry_unconfirmed(host.state));
    e.text[1]="Future rejected append"; put(0,e); server.fail_batch=true; sync(host,setup,false); server.fail_batch=false; mutations=server.mutations;
    sync(host,setup,false); assert(server.mutations==mutations); assert(expensesync_retry_unconfirmed(host.state)); sync(host,setup);
    // Strict persistent state parsing and one-time retry semantics.
    std::string encoded; assert(expensesync_encode(host.state,encoded)); ExpenseSyncState decoded; assert(expensesync_decode(encoded,64,decoded));
    assert(!expensesync_decode(encoded+" trailing",64,decoded)); auto duplicate=host.state; duplicate.map.push_back(duplicate.map.front()); assert(expensesync_encode(duplicate,encoded) && !expensesync_decode(encoded,64,decoded));
    assert(hotsync_add_log("Expenses: host tests passed\n") && hotsync_end(0)); close(fd); puts("done");
}
