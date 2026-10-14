// Real HotSync against a simulated Palm and a scripted Google People API.
#include "contactsync.h"
#include "hotsync.h"
#include "cJSON.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static int fd=-1;
static int json_allocations_left=-1;
static void *limited_json_malloc(size_t bytes) {
    if (json_allocations_left==0) return nullptr;
    if (json_allocations_left>0) --json_allocations_left;
    return malloc(bytes);
}
static int port_read(uint8_t *p,size_t max,uint32_t ms) {
    pollfd wait={fd,POLLIN,0}; if (poll(&wait,1,ms)<=0) return 0;
    ssize_t n=read(fd,p,max); return n>0 ? (int)n : -1;
}
static bool port_write(const uint8_t *p,size_t n) { return write(fd,p,n)==(ssize_t)n; }
static uint32_t port_millis() { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000; }
static std::string field(cJSON *j,const char *key) {
    auto *v=cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsString(v) ? v->valuestring : "";
}
static void set(cJSON *j,const char *key,const std::string &value) {
    cJSON_DeleteItemFromObjectCaseSensitive(j,key); cJSON_AddStringToObject(j,key,value.c_str());
}
static std::string json(cJSON *j) {
    char *raw=cJSON_PrintUnformatted(j); assert(raw); std::string s(raw); free(raw); cJSON_Delete(j); return s;
}
static std::string decode(const std::string &s) {
    std::string out;
    for (size_t n=0;n<s.size();n++) {
        if (s[n]=='%' && n+2<s.size()) { out+=(char)strtol(s.substr(n+1,2).c_str(),nullptr,16); n+=2; }
        else out+=s[n];
    }
    return out;
}
static std::string param(const std::string &url,const std::string &key) {
    auto at=url.find(key+"="); if (at==std::string::npos) return "";
    at+=key.size()+1; return decode(url.substr(at,url.find('&',at)-at));
}
static cJSON *array_item(cJSON *j,const char *array,int index=0) { return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j,array),index); }
static cJSON *value_item(cJSON *a,const std::string &value,const std::string &type,bool primary=false) {
    auto *item=cJSON_CreateObject(); set(item,"value",value); set(item,"type",type);
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(item,"metadata"),"primary",primary);
    cJSON_AddItemToArray(a,item); return item;
}
static std::string person(const std::string &resource,const std::string &id,const std::string &last) {
    cJSON *j=cJSON_CreateObject(); set(j,"resourceName",resource);
    auto *source=cJSON_CreateObject(); set(source,"type","CONTACT"); set(source,"id",id); set(source,"etag","etag1");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(cJSON_AddObjectToObject(j,"metadata"),"sources"),source);
    auto *names=cJSON_AddArrayToObject(j,"names"),*name=cJSON_CreateObject(); set(name,"familyName",last); set(name,"givenName","Alice");
    set(name,"middleName","Preserve"); cJSON_AddItemToArray(names,name);
    auto *org=cJSON_CreateObject(); set(org,"name","Company"); set(org,"title","Engineer"); set(org,"department","Keep department");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(j,"organizations"),org);
    auto *address=cJSON_CreateObject(); set(address,"streetAddress","1 Main St"); set(address,"city","Wellington"); set(address,"region","WGN");
    set(address,"postalCode","6011"); set(address,"country","New Zealand"); set(address,"poBox","Keep box");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(j,"addresses"),address);
    auto *bio=cJSON_CreateObject(); set(bio,"value",std::string(5000,'n')); set(bio,"contentType","TEXT_PLAIN");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(j,"biographies"),bio);
    auto *custom=cJSON_AddArrayToObject(j,"userDefined"),*item=cJSON_CreateObject(); set(item,"key","External custom"); set(item,"value","Preserve custom");
    cJSON_AddItemToArray(custom,item); item=cJSON_CreateObject(); set(item,"key","Palm Custom 1"); set(item,"value","Palm value"); cJSON_AddItemToArray(custom,item);
    value_item(cJSON_AddArrayToObject(j,"emailAddresses"),"alice@example.com","work",true);
    auto *phones=cJSON_AddArrayToObject(j,"phoneNumbers");
    value_item(phones,"111","mobile",true);
    value_item(phones,"222","home"); value_item(phones,"333","work"); value_item(phones,"444","pager");
    value_item(phones,"555","custom label"); value_item(phones,"666","other");
    cJSON_AddArrayToObject(j,"photos"); cJSON_AddArrayToObject(j,"birthdays");
    return json(j);
}
class Server : public GoogleHttp {
public:
    std::map<std::string,std::string> people;
    std::string account="people/account1",last_mask,last_body;
    unsigned posts=0,patches=0,deletes=0,gets=0,version=1;
    bool pages=true,bad_page=false,malformed=false,conflict=false,lose_create=false,hide=false,deny=false,reject_create=false,delete_conflict=false,
         empty_defaults=false,wrong_count=false,drop_create=false,missing_profile=false;
    int request(const std::string &url,const char *method,const std::string &auth,const char *,const std::string &body,std::string &reply) override {
        // Allocation fault injection belongs to the client; the simulated
        // remote server has its own memory and is outside that failure domain.
        if (json_allocations_left>=0) { cJSON_InitHooks(nullptr); json_allocations_left=-1; }
        assert(url.find("https://people.googleapis.com/v1/")==0 && auth=="Bearer token");
        if (deny) { reply="{}"; return 403; }
        std::string path=url.substr(strlen("https://people.googleapis.com/v1")); path=path.substr(0,path.find('?'));
        if (path=="/people/me") {
            assert(param(url,"personFields")=="metadata");
            if (missing_profile) {
                reply="{\"error\":{\"message\":\"Request had insufficient authentication scopes.\",\"details\":[{\"reason\":\"ACCESS_TOKEN_SCOPE_INSUFFICIENT\"}]}}";
                return 403;
            }
            auto *j=cJSON_CreateObject(); set(j,"resourceName",account); reply=json(j); return 200;
        }
        if (path=="/people/me/connections") {
            assert(param(url,"sources")=="READ_SOURCE_TYPE_CONTACT" && !param(url,"personFields").empty());
            bool next=!param(url,"pageToken").empty(); if (next) assert(param(url,"pageToken")=="next+&");
            if (bad_page && next) { reply="{}"; return 503; }
            if (malformed) { reply="{\"connections\":{}}"; return 200; }
            if (empty_defaults) { reply="{}"; return 200; }
            auto *j=cJSON_CreateObject(),*a=cJSON_AddArrayToObject(j,"connections"); unsigned n=0;
            for (const auto &p:people) {
                if (!hide && (!pages || (next ? n>=1 : n<1))) cJSON_AddItemToArray(a,cJSON_Parse(p.second.c_str()));
                n++;
            }
            if (pages && !next) set(j,"nextPageToken","next+&");
            cJSON_AddNumberToObject(j,"totalItems",hide ? 0 : people.size()+(wrong_count ? 1 : 0)); reply=json(j); return 200;
        }
        bool create=path=="/people:createContact";
        size_t colon=path.find(':'); std::string resource=path.substr(1,colon==std::string::npos ? colon : colon-1);
        if (!strcmp(method,"GET")) {
            gets++;
            if (!people.count(resource)) { reply="{}"; return 404; }
            if (delete_conflict) {
                auto *j=cJSON_Parse(people[resource].c_str()); set(array_item(cJSON_GetObjectItemCaseSensitive(j,"metadata"),"sources"),"etag","changed-before-delete");
                people[resource]=json(j); delete_conflict=false;
            }
            reply=people[resource]; return 200;
        }
        if (!strcmp(method,"DELETE")) {
            assert(path.find(":deleteContact")!=std::string::npos); deletes++;
            if (!people.erase(resource)) { reply="{}"; return 404; }
            reply="{}"; return 200;
        }
        auto *input=cJSON_Parse(body.c_str()); assert(input);
        auto *j=create ? cJSON_CreateObject() : cJSON_Parse(people[resource].c_str()); assert(j);
        if (create) {
            posts++;
            if (drop_create) { drop_create=false; cJSON_Delete(input); cJSON_Delete(j); reply.clear(); return -1; }
            if (reject_create) { cJSON_Delete(input); cJSON_Delete(j); reply="{}"; return 400; }
            resource="people/cnew"+std::to_string(posts); set(j,"resourceName",resource);
            auto *source=cJSON_CreateObject(); set(source,"id","new"+std::to_string(posts)); set(source,"type","CONTACT");
            cJSON_AddItemToArray(cJSON_AddArrayToObject(cJSON_AddObjectToObject(j,"metadata"),"sources"),source);
            const cJSON *item;
            cJSON_ArrayForEach(item,input) cJSON_AddItemToObject(j,item->string,cJSON_Duplicate(item,true));
        } else {
            assert(path.find(":updateContact")!=std::string::npos); patches++;
            last_mask=param(url,"updatePersonFields"); last_body=body;
            auto *sent=array_item(cJSON_GetObjectItemCaseSensitive(input,"metadata"),"sources");
            auto *stored=array_item(cJSON_GetObjectItemCaseSensitive(j,"metadata"),"sources");
            assert(field(sent,"id")==field(stored,"id") && field(sent,"type")=="CONTACT");
            if (conflict || field(sent,"etag")!=field(stored,"etag")) {
                cJSON_Delete(input); cJSON_Delete(j); reply="{\"error\":{\"status\":\"failedPrecondition\"}}"; return 400;
            }
            size_t at=0;
            do {
                auto end=last_mask.find(',',at); std::string key=last_mask.substr(at,end-at);
                auto *a=cJSON_GetObjectItemCaseSensitive(input,key.c_str()); assert(cJSON_IsArray(a));
                cJSON_DeleteItemFromObjectCaseSensitive(j,key.c_str()); cJSON_AddItemToObject(j,key.c_str(),cJSON_Duplicate(a,true));
                if (end==std::string::npos) break;
                at=end+1;
            } while (true);
        }
        auto *source=array_item(cJSON_GetObjectItemCaseSensitive(j,"metadata"),"sources");
        set(source,"etag","etag"+std::to_string(++version));
        people[resource]=json(j); cJSON_Delete(input); reply=people[resource];
        if (create && lose_create) { lose_create=false; reply.clear(); return -1; }
        return 200;
    }
    void edit(const std::string &resource,const char *array,const char *key,const std::string &value) {
        auto *j=cJSON_Parse(people.at(resource).c_str()); set(array_item(j,array),key,value);
        set(array_item(cJSON_GetObjectItemCaseSensitive(j,"metadata"),"sources"),"etag","etag"+std::to_string(++version));
        people[resource]=json(j);
    }
};
class Host : public ContactSyncHost {
public:
    Server server; std::vector<ContactSyncMap> map; std::string bound;
    bool is_online=true,writable=true,bad_auth=false; uint32_t next_random=1;
    GoogleHttp &http() override { return server; }
    bool online() override { return is_online; }
    uint32_t random() override { return next_random++; }
    bool access_token(std::string &token,std::string &problem) override {
        if (bad_auth) { problem="Google sign-in expired"; return false; } token="token"; return true;
    }
    bool bind_account(const std::string &account,uint32_t palm) override {
        assert(palm==1); if (!bound.empty() && bound!=account) return false; bound=account; return true;
    }
    bool load_map(std::vector<ContactSyncMap> &out) override {
        std::string s; return contactsync_encode_map(map,s) && contactsync_decode_map(s,64,out);
    }
    bool store_map(const std::vector<ContactSyncMap> &in) override {
        if (!writable) return false;
        std::string s; return contactsync_encode_map(in,s) && contactsync_decode_map(s,64,map);
    }
};
static uint32_t put(const PalmContact &p,uint32_t id=0,uint8_t attr=HOTSYNC_ATTR_DIRTY,uint8_t category=0) {
    uint8_t db; assert(hotsync_open_db("AddressDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE,&db));
    size_t max; auto *data=hotsync_record_buffer(&max); auto len=contact_pack_record(p,data,max); assert(len);
    uint32_t saved; assert(hotsync_write_record(db,id,attr,category,len,&saved)); assert(hotsync_close_db(db)); return saved;
}
static uint32_t put_raw(const std::vector<uint8_t> &record,uint32_t id=0,uint8_t attr=HOTSYNC_ATTR_DIRTY,uint8_t category=0) {
    uint8_t db; assert(hotsync_open_db("AddressDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE,&db));
    size_t max; auto *data=hotsync_record_buffer(&max); assert(record.size()<=max);
    memcpy(data,record.data(),record.size());
    uint32_t saved; assert(hotsync_write_record(db,id,attr,category,record.size(),&saved)); assert(hotsync_close_db(db)); return saved;
}
static PalmContact get(uint32_t id,uint8_t *category=nullptr) {
    uint8_t db; assert(hotsync_open_db("AddressDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE,&db));
    HotSync_Record r; assert(hotsync_read_record(db,id,&r)); PalmContact c; assert(contact_unpack_record(r.data,r.len,c));
    if (category) *category=r.category;
    assert(hotsync_close_db(db)); return c;
}
static uint32_t mapped(const Host &h,const std::string &source) {
    for (const auto &m:h.map) if (m.source_id==source) return m.palm_id;
    assert(false); return 0;
}
static std::string resource(const Host &h,uint32_t palm) {
    for (const auto &m:h.map) if (m.palm_id==palm) return m.resource;
    assert(false); return "";
}
static ContactSyncResult sync(Host &h,const GoogleContactsSetup &setup,bool success=true) {
    ContactSyncResult r; contactsync_run(setup,PALM_WESTERN,h,r);
    printf("Contacts: %s\n",contactsync_summary(r).c_str()); fflush(stdout);
    assert(r.problem.empty()==success && !r.link_lost); return r;
}
static void codec() {
    // Independent v1 fixture: Work/Home/Fax/Mobile/Email labels, last name
    // Doe, first name Ann, company Acme. Company string offset is 9.
    const uint8_t fixture[]={0,4,0x72,0x10,0,0,0,7,9,'D','o','e',0,'A','n','n',0,'A','c','m','e',0};
    PalmContact p; assert(contact_unpack_record(fixture,sizeof(fixture),p));
    assert(p.text[CONTACT_LAST]=="Doe" && p.text[CONTACT_FIRST]=="Ann" && p.text[CONTACT_COMPANY]=="Acme" && p.labels[3]==7 && p.labels[4]==4);
    uint8_t data[CONTACT_RECORD_MAX]; size_t len=contact_pack_record(p,data,sizeof(data));
    assert(len==sizeof(fixture) && !memcmp(data,fixture,len));
    PalmContact expected=p;
    // Reserved gapfill and field-mask bits do not describe contact fields.
    // Real/imported records need not have our packer's canonical zero values.
    std::vector<uint8_t> legacy(fixture,fixture+sizeof(fixture));
    legacy[0]=0xa5; legacy[4]=0xff; legacy[5]=0xf8; legacy.insert(legacy.end(),2,0);
    std::string problem="old error";
    assert(contact_unpack_record(legacy.data(),legacy.size(),p,&problem) && problem.empty() &&
           p.text==expected.text && p.labels==expected.labels && p.show_phone==expected.show_phone);
    len=contact_pack_record(p,data,sizeof(data)); assert(len==sizeof(fixture) && !memcmp(data,fixture,len));
    auto malformed=legacy; malformed.back()=1;
    assert(!contact_unpack_record(malformed.data(),malformed.size(),p,&problem) && problem.find("trailing data")!=std::string::npos);
    malformed=legacy; malformed[1]=0x54;
    assert(!contact_unpack_record(malformed.data(),malformed.size(),p,&problem) && problem.find("Displayed phone")!=std::string::npos);
    malformed=legacy; malformed[3]=0x18;
    assert(!contact_unpack_record(malformed.data(),malformed.size(),p,&problem) && problem.find("Phone 1")!=std::string::npos);
    assert(!contact_unpack_record(nullptr,0,p,&problem) && problem.find("Header")!=std::string::npos);
    std::vector<uint8_t> oversized={0,4,0x72,0x10,0,0,0,1,0};
    oversized.insert(oversized.end(),CONTACT_NAME_MAX+1,'x'); oversized.push_back(0);
    assert(!contact_unpack_record(oversized.data(),oversized.size(),p,&problem) &&
           problem.find("Last name is 81 bytes; limit is 80")!=std::string::npos);
    for (size_t n=0;n<sizeof(fixture);n++) assert(!contact_unpack_record(fixture,n,p));
    assert(problem.find("Last name")!=std::string::npos);
    p=PalmContact(); p.show_phone=4;
    for (unsigned n=0;n<19;n++) p.text[n]=std::string(contact_field_limit(n),'x');
    len=contact_pack_record(p,data,sizeof(data)); assert(len==sizeof(data) && contact_unpack_record(data,len,p));
    assert(data[8]==2*CONTACT_NAME_MAX+3);
    auto bad=p; bad.labels[0]=8; assert(!contact_pack_record(bad,data,sizeof(data)));
    bad=p; bad.text[CONTACT_LAST]+="x"; assert(!contact_pack_record(bad,data,sizeof(data)));
    bad=p; bad.text[CONTACT_NOTE]=std::string("a\0b",3); assert(!contact_pack_record(bad,data,sizeof(data)));
    ContactSyncMap m; m.palm_id=0xffffff; m.source_id="id"; m.resource="people/cid"; m.fields[0]=0xffffffffu;
    std::string state; std::vector<ContactSyncMap> decoded;
    assert(contactsync_encode_map({m},state) && contactsync_decode_map(state,64,decoded));
    assert(decoded[0].fields==m.fields && decoded[0].resource==m.resource);
    for (size_t n=0;n<state.size();n++) assert(!contactsync_decode_map(state.substr(0,n),64,decoded));
    assert(!contactsync_decode_map(state+"x",64,decoded) && !contactsync_decode_map(state,0,decoded));
    assert(contactsync_encode_map({m,m},state) && !contactsync_decode_map(state,64,decoded));
    m.pending=2; m.source_id.clear(); m.resource.clear(); m.marker="pending-create";
    assert(contactsync_encode_map({m},state) && contactsync_decode_map(state,64,decoded) && decoded[0].pending==2);
    assert(contactsync_encode_map({},state) && contactsync_decode_map(state,64,decoded) && decoded.empty());
}
static void api_edges() {
    Server server; GoogleContactsSetup setup; GoogleContactsClient client(server,setup,"token");
    std::vector<GoogleContact> contacts;
    server.empty_defaults=true; assert(client.list(contacts) && contacts.empty()); server.empty_defaults=false;
    std::string resource="people/clarge";
    auto *j=cJSON_Parse(person(resource,"large","Large").c_str());
    cJSON_DeleteItemFromObjectCaseSensitive(j,"emailAddresses"); auto *a=cJSON_AddArrayToObject(j,"emailAddresses");
    for (unsigned n=0;n<100;n++) value_item(a,"email"+std::to_string(n)+"@example.com","work",n==0);
    set(array_item(j,"biographies"),"value","<p>Plain &amp; text</p>"); set(array_item(j,"biographies"),"contentType","TEXT_HTML");
    set(array_item(j,"names"),"givenName","\xd0\x90\xd0\xbd\xd0\xbd\xd0\xb0"); server.people[resource]=json(j);
    assert(client.list(contacts) && contacts.size()==1); PalmContact p;
    assert(client.display(contacts[0],PALM_CYRILLIC,p));
    assert(p.text[CONTACT_FIRST]==std::string("\xc0\xed\xed\xe0") && p.text[CONTACT_NOTE].find("Plain & text")!=std::string::npos);
    std::array<bool,CONTACT_SYNC_FIELDS> changes={}; changes[4]=true; p.text[CONTACT_PHONE1]="replacement@example.com";
    GoogleContact saved; assert(!client.save(&contacts[0],"",p,changes,PALM_CYRILLIC,saved) && !server.patches);
    // An unrelated edit is allowed and never includes the possibly truncated
    // array in the update mask.
    changes[4]=false; changes[2]=true; p.text[CONTACT_COMPANY]="Edited company";
    assert(client.save(&contacts[0],"",p,changes,PALM_CYRILLIC,saved) && server.last_mask=="organizations");
    server.wrong_count=true; assert(!client.list(contacts));

    // Every allocation failure while building an update must stop before
    // HTTPS; a half-built replacement array could otherwise clear real data.
    Server limited; limited.people["people/cmemory"]=person("people/cmemory","memory","Memory");
    GoogleContactsClient checked(limited,setup,"token"); assert(checked.list(contacts) && contacts.size()==1);
    assert(checked.display(contacts[0],PALM_WESTERN,p)); p.text[CONTACT_LAST]="Memory edit";
    p.labels[0]=0; p.text[CONTACT_PHONE1]="77"; changes.fill(true);
    bool reached_server=false;
    for (int n=0;n<1000;n++) {
        cJSON_Hooks hooks={limited_json_malloc,free}; json_allocations_left=n; cJSON_InitHooks(&hooks);
        bool ok=checked.save(&contacts[0],"",p,changes,PALM_WESTERN,saved);
        cJSON_InitHooks(nullptr); json_allocations_left=-1;
        if (ok) { reached_server=true; break; }
        assert(!limited.patches && checked.error().find("memory")!=std::string::npos);
    }
    assert(reached_server && limited.patches==1);
}
int main(int argc,char **argv) {
    codec(); api_edges(); assert(argc==2); fd=open(argv[1],O_RDWR|O_NOCTTY); assert(fd>=0);
    uint8_t memory[HOTSYNC_BUFFER_SIZE(CONTACT_RECORD_MAX)];
    HotSync_Io io={port_read,port_write,port_millis}; assert(hotsync_accept(&io,memory,sizeof(memory),10000));
    Host h; GoogleContactsSetup setup; h.server.people["people/ccloud"]=person("people/ccloud","cloud","Caf\xc3\xa9");
    h.server.people["people/cother"]=person("people/cother","other","Other");
    PalmContact local; local.text[CONTACT_LAST]="Palm"; local.text[CONTACT_FIRST]="Bob"; local.text[CONTACT_PHONE1]="999";
    local.text[CONTACT_CITY]="Auckland"; local.text[CONTACT_CUSTOM2]="Local custom"; local.text[CONTACT_NOTE]="Palm note";
    local.show_phone=2; uint32_t local_id=put(local,0,HOTSYNC_ATTR_DIRTY,3);
    // Exercise reserved bytes and zero padding through real DLP reads, not
    // just the standalone codec. These used to stop before either direction.
    uint8_t packed[CONTACT_RECORD_MAX]; size_t packed_len=contact_pack_record(local,packed,sizeof(packed)); assert(packed_len);
    std::vector<uint8_t> legacy(packed,packed+packed_len);
    legacy[0]=0xa5; legacy[4]|=0xff; legacy[5]|=0xf8; legacy.insert(legacy.end(),2,0);
    put_raw(legacy,local_id,HOTSYNC_ATTR_DIRTY,3);
    auto secret=local; secret.text[CONTACT_LAST]="Private"; uint32_t private_id=put(secret,0,HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_DIRTY);
    put(secret,0,HOTSYNC_ATTR_ARCHIVED|HOTSYNC_ATTR_DIRTY);
    // The old shared grant allows Contacts but omits profile permission.
    // Failed account identification must leave both address books and links
    // intact, so adding the grant can resume the same first sync.
    h.server.missing_profile=true;
    auto r=sync(h,setup,false);
    assert(r.problem.find("basic profile permission")!=std::string::npos &&
           !r.uploaded && !r.downloaded && !r.deleted && h.map.empty() && h.bound.empty() &&
           !h.server.posts && !h.server.patches && !h.server.deletes && get(local_id).text==local.text);
    h.server.missing_profile=false;
    // A genuinely malformed public record still stops the entire snapshot
    // before any upload/download or mapping change, with actionable context.
    auto malformed=legacy; malformed.back()=1; put_raw(malformed,local_id,HOTSYNC_ATTR_DIRTY,3);
    r=sync(h,setup,false);
    char record_id[24]; snprintf(record_id,sizeof(record_id),"ID %06lx",(unsigned long)local_id);
    assert(r.problem.find("Address Book record 0")!=std::string::npos && r.problem.find(record_id)!=std::string::npos &&
           r.problem.find("trailing data")!=std::string::npos && !r.uploaded && !r.downloaded && !r.deleted &&
           h.map.empty() && !h.server.posts && !h.server.patches && !h.server.deletes);
    put_raw(legacy,local_id,HOTSYNC_ATTR_DIRTY,3);
    r=sync(h,setup); assert(r.uploaded==1 && r.downloaded==3 && r.skipped==2 && h.map.size()==3);
    uint32_t cloud_id=mapped(h,"cloud"); auto palm=get(cloud_id);
    assert(palm.text[CONTACT_LAST]==std::string("Caf\xe9") && palm.text[CONTACT_NOTE].size()==CONTACT_NOTE_MAX &&
           palm.text[CONTACT_COMPANY]=="Company" && palm.text[CONTACT_TITLE]=="Engineer" && palm.text[CONTACT_CUSTOM1]=="Palm value");
    assert(palm.labels[0]==4 && palm.text[CONTACT_PHONE1]=="alice@example.com" && palm.labels[1]==7);
    uint8_t category; assert(get(local_id,&category).show_phone==2 && category==3);
    r=sync(h,setup); assert(!r.uploaded && !r.downloaded && h.server.posts==1);

    // Per-field merge, preserving additional fields inside singleton arrays.
    palm.text[CONTACT_LAST]="Palm rename"; palm.text[CONTACT_COMPANY]="Palm company"; put(palm,cloud_id);
    h.server.edit("people/ccloud","names","givenName","Cloud first");
    h.server.edit("people/ccloud","organizations","title","Cloud title");
    sync(h,setup); auto *j=cJSON_Parse(h.server.people["people/ccloud"].c_str());
    assert(field(array_item(j,"names"),"familyName")=="Palm rename" && field(array_item(j,"names"),"givenName")=="Cloud first" &&
           field(array_item(j,"names"),"middleName")=="Preserve" && field(array_item(j,"organizations"),"title")=="Cloud title" &&
           field(array_item(j,"organizations"),"department")=="Keep department" && field(array_item(j,"biographies"),"value").size()==5000);
    cJSON_Delete(j); assert(h.server.last_mask=="names,organizations");

    // Edit one visible number, preserving every number beyond the five slots,
    // the email's specific label, and custom labels on untouched values.
    palm=get(cloud_id); palm.text[CONTACT_PHONE2]="112"; put(palm,cloud_id); sync(h,setup);
    j=cJSON_Parse(h.server.people["people/ccloud"].c_str());
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j,"phoneNumbers"))==6 &&
           field(array_item(j,"phoneNumbers"),"value")=="112" && field(array_item(j,"emailAddresses"),"type")=="work" &&
           field(array_item(j,"phoneNumbers",4),"type")=="custom label"); cJSON_Delete(j);
    assert(h.server.last_mask=="phoneNumbers");

    // Slot type conversion crosses phone/email arrays without losing extras.
    palm=get(cloud_id); palm.labels[1]=4; palm.text[CONTACT_PHONE2]="new@example.com"; put(palm,cloud_id); sync(h,setup);
    j=cJSON_Parse(h.server.people["people/ccloud"].c_str());
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j,"phoneNumbers"))==5 &&
           cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j,"emailAddresses"))==2); cJSON_Delete(j);

    // Address and Palm custom edits preserve Google's unrelated values.
    palm=get(cloud_id); palm.text[CONTACT_CITY]="Palm city"; palm.text[CONTACT_CUSTOM1]="Edited custom"; put(palm,cloud_id);
    h.server.edit("people/ccloud","addresses","postalCode","Cloud postcode"); sync(h,setup);
    j=cJSON_Parse(h.server.people["people/ccloud"].c_str());
    assert(field(array_item(j,"addresses"),"city")=="Palm city" && field(array_item(j,"addresses"),"postalCode")=="Cloud postcode" &&
           field(array_item(j,"addresses"),"poBox")=="Keep box" && field(array_item(j,"userDefined"),"value")=="Preserve custom"); cJSON_Delete(j);
    assert(get(cloud_id).text[CONTACT_ZIP]=="Cloud postcode");

    // A list absence is individually checked, and changing the merged People
    // resource name retains the same Palm contact through its CONTACT source.
    h.server.hide=true; auto gets=h.server.gets; r=sync(h,setup); assert(!r.deleted && h.server.gets>gets); h.server.hide=false;
    j=cJSON_Parse(h.server.people["people/ccloud"].c_str()); set(j,"resourceName","people/cnewresource");
    h.server.people.erase("people/ccloud"); h.server.people["people/cnewresource"]=json(j);
    sync(h,setup); assert(mapped(h,"cloud")==cloud_id && resource(h,cloud_id)=="people/cnewresource");

    unsigned writes=h.server.posts+h.server.patches+h.server.deletes;
    h.server.bad_page=true; sync(h,setup,false); h.server.bad_page=false;
    h.server.malformed=true; sync(h,setup,false); h.server.malformed=false;
    h.is_online=false; sync(h,setup,false); h.is_online=true;
    h.bad_auth=true; sync(h,setup,false); h.bad_auth=false;
    h.server.deny=true; sync(h,setup,false); h.server.deny=false;
    h.server.account="people/otheraccount"; sync(h,setup,false); h.server.account="people/account1";
    auto tiny=setup; tiny.max_contacts=1; sync(h,tiny,false);
    assert(writes==h.server.posts+h.server.patches+h.server.deletes);

    // Source etags reject concurrent writes, leaving pending Palm edits intact.
    palm=get(cloud_id); palm.text[CONTACT_LAST]="Etag edit"; put(palm,cloud_id);
    h.server.conflict=true; sync(h,setup,false); h.server.conflict=false; sync(h,setup);
    assert(get(cloud_id).text[CONTACT_LAST]=="Etag edit");

    // Concurrent phone slot changes cannot identify the original field safely.
    palm=get(cloud_id); auto original=palm; palm.text[CONTACT_PHONE1]="local@example.com"; put(palm,cloud_id);
    h.server.edit("people/cnewresource","phoneNumbers","value","Cloud phone edit");
    writes=h.server.posts+h.server.patches+h.server.deletes; sync(h,setup,false);
    assert(writes==h.server.posts+h.server.patches+h.server.deletes && get(cloud_id).text[CONTACT_PHONE1]=="local@example.com");
    put(original,cloud_id); sync(h,setup); // Restoring Palm slots permits cloud import.

    // Unconfirmed creation is journalled before POST. A lagging list stops
    // retry; when the marker appears, the same contact is linked after reboot.
    local.text[CONTACT_LAST]="Lost reply"; uint32_t retry_id=put(local); h.server.lose_create=true;
    sync(h,setup,false); assert(h.map.back().pending==2);
    unsigned posts=h.server.posts; h.server.hide=true; sync(h,setup,false); assert(h.server.posts==posts); h.server.hide=false;
    Host restarted=h; sync(restarted,setup); assert(restarted.server.posts==posts && !resource(restarted,retry_id).empty());
    h=restarted; r=sync(h,setup); assert(!r.uploaded && !r.downloaded);

    // A request that never reached Google also waits rather than creating
    // automatically twice. The explicit portal retry re-arms the journal.
    local.text[CONTACT_LAST]="Never reached Google"; put(local); h.server.drop_create=true;
    sync(h,setup,false); assert(h.map.back().pending==2);
    posts=h.server.posts; sync(h,setup,false); assert(h.server.posts==posts);
    h.map.back().pending=1; sync(h,setup); assert(h.server.posts==posts+1);

    // A definite rejection may retry; failed persistence must never send POST.
    local.text[CONTACT_LAST]="Rejected"; put(local); h.server.reject_create=true; sync(h,setup,false);
    assert(h.map.back().pending==1); h.server.reject_create=false; sync(h,setup);
    local.text[CONTACT_LAST]="Storage failure"; put(local); h.writable=false; posts=h.server.posts;
    sync(h,setup,false); assert(h.server.posts==posts); h.writable=true; sync(h,setup);

    // Google deletion and Palm tombstones; a changed contact immediately
    // before DELETE is protected by an extra read.
    h.server.people.erase("people/cother"); r=sync(h,setup); assert(r.deleted==1);
    palm=get(cloud_id); put(palm,cloud_id,HOTSYNC_ATTR_DELETED|HOTSYNC_ATTR_DIRTY);
    unsigned deletes=h.server.deletes; h.server.delete_conflict=true; sync(h,setup,false); assert(h.server.deletes==deletes);
    r=sync(h,setup); assert(r.deleted==1 && !h.server.people.count("people/cnewresource"));
    assert(get(private_id).text[CONTACT_LAST]=="Private");
    assert(hotsync_end(0)); close(fd); puts("done");
}
