#include "expensesync.h"
#include "cJSON.h"
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
static std::string field(const cJSON *j,const char *key) { auto *v=cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsString(v) ? v->valuestring : ""; }
static bool integer(const cJSON *v,uint32_t max,uint32_t &out) { if (!cJSON_IsNumber(v) || v->valuedouble<0 || v->valuedouble>max || floor(v->valuedouble)!=v->valuedouble) return false; out=v->valuedouble; return true; }
static bool valid_id(const std::string &s) { return s.size()<=256 && s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos; }
bool expensesync_encode(const ExpenseSyncState &s,std::string &data) {
    Json j(cJSON_CreateObject(),cJSON_Delete); bool ok=j && cJSON_AddNumberToObject(j.get(),"v",1) && cJSON_AddNumberToObject(j.get(),"palm",s.palm_id);
    const char *keys[]={"account","user","root","parent","sheet","marker"}; const std::string *vals[]={&s.account,&s.username,&s.root_folder,&s.user_folder,&s.spreadsheet,&s.marker};
    for (unsigned i=0;i<6;i++) if (!cJSON_AddStringToObject(j.get(),keys[i],vals[i]->c_str())) ok=false;
    if (!cJSON_AddBoolToObject(j.get(),"creating",s.creating) || !cJSON_AddBoolToObject(j.get(),"ready",s.initialized)) ok=false;
    auto *map=cJSON_AddArrayToObject(j.get(),"map"); if (!map) return false;
    for (const auto &m:s.map) {
        auto *a=cJSON_CreateArray(); if (!a || !cJSON_AddItemToArray(map,a)) { cJSON_Delete(a); return false; }
        std::string hashes; for (uint32_t n:m.fields) { char hex[9]; snprintf(hex,sizeof(hex),"%08lx",(unsigned long)n); hashes+=hex; }
        cJSON *items[]={cJSON_CreateNumber(m.palm_id),cJSON_CreateString(m.row_id.c_str()),cJSON_CreateString(hashes.c_str()),cJSON_CreateNumber(m.pending)};
        for (auto *item:items) if (!item || !cJSON_AddItemToArray(a,item)) { cJSON_Delete(item); ok=false; }
    }
    if (!ok) return false;
    char *raw=cJSON_PrintUnformatted(j.get()); if (!raw) return false; data=raw; free(raw); return data.size()<=16000;
}
bool expensesync_decode(const std::string &data,unsigned max,ExpenseSyncState &s) {
    s=ExpenseSyncState(); if (data.size()>16000) return false;
    Json j(cJSON_ParseWithOpts(data.c_str(),nullptr,true),cJSON_Delete); uint32_t v;
    if (!integer(cJSON_GetObjectItemCaseSensitive(j.get(),"v"),1,v) || v!=1 || !integer(cJSON_GetObjectItemCaseSensitive(j.get(),"palm"),UINT32_MAX,s.palm_id)) return false;
    const char *keys[]={"account","user","root","parent","sheet","marker"}; std::string *vals[]={&s.account,&s.username,&s.root_folder,&s.user_folder,&s.spreadsheet,&s.marker};
    for (unsigned i=0;i<6;i++) { if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j.get(),keys[i]))) return false; *vals[i]=field(j.get(),keys[i]); if (vals[i]->size()>256 || (i!=1 && !valid_id(*vals[i]))) return false; }
    auto *creating=cJSON_GetObjectItemCaseSensitive(j.get(),"creating"),*ready=cJSON_GetObjectItemCaseSensitive(j.get(),"ready"),*map=cJSON_GetObjectItemCaseSensitive(j.get(),"map");
    if (!cJSON_IsBool(creating) || !cJSON_IsBool(ready) || s.account.empty() || s.username.empty() || !cJSON_IsArray(map) || (unsigned)cJSON_GetArraySize(map)>max) return false;
    s.creating=cJSON_IsTrue(creating); s.initialized=cJSON_IsTrue(ready);
    if ((s.creating && s.marker.empty()) || (s.initialized && s.spreadsheet.empty())) return false;
    const cJSON *a; cJSON_ArrayForEach(a,map) {
        ExpenseSyncMap m; uint32_t pending;
        auto *id=cJSON_GetArrayItem(a,1),*hash=cJSON_GetArrayItem(a,2);
        if (!cJSON_IsArray(a) || cJSON_GetArraySize(a)!=4 || !integer(cJSON_GetArrayItem(a,0),0xffffff,m.palm_id) || !m.palm_id ||
            !cJSON_IsString(id) || !cJSON_IsString(hash) || !integer(cJSON_GetArrayItem(a,3),3,pending)) return false;
        m.row_id=id->valuestring; m.pending=pending; std::string hashes=hash->valuestring;
        if (m.row_id.empty() || m.row_id.size()>100 || !valid_id(m.row_id) || hashes.size()!=80 || hashes.find_first_not_of("0123456789abcdef")!=std::string::npos || !s.initialized) return false;
        for (unsigned i=0;i<10;i++) m.fields[i]=strtoul(hashes.substr(i*8,8).c_str(),nullptr,16);
        if (std::any_of(s.map.begin(),s.map.end(),[&](const ExpenseSyncMap &o) { return o.palm_id==m.palm_id || o.row_id==m.row_id; })) return false;
        s.map.push_back(m);
    }
    return true;
}
bool expensesync_retry_unconfirmed(ExpenseSyncState &s) {
    bool changed=false;
    if (s.creating && s.spreadsheet.empty()) { s.creating=false; changed=true; }
    for (auto &m:s.map) if (m.pending==2) { m.pending=1; changed=true; }
    return changed;
}
struct LocalExpense { uint32_t id=0; uint8_t attr=0; PalmExpense expense; };
struct RemoteExpense {
    const GoogleExpenseRow *source=nullptr;
    const GoogleExpenseRow &row() const { return *source; }
};
static ExpenseFields projected(const RemoteExpense &g,PalmCharset charset) {
    PalmExpense e; std::string problem; auto fields=g.row().fields;
    if (fields[9].empty()) fields[9]="0";
    expense_project(fields,charset,e,problem); // validated in the complete snapshot pass
    return expense_fields(e,charset);
}
struct ExpensePlan {
    uint32_t id=0;
    std::string row_id;
    const PalmExpense *original=nullptr;
    std::array<uint32_t,10> fields={};
    uint8_t attr=0;
    bool write=false,remove=false,exported=false;
};
static size_t content_size(const ExpenseFields &f) { size_t n=0; for (const auto &s:f) n+=s.size(); return n; }
static size_t content_size(const PalmExpense &e) { size_t n=0; for (const auto &s:e.text) n+=s.size(); return n; }
void expensesync_run(const ExpenseSyncSetup &setup,ExpenseSyncHost &host,ExpenseSyncResult &r) {
    r=ExpenseSyncResult(); uint8_t db=0; bool opened=false;
    auto work=[&]() {
        if (!host.online()) { r.problem="Wi-Fi is not connected"; return; }
        if (!setup.max_expenses || setup.max_expenses>512 || setup.conflicts>2) { r.problem="Invalid Expense sync limits"; return; }
        ExpenseSyncState state;
        if (!host.load(state) || state.map.size()>setup.max_expenses) { r.problem="Could not load Expense sync state"; return; }
        auto checkpoint=[&]() { if (host.store(state)) return true; r.problem="Could not save Expense sync state; sync stopped"; return false; };
        uint32_t user; char username[128];
        if (!hotsync_read_user(&user,username,sizeof(username))) { r.problem="Could not read Palm HotSync identity"; return; }
        std::string name=palm_to_utf8(username,setup.charset);
        if (name.empty()) { r.problem="Set a user name in wifi.setup -> HotSync settings, then sync again"; return; }
        std::string token; if (!host.access_token(token,r.problem)) return;
        GoogleSheetsClient google(host.http(),token,setup.max_expenses,setup.content_memory); std::string account;
        if (!google.drive.root(account)) { r.problem=google.drive.error(); return; }
        if (!state.account.empty() && (state.account!=account || state.palm_id!=user || state.username!=name)) { r.problem="Google account or Palm HotSync identity differs from the saved Expense binding"; return; }
        if (!hotsync_open_conduit() || !hotsync_open_db("ExpenseDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE|HOTSYNC_MODE_SECRET,&db)) { r.problem="Could not open ExpenseDB. The classic Palm Expense application must be installed."; return; }
        opened=true; std::vector<LocalExpense> local; size_t bytes=0;
        for (unsigned i=0;i<=setup.max_expenses;i++) {
            HotSync_Record rec;
            if (!hotsync_read_record_by_index(db,i,&rec)) { if (hotsync_error()==HOTSYNC_DLP_NOT_FOUND) break; r.problem="Could not read the complete Palm Expense database"; return; }
            if (i==setup.max_expenses) { r.problem="Palm Expenses exceed the configured record limit"; return; }
            LocalExpense l; l.id=rec.id; l.attr=rec.attr; l.expense.category=rec.category;
            if (!l.id || l.id>0xffffff || rec.category>15 || std::any_of(local.begin(),local.end(),[&](const LocalExpense &v) { return v.id==l.id; })) { r.problem="Invalid or repeated Palm Expense identity"; return; }
            if (!(rec.attr&(HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED|HOTSYNC_ATTR_DELETED))) {
                if (rec.truncated || !expense_unpack(rec.data,rec.len,rec.category,l.expense,r.problem)) { r.problem="Expense record "+std::to_string(i)+" (ID "+std::to_string(rec.id)+"): "+(r.problem.empty() ? "truncated record" : r.problem); return; }
                for (const auto &s:l.expense.text) bytes+=s.size();
                if (bytes>setup.content_memory) { r.problem="Palm Expenses exceed the content memory budget"; return; }
            }
            local.push_back(std::move(l));
        }
        if (state.account.empty()) { state.account=account; state.palm_id=user; state.username=name; if (!checkpoint()) return; }
        if (!google.drive.folder(account,"Palm",state.root_folder,checkpoint) || !google.drive.folder(state.root_folder,name,state.user_folder,checkpoint)) { if (r.problem.empty()) r.problem=google.drive.error(); return; }
        if (state.spreadsheet.empty()) {
            if (state.marker.empty()) { state.marker="expenses-"+std::to_string(user)+"-"+std::to_string(host.random())+"-"+std::to_string(host.random()); if (!checkpoint()) return; }
            std::vector<GoogleDriveFile> files;
            if (!google.drive.list(state.user_folder,files)) { r.problem=google.drive.error(); return; }
            for (const auto &f:files) if (f.sync_marker==state.marker) {
                if (!state.spreadsheet.empty() || f.mime!="application/vnd.google-apps.spreadsheet") { r.problem="Duplicate or invalid Expenses spreadsheet creation markers"; return; }
                state.spreadsheet=f.id;
            }
            if (!state.spreadsheet.empty()) { state.creating=false; if (!checkpoint()) return; }
            else {
                if (state.creating) { r.problem="Expenses spreadsheet creation is unconfirmed. Sync later, or check Drive before using Retry unconfirmed creations."; return; }
                state.creating=true; if (!checkpoint()) return;
                if (!google.create(state.user_folder,state.marker,state.spreadsheet)) { r.problem=google.error(); return; }
                state.creating=false; if (!checkpoint()) return;
            }
        }
        GoogleDriveFile sheet; bool found;
        if (!google.drive.get(state.spreadsheet,sheet,found)) { r.problem=google.drive.error(); return; }
        if (!found || sheet.trashed || sheet.mime!="application/vnd.google-apps.spreadsheet" || std::find(sheet.parents.begin(),sheet.parents.end(),state.user_folder)==sheet.parents.end()) { r.problem="The bound Expenses spreadsheet is missing, trashed or moved; restore it in Drive before syncing"; return; }
        if (!google.initialize(state.spreadsheet,!state.initialized)) { r.problem=google.error(); return; }
        if (!state.initialized) { state.initialized=true; if (!checkpoint()) return; }
        std::vector<GoogleExpenseRow> rows;
        if (!google.read(state.spreadsheet,rows)) { r.problem=google.error(); return; }
        std::vector<RemoteExpense> remote;
        for (const auto &row:rows) {
            RemoteExpense g; g.source=&row;
            if (!row.deleted) {
                ExpenseFields input=row.fields; if (input[9].empty()) input[9]="0";
                PalmExpense checked; if (!expense_project(input,setup.charset,checked,r.problem)) { r.problem="Expenses sheet row "+std::to_string(row.row+1)+": "+r.problem; return; }
            }
            for (const auto &s:row.fields) bytes+=s.size();
            if (bytes>setup.content_memory) { r.problem="Combined Palm/Sheets Expenses exceed the content memory budget"; return; }
            remote.push_back(g);
        }
        auto cloud=[&](const std::string &id) { return std::find_if(remote.begin(),remote.end(),[&](const RemoteExpense &g) { return g.row().id==id; }); };
        for (auto &m:state.map) {
            auto g=cloud(m.row_id);
            if ((m.pending==1 || m.pending==2) && g!=remote.end()) m.pending=0;
            else if (m.pending==2) { r.problem="An Expense append is unconfirmed. Sync later, or check the spreadsheet before Retry unconfirmed creations. Record ID: "+m.row_id; return; }
            else if (m.pending==3 && g==remote.end()) { r.problem="An incoming Expense row link is unconfirmed. Restore Record ID "+m.row_id+" on its spreadsheet row, then sync again."; return; }
            if ((m.pending==1 || m.pending==2) && g==remote.end() && std::none_of(local.begin(),local.end(),[&](const LocalExpense &l) { return l.id==m.palm_id; })) {
                r.problem="An unconfirmed Expense export is no longer on Palm; restore its Palm backup before retrying"; return;
            }
        }
        std::vector<ExpensePlan> plans; std::vector<GoogleExpenseChange> changes;
        auto fresh_row_id=[&]() {
            for (unsigned tries=0;tries<32;tries++) {
                std::string id="e-"+std::to_string(user)+"-"+std::to_string(host.random())+"-"+std::to_string(host.random());
                if (cloud(id)==remote.end() && std::none_of(state.map.begin(),state.map.end(),[&](const ExpenseSyncMap &m) { return m.row_id==id; })) return id;
            }
            return std::string();
        };
        auto conflict=[&](const std::string &s) { r.problem="Both Palm and Google changed "+s+". Reconcile the edits or choose a conflict preference on the Expenses page."; };
        for (const auto &l:local) {
            if (l.attr&(HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED)) { r.skipped++; continue; }
            auto m=std::find_if(state.map.begin(),state.map.end(),[&](const ExpenseSyncMap &v) { return v.palm_id==l.id; });
            auto g=m==state.map.end() ? remote.end() : cloud(m->row_id);
            ExpensePlan p; p.id=l.id; p.attr=l.attr;
            auto lf=expense_fields(l.expense,setup.charset); auto lh=expense_hashes(lf);
            if (l.attr&HOTSYNC_ATTR_DELETED) {
                p.remove=true;
                if (m!=state.map.end()) {
                    p.row_id=m->row_id;
                    if (g==remote.end() && !m->pending) { r.problem="A linked Expense row is missing; restore it or its Record ID before syncing"; return; }
                    if (g!=remote.end() && !g->row().deleted) {
                        if (expense_hashes(projected(*g,setup.charset))!=m->fields) {
                            if (!setup.conflicts) { conflict("an Expense deleted on Palm"); return; }
                            if (setup.conflicts==2) { p.remove=false; p.write=true; p.attr&=~HOTSYNC_ATTR_DELETED; p.fields=expense_hashes(projected(*g,setup.charset)); }
                        }
                        if (p.remove) { auto after=g->row(); after.deleted=true; changes.push_back({g->row(),after,false}); }
                    }
                }
                plans.push_back(p); continue;
            }
            if (m!=state.map.end() && g==remote.end() && !m->pending) { r.problem="A linked Expense row is missing; restore it or its Record ID, then use Deleted=TRUE to delete"; return; }
            if (m==state.map.end() || ((m->pending==1) && g==remote.end())) {
                if (m==state.map.end()) {
                    if (state.map.size()>=setup.max_expenses) { r.problem="Expense sync map is full"; return; }
                    ExpenseSyncMap fresh; fresh.palm_id=l.id; fresh.row_id=fresh_row_id(); fresh.fields=lh; fresh.pending=1;
                    if (fresh.row_id.empty()) { r.problem="Could not reserve an Expense Record ID"; return; }
                    state.map.push_back(fresh); m=state.map.end()-1;
                }
                GoogleExpenseRow after; after.id=m->row_id; after.fields=lf;
                changes.push_back({GoogleExpenseRow(),after,true}); m->pending=2; m->fields=lh;
                p.row_id=m->row_id; p.fields=lh; p.exported=true; p.original=&l.expense; p.write=l.attr&HOTSYNC_ATTR_DIRTY;
                plans.push_back(p); continue;
            }
            p.row_id=m->row_id;
            if (g->row().deleted) {
                if (lh!=m->fields && !setup.conflicts) { conflict("an Expense deleted in Sheets"); return; }
                if (lh!=m->fields && setup.conflicts==1) {
                    auto after=g->row(); after.deleted=false; after.fields=lf; changes.push_back({g->row(),after,false}); p.fields=lh; p.exported=true; p.original=&l.expense; p.write=l.attr&HOTSYNC_ATTR_DIRTY;
                } else p.remove=true;
                plans.push_back(p); continue;
            }
            ExpenseFields target=projected(*g,setup.charset); auto gh=expense_hashes(target); auto after=g->row();
            for (unsigned f=0;f<10;f++) {
                bool palm=lh[f]!=m->fields[f],google_changed=gh[f]!=m->fields[f];
                if (palm && google_changed && lh[f]!=gh[f] && !setup.conflicts) {
                    const char *names[]={"date","type","payment","currency","amount","vendor","city","attendees","note","category"};
                    conflict("Expense "+m->row_id+", "+names[f]); return;
                }
                if (palm && (!google_changed || lh[f]==gh[f] || setup.conflicts==1)) {
                    target[f]=lf[f];
                    // If both sides already agree after Palm projection, retain
                    // unrepresentable Google characters in the original cell.
                    if (lh[f]!=gh[f]) after.fields[f]=lf[f];
                }
            }
            PalmExpense checked; if (!expense_project(target,setup.charset,checked,r.problem)) return;
            // Upgrade old numeric cells and ISO-only edits to readable labels
            // without changing their Palm selector or persisted field hashes.
            after.fields[3]=expense_currency_label(checked.currency);
            p.fields=expense_hashes(target); if (p.fields==lh) p.original=&l.expense; p.write=p.fields!=lh || (l.attr&HOTSYNC_ATTR_DIRTY);
            if (after.fields!=g->row().fields) { changes.push_back({g->row(),after,false}); p.exported=true; }
            plans.push_back(p);
        }
        // New Google rows and Palm records lost without a deletion tombstone.
        for (const auto &g:remote) {
            if (g.row().deleted || std::any_of(plans.begin(),plans.end(),[&](const ExpensePlan &p) { return !p.row_id.empty() && p.row_id==g.row().id; })) continue;
            auto m=std::find_if(state.map.begin(),state.map.end(),[&](const ExpenseSyncMap &v) { return v.row_id==g.row().id; });
            if (m!=state.map.end() && std::any_of(local.begin(),local.end(),[&](const LocalExpense &l) { return l.id==m->palm_id; })) continue; // private/archived
            if (m==state.map.end()) {
                if (state.map.size()>=setup.max_expenses) { r.problem="Combined Expenses exceed the configured record limit"; return; }
                ExpenseSyncMap fresh; fresh.row_id=g.row().id.empty() ? fresh_row_id() : g.row().id;
                uint32_t id=host.random()&0xffffff; if (!id) id=1;
                for (unsigned n=0;n<=setup.max_expenses*2;n++,id=id==0xffffff ? 1 : id+1) {
                    if (std::none_of(local.begin(),local.end(),[&](const LocalExpense &l) { return l.id==id; }) && std::none_of(state.map.begin(),state.map.end(),[&](const ExpenseSyncMap &v) { return v.palm_id==id; })) { fresh.palm_id=id; break; }
                }
                if (fresh.row_id.empty() || !fresh.palm_id) { r.problem="Could not reserve an incoming Expense identity"; return; }
                state.map.push_back(fresh); m=state.map.end()-1;
            }
            m->pending=3; m->fields=expense_hashes(projected(g,setup.charset));
            auto after=g.row(); after.id=m->row_id; after.fields[3]=projected(g,setup.charset)[3];
            if (after.id!=g.row().id || after.fields!=g.row().fields) changes.push_back({g.row(),after,false});
            ExpensePlan p; p.id=m->palm_id; p.row_id=m->row_id; p.fields=m->fields; p.write=true; plans.push_back(p);
        }
        unsigned appended=0,imported=0,removed=0;
        int64_t projected_bytes=bytes;
        for (const auto &change:changes) {
            projected_bytes+=content_size(change.after.fields);
            if (change.append) appended++;
            else {
                auto before=std::find_if(rows.begin(),rows.end(),[&](const GoogleExpenseRow &v) { return v.row==change.before_row; });
                if (before!=rows.end()) projected_bytes-=content_size(before->fields);
            }
        }
        for (const auto &p:plans) {
            auto l=std::find_if(local.begin(),local.end(),[&](const LocalExpense &v) { return v.id==p.id; });
            if (p.remove) { if (l!=local.end()) { projected_bytes-=content_size(l->expense); removed++; } continue; }
            if (!p.write || p.original) continue;
            if (l==local.end()) imported++; else projected_bytes-=content_size(l->expense);
            auto c=std::find_if(changes.begin(),changes.end(),[&](const GoogleExpenseChange &v) { return v.after.id==p.row_id; });
            auto g=cloud(p.row_id);
            if (c==changes.end() && g==remote.end()) { r.problem="Expense plan has no source row"; return; }
            auto fields=c!=changes.end() ? c->after.fields : g->row().fields; if (fields[9].empty()) fields[9]="0";
            PalmExpense projected; if (!expense_project(fields,setup.charset,projected,r.problem)) return;
            projected_bytes+=content_size(projected);
        }
        if (rows.size()+appended>setup.max_expenses || local.size()+imported-removed>setup.max_expenses) { r.problem="Combined Expenses exceed the row/record limit; remove completed Deleted=TRUE rows or increase the configured limit"; return; }
        if (projected_bytes<0 || (uint64_t)projected_bytes>setup.content_memory) { r.problem="Resulting Palm/Sheets Expenses would exceed the content memory budget"; return; }
        // Journal only after the fresh-table check and request preparation have
        // passed. A preflight conflict must not become an ambiguous creation.
        if (!google.apply(state.spreadsheet,rows,changes,checkpoint)) { if (r.problem.empty()) r.problem=google.error(); return; }
        std::vector<GoogleExpenseRow> confirmed;
        if (!google.read(state.spreadsheet,confirmed)) { r.problem=google.error(); return; }
        for (const auto &p:plans) if (!p.row_id.empty()) {
            auto expected=std::find_if(rows.begin(),rows.end(),[&](const GoogleExpenseRow &v) { return v.id==p.row_id; });
            auto changed=std::find_if(changes.begin(),changes.end(),[&](const GoogleExpenseChange &v) { return v.after.id==p.row_id; });
            const GoogleExpenseRow *want=changed!=changes.end() ? &changed->after : expected!=rows.end() ? &*expected : nullptr;
            auto actual=std::find_if(confirmed.begin(),confirmed.end(),[&](const GoogleExpenseRow &v) { return v.id==p.row_id; });
            if (want && (actual==confirmed.end() || actual->fields!=want->fields || actual->deleted!=want->deleted)) { r.problem="Expense rows changed during sync; no Palm updates were made. Finish spreadsheet editing and sync again."; return; }
        }
        // Release planning copies before producing Palm records.
        std::vector<GoogleExpenseChange>().swap(changes);
        std::vector<RemoteExpense>().swap(remote);
        std::vector<GoogleExpenseRow>().swap(rows);
        if (!checkpoint()) return; // also reserve incoming IDs when Sheets needed no write
        for (const auto &p:plans) {
            auto m=std::find_if(state.map.begin(),state.map.end(),[&](const ExpenseSyncMap &v) { return v.palm_id==p.id; });
            if (p.remove) {
                if (!hotsync_delete_record(db,p.id)) { r.problem="Could not delete the Palm Expense record"; return; }
                if (m!=state.map.end()) state.map.erase(m);
                r.deleted++;
            } else {
                if (p.exported) r.uploaded++;
                if (p.write) {
                    PalmExpense body;
                    if (p.original) body=*p.original;
                    else {
                        auto row=std::find_if(confirmed.begin(),confirmed.end(),[&](const GoogleExpenseRow &v) { return v.id==p.row_id; });
                        if (row==confirmed.end()) { r.problem="The incoming Expense row disappeared"; return; }
                        auto input=row->fields; if (input[9].empty()) input[9]="0";
                        if (!expense_project(input,setup.charset,body,r.problem)) return;
                    }
                    size_t room; uint8_t *buffer=hotsync_record_buffer(&room); uint32_t saved=0; size_t n=expense_pack(body,buffer,room);
                    if (!n || !hotsync_write_record(db,p.id,p.attr&~(HOTSYNC_ATTR_DIRTY|HOTSYNC_ATTR_DELETED),body.category,n,&saved) || saved!=p.id) { r.problem="Could not write the Palm Expense record"; return; }
                    r.downloaded++;
                }
                if (m!=state.map.end()) { m->fields=p.fields; m->pending=0; }
            }
            if (!checkpoint()) return;
        }
        // A fully removed Palm record paired with an already-deleted Sheet row
        // has no transfer left to perform; keep the visible Sheet audit row.
        state.map.erase(std::remove_if(state.map.begin(),state.map.end(),[&](const ExpenseSyncMap &m) {
            auto g=std::find_if(confirmed.begin(),confirmed.end(),[&](const GoogleExpenseRow &v) { return v.id==m.row_id; });
            return !m.pending && g!=confirmed.end() && g->deleted && std::none_of(local.begin(),local.end(),[&](const LocalExpense &l) { return l.id==m.palm_id; });
        }),state.map.end());
        checkpoint();
    };
    work(); r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK;
    if (opened && !r.link_lost && !hotsync_close_db(db) && r.problem.empty()) r.problem="Could not close ExpenseDB";
    r.link_lost=r.link_lost || hotsync_error()==HOTSYNC_ERR_LINK;
}
std::string expensesync_summary(const ExpenseSyncResult &r) {
    std::string s=std::to_string(r.uploaded)+" uploaded, "+std::to_string(r.downloaded)+" downloaded, "+std::to_string(r.deleted)+" deleted";
    if (r.skipped) s+=", "+std::to_string(r.skipped)+" skipped";
    if (!r.problem.empty()) s+=". "+r.problem;
    return s;
}
