#include "notesync.h"
#include "hotsyncuser.h"
#include "mailoauth.h"
#include "drive_server.h"
#include <set>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <new>
#include <cstdlib>
#include "data/notes_png_fixtures.h"

// Track live C++ allocations during a whole sync, including strings/vectors
// retained between image downloads. Fixtures already in the mock server are
// outside this measurement, like files stored remotely on Google Drive.
static struct { void *p; size_t bytes; } measured[8192];
static bool measure_memory=false;
static size_t measured_live=0,measured_peak=0;
void *operator new(size_t n) {
    void *p=malloc(n ? n : 1); if (!p) throw std::bad_alloc();
    if (measure_memory) {
        bool added=false;
        for (auto &entry:measured) if (!entry.p) { entry={p,n}; added=true; break; }
        assert(added); measured_live+=n; measured_peak=std::max(measured_peak,measured_live);
    }
    return p;
}
static void unmeasure(void *p) {
    if (p) for (auto &entry:measured) if (entry.p==p) { measured_live-=entry.bytes; entry={}; break; }
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { unmeasure(p); free(p); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p,size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p,size_t) noexcept { ::operator delete(p); }
static void memory_begin() {
    for (auto &entry:measured) entry={};
    measured_live=measured_peak=0; measure_memory=true;
}
static void memory_end() { measure_memory=false; }
static size_t tracked_bytes(void *p) {
    for (const auto &entry:measured) if (entry.p==p) return entry.bytes;
    return 0;
}

static int fd=-1;
static std::function<bool(size_t)> test_reserve;
static int io_read(uint8_t *p,size_t n,uint32_t ms) { pollfd f={fd,POLLIN,0}; if (poll(&f,1,ms)<=0) return 0; int r=::read(fd,p,n); return r>0 ? r : -1; }
static bool io_write(const uint8_t *p,size_t n) { return ::write(fd,p,n)==(ssize_t)n; }
static uint32_t ticks() { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000; }
static unsigned utf16(const std::string &s) { unsigned n=0; for (unsigned char c:s) if ((c&0xc0)!=0x80) n+=c>=0xf0 ? 2 : 1; return n; }
// Model Google's partial-response projection, including empty array elements.
// A masked-out table/image must still make the client reject incomplete text.
struct FieldsMask { bool all=false; std::map<std::string,FieldsMask> children; };
static FieldsMask parse_mask(const std::string &s,size_t &at) {
    FieldsMask mask;
    while (at<s.size() && s[at]!=')') {
        size_t start=at;
        while (at<s.size() && (isalnum((unsigned char)s[at]) || s[at]=='_')) at++;
        assert(at>start); auto name=s.substr(start,at-start);
        if (at<s.size() && s[at]=='(') { at++; mask.children[name]=parse_mask(s,at); assert(at<s.size() && s[at]==')'); at++; }
        else mask.children[name].all=true;
        if (at<s.size() && s[at]==',') at++; else break;
    }
    return mask;
}
static cJSON *project_fields(const cJSON *j,const FieldsMask &mask) {
    if (mask.all) return cJSON_Duplicate(j,true);
    if (cJSON_IsArray(j)) {
        auto *out=cJSON_CreateArray(); const cJSON *item;
        cJSON_ArrayForEach(item,j) assert(cJSON_AddItemToArray(out,project_fields(item,mask)));
        return out;
    }
    auto *out=cJSON_CreateObject();
    for (const auto &f:mask.children) if (auto *value=cJSON_GetObjectItemCaseSensitive(j,f.first.c_str()))
        assert(cJSON_AddItemToObject(out,f.first.c_str(),project_fields(value,f.second)));
    return out;
}
static const char *const legacy_doc_fields[]={
    "body","headers","footers","footnotes","documentStyle",
    "suggestedDocumentStyleChanges","namedStyles","suggestedNamedStylesChanges",
    "lists","namedRanges","inlineObjects","positionedObjects"
};
class NotesServer : public Server {
public:
    struct Doc { std::string text; unsigned revision=1; bool rich=false,table=false,object=false,child=false; std::string tab="t.0"; };
    std::map<std::string,Doc> docs;
    std::set<std::string> hidden;
    unsigned mutations=0,masked_reads=0; size_t largest_unfiltered=0,largest_filtered=0;
    bool lose_doc=false,race_doc=false,deny_docs=false,fail_create=false;
    std::string race_after_read;
    std::map<std::string,unsigned> binary_downloads;
    std::function<void()> before_download;
    int download(const std::string &url,const std::string &auth,const GoogleHeaders &headers,size_t limit,std::string &reply,GoogleHeaders &out) override {
        if (before_download) before_download();
        assert(reply.capacity()<32); // no full-image reservation before TLS
        assert(limit<=PALM_NOTE_PNG_MAX);
        auto id=url.substr(url.find("/files/")+7); id=id.substr(0,id.find('/'));
        binary_downloads[id]++;
        return GoogleHttp::download(url,auth,headers,limit,reply,out);
    }
    std::string add_doc(const std::string &parent,const std::string &name,const std::string &text) {
        File f; f.id="doc-"+std::to_string(next_id++); f.name=name; f.parent=parent; f.mime="application/vnd.google-apps.document"; f.modified=when("2026-10-15T07:00:00Z");
        files[f.id]=f; docs[f.id].text=text; return f.id;
    }
    void edit(const std::string &id,const std::string &text) { docs[id].text=text; docs[id].revision++; files[id].modified++; files[id].version++; }
    int request(const std::string &url,const char *method,const std::string &auth,const char *type,const std::string &body,std::string &reply) override {
        return request_if_match(url,method,auth,type,body,"",reply);
    }
    int request_if_match(const std::string &url,const char *method,const std::string &auth,const char *type,const std::string &body,const std::string &etag,std::string &reply) override {
        size_t at=0; GoogleHeaders headers,out; if (!etag.empty()) headers.emplace_back("If-Match",etag);
        return transfer(url,method,auth,type,body.size(),[&](uint8_t *p,size_t n) { assert(n<=body.size()-at); memcpy(p,body.data()+at,n); at+=n; return true; },headers,reply,out);
    }
    int transfer(const std::string &url,const char *method,const std::string &auth,const char *type,size_t size,const GoogleRead &source,const GoogleHeaders &headers,std::string &reply,GoogleHeaders &out) override {
        assert(auth=="Bearer token");
        bool docs_url=url.rfind("https://docs.googleapis.com/v1/documents/",0)==0;
        bool create=!strcmp(method,"POST") && url.find("/drive/v3/files?")!=std::string::npos && param(url,"fields")=="id";
        if (docs_url || create) {
            if (deny || (docs_url && deny_docs)) return 403;
            std::string body(size,0); if (size && !source((uint8_t *)&body[0],size)) return -1;
            if (create) {
                if (fail_create) return 503;
                auto *j=cJSON_Parse(body.c_str()); assert(j && field(j,"mimeType")=="application/vnd.google-apps.document");
                std::string parent=field(cJSON_GetObjectItemCaseSensitive(j,"parents"),"");
                auto *p=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j,"parents"),0); assert(cJSON_IsString(p)); parent=p->valuestring;
                auto id=add_doc(parent,field(j,"name"),""); metadata(files[id],j); cJSON_Delete(j); mutations++;
                auto *response=cJSON_CreateObject(); cJSON_AddStringToObject(response,"id",id.c_str()); reply=print(response);
                if (lose_doc) { lose_doc=false; return -1; } return 200;
            }
            std::string id=url.substr(strlen("https://docs.googleapis.com/v1/documents/")); id=id.substr(0,id.find_first_of("?:"));
            if (!docs.count(id) || files[id].trashed) return 404;
            auto &d=docs[id];
            if (!strcmp(method,"GET")) {
                auto selected=param(url,"fields"); assert(!selected.empty() && param(url,"includeTabsContent")=="true");
                size_t at=0; auto mask=parse_mask(selected,at); assert(at==selected.size()); masked_reads++;
                // Google rejects legacy top-level text fields when requesting
                // tabs content; body nested under tabs.documentTab is valid.
                for (const char *name:legacy_doc_fields) if (mask.children.count(name)) {
                    reply="{\"error\":{\"message\":\"Field mask may not contain legacy text-level Document resource fields while requesting tabs content\"}}";
                    return 400;
                }
                auto finish=[&](cJSON *doc) {
                    auto *metadata=cJSON_AddObjectToObject(doc,"namedStyles");
                    cJSON_AddStringToObject(metadata,"unusedStyleData",std::string(45000,'x').c_str());
                    auto *filtered=project_fields(doc,mask);
                    auto full=print(doc); largest_unfiltered=std::max(largest_unfiltered,full.size());
                    reply=print(filtered); largest_filtered=std::max(largest_filtered,reply.size());
                    // An ordinary short memo must fit without the 45 KB of
                    // irrelevant styles present in the complete API response.
                    return reply.size()<=8192 ? 200 : -1;
                };
                auto *j=cJSON_CreateObject(); cJSON_AddStringToObject(j,"documentId",id.c_str()); cJSON_AddStringToObject(j,"revisionId",("rev-"+std::to_string(d.revision)).c_str());
                auto *tabs=cJSON_AddArrayToObject(j,"tabs"),*tab=cJSON_CreateObject(); cJSON_AddItemToArray(tabs,tab);
                cJSON_AddStringToObject(cJSON_AddObjectToObject(tab,"tabProperties"),"tabId",d.tab.c_str()); auto *children=cJSON_AddArrayToObject(tab,"childTabs");
                if (d.child) { auto *child=cJSON_CreateObject(); cJSON_AddItemToArray(children,child); cJSON_AddStringToObject(cJSON_AddObjectToObject(child,"tabProperties"),"tabId","t.child"); }
                auto *content=cJSON_AddArrayToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(tab,"documentTab"),"body"),"content");
                auto *section=cJSON_CreateObject(); cJSON_AddNumberToObject(section,"endIndex",1); cJSON_AddObjectToObject(section,"sectionBreak"); cJSON_AddItemToArray(content,section);
                auto *item=cJSON_CreateObject(); cJSON_AddNumberToObject(item,"startIndex",1); cJSON_AddNumberToObject(item,"endIndex",utf16(d.text)+2); cJSON_AddItemToArray(content,item);
                if (d.table) { cJSON_AddObjectToObject(item,"table"); return finish(j); }
                auto *paragraph=cJSON_AddObjectToObject(item,"paragraph"); cJSON_AddStringToObject(cJSON_AddObjectToObject(paragraph,"paragraphStyle"),"namedStyleType","NORMAL_TEXT");
                auto *elements=cJSON_AddArrayToObject(paragraph,"elements"),*element=cJSON_CreateObject(); cJSON_AddItemToArray(elements,element);
                auto *run=cJSON_AddObjectToObject(element,"textRun"); cJSON_AddStringToObject(run,"content",(d.text+"\n").c_str());
                auto *style=cJSON_AddObjectToObject(run,"textStyle"); if (d.rich) cJSON_AddBoolToObject(style,"bold",true);
                if (d.object) { cJSON_DeleteItemFromObject(element,"textRun"); cJSON_AddObjectToObject(element,"inlineObjectElement"); }
                int code=finish(j);
                if (race_after_read==id) { race_after_read.clear(); auto stamp=files[id].modified; edit(id,"Mid-sync edit"); files[id].modified=stamp; }
                return code;
            }
            assert(!strcmp(method,"POST") && url.find(":batchUpdate")!=std::string::npos);
            auto *j=cJSON_Parse(body.c_str()); assert(j);
            auto revision=field(cJSON_GetObjectItemCaseSensitive(j,"writeControl"),"requiredRevisionId");
            if (race_doc) { race_doc=false; edit(id,"Concurrent cloud edit"); }
            if (revision!="rev-"+std::to_string(d.revision)) { cJSON_Delete(j); reply="{\"error\":{\"message\":\"revision conflict\"}}"; return 400; }
            auto *requests=cJSON_GetObjectItemCaseSensitive(j,"requests"); assert(cJSON_IsArray(requests));
            const cJSON *r; cJSON_ArrayForEach(r,requests) {
                auto *del=cJSON_GetObjectItemCaseSensitive(r,"deleteContentRange"),*insert=cJSON_GetObjectItemCaseSensitive(r,"insertText");
                if (del) {
                    auto *range=cJSON_GetObjectItemCaseSensitive(del,"range"); assert(field(range,"tabId")==d.tab);
                    assert(cJSON_GetObjectItemCaseSensitive(range,"startIndex")->valueint==1 && cJSON_GetObjectItemCaseSensitive(range,"endIndex")->valueint==(int)utf16(d.text)+1); d.text.clear();
                } else { assert(insert && d.text.empty() && field(cJSON_GetObjectItemCaseSensitive(insert,"location"),"tabId")==d.tab); d.text=field(insert,"text"); }
            }
            cJSON_Delete(j); d.revision++; files[id].modified++; files[id].version++; mutations++; reply="{}"; return 200;
        }
        if (!strcmp(method,"GET") && url.find("/files?")!=std::string::npos && !hidden.empty()) {
            std::map<std::string,File> removed; for (const auto &id:hidden) { if (files.count(id)) { removed[id]=files[id]; files.erase(id); } }
            int code=Server::transfer(url,method,auth,type,size,source,headers,reply,out); for (const auto &f:removed) files[f.first]=f.second; return code;
        }
        if (strcmp(method,"GET")) mutations++;
        return Server::transfer(url,method,auth,type,size,source,headers,reply,out);
    }
};
class NotesHost : public NoteSyncHost {
public:
    NotesServer &server; NoteSyncState state; bool writable=true,connected=true,fail_import_commit=false; uint32_t entropy=500;
    std::function<bool(size_t)> reserve;
    std::function<bool()> release;
    unsigned releases=0;
    bool deny_release=false;
    bool release_record_buffer() override { releases++; return !deny_release && (!release || release()); }
    size_t largest_reservation=0; bool deny_reserve=false;
    bool reserve_record_buffer(size_t bytes) override {
        largest_reservation=std::max(largest_reservation,bytes);
        return !deny_reserve && (reserve ? reserve(bytes) : NoteSyncHost::reserve_record_buffer(bytes));
    }
    explicit NotesHost(NotesServer &s) : server(s) {}
    GoogleHttp &http() override { return server; }
    bool online() override { return connected; }
    int64_t now() override { return when("2026-10-15T07:00:00Z"); }
    uint32_t random() override { return ++entropy; }
    bool access_token(std::string &token,std::string &) override { token="token"; return true; }
    bool load(NoteSyncState &s) override { s=state; return true; }
    bool store(const NoteSyncState &s) override {
        if (!writable) return false;
        if (fail_import_commit) for (const auto &old:state.map) if (old.pending==3) for (const auto &m:s.map) if (m.palm_id==old.palm_id && !m.pending) { fail_import_commit=false; return false; }
        std::string raw; assert(notesync_encode(s,raw)); NoteSyncState decoded; assert(notesync_decode(raw,64,decoded)); state=decoded; return true;
    }
};
static NoteSyncResult sync(NotesHost &host,NoteSyncSetup setup,bool success=true) {
    NoteSyncResult r; notesync_run(setup,host,r); printf("%s: %s\n",setup.drawing ? "Notes" : "Memos",notesync_summary(r).c_str()); fflush(stdout);
    assert(r.problem.empty()==success && !r.link_lost); return r;
}
static uint32_t put(const char *name,uint32_t id,const std::string &data,uint8_t attr=HOTSYNC_ATTR_DIRTY) {
    uint8_t db; assert(hotsync_open_db(name,HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE|HOTSYNC_MODE_SECRET,&db)); size_t room; auto *p=hotsync_record_buffer(&room); assert(data.size()<=room); memcpy(p,data.data(),data.size());
    uint32_t saved; assert(hotsync_write_record(db,id,attr,2,data.size(),&saved) && hotsync_close_db(db)); return saved;
}
static bool get(const char *name,uint32_t id,PalmNote &n,bool drawing) {
    uint8_t db; assert(hotsync_open_db(name,HOTSYNC_MODE_READ|HOTSYNC_MODE_SECRET,&db)); HotSync_Record rec; bool found=hotsync_read_record(db,id,&rec); std::string problem;
    if (found && rec.truncated && drawing) { assert(test_reserve && test_reserve(rec.total_len)); assert(hotsync_read_record(db,id,&rec)); }
    if (found) assert(!rec.truncated && (drawing ? drawing_unpack(rec.data,rec.len,n,problem) : memo_unpack(rec.data,rec.len,n,problem)));
    assert(hotsync_close_db(db)); return found;
}
static void remove(const char *name,uint32_t id) { uint8_t db; assert(hotsync_open_db(name,HOTSYNC_MODE_WRITE,&db) && hotsync_delete_record(db,id) && hotsync_close_db(db)); }
static std::string record(const PalmNote &n) { uint8_t p[8192]; HotSync_Time now={2026,10,15,20,0,0}; auto size=drawing_pack(n,now,p,sizeof(p)); assert(size); return std::string((char *)p,size); }
static void png_chunk(std::string &s,const char *tag,const std::string &data) {
    auto append=[&](uint32_t n) { for (int i=24;i>=0;i-=8) s+=(char)(n>>i); }; append(data.size()); size_t at=s.size(); s.append(tag,4); s+=data; append(crc32(0,(const Bytef *)s.data()+at,4+data.size()));
}
static std::string rgb_png() {
    std::string s("\x89PNG\r\n\x1a\n",8),head(13,0); p32(head,0,160); p32(head,4,160); head[8]=8; head[9]=2; png_chunk(s,"IHDR",head);
    std::string raw; for (int y=0;y<160;y++) { raw+='\0'; for (int x=0;x<160;x++) raw+=(!x && !y) ? std::string("\xff\0\0",3) : std::string(3,'\xff'); }
    uLongf size=compressBound(raw.size()); std::string compressed(size,0); assert(compress2((Bytef *)&compressed[0],&size,(const Bytef *)raw.data(),raw.size(),9)==Z_OK); compressed.resize(size); png_chunk(s,"IDAT",compressed); png_chunk(s,"IEND",""); return s;
}
static std::string gray_png(PalmNote &expected) {
    expected.width=160; expected.height=352; expected.pixels.assign(20*352,0);
    std::string s("\x89PNG\r\n\x1a\n",8),head(13,0); p32(head,0,160); p32(head,4,352); head[8]=8; png_chunk(s,"IHDR",head);
    std::string raw; uint32_t random=12345;
    for (unsigned y=0;y<352;y++) {
        raw+='\0';
        for (unsigned x=0;x<160;x++) {
            random^=random<<13; random^=random>>17; random^=random<<5; uint8_t value=random; raw+=(char)value;
            if (value<128) expected.pixels[y*20+x/8]|=(char)(0x80>>(x%8));
        }
    }
    uLongf size=compressBound(raw.size()); std::string compressed(size,0);
    assert(compress2((Bytef *)&compressed[0],&size,(const Bytef *)raw.data(),raw.size(),0)==Z_OK);
    compressed.resize(size); png_chunk(s,"IDAT",compressed); png_chunk(s,"IEND","");
    assert(s.size()>56000 && s.size()<PALM_NOTE_PNG_MAX); return s;
}
static std::string unhex(const char *p) { std::string s; for (size_t i=0;p[i];i+=2) { char b[3]={p[i],p[i+1],0}; s+=(char)strtoul(b,nullptr,16); } return s; }
int main(int argc,char **argv) {
    // Preserve the persisted v1 fingerprints without constructing another
    // bitmap-sized string for the ":width:height" suffix.
    PalmNote fingerprint; fingerprint.title="Hash fixture"; fingerprint.width=24; fingerprint.height=1;
    fingerprint.pixels=std::string("\x80\0\xff",3);
    assert((note_fields(fingerprint,true)==std::array<uint32_t,2>{{0xefced144,0x41569abb}}));
    fingerprint.width=160; fingerprint.height=1600; fingerprint.pixels.assign(32000,0);
    memory_begin(); note_fields(fingerprint,true); memory_end(); assert(measured_peak<128);
    fingerprint=PalmNote();
    for (const auto &fixture:png_fixtures) {
        PalmNote bitmap; std::string problem;
        assert(note_read_png(unhex(fixture.png),bitmap,problem) && bitmap.pixels==unhex(fixture.pixels));
        PalmNote roundtrip; assert(note_read_png(note_png(bitmap),roundtrip,problem) && roundtrip.pixels==bitmap.pixels);
    }
    for (unsigned height:{320u,480u,1600u}) {
        PalmNote tall; tall.height=height; tall.pixels.assign(20*height,0);
        tall.pixels[0]=(char)0x80; tall.pixels.back()=1;
        auto png=note_png(tall); PalmNote decoded; std::string problem;
        assert(!png.empty() && png.size()<=PALM_NOTE_PNG_MAX && note_read_png(png,decoded,problem));
        assert(decoded.height==height && decoded.pixels==tall.pixels);
        std::vector<uint8_t> record(drawing_record_size(tall)); HotSync_Time now={};
        assert(drawing_pack(tall,now,record.data(),record.size())==record.size());
        assert(drawing_unpack(record.data(),record.size(),decoded,problem) && decoded.height==height && decoded.pixels==tall.pixels);
        record.pop_back(); assert(!drawing_unpack(record.data(),record.size(),decoded,problem));
    }
    {
        // Complete native RLE may carry an orphan byte; its value is ignored
        // by the reference exporter. Never use it to fill missing bitmap data.
        PalmNote tall; tall.height=320; tall.pixels.assign(6400,0); tall.pixels.back()=8;
        auto base=record(tall); const size_t body=32; // 30-byte header + empty padded title
        auto length=(uint32_t)(base.size()-body-24);
        PalmNote decoded; std::string problem;
        for (unsigned trailer:{0u,1u,128u,255u}) {
            auto padded=base; padded+=(char)trailer;
            p32(padded,body,25+length); p32(padded,body+20,length+1);
            assert(drawing_unpack((const uint8_t *)padded.data(),padded.size(),decoded,problem) &&
                   decoded.pixels==tall.pixels && problem.empty());
        }
        auto noops=base; noops.insert(body+24,std::string("\0\x7f",2)); noops.append("\0\x55",2);
        p32(noops,body,28+length); p32(noops,body+20,length+4);
        assert(drawing_unpack((const uint8_t *)noops.data(),noops.size(),decoded,problem) && decoded.pixels==tall.pixels);
        auto truncated=base; truncated.pop_back(); p32(truncated,body,23+length); p32(truncated,body+20,length-1);
        assert(!drawing_unpack((const uint8_t *)truncated.data(),truncated.size(),decoded,problem) &&
               problem.find("Truncated Note Pad RLE pair")!=std::string::npos && problem.find("/6400 bytes")!=std::string::npos);
        truncated.pop_back(); p32(truncated,body,22+length); p32(truncated,body+20,length-2);
        assert(!drawing_unpack((const uint8_t *)truncated.data(),truncated.size(),decoded,problem) && problem.find("Incomplete")!=std::string::npos);
        auto overrun=base; overrun.append("\x01\0",2); p32(overrun,body,26+length); p32(overrun,body+20,length+2);
        assert(!drawing_unpack((const uint8_t *)overrun.data(),overrun.size(),decoded,problem) && problem.find("bounds")!=std::string::npos);
        auto garbage=base; garbage+='x';
        assert(!drawing_unpack((const uint8_t *)garbage.data(),garbage.size(),decoded,problem) && problem.find("trailing data")!=std::string::npos);
        auto short_payload=base; short_payload.pop_back();
        assert(!drawing_unpack((const uint8_t *)short_payload.data(),short_payload.size(),decoded,problem) && problem.find("payload")!=std::string::npos);
    }
    PalmNote too_large; too_large.height=1601; too_large.pixels.assign(32020,0);
    assert(!drawing_record_size(too_large) && note_png(too_large).empty());
    PalmNote crlf; std::string crlf_problem; const uint8_t text[]={'a',13,10,'b',13,'c',0,0};
    assert(memo_unpack(text,sizeof(text),crlf,crlf_problem) && crlf.text=="a\nb\nc");
    assert(argc==2); fd=open(argv[1],O_RDWR|O_NOCTTY); assert(fd>=0);
    std::vector<uint8_t> memory(HOTSYNC_BUFFER_SIZE(9000)); HotSync_Io io={io_read,io_write,ticks}; assert(hotsync_accept(&io,memory.data(),memory.size(),10000));
    uint32_t rom; assert(hotsync_read_sys_info(&rom));
    // Shared setup supplies a real Palm name while preserving the numeric ID
    // and desktop metadata. A named Palm must never be renamed automatically.
    HotSyncUserSetup identity; HotSyncUserResult user; std::string palm_name,identity_problem;
    identity.name=std::string(40,'x'); assert(hotsync_user_validate(identity,palm_name,identity_problem));
    identity.name=std::string(41,'x'); assert(!hotsync_user_validate(identity,palm_name,identity_problem));
    identity.name="Bad\nName"; assert(!hotsync_user_validate(identity,palm_name,identity_problem));
    identity.name="Emoji \xf0\x9f\x98\x80"; assert(!hotsync_user_validate(identity,palm_name,identity_problem));
    identity.name="Caf\xc3\xa9"; assert(hotsync_user_validate(identity,palm_name,identity_problem) && palm_name==std::string("Caf\xe9"));
    identity.name="Test Palm"; assert(hotsync_write_user(1,""));
    assert(hotsync_user_prepare(identity,user) && user.written && user.id==1 && user.name=="Test Palm");
    identity.name="Different name"; assert(hotsync_user_prepare(identity,user) && !user.written && user.id==1 && user.name=="Test Palm");
    assert(hotsync_write_user(0,"")); identity.name.clear();
    assert(hotsync_user_prepare(identity,user) && !user.written && user.id==0 && user.name.empty());
    identity.name="Test Palm"; assert(hotsync_user_prepare(identity,user) && user.written && user.id==0);
    assert(hotsync_write_user(1,"Test Palm"));
    NotesServer server; NotesHost memo(server),drawing(server); NoteSyncSetup memos,notes; notes.drawing=true;
    drawing.reserve=[&](size_t bytes) {
        size_t room=0; hotsync_record_buffer(&room); if (room>=bytes) return true;
        memory.resize(HOTSYNC_BUFFER_SIZE(bytes)); return hotsync_resize_buffer(memory.data(),memory.size());
    };
    test_reserve=drawing.reserve;
    drawing.release=[&]() {
        constexpr size_t baseline=9000;
        if (memory.size()<=HOTSYNC_BUFFER_SIZE(baseline)) return true;
        std::vector<uint8_t> small(HOTSYNC_BUFFER_SIZE(baseline)); memory.swap(small);
        return hotsync_resize_buffer(memory.data(),memory.size());
    };
    server.before_download=[&]() { assert(memory.size()<=HOTSYNC_BUFFER_SIZE(9000)); };
    auto r=sync(memo,memos); assert(r.uploaded==1 && memo.state.map.size()==1);
    auto memo_id=memo.state.map[0].google_id; auto palm_memo=memo.state.map[0].palm_id; assert(server.docs[memo_id].text=="Public memo");
    // Reproduce Google's HTTP 400 for the former mixed legacy/tab mask.
    // Every real Memo read below must also pass the same mock validation.
    for (const char *name:legacy_doc_fields) {
        auto before=server.mutations; auto revision=server.docs[memo_id].revision; std::string reply;
        std::string mask=std::string("documentId,tabs(documentTab(body(content(endIndex)))),")+name;
        int code=server.request("https://docs.googleapis.com/v1/documents/"+memo_id+"?includeTabsContent=true&fields="+oauth_encode(mask),"GET","Bearer token","application/json","",reply);
        assert(code==400 && reply.find("legacy text-level")!=std::string::npos);
        assert(server.mutations==before && server.docs[memo_id].revision==revision && server.docs[memo_id].text=="Public memo");
    }
    r=sync(memo,memos); assert(!r.uploaded && !r.downloaded);
    auto cloud_id=server.add_doc(memo.state.folder,"Cloud title","From Google\nSecond line"); r=sync(memo,memos); assert(r.downloaded==1);
    PalmNote n; auto cloud_palm=memo.state.map.back().palm_id; assert(get("MemoDB",cloud_palm,n,false) && n.text=="From Google\nSecond line");
    put("MemoDB",palm_memo,std::string("Palm edit\0",10)); r=sync(memo,memos); assert(r.uploaded==1 && server.docs[memo_id].text=="Palm edit");
    server.edit(memo_id,"Google edit"); sync(memo,memos); assert(get("MemoDB",palm_memo,n,false) && n.text=="Google edit");
    put("MemoDB",palm_memo,std::string("Conflict Palm\0",14)); server.edit(memo_id,"Conflict cloud"); sync(memo,memos,false);
    assert(server.docs[memo_id].text=="Conflict cloud"); memos.conflicts=2; sync(memo,memos); assert(get("MemoDB",palm_memo,n,false) && n.text=="Conflict cloud"); memos.conflicts=0;
    server.docs[memo_id].rich=true; server.edit(memo_id,"Rich text"); sync(memo,memos);
    put("MemoDB",palm_memo,std::string("Replace rich\0",13)); sync(memo,memos,false); assert(server.docs[memo_id].text=="Rich text"); server.docs[memo_id].rich=false; sync(memo,memos);
    server.race_doc=true; put("MemoDB",palm_memo,std::string("Racing Palm\0",12)); sync(memo,memos,false); assert(server.docs[memo_id].text=="Concurrent cloud edit");
    memos.conflicts=1; sync(memo,memos); assert(server.docs[memo_id].text=="Racing Palm"); memos.conflicts=0;
    put("MemoDB",cloud_palm,"",HOTSYNC_ATTR_DELETED); server.race_after_read=cloud_id;
    sync(memo,memos,false); assert(!server.files[cloud_id].trashed && server.docs[cloud_id].text=="Mid-sync edit");
    memos.conflicts=1; r=sync(memo,memos); memos.conflicts=0; assert(r.deleted==1 && server.files[cloud_id].trashed && !get("MemoDB",cloud_palm,n,false));
    server.files[memo_id].trashed=true; r=sync(memo,memos); assert(r.deleted==1 && !get("MemoDB",palm_memo,n,false));
    // Deletion versus an edit is also a conflict. The explicit preference
    // can recover the edited side without silently throwing it away.
    auto delete_conflict=server.add_doc(memo.state.folder,"Delete conflict","Baseline"); sync(memo,memos);
    auto deleted_palm=memo.state.map.back().palm_id;
    put("MemoDB",deleted_palm,"",HOTSYNC_ATTR_DELETED); server.edit(delete_conflict,"Keep cloud edit"); sync(memo,memos,false);
    memos.conflicts=2; sync(memo,memos); assert(get("MemoDB",deleted_palm,n,false) && n.text=="Keep cloud edit");
    put("MemoDB",deleted_palm,std::string("Keep Palm edit\0",15)); server.files[delete_conflict].trashed=true; memos.conflicts=0; sync(memo,memos,false);
    memos.conflicts=1; sync(memo,memos); assert(get("MemoDB",deleted_palm,n,false) && n.text=="Keep Palm edit"); memos.conflicts=0;
    // An ambiguous Docs create pauses until its marker becomes visible.
    auto fresh=put("MemoDB",0,std::string("Lost create\0",12)); server.lose_doc=true; sync(memo,memos,false); assert(memo.state.map.back().pending==2);
    std::string lost; for (const auto &d:server.docs) if (!server.files[d.first].trashed && server.files[d.first].props["palmSync"]==memo.state.map.back().marker) lost=d.first; assert(!lost.empty());
    size_t creations=server.docs.size(); server.hidden.insert(lost); sync(memo,memos,false); assert(server.docs.size()==creations); server.hidden.clear(); sync(memo,memos); assert(server.docs.size()==creations && server.docs[lost].text=="Lost create");
    server.files[lost].parent="elsewhere"; sync(memo,memos); assert(get("MemoDB",fresh,n,false)); server.files[lost].parent=memo.state.folder;
    // Incoming Palm IDs are reserved before writing: losing the final state
    // write can retry without creating a second record.
    auto imported=server.add_doc(memo.state.folder,"Incoming","Journalled import"); memo.fail_import_commit=true; sync(memo,memos,false);
    auto reserved=memo.state.map.back().palm_id; assert(memo.state.map.back().pending==3 && get("MemoDB",reserved,n,false));
    r=sync(memo,memos); assert(!r.downloaded && memo.state.map.back().google_id==imported && !memo.state.map.back().pending);
    auto bad=put("MemoDB",0,"No terminator"); unsigned mutations=server.mutations; sync(memo,memos,false); assert(server.mutations==mutations); remove("MemoDB",bad);
    for (const auto &bad_tab:std::vector<std::string>{"","bad\ntab",std::string(257,'x')}) {
        server.docs[lost].tab=bad_tab; mutations=server.mutations;
        auto failure=sync(memo,memos,false); assert(failure.problem.find("tab ID")!=std::string::npos && server.mutations==mutations);
    }
    server.docs[lost].tab="t.opaque_more-1"; sync(memo,memos); server.docs[lost].tab="t.0";
    server.docs[lost].table=true; sync(memo,memos,false); server.docs[lost].table=false;
    server.docs[lost].object=true; mutations=server.mutations; sync(memo,memos,false); assert(server.mutations==mutations); server.docs[lost].object=false;
    server.docs[lost].child=true; mutations=server.mutations; sync(memo,memos,false); assert(server.mutations==mutations); server.docs[lost].child=false;
    assert(server.masked_reads>0 && server.largest_unfiltered>45000 && server.largest_filtered<8192);
    server.deny_docs=true; sync(memo,memos,false); server.deny_docs=false;
    auto saved=memo.state; memo.state.account="other-account"; sync(memo,memos,false); memo.state=saved;
    memo.connected=false; sync(memo,memos,false); memo.connected=true;
    NotesHost no_space(server); no_space.writable=false; mutations=server.mutations; sync(no_space,memos,false); assert(mutations==server.mutations);
    auto tiny=memos; tiny.content_memory=1; sync(memo,tiny,false);
    auto unicode=server.add_doc(memo.state.folder,"Unicode","Caf\xc3\xa9 \xf0\x9f\x98\x80");
    sync(memo,memos); auto unicode_text=server.docs[unicode].text; r=sync(memo,memos); assert(!r.uploaded && server.docs[unicode].text==unicode_text);
    auto too_long=server.add_doc(memo.state.folder,"Oversized",std::string(4096,'x')); mutations=server.mutations;
    sync(memo,memos,false); assert(server.mutations==mutations); server.files.erase(too_long);
    put("MemoDB",0,std::string("Explicit retry\0",15)); server.fail_create=true; sync(memo,memos,false); server.fail_create=false;
    assert(notesync_retry_unconfirmed(memo.state)); sync(memo,memos);
    assert(!notesync_retry_unconfirmed(memo.state));
    // The explicit retry applies only to pending creations at that time.
    // A later ambiguous create must still wait for its own confirmation.
    put("MemoDB",0,std::string("Later failure\0",14)); server.fail_create=true; sync(memo,memos,false); server.fail_create=false;
    creations=server.docs.size(); sync(memo,memos,false); assert(server.docs.size()==creations);
    assert(notesync_retry_unconfirmed(memo.state)); sync(memo,memos);
    // Note Pad decoding and encoding use a native OS 4 fixture with an alarm.
    drawing.deny_reserve=true; auto capacity_failure=sync(drawing,notes,false);
    assert(capacity_failure.problem.find("HotSync buffer")!=std::string::npos && drawing.state.map.empty());
    drawing.deny_reserve=false;
    r=sync(drawing,notes); assert(r.uploaded==2 && drawing.state.map.size()==2);
    auto sketch=drawing.state.map[0].google_id; auto palm_sketch=drawing.state.map[0].palm_id;
    assert(server.files[sketch].mime=="image/png" && get("npadDB",palm_sketch,n,true) && n.height==480 && n.pixels.size()==9600 && (uint8_t)n.pixels[0]==128 && (uint8_t)n.pixels.back()==8 && drawing.largest_reservation>9000 && n.header.size()==44);
    PalmNote padded_note;
    assert(get("npadDB",drawing.state.map[1].palm_id,padded_note,true) && padded_note.height==320 && padded_note.pixels.size()==6400 &&
           (uint8_t)padded_note.pixels[0]==128 && (uint8_t)padded_note.pixels.back()==8);
    auto alarm=n.header; r=sync(drawing,notes); assert(!r.uploaded && !r.downloaded);
    // Reproduce the reported 160x352 8-bit grayscale import. Its >56 KiB
    // encoded file must arrive in one version-locked download, with no full
    // PNG buffer reserved before the connection and no grown HotSync buffer.
    auto original_png=server.files[sketch].data; PalmNote grayscale;
    server.files[sketch].data=gray_png(grayscale); server.commit(server.files[sketch]);
    auto downloads=server.binary_downloads[sketch]; mutations=server.mutations;
    server.short_range=true; auto short_image=sync(drawing,notes,false); server.short_range=false;
    assert(short_image.problem.find("Download PNG")!=std::string::npos && server.mutations==mutations);
    assert(get("npadDB",palm_sketch,n,true) && n.height==480 && n.header==alarm);
    r=sync(drawing,notes);
    // Failed preflight, successful preflight, then the one changed image is
    // reloaded from its immutable revision for installation.
    assert(r.downloaded==1 && server.binary_downloads[sketch]==downloads+3);
    assert(get("npadDB",palm_sketch,n,true) && n.height==352 && n.pixels==grayscale.pixels && n.header.substr(30)==alarm.substr(30));
    // The enlarged native buffer is released after the write and before the
    // next network operation; a failed release stops before cloud transfers.
    assert(drawing.releases>0); drawing.deny_release=true; mutations=server.mutations;
    auto release_failure=sync(drawing,notes,false); drawing.deny_release=false;
    assert(release_failure.problem.find("release the notes record buffer")!=std::string::npos && server.mutations==mutations);
    server.files[sketch].data=original_png; server.commit(server.files[sketch]); sync(drawing,notes);
    assert(get("npadDB",palm_sketch,n,true) && n.height==480);
    // Dense scrolling drawings need a larger native record on import, even
    // when their PNG fits comfortably. All rows survive the subsequent read.
    PalmNote dense=n;
    for (size_t i=0;i<dense.pixels.size();i++) dense.pixels[i]=(char)(i%2 ? 0xaa : 0x55);
    server.files[sketch].data=note_png(dense); server.commit(server.files[sketch]);
    drawing.deny_reserve=true; auto denied_import=sync(drawing,notes,false);
    assert(denied_import.problem.find("HotSync buffer")!=std::string::npos);
    assert(get("npadDB",palm_sketch,n,true) && n.pixels!=dense.pixels);
    drawing.deny_reserve=false;
    r=sync(drawing,notes); assert(r.downloaded==1 && drawing.largest_reservation>19000);
    assert(get("npadDB",palm_sketch,n,true) && n.height==480 && n.pixels==dense.pixels);
    r=sync(drawing,notes); assert(!r.uploaded && !r.downloaded);
    server.files[sketch].data=rgb_png(); server.commit(server.files[sketch]); sync(drawing,notes);
    assert(get("npadDB",palm_sketch,n,true)); n.title="Renamed Palm"; put("npadDB",palm_sketch,record(n));
    auto rgb=server.files[sketch].data; r=sync(drawing,notes); assert(r.uploaded==1 && server.files[sketch].name=="Renamed Palm.png" && server.files[sketch].data==rgb);
    // A scrolling PNG imported from Drive retains its bottom-most ink and
    // native alarm, and remains editable on the next outgoing sync.
    assert(get("npadDB",palm_sketch,n,true)); n.height=480; n.pixels.assign(9600,0); n.pixels.back()=4;
    // Imported image edits remain editable drawings, preserving Palm alarms.
    n.title="Cloud renamed"; n.pixels[0]=(char)0x40; server.files[sketch].name="Cloud renamed.png"; server.files[sketch].data=note_png(n); server.commit(server.files[sketch]);
    r=sync(drawing,notes); assert(r.downloaded==1 && get("npadDB",palm_sketch,n,true) && n.title=="Cloud renamed" && n.height==480 && (uint8_t)n.pixels.back()==4 && (uint8_t)n.pixels[0]==64 && n.header.substr(30)==alarm.substr(30));
    n.pixels[1]=(char)0xff; put("npadDB",palm_sketch,record(n)); r=sync(drawing,notes); assert(r.uploaded==1);
    PalmNote decoded; std::string problem; assert(note_read_png(server.files[sketch].data,decoded,problem) && decoded.pixels==n.pixels);
    auto from_drive=server.add("From Drive.png",drawing.state.folder,rgb_png(),drawing.now()).id; r=sync(drawing,notes); assert(r.downloaded==1);
    auto imported_sketch=drawing.state.map.back().palm_id; assert(get("npadDB",imported_sketch,n,true) && n.title=="From Drive" && (uint8_t)n.pixels[0]==128);
    n.pixels[3]=(char)0xaa; put("npadDB",imported_sketch,record(n)); sync(drawing,notes); assert(!server.files[from_drive].props.count("palmBackup"));
    put("npadDB",imported_sketch,"",HOTSYNC_ATTR_DELETED); r=sync(drawing,notes); assert(r.deleted==1 && server.files[from_drive].trashed);
    auto invalid=server.add("Broken.png",drawing.state.folder,"not a PNG",drawing.now()).id; mutations=server.mutations; sync(drawing,notes,false); assert(server.mutations==mutations); server.files.erase(invalid);
    auto broken=note_png(decoded); broken.back()^=1; assert(!note_read_png(broken,decoded,problem));
    auto corrupt=record(n); corrupt.pop_back(); assert(!drawing_unpack((const uint8_t *)corrupt.data(),corrupt.size(),decoded,problem));
    // A book larger than the old aggregate bitmap budget is still handled
    // with one active image. No decoded payload from an earlier image may
    // remain live at the next download. Each file is independently validated
    // before the transfer pass; unchanged notes need no second download.
    PalmNote page; page.width=160; page.height=352; page.pixels.assign(7040,0);
    for (size_t i=0;i<page.pixels.size();i++) page.pixels[i]=(char)(i%2 ? 0xaa : 0x55);
    std::vector<std::string> pages;
    for (unsigned i=0;i<8;i++) pages.push_back(server.add("Page "+std::to_string(i)+".png",drawing.state.folder,note_png(page),drawing.now()).id);
    size_t low=SIZE_MAX,high=0; auto check_buffer=server.before_download;
    server.before_download=[&]() { check_buffer(); low=std::min(low,measured_live); high=std::max(high,measured_live); };
    memory_begin(); r=sync(drawing,notes); memory_end();
    printf("Drawing import memory: peak %zu, between downloads %zu..%zu\n",measured_peak,low,high);
    assert(r.downloaded==pages.size() && high-low<24000);
    for (const auto &id:pages) {
        auto mapping=std::find_if(drawing.state.map.begin(),drawing.state.map.end(),[&](const NoteSyncMap &m) { return m.google_id==id; });
        assert(mapping!=drawing.state.map.end() && get("npadDB",mapping->palm_id,n,true) && n.pixels==page.pixels && n.height==352);
    }
    low=SIZE_MAX; high=0;
    memory_begin(); r=sync(drawing,notes); memory_end();
    size_t transport_bytes=tracked_bytes(memory.data());
    printf("Drawing repeat memory: peak %zu, between downloads %zu..%zu, retained %zu (HotSync buffer %zu)\n",measured_peak,low,high,measured_live,transport_bytes); fflush(stdout);
    // The reusable protocol buffer may have been shrunk during this sync.
    // It is the only allowed surviving allocation; no image payload remains.
    assert(!r.uploaded && !r.downloaded && high-low<24000 && measured_live==transport_bytes);
    for (const auto &id:pages) assert(server.binary_downloads[id]==3); // preflight + install, then preflight only
    server.before_download=check_buffer;
    // Re-reading compact Palm snapshots must not overwrite a record edited
    // after validation. The cloud change forces an actual transfer decision.
    PalmNote original_palm; assert(get("npadDB",palm_sketch,original_palm,true));
    auto cloud_edit=original_palm; cloud_edit.pixels[0]^=1;
    server.files[sketch].data=note_png(cloud_edit); server.commit(server.files[sketch]);
    bool raced=false;
    server.before_download=[&]() {
        check_buffer();
        if (!raced) { raced=true; auto edited=original_palm; edited.title="Mid-sync Palm edit"; put("npadDB",palm_sketch,record(edited)); }
    };
    mutations=server.mutations; auto race=sync(drawing,notes,false);
    assert(raced && race.problem.find("after validation")!=std::string::npos && server.mutations==mutations);
    assert(get("npadDB",palm_sketch,n,true) && n.title=="Mid-sync Palm edit");
    server.before_download=check_buffer;
    put("npadDB",palm_sketch,record(original_palm),0); sync(drawing,notes);
    assert(get("npadDB",palm_sketch,n,true) && n.pixels==cloud_edit.pixels && n.title==original_palm.title);
    // A bad later image must stop the complete validation pass before any
    // earlier valid incoming note is installed or a Palm edit is uploaded.
    auto later=server.add("ZZ invalid.png",drawing.state.folder,"not a PNG",drawing.now()).id;
    auto uninstalled=server.add("Valid incoming.png",drawing.state.folder,note_png(page),drawing.now()).id;
    auto before_state=drawing.state.map; mutations=server.mutations;
    sync(drawing,notes,false);
    assert(server.mutations==mutations && drawing.state.map.size()==before_state.size());
    assert(std::none_of(drawing.state.map.begin(),drawing.state.map.end(),[&](const NoteSyncMap &m) { return m.google_id==uninstalled; }));
    server.files.erase(later); server.files.erase(uninstalled);
    // State blobs must not accept duplicated identities or incomplete data.
    std::string state; assert(notesync_encode(drawing.state,state)); NoteSyncState parsed;
    for (size_t i=0;i<state.size();i++) assert(!notesync_decode(state.substr(0,i),64,parsed));
    auto duplicate=drawing.state; duplicate.map.push_back(duplicate.map[0]); assert(notesync_encode(duplicate,state) && !notesync_decode(state,64,parsed));
    auto unbound=drawing.state; unbound.account.clear(); assert(notesync_encode(unbound,state) && !notesync_decode(state,64,parsed));
    assert(hotsync_add_log("Memo and Note Pad tests passed\n") && hotsync_end(0)); close(fd); puts("done");
}
