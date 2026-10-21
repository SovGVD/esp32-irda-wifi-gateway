#include "googlenotes.h"
#include "mailoauth.h"
#include "cJSON.h"
#include <memory>
#include <cstring>
#include <cmath>
#include <algorithm>
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
static std::string field(const cJSON *j,const char *name) { auto *p=cJSON_GetObjectItemCaseSensitive(j,name); return cJSON_IsString(p) ? p->valuestring : ""; }
static std::string print(cJSON *j) { char *p=cJSON_PrintUnformatted(j); if (!p) return ""; std::string s=p; free(p); return s; }
static bool id_ok(const std::string &id) { return !id.empty() && id.size()<=256 && id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos; }
static bool integer(const cJSON *j,unsigned &n) { if (!cJSON_IsNumber(j) || j->valuedouble<0 || j->valuedouble>10000000 || floor(j->valuedouble)!=j->valuedouble) return false; n=(unsigned)j->valuedouble; return true; }
static bool utf16_size(const std::string &s,unsigned &units) {
    units=0;
    for (size_t i=0;i<s.size();) {
        unsigned c=(uint8_t)s[i++],more=0,min=0;
        if (c<0x80) { units++; continue; }
        if (c>=0xc2 && c<=0xdf) { c&=31; more=1; min=0x80; }
        else if (c>=0xe0 && c<=0xef) { c&=15; more=2; min=0x800; }
        else if (c>=0xf0 && c<=0xf4) { c&=7; more=3; min=0x10000; }
        else return false;
        while (more--) { if (i==s.size() || ((uint8_t)s[i]&0xc0)!=0x80) return false; c=(c<<6)|((uint8_t)s[i++]&63); }
        if (c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return false;
        units+=c>=0x10000 ? 2 : 1;
    }
    return true;
}
bool GoogleNotesClient::call(const std::string &url,const char *method,const std::string &body,std::string &reply,const std::string &etag) {
    error_.clear();
    if (token_.empty() || (strcmp(method,"GET") && body.empty())) { error_="Could not prepare Google notes request"; return false; }
    int code=http_.request_if_match(url,method,"Bearer "+token_,"application/json",body,etag,reply);
    if (code>=200 && code<300) return true;
    error_="Google notes request failed (HTTP "+std::to_string(code)+")";
    if (code<0 && !http_.error().empty()) error_+=". "+http_.error();
    Json j(cJSON_ParseWithOpts(reply.c_str(),nullptr,true),cJSON_Delete); auto message=field(cJSON_GetObjectItemCaseSensitive(j.get(),"error"),"message");
    if (!message.empty()) error_+=". "+message.substr(0,200);
    return false;
}
bool GoogleNotesClient::read(const GoogleDriveFile &file,bool drawing,GoogleNote &out) {
    GoogleDriveFile input=file;
    out=GoogleNote(); out.file=std::move(input);
    const GoogleDriveFile &f=out.file;
    auto fail=[&](const char *s) { error_=file.name+": "+s; return false; };
    if (drawing) {
        if (f.size>PALM_NOTE_PNG_MAX || !f.size) return fail("PNG exceeds the 64 KiB limit or is empty");
        // Do not reserve/retain the full PNG before opening TLS, or reopen TLS
        // for every chunk beside an ever larger accumulated image.
        std::string png;
        if (!drive.download(f,PALM_NOTE_PNG_MAX,png)) { error_=f.name+": Download PNG: "+drive.error(); return false; }
        if (!note_read_png(png,out.note,error_)) { error_=f.name+": "+error_; return false; }
        out.note.title=f.name.substr(0,f.name.size()-4); return true;
    }
    std::string reply;
    // Omit named styles, document styles, headers and other metadata that can
    // dwarf a short memo. Keep bounds, formatting indicators and every array
    // element: a masked-out table/object still leaves an invalid text element
    // and is rejected rather than silently converted into incomplete text.
    const std::string body_fields="body(content(endIndex,sectionBreak,paragraph(bullet,paragraphStyle(namedStyleType),elements(textRun(content,textStyle)))))";
    // includeTabsContent forbids legacy document-level text fields. Request
    // the body only inside documentTab, alongside the revision guard.
    const std::string fields="documentId,revisionId,tabs(tabProperties(tabId),childTabs(tabProperties(tabId)),documentTab("+body_fields+"))";
    if (!call("https://docs.googleapis.com/v1/documents/"+oauth_encode(f.id)+"?includeTabsContent=true&fields="+oauth_encode(fields),"GET","",reply)) {
        error_="Read memo: "+error_; return false;
    }
    Json j(cJSON_ParseWithOpts(reply.c_str(),nullptr,true),cJSON_Delete);
    std::string().swap(reply); // release encoded JSON before building memo text
    if (!j) return fail("Could not parse Google Docs JSON within available memory");
    if (field(j.get(),"documentId")!=f.id || (out.revision=field(j.get(),"revisionId")).empty()) return fail("Missing Google Docs identity/revision");
    if (out.revision.size()>2048) return fail("Google Docs revision exceeds its memory limit");
    auto *tabs=cJSON_GetObjectItemCaseSensitive(j.get(),"tabs"),*tab=cJSON_GetArrayItem(tabs,0);
    const cJSON *document=j.get();
    if (tabs) {
        if (!cJSON_IsArray(tabs) || cJSON_GetArraySize(tabs)!=1 || cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(tab,"childTabs"))) return fail("Only a single document tab is supported");
        out.tab=field(cJSON_GetObjectItemCaseSensitive(tab,"tabProperties"),"tabId");
        // Tab IDs are opaque strings, not Drive file IDs. Google's default
        // first tab is t.0; JSON encoding safely carries the ID back on writes.
        if (out.tab.empty() || out.tab.size()>256 ||
            std::any_of(out.tab.begin(),out.tab.end(),[](unsigned char c) { return c<32 || c==127; })) return fail("Invalid Google Docs tab ID");
        document=cJSON_GetObjectItemCaseSensitive(tab,"documentTab");
    }
    auto *content=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(document,"body"),"content");
    if (!cJSON_IsArray(content)) return fail("Document has no complete text body");
    const cJSON *item; unsigned previous=0;
    cJSON_ArrayForEach(item,content) {
        auto *paragraph=cJSON_GetObjectItemCaseSensitive(item,"paragraph"); unsigned end;
        if (!integer(cJSON_GetObjectItemCaseSensitive(item,"endIndex"),end) || end<=previous) return fail("Invalid Google Docs body bounds");
        previous=end;
        if (!paragraph) {
            if (cJSON_GetObjectItemCaseSensitive(item,"sectionBreak") && end<=1) continue;
            return fail("Tables, images and other non-text content cannot become a Palm memo");
        }
        if (cJSON_GetObjectItemCaseSensitive(paragraph,"bullet")) out.plain=false;
        auto style=field(cJSON_GetObjectItemCaseSensitive(paragraph,"paragraphStyle"),"namedStyleType");
        if (!style.empty() && style!="NORMAL_TEXT") out.plain=false;
        auto *elements=cJSON_GetObjectItemCaseSensitive(paragraph,"elements");
        if (!cJSON_IsArray(elements)) return fail("Invalid Google Docs paragraph");
        const cJSON *e; cJSON_ArrayForEach(e,elements) {
            auto *run=cJSON_GetObjectItemCaseSensitive(e,"textRun");
            if (!cJSON_IsObject(run) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(run,"content"))) return fail("Non-text paragraph element is unsupported");
            out.note.text+=field(run,"content");
            if (out.note.text.size()>16384) return fail("Document exceeds the memo text limit");
            auto *format=cJSON_GetObjectItemCaseSensitive(run,"textStyle"); if (cJSON_IsObject(format) && format->child) out.plain=false;
        }
    }
    if (out.note.text.empty() || out.note.text.back()!='\n' || previous<2) return fail("Document lacks its terminal paragraph");
    out.note.text.pop_back(); out.end=previous-1;
    unsigned units;
    if (!utf16_size(out.note.text,units) || out.end!=units+1) return fail("Incomplete Google Docs text or mismatched UTF-16 bounds");
    return true;
}
bool GoogleNotesClient::create_memo(const std::string &folder,const std::string &title,const std::string &marker,GoogleDriveFile &file) {
    Json j(cJSON_CreateObject(),cJSON_Delete);
    auto *parents=cJSON_AddArrayToObject(j.get(),"parents"),*props=cJSON_AddObjectToObject(j.get(),"appProperties");
    auto *parent=cJSON_CreateString(folder.c_str());
    if (!j || !parents || !props || !parent) { cJSON_Delete(parent); error_="Not enough memory for memo metadata"; return false; }
    if (!cJSON_AddItemToArray(parents,parent)) { cJSON_Delete(parent); error_="Not enough memory for memo folder"; return false; }
    if (!cJSON_AddStringToObject(j.get(),"name",title.c_str()) || !cJSON_AddStringToObject(j.get(),"mimeType","application/vnd.google-apps.document") ||
        !cJSON_AddStringToObject(props,"palmSync",marker.c_str())) { error_="Not enough memory for complete memo metadata"; return false; }
    std::string body=print(j.get()),reply; j.reset();
    if (body.empty()) { error_="Could not encode memo creation metadata"; return false; }
    if (!call("https://www.googleapis.com/drive/v3/files?fields=id","POST",body,reply)) { error_="Create memo: "+error_; return false; }
    Json response(cJSON_ParseWithOpts(reply.c_str(),nullptr,true),cJSON_Delete); auto id=field(response.get(),"id"); bool found;
    if (!id_ok(id)) { error_="Google did not confirm the new memo ID; check the next sync"; return false; }
    response.reset(); std::string().swap(reply); std::string().swap(body);
    if (!drive.get(id,file,found)) { error_=drive.error(); return false; }
    if (!found || file.sync_marker!=marker || file.mime!="application/vnd.google-apps.document" || std::find(file.parents.begin(),file.parents.end(),folder)==file.parents.end()) {
        error_="New memo metadata is not confirmed; check the next sync"; return false;
    }
    return true;
}
bool GoogleNotesClient::write_memo(const GoogleNote &old,const std::string &text) {
    if (!old.plain) { error_="Memo has Google Docs formatting. Remove formatting before replacing its text from Palm."; return false; }
    Json j(cJSON_CreateObject(),cJSON_Delete); auto *requests=cJSON_AddArrayToObject(j.get(),"requests"),*control=cJSON_AddObjectToObject(j.get(),"writeControl");
    bool ok=j && requests && control && cJSON_AddStringToObject(control,"requiredRevisionId",old.revision.c_str());
    auto add=[&](const char *name) {
        auto *request=cJSON_CreateObject(); if (!request || !cJSON_AddItemToArray(requests,request)) { cJSON_Delete(request); return (cJSON *)nullptr; }
        return cJSON_AddObjectToObject(request,name);
    };
    if (ok && old.end>1) {
        auto *d=add("deleteContentRange"),*range=cJSON_AddObjectToObject(d,"range");
        ok=range && cJSON_AddNumberToObject(range,"startIndex",1) && cJSON_AddNumberToObject(range,"endIndex",old.end) &&
            (old.tab.empty() || cJSON_AddStringToObject(range,"tabId",old.tab.c_str()));
    }
    if (ok && !text.empty()) {
        auto *insert=add("insertText"),*where=cJSON_AddObjectToObject(insert,"location");
        ok=insert && where && cJSON_AddStringToObject(insert,"text",text.c_str()) && cJSON_AddNumberToObject(where,"index",1) &&
            (old.tab.empty() || cJSON_AddStringToObject(where,"tabId",old.tab.c_str()));
    }
    if (!ok) { error_="Not enough memory for complete memo update"; return false; }
    if (!cJSON_GetArraySize(requests)) return true;
    std::string body=print(j.get()),reply; j.reset();
    if (body.empty()) { error_="Could not encode memo update"; return false; }
    // Only the HTTP status is needed from a successful revision-guarded write.
    if (!call("https://docs.googleapis.com/v1/documents/"+oauth_encode(old.file.id)+":batchUpdate?fields=documentId","POST",body,reply)) {
        error_="Write memo: "+error_; return false;
    }
    return true;
}
bool GoogleNotesClient::write_drawing(const GoogleDriveFile &old,bool create,const std::string &folder,const PalmNote &note,const std::string &title,const std::string &marker,int64_t now,GoogleDriveFile &saved) {
    std::string png=note_png(note); if (png.empty()) { error_="Could not encode Note Pad drawing"; return false; }
    size_t at=0;
    if (!drive.upload(old,create,folder,(title.empty() ? "Untitled note" : title)+".png","","",now,png.size(),[&](uint8_t *p,size_t n) {
        if (n>png.size()-at) return false;
        memcpy(p,png.data()+at,n); at+=n; return true;
    },saved,"image/png",marker,[&]() { at=0; return true; })) { error_=drive.error(); return false; }
    return true;
}
bool GoogleNotesClient::rename_drawing(const GoogleDriveFile &old,const std::string &title) {
    GoogleDriveFile current; bool found;
    if (!drive.get(old.id,current,found)) { error_=drive.error(); return false; }
    if (!found || current.trashed || current.modified!=old.modified || current.revision!=old.revision || current.md5!=old.md5 || current.version!=old.version) { error_="Drawing changed before rename; sync again"; return false; }
    Json j(cJSON_CreateObject(),cJSON_Delete);
    if (!j || !cJSON_AddStringToObject(j.get(),"name",(title+".png").c_str())) { error_="Could not prepare drawing title"; return false; }
    std::string reply; return call("https://www.googleapis.com/drive/v3/files/"+oauth_encode(old.id),"PATCH",print(j.get()),reply,current.etag);
}
bool GoogleNotesClient::trash(const GoogleDriveFile &old) {
    GoogleDriveFile current; bool found;
    if (!drive.get(old.id,current,found)) { error_=drive.error(); return false; }
    if (!found || current.trashed) return true;
    if (current.modified!=old.modified || current.revision!=old.revision || current.md5!=old.md5 || current.version!=old.version) { error_="Google note changed before deletion; sync again"; return false; }
    std::string reply; return call("https://www.googleapis.com/drive/v3/files/"+oauth_encode(old.id),"PATCH","{\"trashed\":true}",reply,current.etag);
}
