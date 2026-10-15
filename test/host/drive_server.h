#pragma once
// Scripted Drive v3 server shared by backup and application-conduit tests.
#include "googlehttp.h"
#include "calendartext.h"
#include "cJSON.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <algorithm>
static std::string field(cJSON *j,const char *key) { auto *p=cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsString(p) ? p->valuestring : ""; }
static std::string print(cJSON *j) { char *p=cJSON_PrintUnformatted(j); assert(p); std::string s=p; free(p); cJSON_Delete(j); return s; }
static std::string decode(const std::string &s) { std::string out; for (size_t i=0;i<s.size();i++) { if (s[i]=='%' && i+2<s.size()) { out+=(char)strtol(s.substr(i+1,2).c_str(),nullptr,16); i+=2; } else out+=s[i]; } return out; }
static std::string param(const std::string &url,const std::string &key) { auto p=url.find(key+"="); if (p==std::string::npos) return ""; p+=key.size()+1; return decode(url.substr(p,url.find('&',p)-p)); }
static std::string digest(const std::string &s) { uint32_t h=2166136261u; for (unsigned char c:s) { h^=c; h*=16777619u; } char p[33]; snprintf(p,sizeof(p),"%08x%08x%08x%08x",h,h,h,h); return p; }
static int64_t when(const char *s) { int64_t n; assert(calendar_timestamp(s,n)); return n; }
static void p16(std::string &s,size_t p,uint16_t n) { s[p]=n>>8; s[p+1]=n; }
static void p32(std::string &s,size_t p,uint32_t n) { s[p]=n>>24; s[p+1]=n>>16; s[p+2]=n>>8; s[p+3]=n; }
static std::string pdb(const std::string &name,const std::string &payload) {
    // Independent standard PDB fixture: fixed header, record list, app/sort
    // blocks, secret/category bits, and a preserved 24-bit record ID.
    std::string s(88,0); memcpy(&s[0],name.data(),name.size()); p16(s,32,8); p16(s,34,2);
    p32(s,36,3874838400u); p32(s,40,3874838400u); p32(s,44,3874838400u);
    p32(s,48,1); p32(s,52,88); p32(s,56,91); memcpy(&s[60],"DATA",4); memcpy(&s[64],"Tst2",4);
    p32(s,68,8); p16(s,76,1); p32(s,78,94); s[82]=0x12; s[85]=7;
    s+="app"; s+="srt"; s+=payload; return s;
}

