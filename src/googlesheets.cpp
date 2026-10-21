#include "googlesheets.h"
#include "mailoauth.h"
#include "cJSON.h"
#include <memory>
#include <algorithm>
#include <cmath>

using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
static constexpr unsigned sheet_id=830016;
static const char *headers[]={"Record ID","Date","Type","Payment","Currency","Amount","Vendor","City","Attendees","Note","Category","Deleted"};
static std::string str(const cJSON *j,const char *key) { auto *v=cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsString(v) ? v->valuestring : ""; }
static std::string print(cJSON *j) { char *raw=cJSON_PrintUnformatted(j); if (!raw) return ""; std::string s=raw; free(raw); return s; }
static std::string base(const std::string &id) { return "https://sheets.googleapis.com/v4/spreadsheets/"+oauth_encode(id); }
static bool id_ok(const std::string &id) { return !id.empty() && id.size()<=100 && id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos; }
bool GoogleSheetsClient::call(const std::string &url,const char *method,const std::string &body,std::string &reply) {
    error_.clear();
    if (token_.empty() || token_.find_first_of("\r\n")!=std::string::npos || (std::string(method)!="GET" && body.empty())) { error_="Could not prepare Sheets request"; return false; }
    int code=http_.request(url,method,"Bearer "+token_,"application/json",body,reply);
    if (code>=200 && code<300) return true;
    error_="Google Sheets request failed (HTTP "+std::to_string(code)+")";
    if (code<0 && !http_.error().empty()) error_+=". "+http_.error();
    Json j(cJSON_ParseWithOpts(reply.c_str(),nullptr,true),cJSON_Delete);
    auto message=str(cJSON_GetObjectItemCaseSensitive(j.get(),"error"),"message"); if (!message.empty()) error_+=". "+message.substr(0,200);
    return false;
}
bool GoogleSheetsClient::create(const std::string &parent,const std::string &marker,std::string &id) {
    Json j(cJSON_CreateObject(),cJSON_Delete);
    auto *parents=cJSON_AddArrayToObject(j.get(),"parents"),*props=cJSON_AddObjectToObject(j.get(),"appProperties");
    auto *p=cJSON_CreateString(parent.c_str());
    if (!parents || !p || !cJSON_AddItemToArray(parents,p)) { cJSON_Delete(p); error_="Could not prepare spreadsheet creation"; return false; }
    if (!props || !cJSON_AddStringToObject(props,"palmSync",marker.c_str()) || !cJSON_AddStringToObject(j.get(),"name","Expenses") ||
        !cJSON_AddStringToObject(j.get(),"mimeType","application/vnd.google-apps.spreadsheet")) { error_="Could not prepare spreadsheet creation"; return false; }
    std::string reply;
    if (!call("https://www.googleapis.com/drive/v3/files?fields=id","POST",print(j.get()),reply)) return false;
    Json result(cJSON_ParseWithOpts(reply.c_str(),nullptr,true),cJSON_Delete); id=str(result.get(),"id");
    if (!id_ok(id)) { error_="Google returned an invalid spreadsheet identity"; return false; }
    return true;
}
bool GoogleSheetsClient::initialize(const std::string &id,bool create_if_missing) {
    std::string reply;
    if (!call(base(id)+"?fields=spreadsheetId,sheets(properties(sheetId,title))","GET","",reply)) return false;
    Json j(cJSON_ParseWithOpts(reply.c_str(),nullptr,true),cJSON_Delete);
    auto *sheets=cJSON_GetObjectItemCaseSensitive(j.get(),"sheets");
    if (str(j.get(),"spreadsheetId")!=id || (sheets && !cJSON_IsArray(sheets))) { error_="Incomplete spreadsheet metadata"; return false; }
    const cJSON *s; cJSON_ArrayForEach(s,sheets) {
        auto *p=cJSON_GetObjectItemCaseSensitive(s,"properties"),*n=cJSON_GetObjectItemCaseSensitive(p,"sheetId");
        if (cJSON_IsNumber(n) && n->valuedouble==sheet_id) {
            if (str(p,"title")!="Expenses") { error_="The Expenses tab was renamed"; return false; }
            return true; // read() verifies its headers; never clear an existing tab
        }
        if (str(p,"title")=="Expenses") { error_="A different Expenses tab already exists; keep the adapter's original tab"; return false; }
    }
    if (!create_if_missing) { error_="The bound Expenses tab is missing; restore the original tab before syncing"; return false; }
    // All requests apply atomically, including the fixed sheet ID. Repeating an
    // ambiguous initialization cannot create a second tab or erase existing data.
    std::string body="{\"requests\":[{\"addSheet\":{\"properties\":{\"sheetId\":830016,\"title\":\"Expenses\",\"gridProperties\":{\"rowCount\":1000,\"columnCount\":12,\"frozenRowCount\":1}}}},{\"repeatCell\":{\"range\":{\"sheetId\":830016},\"cell\":{\"userEnteredFormat\":{\"numberFormat\":{\"type\":\"TEXT\"}}},\"fields\":\"userEnteredFormat.numberFormat\"}},{\"updateCells\":{\"start\":{\"sheetId\":830016,\"rowIndex\":0,\"columnIndex\":0},\"rows\":[{\"values\":[]}],\"fields\":\"userEnteredValue\"}}]}";
    Json request(cJSON_Parse(body.c_str()),cJSON_Delete);
    auto *req=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(request.get(),"requests"),2);
    auto *row=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(req,"updateCells"),"rows"),0);
    auto *values=cJSON_GetObjectItemCaseSensitive(row,"values");
    if (!request || !values) { error_="Could not prepare spreadsheet headers"; return false; }
    for (const auto *h:headers) {
        auto *cell=cJSON_CreateObject(); if (!cell || !cJSON_AddItemToArray(values,cell)) { cJSON_Delete(cell); error_="Could not prepare spreadsheet headers"; return false; }
        if (!cJSON_AddStringToObject(cJSON_AddObjectToObject(cell,"userEnteredValue"),"stringValue",h)) { error_="Could not prepare spreadsheet headers"; return false; }
    }
    return call(base(id)+":batchUpdate","POST",print(request.get()),reply);
}
bool GoogleSheetsClient::literal(const std::string &id,unsigned row,unsigned col,const std::string &value) {
    std::string cell="'Expenses'!"+std::string(1,char('A'+col))+std::to_string(row+1),reply;
    if (!call(base(id)+"?ranges="+oauth_encode(cell)+"&fields=sheets(data(rowData(values(userEnteredValue))))","GET","",reply)) return false;
    Json j(cJSON_ParseWithOpts(reply.c_str(),nullptr,true),cJSON_Delete);
    auto *sheet=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j.get(),"sheets"),0);
    auto *grid=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(sheet,"data"),0);
    auto *r=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(grid,"rowData"),0);
    auto *v=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(r,"values"),0);
    if (str(cJSON_GetObjectItemCaseSensitive(v,"userEnteredValue"),"stringValue")==value) return true;
    error_="Expense row "+std::to_string(row+1)+": formulas in A:L are unsupported; use plain text (put reports in other columns or tabs)"; return false;
}
bool GoogleSheetsClient::read(const std::string &id,std::vector<GoogleExpenseRow> &rows) {
    rows.clear(); std::string reply;
    // An unbounded column range is intentional: a clipped prefix must never
    // look like a complete snapshot. HTTP and decoded-content budgets fail closed.
    if (!call(base(id)+"/values/"+oauth_encode("'Expenses'!A:L")+"?valueRenderOption=FORMULA&majorDimension=ROWS","GET","",reply)) return false;
    if (reply.find("\\u0000")!=std::string::npos) { error_="Expense cells containing NUL characters are unsupported"; return false; }
    Json j(cJSON_ParseWithOpts(reply.c_str(),nullptr,true),cJSON_Delete);
    std::string().swap(reply);
    auto *values=cJSON_GetObjectItemCaseSensitive(j.get(),"values"),*first=cJSON_GetArrayItem(values,0);
    auto fail=[&](const std::string &s) { error_=s; return false; };
    if (!cJSON_IsArray(values) || !cJSON_IsArray(first) || cJSON_GetArraySize(first)!=12 || cJSON_GetArraySize(values)>4096) return fail("Expenses tab is missing its complete twelve-column header");
    for (unsigned i=0;i<12;i++) { auto *v=cJSON_GetArrayItem(first,i); if (!cJSON_IsString(v) || std::string(v->valuestring)!=headers[i]) return fail("Keep the original Expenses headers and column order"); }
    size_t bytes=0;
    const int count=cJSON_GetArraySize(values);
    for (int i=1;i<count;i++) {
        Json owner(cJSON_DetachItemFromArray(values,1),cJSON_Delete); auto *row=owner.get();
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row)>12) return fail("Invalid Expense row");
        GoogleExpenseRow r; r.row=i; bool nonempty=false;
        for (unsigned col=0;col<12;col++) {
            auto *v=cJSON_GetArrayItem(row,col); std::string text;
            if (!v || cJSON_IsNull(v)) continue;
            if (cJSON_IsString(v)) text=v->valuestring;
            else if (col==11 && cJSON_IsBool(v)) text=cJSON_IsTrue(v) ? "TRUE" : "FALSE";
            else if ((col==4 || col==10) && cJSON_IsNumber(v) && v->valuedouble>=0 && v->valuedouble<=255 && floor(v->valuedouble)==v->valuedouble) text=std::to_string((unsigned)v->valuedouble);
            else return fail("Expense row "+std::to_string(i+1)+": use plain text for Date and Amount, and text values in the other fields");
            if (!text.empty() && text[0]=='=' && !literal(id,i,col,text)) return false;
            bytes+=text.size(); if (bytes>budget_) return fail("Expenses spreadsheet exceeds the content memory budget");
            nonempty=nonempty || !text.empty();
            if (!col) r.id=text;
            else if (col==11) {
                if (text!="" && text!="FALSE" && text!="TRUE") return fail("Deleted must be TRUE, FALSE or empty");
                r.deleted=text=="TRUE";
            } else r.fields[col-1]=std::move(text);
        }
        if (!nonempty) continue;
        if ((!r.id.empty() && !id_ok(r.id)) || (!r.id.empty() && std::any_of(rows.begin(),rows.end(),[&](const GoogleExpenseRow &v) { return v.id==r.id; }))) return fail("Invalid or repeated Expense Record ID; leave new IDs empty and keep linked IDs unchanged");
        if (rows.size()>=max_) return fail("Expenses spreadsheet exceeds the configured row limit");
        rows.push_back(std::move(r));
    }
    return true;
}
bool GoogleSheetsClient::apply(const std::string &id,const std::vector<GoogleExpenseRow> &snapshot,const std::vector<GoogleExpenseChange> &changes,
                              const std::function<bool()> &before_write) {
    if (changes.empty()) return true;
    std::vector<GoogleExpenseRow> fresh;
    if (!read(id,fresh)) return false;
    if (fresh!=snapshot) { error_="Expenses spreadsheet changed during sync. Finish editing it and sync again."; return false; }
    std::vector<GoogleExpenseRow>().swap(fresh);
    // Values API nulls skip unmodified cells. This keeps the request tree small
    // enough for the ESP32 and preserves all cells that Palm did not change.
    Json j(cJSON_CreateObject(),cJSON_Delete); auto *requests=cJSON_AddArrayToObject(j.get(),"data");
    bool ok=requests && cJSON_AddStringToObject(j.get(),"valueInputOption","RAW");
    // Only changed cells are sent. Reports, formatting and unedited Unicode
    // remain untouched. Native stringValue prevents formula interpretation.
    auto cell=[&](cJSON *values,const std::string &s,bool skip) {
        auto *v=skip ? cJSON_CreateNull() : cJSON_CreateString(s.c_str());
        if (!v || !cJSON_AddItemToArray(values,v)) { cJSON_Delete(v); ok=false; }
    };
    unsigned next=1; for (const auto &row:snapshot) next=std::max(next,row.row+1);
    for (const auto &change:changes) {
        const auto &a=change.after;
        auto before=std::find_if(snapshot.begin(),snapshot.end(),[&](const GoogleExpenseRow &r) { return r.row==change.before_row; });
        if (!change.append && before==snapshot.end()) { error_="Expense update has no matching snapshot row"; return false; }
        GoogleExpenseRow empty; const auto &b=change.append ? empty : *before;
        unsigned row=change.append ? next++ : change.before_row;
        auto *req=cJSON_CreateObject(); if (!req || !cJSON_AddItemToArray(requests,req)) { cJSON_Delete(req); ok=false; break; }
        auto range="'Expenses'!A"+std::to_string(row+1)+":L"+std::to_string(row+1);
        if (!cJSON_AddStringToObject(req,"range",range.c_str()) || !cJSON_AddStringToObject(req,"majorDimension","ROWS")) ok=false;
        auto *rs=cJSON_AddArrayToObject(req,"values"),*vs=cJSON_CreateArray();
        if (!rs || !vs || !cJSON_AddItemToArray(rs,vs)) { cJSON_Delete(vs); ok=false; break; }
        for (unsigned col=0;col<12;col++) {
            std::string after=!col ? a.id : col==11 ? (a.deleted ? "TRUE" : "FALSE") : a.fields[col-1];
            std::string before=!col ? b.id : col==11 ? (b.deleted ? "TRUE" : "FALSE") : b.fields[col-1];
            cell(vs,after,!change.append && after==before);
        }
    }
    if (!ok) { error_="Could not prepare Expense updates"; return false; }
    std::string body=print(j.get()),reply;
    j.reset(); // release the request tree before TLS allocates its buffers
    if (body.empty()) { error_="Could not prepare Expense updates"; return false; }
    if (!before_write()) { error_="Could not save Expense transfer journal"; return false; }
    return call(base(id)+"/values:batchUpdate","POST",body,reply);
}
