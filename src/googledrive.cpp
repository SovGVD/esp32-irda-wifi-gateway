#include "googledrive.h"
#include "mailoauth.h"
#include "calendartext.h"
#include "cJSON.h"
#include <memory>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <limits>

using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
static Json parse(const std::string &s) { return Json(cJSON_ParseWithOpts(s.c_str(),nullptr,true),cJSON_Delete); }
static std::string field(const cJSON *j,const char *name) { auto *p=cJSON_GetObjectItemCaseSensitive(j,name); return cJSON_IsString(p) ? p->valuestring : ""; }
static std::string print(cJSON *j) { char *p=cJSON_PrintUnformatted(j); if (!p) return ""; std::string s=p; free(p); return s; }
static bool valid_id(const std::string &s) { return !s.empty() && s.size()<=256 && s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos; }
static std::string header(const GoogleHeaders &headers,const char *name) { for (const auto &h:headers) if (h.first==name) return h.second; return ""; }
static bool parent(cJSON *j,const std::string &id) {
    auto *a=cJSON_AddArrayToObject(j,"parents"),*s=cJSON_CreateString(id.c_str());
    if (!a || !s || !cJSON_AddItemToArray(a,s)) { cJSON_Delete(s); return false; }
    return true;
}
static bool same_version(const GoogleDriveFile &a,const GoogleDriveFile &b) {
    return a.id==b.id && !b.trashed && a.revision==b.revision && a.md5==b.md5 && a.modified==b.modified && a.size==b.size && a.parents==b.parents;
}
static const char *file_fields="id,name,mimeType,size,headRevisionId,md5Checksum,modifiedTime,appProperties,parents,trashed,version";
static bool number(const std::string &s,uint32_t &n) {
    if (s.empty() || s.size()>10) return false;
    uint64_t value=0; for (unsigned char c:s) { if (c<'0' || c>'9') return false; value=value*10+c-'0'; }
    if (value>0xffffffffu) return false;
    n=(uint32_t)value; return true;
}
static bool unpack(const cJSON *j,GoogleDriveFile &f) {
    if (!cJSON_IsObject(j)) return false;
    f=GoogleDriveFile(); f.id=field(j,"id"); f.name=field(j,"name"); f.mime=field(j,"mimeType");
    f.revision=field(j,"headRevisionId"); f.md5=field(j,"md5Checksum");
    f.version=field(j,"version");
    if (f.version.size()>32 || f.version.find_first_not_of("0123456789")!=std::string::npos) return false;
    f.trashed=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"trashed")); f.folder=f.mime=="application/vnd.google-apps.folder";
    auto modified=field(j,"modifiedTime");
    if (!modified.empty() && !calendar_timestamp(modified,f.modified)) return false;
    if (!valid_id(f.id) || f.name.empty() || f.name.size()>1024 || f.mime.empty() || (!f.revision.empty() && !valid_id(f.revision))) return false;
    auto *size=cJSON_GetObjectItemCaseSensitive(j,"size");
    if (size && !number(field(j,"size"),f.size)) return false;
    auto *parents=cJSON_GetObjectItemCaseSensitive(j,"parents");
    if (parents && !cJSON_IsArray(parents)) return false;
    const cJSON *item; cJSON_ArrayForEach(item,parents) {
        if (!cJSON_IsString(item) || !valid_id(item->valuestring) || f.parents.size()>=4) return false;
        f.parents.emplace_back(item->valuestring);
    }
    auto *props=cJSON_GetObjectItemCaseSensitive(j,"appProperties");
    f.managed=field(props,"palmBackup")=="1"; f.signature=field(props,"palmSignature");
    f.backup_md5=field(props,"palmMD5"); f.db_name=field(props,"palmDB");
    f.sync_marker=field(props,"palmSync");
    return f.signature.size()<=128 && f.db_name.size()<=256 && f.md5.size()<=64 && f.backup_md5.size()<=64 && f.sync_marker.size()<=100;
}
int GoogleDriveClient::call(const std::string &path,const char *method,const std::string &body,std::string &reply,
                           const GoogleHeaders &headers,GoogleHeaders *response_headers,bool upload,size_t download_limit) {
    problem_.clear(); reply.clear();
    if (token_.empty() || token_.find_first_of("\r\n")!=std::string::npos || (body.empty() && strcmp(method,"GET") && strcmp(method,"DELETE"))) {
        // An empty body is allowed for a status query but never a file/folder
        // metadata mutation assembled after an allocation failure.
        if (strcmp(method,"GET") && strcmp(method,"DELETE") && body.empty()) problem_="Could not prepare Drive metadata";
        else problem_="Google Drive is not authorized";
        return -1;
    }
    size_t at=0; GoogleHeaders out;
    std::string url=std::string("https://www.googleapis.com/")+(upload ? "upload/drive/v3" : "drive/v3")+path;
    int code=download_limit ? http_.download(url,"Bearer "+token_,headers,download_limit,reply,out) : http_.transfer(url,
        method,"Bearer "+token_,"application/json",body.size(),[&](uint8_t *p,size_t n) {
            if (n>body.size()-at) return false;
            memcpy(p,body.data()+at,n); at+=n; return true;
        },headers,reply,out);
    if (response_headers) *response_headers=out;
    if (code<200 || code>=300) {
        problem_="Google Drive request failed (HTTP "+std::to_string(code)+")";
        if (code<0 && !http_.error().empty()) problem_="Google Drive: "+http_.error();
        auto j=parse(reply); auto *error=cJSON_GetObjectItemCaseSensitive(j.get(),"error");
        auto message=field(error,"message"); if (!message.empty()) problem_+=". "+message.substr(0,256);
    }
    return code;
}
bool GoogleDriveClient::decode(const std::string &s,GoogleDriveFile &f) {
    auto j=parse(s); if (!unpack(j.get(),f)) { problem_="Invalid Google Drive file metadata"; return false; } return true;
}
bool GoogleDriveClient::get(const std::string &id,GoogleDriveFile &f,bool &found) {
    found=false; if (!valid_id(id)) { problem_="Invalid Google Drive file ID"; return false; }
    std::string reply; GoogleHeaders headers;
    int code=call("/files/"+oauth_encode(id)+"?fields="+oauth_encode(file_fields),"GET","",reply,{},&headers);
    if (code==404 || code==410) { problem_.clear(); return true; }
    if (code!=200 || !decode(reply,f)) return false;
    if (f.id!=id && id!="root") { problem_="Drive returned another file's metadata"; return false; }
    f.etag=header(headers,"etag");
    found=true; return true;
}
bool GoogleDriveClient::root(std::string &id) {
    GoogleDriveFile f; bool found; if (!get("root",f,found)) return false;
    if (!found || !f.folder || f.trashed) { problem_="Could not identify this account's My Drive"; return false; }
    id=f.id; return true;
}
static std::string quote(const std::string &s) { std::string out; for (char c:s) { if (c=='\\' || c=='\'') out+='\\'; out+=c; } return "'"+out+"'"; }
bool GoogleDriveClient::list(const std::string &parent,std::vector<GoogleDriveFile> &files,const std::string &folder_name) {
    files.clear(); if (!valid_id(parent)) { problem_="Invalid Drive parent ID"; return false; }
    std::string query=quote(parent)+" in parents and trashed = false";
    if (!folder_name.empty()) query+=" and mimeType = 'application/vnd.google-apps.folder' and name = "+quote(folder_name);
    std::string page; std::vector<std::string> seen; size_t bytes=0;
    for (unsigned count=0;count<1000;count++) {
        std::string reply,path="/files?spaces=drive&pageSize=20&q="+oauth_encode(query)+"&fields="+
            oauth_encode(std::string("nextPageToken,incompleteSearch,files(")+file_fields+")");
        if (!page.empty()) path+="&pageToken="+oauth_encode(page);
        if (call(path,"GET","",reply)!=200) return false;
        auto j=parse(reply); auto *a=cJSON_GetObjectItemCaseSensitive(j.get(),"files");
        if (!cJSON_IsObject(j.get()) || !cJSON_IsArray(a) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j.get(),"incompleteSearch"))) { problem_="Incomplete Google Drive folder listing"; return false; }
        const cJSON *item; cJSON_ArrayForEach(item,a) {
            GoogleDriveFile f;
            if (!unpack(item,f) || f.trashed || std::find(f.parents.begin(),f.parents.end(),parent)==f.parents.end()) { problem_="Invalid Drive folder entry"; return false; }
            for (const auto &old:files) if (old.id==f.id) { problem_="Drive repeated a folder entry"; return false; }
            bytes+=sizeof(f)+f.id.size()+f.name.size()+f.mime.size()+f.md5.size()+f.revision.size()+f.signature.size()+f.db_name.size()+f.backup_md5.size()+f.sync_marker.size()+f.version.size();
            for (const auto &p:f.parents) bytes+=p.size()+sizeof(std::string);
            if (bytes>budget_ || files.size()>=max_files_) { problem_="Drive folder exceeds the configured file/memory limit"; return false; }
            files.push_back(std::move(f));
        }
        auto next=cJSON_GetObjectItemCaseSensitive(j.get(),"nextPageToken");
        if (next && !cJSON_IsString(next)) { problem_="Invalid Drive page token"; return false; }
        page=field(j.get(),"nextPageToken"); if (page.empty()) return true;
        if (page.size()>4096 || std::find(seen.begin(),seen.end(),page)!=seen.end()) { problem_="Drive repeated an invalid page token"; return false; }
        seen.push_back(page);
    }
    problem_="Too many Drive folder pages"; return false;
}
bool GoogleDriveClient::generate_id(std::string &id) {
    std::string reply; if (call("/files/generateIds?count=1&space=drive&type=files","GET","",reply)!=200) return false;
    auto j=parse(reply); auto *a=cJSON_GetObjectItemCaseSensitive(j.get(),"ids"); auto *p=cJSON_GetArrayItem(a,0);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a)!=1 || !cJSON_IsString(p) || !valid_id(p->valuestring)) { problem_="Invalid generated Drive file ID"; return false; }
    id=p->valuestring; return true;
}
bool GoogleDriveClient::folder(const std::string &parent,const std::string &name,std::string &id,const std::function<bool()> &checkpoint) {
    if (!id.empty()) {
        GoogleDriveFile f; bool found;
        if (!get(id,f,found)) return false;
        if (found) {
            if (!f.folder || f.trashed || f.name!=name || std::find(f.parents.begin(),f.parents.end(),parent)==f.parents.end()) { problem_="Saved Drive backup folder moved, renamed or trashed. Restore its original location/name."; return false; }
            return true;
        }
    } else {
        std::vector<GoogleDriveFile> files; if (!list(parent,files,name)) return false;
        if (files.size()>1) { problem_="Multiple Drive folders named "+name+". Keep one matching backup folder."; return false; }
        if (!files.empty()) { id=files[0].id; return checkpoint(); }
        if (!generate_id(id) || !checkpoint()) return false;
    }
    Json j(cJSON_CreateObject(),cJSON_Delete); if (!j) { problem_="Not enough memory for Drive folder"; return false; }
    if (!cJSON_AddStringToObject(j.get(),"id",id.c_str()) || !cJSON_AddStringToObject(j.get(),"name",name.c_str()) ||
        !cJSON_AddStringToObject(j.get(),"mimeType","application/vnd.google-apps.folder") || !::parent(j.get(),parent)) {
        problem_="Not enough memory for complete Drive folder metadata"; return false;
    }
    std::string reply; int code=call("/files?fields="+oauth_encode(file_fields),"POST",print(j.get()),reply);
    if (code==200 || code==201) { GoogleDriveFile f; return decode(reply,f) && f.id==id && f.folder; }
    // Preassigned, persisted IDs make a lost creation reply safe to reconcile.
    GoogleDriveFile f; bool found; std::string first=problem_;
    if (get(id,f,found) && found && f.folder && f.name==name && std::find(f.parents.begin(),f.parents.end(),parent)!=f.parents.end()) return true;
    problem_=first; return false;
}
bool GoogleDriveClient::upload(const GoogleDriveFile &target,bool create,const std::string &parent,const std::string &name,
                              const std::string &db_name,const std::string &signature,int64_t modified,size_t size,const GoogleRead &read,GoogleDriveFile &saved,
                              const std::string &mime,const std::string &sync_marker,const std::function<bool()> &rewind) {
    if (!valid_id(target.id) || !size || size>0xffffffffu) { problem_="Invalid backup upload target/size"; return false; }
    bool backup=mime=="application/octet-stream" && sync_marker.empty();
    GoogleDriveFile current;
    auto unchanged=[&](const GoogleDriveFile &expected) {
        bool found;
        if (!get(expected.id,current,found)) return false;
        if (!found || !same_version(expected,current)) { problem_="Drive file changed during backup. Run HotSync again."; return false; }
        return true;
    };
    if (!create && !unchanged(target)) return false;
    Json j(cJSON_CreateObject(),cJSON_Delete);
    if (!j) { problem_="Not enough memory for Drive upload"; return false; }
    bool complete=(!create || (cJSON_AddStringToObject(j.get(),"id",target.id.c_str()) && ::parent(j.get(),parent))) &&
        cJSON_AddStringToObject(j.get(),"name",name.c_str()) && cJSON_AddStringToObject(j.get(),"mimeType",mime.c_str()) &&
        cJSON_AddStringToObject(j.get(),"modifiedTime",calendar_utc(modified).c_str());
    auto *props=cJSON_AddObjectToObject(j.get(),"appProperties");
    if (!complete || !props || (backup ?
        (!cJSON_AddStringToObject(props,"palmBackup","1") || !cJSON_AddStringToObject(props,"palmDB",db_name.c_str()) || !cJSON_AddStringToObject(props,"palmSignature",signature.c_str())) :
        (!sync_marker.empty() && !cJSON_AddStringToObject(props,"palmSync",sync_marker.c_str())))) {
        problem_="Not enough memory for complete Drive upload metadata"; return false;
    }
    std::string metadata=print(j.get()); j.reset();
    GoogleHeaders headers,out; std::string reply;
    const std::string path=create ? "/files" : "/files/"+oauth_encode(target.id);
    bool uploaded=false;
    for (unsigned attempt=0;attempt<2 && !uploaded;attempt++) {
        if (!create && !unchanged(target)) return false;
        headers={{"X-Upload-Content-Type",mime},{"X-Upload-Content-Length",std::to_string(size)}};
        if (!create && !current.etag.empty()) headers.emplace_back("If-Match",current.etag);
        if (call(path+"?uploadType=resumable&fields="+oauth_encode(file_fields),create ? "POST" : "PATCH",metadata,reply,headers,&out,true)<200 || !problem_.empty()) return false;
        const std::string location=header(out,"location");
        if (location.rfind("https://www.googleapis.com/",0)!=0 || location.find_first_of("\r\n")!=std::string::npos) { problem_="Invalid Drive resumable upload URL"; return false; }
        bool restart=false;
        for (size_t at=0;at<size;) {
            if (!create && !unchanged(target)) return false;
            size_t length=std::min((size_t)262144,size-at);
            headers={{"Content-Range","bytes "+std::to_string(at)+"-"+std::to_string(at+length-1)+"/"+std::to_string(size)}};
            if (!create && !current.etag.empty()) headers.emplace_back("If-Match",current.etag);
            int code=http_.transfer(location,"PUT","Bearer "+token_,mime.c_str(),length,read,headers,reply,out);
            if (code==308 && at+length<size && header(out,"range")=="bytes=0-"+std::to_string(at+length-1)) { at+=length; continue; }
            if ((code==200 || code==201) && at+length==size && decode(reply,saved) && saved.id==target.id && saved.size==size && !saved.md5.empty()) { uploaded=true; break; }
            // A 404 invalidates the resumable session, not the OAuth grant.
            // Restart once using the SAME file ID and a validated rewound source.
            // Ambiguous transport/5xx failures retain their existing journal flow.
            if (code==404 && !attempt && rewind && rewind()) { restart=true; break; }
            problem_=(code==404 ? "Drive upload session was not found" : "Drive backup upload was not confirmed");
            problem_+=" (HTTP "+std::to_string(code)+"), bytes "+std::to_string(at)+"-"+std::to_string(at+length-1);
            if (code==404) problem_+=". Next HotSync will open a new upload session using the saved file ID.";
            if (code<0 && !http_.error().empty()) problem_+=". "+http_.error();
            auto error=parse(reply); auto message=field(cJSON_GetObjectItemCaseSensitive(error.get(),"error"),"message");
            if (!message.empty()) problem_+=". "+message.substr(0,200);
            return false;
        }
        if (!uploaded && !restart) { problem_="Drive upload did not complete"; return false; }
    }
    if (!uploaded) { problem_="Drive upload session could not be restarted"; return false; }
    if (!backup) return true;
    // Store Drive's content digest with our metadata. Later external changes
    // invalidate the unchanged-backup shortcut even if Palm metadata matches.
    if (!unchanged(saved)) return false;
    Json mark(cJSON_CreateObject(),cJSON_Delete); auto *p=cJSON_AddObjectToObject(mark.get(),"appProperties");
    if (!mark || !p || !cJSON_AddStringToObject(p,"palmMD5",saved.md5.c_str()) ||
        !cJSON_AddStringToObject(mark.get(),"modifiedTime",calendar_utc(modified).c_str())) {
        problem_="Not enough memory for Drive backup confirmation"; return false;
    }
    headers.clear(); if (!current.etag.empty()) headers.emplace_back("If-Match",current.etag);
    if (call("/files/"+oauth_encode(target.id)+"?fields="+oauth_encode(file_fields),"PATCH",print(mark.get()),reply,headers)!=200) return false;
    return decode(reply,saved) && saved.id==target.id;
}
bool GoogleDriveClient::read_range(const GoogleDriveFile &f,uint32_t offset,size_t length,std::string &data) {
    if (!valid_id(f.id) || !valid_id(f.revision) || offset>f.size || length>f.size-offset || length>8192) { problem_="Invalid or unversioned Drive backup range"; return false; }
    if (!length) { data.clear(); return true; }
    GoogleHeaders headers={{"Range","bytes="+std::to_string(offset)+"-"+std::to_string(offset+length-1)}},out;
    std::string path="/files/"+oauth_encode(f.id)+"/revisions/"+oauth_encode(f.revision)+"?alt=media";
    int code=call(path,"GET","",data,headers,&out);
    std::string expected="bytes "+std::to_string(offset)+"-"+std::to_string(offset+length-1)+"/"+std::to_string(f.size);
    if (((code==206 && header(out,"content-range")==expected) || (code==200 && !offset && length==f.size)) && data.size()==length) { problem_.clear(); return true; }
    if (code==206 || code==200) problem_="Drive returned an incomplete or mismatched file range";
    return false;
}
bool GoogleDriveClient::download(const GoogleDriveFile &f,size_t limit,std::string &data) {
    if (!valid_id(f.id) || !valid_id(f.revision) || !f.size || f.size>limit) { problem_="Invalid, unversioned or oversized Drive file download"; return false; }
    GoogleHeaders headers={{"Range","bytes=0-"+std::to_string(f.size-1)}},out;
    int code=call("/files/"+oauth_encode(f.id)+"/revisions/"+oauth_encode(f.revision)+"?alt=media","GET","",data,headers,&out,false,f.size);
    std::string expected="bytes 0-"+std::to_string(f.size-1)+"/"+std::to_string(f.size);
    if ((code==200 || (code==206 && header(out,"content-range")==expected)) && data.size()==f.size) { problem_.clear(); return true; }
    if (code==200 || code==206) problem_="Drive returned an incomplete or mismatched file download";
    return false;
}
bool GoogleDriveReader::read(uint32_t offset,uint8_t *data,size_t length) {
    if (offset>file_.size || length>file_.size-offset) return false;
    while (length) {
        auto &cache=offset<table_end_ ? table_ : body_;
        if (cache.data.empty() || offset<cache.offset || (uint64_t)offset>=cache.offset+cache.data.size()) {
            cache.offset=offset/4096*4096;
            size_t n=std::min((size_t)4096,(size_t)(file_.size-cache.offset));
            if (!client_.read_range(file_,cache.offset,n,cache.data)) return false;
        }
        size_t at=offset-cache.offset,n=std::min(length,cache.data.size()-at);
        memcpy(data,cache.data.data()+at,n); data+=n; offset+=n; length-=n;
    }
    return true;
}