struct File {
    std::string id,name,parent,mime="application/octet-stream",data,revision,md5;
    int64_t modified=0;
    uint64_t version=1;
    std::map<std::string,std::string> props;
    bool folder=false,trashed=false;
};
class Server : public GoogleHttp {
public:
    std::map<std::string,File> files;
    std::map<std::string,std::string> revisions;
    struct Session { File file; std::string data; size_t size=0; };
    std::map<std::string,Session> sessions;
    unsigned next_id=1,next_revision=1,uploads=0,chunks=0,creates=0,downloads=0;
    bool lose_folder=false,lose_upload=false,deny=false,short_range=false,duplicate_page=false;
    bool fail_recovery=false,race_upload=false;
    unsigned expired_sessions=0,session_starts=0;
    bool expire_after_chunk=false;
    std::string fail_range_id;
    Server() { File f; f.id="my-root"; f.name="My Drive"; f.folder=true; f.mime="application/vnd.google-apps.folder"; files[f.id]=f; }
    void commit(File &f) { f.version++; f.md5=digest(f.data); f.revision="revision-"+std::to_string(next_revision++); revisions[f.id+":"+f.revision]=f.data; }
    File &add(const std::string &name,const std::string &parent,const std::string &data,int64_t modified) {
        File f; f.id="manual-"+std::to_string(next_id++); f.name=name; f.parent=parent; f.data=data; f.modified=modified;
        commit(f); auto id=f.id; files[id]=f; return files[id];
    }
    cJSON *json(const File &f) {
        auto *j=cJSON_CreateObject(); cJSON_AddStringToObject(j,"id",f.id.c_str()); cJSON_AddStringToObject(j,"name",f.name.c_str());
        cJSON_AddStringToObject(j,"version",std::to_string(f.version).c_str());
        cJSON_AddStringToObject(j,"mimeType",f.mime.c_str()); cJSON_AddBoolToObject(j,"trashed",f.trashed);
        auto *parents=cJSON_AddArrayToObject(j,"parents"); if (!f.parent.empty()) cJSON_AddItemToArray(parents,cJSON_CreateString(f.parent.c_str()));
        if (!f.folder) {
            cJSON_AddStringToObject(j,"size",std::to_string(f.data.size()).c_str()); cJSON_AddStringToObject(j,"headRevisionId",f.revision.c_str());
            cJSON_AddStringToObject(j,"md5Checksum",f.md5.c_str()); cJSON_AddStringToObject(j,"modifiedTime",calendar_utc(f.modified).c_str());
            auto *p=cJSON_AddObjectToObject(j,"appProperties"); for (const auto &kv:f.props) cJSON_AddStringToObject(p,kv.first.c_str(),kv.second.c_str());
        }
        return j;
    }
    void metadata(File &f,cJSON *j) {
        f.version++;
        if (cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(j,"trashed"))) f.trashed=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"trashed"));
        if (!field(j,"name").empty()) f.name=field(j,"name");
        if (!field(j,"mimeType").empty()) f.mime=field(j,"mimeType");
        f.folder=f.mime=="application/vnd.google-apps.folder";
        auto *p=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j,"parents"),0); if (cJSON_IsString(p)) f.parent=p->valuestring;
        auto *props=cJSON_GetObjectItemCaseSensitive(j,"appProperties"); const cJSON *item;
        cJSON_ArrayForEach(item,props) { assert(cJSON_IsString(item)); f.props[item->string]=item->valuestring; }
        auto modified=field(j,"modifiedTime"); if (!modified.empty()) assert(calendar_timestamp(modified,f.modified));
    }
    int request(const std::string &,const char *,const std::string &,const char *,const std::string &,std::string &) override { assert(false); return -1; }
    int transfer(const std::string &url,const char *method,const std::string &auth,const char *,size_t size,const GoogleRead &read,
                 const GoogleHeaders &headers,std::string &reply,GoogleHeaders &out) override {
        assert(auth=="Bearer token" && url.rfind("https://www.googleapis.com/",0)==0);
        out.clear(); reply="{}";
        if (deny) { reply="{\"error\":{\"message\":\"Drive API disabled\"}}"; return 403; }
        std::string body;
        for (size_t at=0;at<size;) { uint8_t b[257]; size_t n=std::min(sizeof(b),size-at); if (!read(b,n)) return -1; body.append((const char *)b,n); at+=n; }
        std::map<std::string,std::string> h; for (const auto &p:headers) h[p.first]=p.second;
        if (!strcmp(method,"PUT")) {
            assert(sessions.count(url)); auto &s=sessions[url]; unsigned start,end,total;
            assert(sscanf(h["Content-Range"].c_str(),"bytes %u-%u/%u",&start,&end,&total)==3 && start==s.data.size() && total==s.size && end-start+1==body.size());
            if (expired_sessions && (!expire_after_chunk || start>0)) {
                expired_sessions--; sessions.erase(url); reply="{\"error\":{\"message\":\"Upload session not found\"}}"; return 404;
            }
            assert(end+1==total || body.size()%262144==0); chunks++; s.data+=body;
            if (s.data.size()<s.size) { out={{"range","bytes=0-"+std::to_string(s.data.size()-1)}}; return 308; }
            assert(s.data.size()==s.size); s.file.data=s.data; commit(s.file); files[s.file.id]=s.file;
            reply=print(json(s.file)); uploads++; sessions.erase(url);
            if (lose_upload) { lose_upload=false; return -1; } return 200;
        }
        auto split=url.find("/drive/v3"); assert(split!=std::string::npos); std::string path=url.substr(split+9); path=path.substr(0,path.find('?'));
        if (path=="/files/generateIds") { reply="{\"ids\":[\"generated-"+std::to_string(next_id++)+"\"]}"; return 200; }
        if (path=="/files" && !strcmp(method,"GET")) {
            std::string q=param(url,"q"); auto first=q.find('\''),last=q.find('\'',first+1); assert(first!=std::string::npos && last!=std::string::npos);
            std::string parent=q.substr(first+1,last-first-1),name;
            auto at=q.find(" and name = '"); if (at!=std::string::npos) name=q.substr(at+13,q.size()-at-14);
            std::vector<File> matching;
            for (const auto &kv:files) if (!kv.second.trashed && kv.second.parent==parent && (name.empty() || (kv.second.folder && kv.second.name==name))) matching.push_back(kv.second);
            unsigned page=param(url,"pageToken").empty() ? 0 : std::stoul(param(url,"pageToken"));
            auto *j=cJSON_CreateObject(),*a=cJSON_AddArrayToObject(j,"files");
            for (unsigned n=page;n<std::min((unsigned)matching.size(),page+2);n++) cJSON_AddItemToArray(a,json(matching[n]));
            if (page+2<matching.size()) cJSON_AddStringToObject(j,"nextPageToken",std::to_string(duplicate_page ? page : page+2).c_str());
            cJSON_AddBoolToObject(j,"incompleteSearch",false); reply=print(j); return 200;
        }
        std::string id=path=="/files" ? "" : path.substr(7); auto slash=id.find('/'); if (slash!=std::string::npos) id=id.substr(0,slash);
        if (id=="root") id="my-root";
        if (path.find("/revisions/")!=std::string::npos) {
            downloads++; std::string revision=path.substr(path.find("/revisions/")+11); auto key=id+":"+revision;
            if (!revisions.count(key)) return 404;
            auto &data=revisions[key]; unsigned start,end;
            assert(sscanf(h["Range"].c_str(),"bytes=%u-%u",&start,&end)==2 && end>=start && end<data.size());
            if (id==fail_range_id && start>=4096) return 503;
            reply=data.substr(start,end-start+1); if (short_range) reply.pop_back();
            out={{"content-range","bytes "+std::to_string(start)+"-"+std::to_string(end)+"/"+std::to_string(data.size())}}; return 206;
        }
        if (!strcmp(method,"GET")) {
            if (!files.count(id)) return 404;
            reply=print(json(files[id])); out={{"etag","\""+files[id].revision+"\""}}; return 200;
        }
        auto *j=cJSON_Parse(body.c_str()); assert(j);
        if (param(url,"uploadType")=="resumable") {
            bool create=!strcmp(method,"POST");
            File f;
            if (create) { f.id=field(j,"id"); assert(!f.id.empty()); if (files.count(f.id)) { cJSON_Delete(j); return 409; } }
            else { assert(files.count(id)); f=files[id]; }
            if (!create && !f.revision.empty()) assert(h["If-Match"]=="\""+f.revision+"\"");
            metadata(f,j); cJSON_Delete(j);
            if (fail_recovery && files.count(f.parent) && files[f.parent].name=="Recovery") return 503;
            if (!create && race_upload) { race_upload=false; files[id].data=pdb(f.props["palmDB"],"Concurrent Drive edit"); files[id].modified+=120; commit(files[id]); }
            std::string session="https://www.googleapis.com/upload/drive/v3/session-"+std::to_string(next_id++);
            session_starts++;
            sessions[session]={f,"",(size_t)std::stoul(h["X-Upload-Content-Length"])}; out={{"location",session}}; return 200;
        }
        if (!strcmp(method,"POST")) {
            File f; f.id=field(j,"id"); assert(!f.id.empty()); metadata(f,j); cJSON_Delete(j);
            if (files.count(f.id)) return 409;
            files[f.id]=f; creates++; reply=print(json(f)); if (lose_folder) { lose_folder=false; return -1; } return 200;
        }
        assert(!strcmp(method,"PATCH") && files.count(id)); metadata(files[id],j); cJSON_Delete(j); reply=print(json(files[id])); return 200;
    }
};
