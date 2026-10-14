#include "googlecontacts.h"
#include "mailoauth.h"
#include "cJSON.h"
#include <memory>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>

using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
static Json parse(const std::string &s) { return Json(cJSON_ParseWithOpts(s.c_str(),nullptr,true),cJSON_Delete); }
static const char *person_fields="metadata,names,organizations,phoneNumbers,emailAddresses,addresses,biographies,userDefined,clientData";
static std::string field(const cJSON *j,const char *key) {
    const auto *v=cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static std::string print(const cJSON *j) {
    char *raw=cJSON_PrintUnformatted(j);
    if (!raw) return "";
    std::string s(raw); free(raw); return s;
}
static bool resource_ok(const std::string &s) {
    return s.size()>7 && s.size()<=256 && s.compare(0,7,"people/")==0 &&
        s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-",7)==std::string::npos;
}
static std::string resource_path(const std::string &s) { return "/people/"+oauth_encode(s.substr(7)); }
static bool unpack(const cJSON *j,GoogleContact &c) {
    if (!cJSON_IsObject(j)) return false;
    c=GoogleContact(); c.resource=field(j,"resourceName");
    if (!resource_ok(c.resource) || c.resource=="people/me") return false;
    const auto *meta=cJSON_GetObjectItemCaseSensitive(j,"metadata");
    const auto *deleted=cJSON_GetObjectItemCaseSensitive(meta,"deleted");
    if (!cJSON_IsObject(meta) || (deleted && !cJSON_IsBool(deleted))) return false;
    c.deleted=cJSON_IsTrue(deleted);
    auto *sources=cJSON_GetObjectItemCaseSensitive(meta,"sources");
    if (sources && !cJSON_IsArray(sources)) return false;
    const cJSON *item;
    cJSON_ArrayForEach(item,sources) {
        if (!cJSON_IsObject(item)) return false;
        if (field(item,"type")!="CONTACT") continue;
        if (!c.source_id.empty()) return false;
        c.source_id=field(item,"id"); c.source_etag=field(item,"etag");
        if (c.source_id.empty() || c.source_id.size()>256 || c.source_etag.empty()) return false;
    }
    if (!c.deleted && c.source_id.empty()) return false;
    for (const char *key:{"names","organizations","phoneNumbers","emailAddresses","addresses","biographies","userDefined","clientData"}) {
        auto *a=cJSON_GetObjectItemCaseSensitive(j,key);
        if (a && (!cJSON_IsArray(a) || cJSON_GetArraySize(a)>100)) return false;
        cJSON_ArrayForEach(item,a) {
            if (!cJSON_IsObject(item)) return false;
            for (const char *text:{"familyName","givenName","name","title","value","type","streetAddress","city","region","postalCode","country","key","contentType"}) {
                auto *v=cJSON_GetObjectItemCaseSensitive(item,text);
                if (v && !cJSON_IsString(v)) return false;
            }
            if (!strcmp(key,"clientData") && field(item,"key")=="PalmIrdaSync") {
                if (!c.marker.empty()) return false;
                c.marker=field(item,"value");
            }
        }
    }
    // With READ_SOURCE_TYPE_CONTACT, these fields are contact singletons.
    for (const char *key:{"names","biographies"})
        if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(j,key))>1) return false;
    c.json=print(j);
    return !c.json.empty();
}
int GoogleContactsClient::request(const std::string &path,const char *method,const std::string &body,std::string &reply,
                                 const char *permission) {
    error_.clear();
    if (token_.empty() || token_.find_first_of("\r\n")!=std::string::npos) { error_="Contacts is not authorized"; return -1; }
    int code=http_.request("https://people.googleapis.com/v1"+path,method,"Bearer "+token_,"application/json",body,reply);
    if (code<200 || code>=300) {
        error_="Google Contacts request failed (HTTP "+std::to_string(code)+")";
        if (code<0 && !http_.error().empty()) error_="Google Contacts: "+http_.error();
        if (code>0) {
            auto j=parse(reply);
            auto *api_error=cJSON_GetObjectItemCaseSensitive(j.get(),"error");
            bool api_disabled=false,scope_missing=false;
            const cJSON *detail;
            cJSON_ArrayForEach(detail,cJSON_GetObjectItemCaseSensitive(api_error,"details")) {
                const std::string reason=field(detail,"reason");
                api_disabled=api_disabled || reason=="SERVICE_DISABLED" || reason=="API_DISABLED" || reason=="accessNotConfigured";
                scope_missing=scope_missing || reason=="ACCESS_TOKEN_SCOPE_INSUFFICIENT";
            }
            cJSON_ArrayForEach(detail,cJSON_GetObjectItemCaseSensitive(api_error,"errors")) {
                api_disabled=api_disabled || field(detail,"reason")=="accessNotConfigured";
                scope_missing=scope_missing || field(detail,"reason")=="insufficientPermissions";
            }
            scope_missing=scope_missing || field(api_error,"message")=="Request had insufficient authentication scopes.";
            if (api_disabled) error_="People API is disabled for this Google OAuth project. Enable People API in Google Cloud, then check Contacts again.";
            else if (scope_missing) error_=std::string("Google did not grant ")+permission+" permission. Sign in again and allow it.";
            else {
                std::string message=field(api_error,"message");
                if (!message.empty()) error_+=". "+message.substr(0,384);
                if (code==403 && message.empty()) error_+=". Check Contacts permission and People API access.";
            }
        }
        if (code==400 && reply.find("failedPrecondition")!=std::string::npos)
            error_="Contact changed during sync. Sync again.";
    }
    return code;
}
bool GoogleContactsClient::identity(std::string &account) {
    account.clear();
    std::string reply;
    // This is a profile lookup, not a contact lookup. Request only the account
    // metadata needed for binding; profile permission is required nonetheless.
    if (request("/people/me?personFields=metadata","GET","",reply,"basic profile")!=200) return false;
    auto j=parse(reply); account=field(j.get(),"resourceName");
    if (!resource_ok(account) || account=="people/me") { error_="Could not identify the signed-in Google account"; return false; }
    return true;
}
bool GoogleContactsClient::list(std::vector<GoogleContact> &contacts) {
    contacts.clear(); std::string page; size_t bytes=0; int expected=-1;
    std::vector<std::string> pages;
    for (unsigned n=0;n<1000;n++) {
        std::string path="/people/me/connections?pageSize=1&sources=READ_SOURCE_TYPE_CONTACT&personFields="+oauth_encode(person_fields);
        if (!page.empty()) path+="&pageToken="+oauth_encode(page);
        std::string reply;
        if (request(path,"GET","",reply)!=200) return false;
        auto j=parse(reply);
        auto *a=cJSON_GetObjectItemCaseSensitive(j.get(),"connections"),*next=cJSON_GetObjectItemCaseSensitive(j.get(),"nextPageToken");
        auto *total=cJSON_GetObjectItemCaseSensitive(j.get(),"totalItems");
        // A protobuf JSON response can omit empty repeated/zero fields and
        // represent a genuinely empty contact book as {}.
        if (!cJSON_IsObject(j.get()) || (a && !cJSON_IsArray(a)) || (next && !cJSON_IsString(next)) ||
            (!a && j->child && (!cJSON_IsNumber(total) || total->valuedouble!=0))) {
            error_="Invalid Contacts list response"; return false;
        }
        if (total) {
            if (!cJSON_IsNumber(total) || total->valuedouble<0 || total->valuedouble>setup_.max_contacts ||
                total->valuedouble!=(int)total->valuedouble) {
                error_="Contacts list exceeds the configured limit or has an invalid count"; return false;
            }
            if (expected!=-1 && expected!=(int)total->valuedouble) { error_="Contacts changed while reading pages. Sync again."; return false; }
            expected=(int)total->valuedouble;
        }
        const cJSON *item;
        cJSON_ArrayForEach(item,a) {
            GoogleContact c;
            if (!unpack(item,c)) { error_="Invalid Google contact"; return false; }
            for (const auto &old:contacts)
                if (old.resource==c.resource || (!c.source_id.empty() && old.source_id==c.source_id)) {
                    error_="Duplicate Google contact identity"; return false;
                }
            bytes+=c.json.size();
            if (bytes>48000 || contacts.size()>=setup_.max_contacts) {
                error_="Google Contacts exceeds the configured contact or memory limit"; return false;
            }
            contacts.push_back(c);
        }
        std::string next_page=field(j.get(),"nextPageToken");
        if (next_page.empty()) {
            if (expected!=-1 && (size_t)expected!=contacts.size()) { error_="Incomplete Contacts list response"; return false; }
            return true;
        }
        if (next_page==page || std::find(pages.begin(),pages.end(),next_page)!=pages.end()) { error_="Contacts repeated a page"; return false; }
        pages.push_back(next_page);
        page=next_page;
    }
    error_="Too many Contacts pages"; return false;
}
bool GoogleContactsClient::get(const std::string &resource,GoogleContact &c,bool &found) {
    found=false;
    if (!resource_ok(resource)) { error_="Invalid saved contact identity"; return false; }
    std::string reply;
    int code=request(resource_path(resource)+"?sources=READ_SOURCE_TYPE_CONTACT&personFields="+oauth_encode(person_fields),"GET","",reply);
    if (code==404 || code==410) { error_.clear(); return true; }
    if (code!=200) return false;
    auto j=parse(reply);
    if (!unpack(j.get(),c)) { error_="Invalid Contacts response"; return false; }
    found=!c.deleted; return true;
}
static bool primary(const cJSON *j) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j,"metadata"),"primary"));
}
static cJSON *selected(cJSON *array) {
    cJSON *item;
    cJSON_ArrayForEach(item,array) if (primary(item)) return item;
    return cJSON_GetArrayItem(array,0);
}
static uint8_t phone_label(std::string type) {
    std::transform(type.begin(),type.end(),type.begin(),[](unsigned char c) { return (char)std::tolower(c); });
    if (type=="work") return 0;
    if (type=="home") return 1;
    if (type=="workfax" || type=="homefax" || type=="fax" || type=="otherfax") return 2;
    if (type=="main") return 5;
    if (type=="pager") return 6;
    if (type=="mobile") return 7;
    return 3;
}
struct Slot { bool email; int index; uint8_t label; bool preferred; std::string type,value; };
static std::vector<Slot> slots(cJSON *person) {
    std::vector<Slot> out;
    for (unsigned kind=0;kind<2;kind++) {
        auto *a=cJSON_GetObjectItemCaseSensitive(person,kind ? "emailAddresses" : "phoneNumbers");
        int index=0; cJSON *item;
        cJSON_ArrayForEach(item,a) {
            std::string value=field(item,"value"),type=field(item,"type");
            if (!value.empty()) out.push_back({kind!=0,index,(uint8_t)(kind ? 4 : phone_label(type)),primary(item),type,value});
            index++;
        }
    }
    // People does not guarantee array order. Primary entries first, then a
    // stable ordering; the current projection is checked before any slot edit.
    std::sort(out.begin(),out.end(),[](const Slot &a,const Slot &b) {
        if (a.preferred!=b.preferred) return a.preferred>b.preferred;
        if (a.email!=b.email) return a.email>b.email;
        if (a.type!=b.type) return a.type<b.type;
        if (a.value!=b.value) return a.value<b.value;
        return a.index<b.index;
    });
    if (out.size()>5) out.resize(5);
    return out;
}
static const char *custom_keys[]={"Palm Custom 1","Palm Custom 2","Palm Custom 3","Palm Custom 4"};
static const char *address_keys[]={"streetAddress","city","region","postalCode","country"};
bool GoogleContactsClient::display(const GoogleContact &c,PalmCharset charset,PalmContact &p) {
    auto j=parse(c.json);
    if (!j || c.deleted) { error_="Contact data is invalid or there is not enough memory to display it"; return false; }
    p=PalmContact();
    auto assign=[&](unsigned index,const std::string &s) { p.text[index]=palm_from_utf8(s,charset,contact_field_limit(index)); };
    auto *name=selected(cJSON_GetObjectItemCaseSensitive(j.get(),"names"));
    assign(CONTACT_LAST,field(name,"familyName")); assign(CONTACT_FIRST,field(name,"givenName"));
    if (p.text[CONTACT_LAST].empty() && p.text[CONTACT_FIRST].empty())
        assign(CONTACT_LAST,field(name,"unstructuredName"));
    auto *org=selected(cJSON_GetObjectItemCaseSensitive(j.get(),"organizations"));
    assign(CONTACT_COMPANY,field(org,"name")); assign(CONTACT_TITLE,field(org,"title"));
    auto numbers=slots(j.get());
    for (unsigned n=0;n<numbers.size();n++) { assign(CONTACT_PHONE1+n,numbers[n].value); p.labels[n]=numbers[n].label; }
    auto *address=selected(cJSON_GetObjectItemCaseSensitive(j.get(),"addresses"));
    for (unsigned n=0;n<5;n++) assign(CONTACT_STREET+n,field(address,address_keys[n]));
    auto *bio=selected(cJSON_GetObjectItemCaseSensitive(j.get(),"biographies"));
    std::string note=field(bio,"value");
    if (field(bio,"contentType")=="TEXT_HTML") {
        MailTextDecoder decoder("utf-8","",true,charset,CONTACT_NOTE_MAX);
        decoder.feed((const uint8_t *)note.data(),note.size()); decoder.finish(); p.text[CONTACT_NOTE]=decoder.text();
    } else assign(CONTACT_NOTE,note);
    auto *custom=cJSON_GetObjectItemCaseSensitive(j.get(),"userDefined"); cJSON *item;
    bool seen[4]={};
    cJSON_ArrayForEach(item,custom) for (unsigned n=0;n<4;n++) if (field(item,"key")==custom_keys[n]) {
        if (!seen[n]) assign(CONTACT_CUSTOM1+n,field(item,"value"));
        seen[n]=true;
    }
    return true;
}
static cJSON *copy_array(cJSON *dest,cJSON *current,const char *key) {
    if (!dest) return nullptr;
    auto *a=cJSON_GetObjectItemCaseSensitive(current,key);
    cJSON *copy=a ? cJSON_Duplicate(a,true) : cJSON_CreateArray();
    if (!copy || !cJSON_AddItemToObject(dest,key,copy)) { cJSON_Delete(copy); return nullptr; }
    return copy;
}
static cJSON *ensure_selected(cJSON *a) {
    if (!a) return nullptr;
    auto *item=selected(a);
    if (!item) {
        item=cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(a,item)) { cJSON_Delete(item); return nullptr; }
    }
    return item;
}
static bool put_text(cJSON *j,const char *key,const std::string &s) {
    auto *value=cJSON_CreateString(s.c_str());
    if (!j || !value) { cJSON_Delete(value); return false; }
    bool ok=cJSON_GetObjectItemCaseSensitive(j,key) ? cJSON_ReplaceItemInObjectCaseSensitive(j,key,value) : cJSON_AddItemToObject(j,key,value);
    if (!ok) cJSON_Delete(value);
    return ok;
}
bool GoogleContactsClient::save(const GoogleContact *current,const std::string &marker,const PalmContact &p,
                               const std::array<bool,CONTACT_SYNC_FIELDS> &changes,PalmCharset charset,GoogleContact &saved) {
    create_rejected_=false;
    auto old=parse(current ? current->json : "{}"); Json body(cJSON_CreateObject(),cJSON_Delete);
    if (!old || !body) { error_="Not enough memory for Contacts update"; return false; }
    std::string mask; bool allocation_ok=true;
    auto set=[&](cJSON *j,const char *key,const std::string &s) { allocation_ok=put_text(j,key,s) && allocation_ok; };
    auto include=[&](const char *key) {
        if (!mask.empty()) mask+=",";
        mask+=key; auto *a=copy_array(body.get(),old.get(),key); allocation_ok=allocation_ok && a; return a;
    };
    auto text=[&](unsigned i) { return palm_to_utf8(p.text[i],charset); };
    if (changes[0] || changes[1]) {
        auto *name=ensure_selected(include("names"));
        if (changes[0]) set(name,"familyName",text(CONTACT_LAST));
        if (changes[1]) set(name,"givenName",text(CONTACT_FIRST));
    }
    if (changes[2] || changes[3]) {
        auto *org=ensure_selected(include("organizations"));
        if (changes[2]) set(org,"name",text(CONTACT_COMPANY));
        if (changes[3]) set(org,"title",text(CONTACT_TITLE));
    }
    if (changes[4]) {
        // connections.list caps email arrays at 100. At that boundary the
        // response may omit additional values; replacing it would lose them.
        if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(old.get(),"emailAddresses"))>=100 ||
            cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(old.get(),"phoneNumbers"))>=100) {
            error_="This contact has too many phone/email values to edit safely from Palm."; return false;
        }
        Json numbers(cJSON_CreateObject(),cJSON_Delete);
        cJSON *arrays[]={copy_array(numbers.get(),old.get(),"phoneNumbers"),copy_array(numbers.get(),old.get(),"emailAddresses")};
        allocation_ok=allocation_ok && arrays[0] && arrays[1];
        std::vector<int> erase[2]; bool touched[2]={};
        auto projected=slots(old.get()); PalmContact shown;
        if (current && !display(*current,charset,shown)) return false;
        static const char *types[]={"work","home","workFax","other","other","main","pager","mobile"};
        for (unsigned n=0;n<5;n++) {
            const std::string &value=p.text[CONTACT_PHONE1+n]; bool email=p.labels[n]==4;
            const Slot *previous=n<projected.size() ? &projected[n] : nullptr;
            if (previous && value==shown.text[CONTACT_PHONE1+n] && p.labels[n]==shown.labels[n]) continue;
            if (previous && (value.empty() || email!=previous->email)) {
                erase[previous->email].push_back(previous->index); touched[previous->email]=true;
            }
            if (value.empty()) continue;
            cJSON *item=previous && email==previous->email ? cJSON_GetArrayItem(arrays[email],previous->index) : nullptr;
            if (!item) {
                item=cJSON_CreateObject();
                if (!item || !cJSON_AddItemToArray(arrays[email],item)) { cJSON_Delete(item); item=nullptr; allocation_ok=false; }
            }
            // Unchanged labels preserve Google's specific/custom type.
            if (!previous || p.labels[n]!=previous->label || email!=previous->email) set(item,"type",types[p.labels[n]]);
            set(item,"value",text(CONTACT_PHONE1+n)); touched[email]=true;
        }
        for (unsigned kind=0;kind<2;kind++) {
            std::sort(erase[kind].rbegin(),erase[kind].rend());
            for (int i:erase[kind]) cJSON_DeleteItemFromArray(arrays[kind],i);
            if (touched[kind]) {
                const char *key=kind ? "emailAddresses" : "phoneNumbers";
                if (!mask.empty()) mask+=",";
                mask+=key;
                auto *a=cJSON_DetachItemFromObjectCaseSensitive(numbers.get(),key);
                if (!a || !cJSON_AddItemToObject(body.get(),key,a)) { cJSON_Delete(a); allocation_ok=false; }
            }
        }
    }
    if (std::any_of(changes.begin()+5,changes.begin()+10,[](bool b) { return b; })) {
        auto *address=ensure_selected(include("addresses"));
        for (unsigned n=0;n<5;n++) if (changes[5+n]) set(address,address_keys[n],text(CONTACT_STREET+n));
        if (changes[9]) cJSON_DeleteItemFromObjectCaseSensitive(address,"countryCode");
    }
    if (std::any_of(changes.begin()+10,changes.begin()+14,[](bool b) { return b; })) {
        auto *a=include("userDefined");
        for (unsigned n=0;n<4;n++) {
            if (!changes[10+n]) continue;
            cJSON *item,*match=nullptr;
            cJSON_ArrayForEach(item,a) if (field(item,"key")==custom_keys[n]) { match=item; break; }
            if (!match && p.text[CONTACT_CUSTOM1+n].empty()) continue;
            if (!match) {
                match=cJSON_CreateObject();
                if (!match || !cJSON_AddItemToArray(a,match)) { cJSON_Delete(match); match=nullptr; allocation_ok=false; }
                set(match,"key",custom_keys[n]);
            }
            set(match,"value",text(CONTACT_CUSTOM1+n));
        }
    }
    if (changes[14]) {
        auto *bio=ensure_selected(include("biographies"));
        set(bio,"value",text(CONTACT_NOTE)); set(bio,"contentType","TEXT_PLAIN");
    }
    if (current) {
        auto *sources=cJSON_AddArrayToObject(cJSON_AddObjectToObject(body.get(),"metadata"),"sources");
        auto *source=cJSON_CreateObject();
        set(source,"type","CONTACT"); set(source,"id",current->source_id); set(source,"etag",current->source_etag);
        if (!source || !cJSON_AddItemToArray(sources,source)) { cJSON_Delete(source); allocation_ok=false; }
        set(body.get(),"resourceName",current->resource);
        if (mask.empty()) { saved=*current; return true; }
    } else {
        // Do not create placeholder empty organizations, addresses or names.
        if (p.text[CONTACT_LAST].empty() && p.text[CONTACT_FIRST].empty()) cJSON_DeleteItemFromObjectCaseSensitive(body.get(),"names");
        if (p.text[CONTACT_COMPANY].empty() && p.text[CONTACT_TITLE].empty()) cJSON_DeleteItemFromObjectCaseSensitive(body.get(),"organizations");
        bool address_empty=true;
        for (unsigned n=CONTACT_STREET;n<=CONTACT_COUNTRY;n++) address_empty=address_empty && p.text[n].empty();
        if (address_empty) cJSON_DeleteItemFromObjectCaseSensitive(body.get(),"addresses");
        if (p.text[CONTACT_NOTE].empty()) cJSON_DeleteItemFromObjectCaseSensitive(body.get(),"biographies");
        if (!cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(body.get(),"userDefined"))) cJSON_DeleteItemFromObjectCaseSensitive(body.get(),"userDefined");
        auto *a=cJSON_AddArrayToObject(body.get(),"clientData"); auto *item=cJSON_CreateObject();
        set(item,"key","PalmIrdaSync"); set(item,"value",marker);
        if (!item || !cJSON_AddItemToArray(a,item)) { cJSON_Delete(item); allocation_ok=false; }
    }
    if (!allocation_ok) { error_="Not enough memory for Contacts update"; return false; }
    std::string raw=print(body.get()),reply;
    if (raw.empty()) { error_="Not enough memory for Contacts update"; return false; }
    std::string path=current ? resource_path(current->resource)+":updateContact?updatePersonFields="+oauth_encode(mask)+"&" : "/people:createContact?";
    path+="sources=READ_SOURCE_TYPE_CONTACT&personFields="+oauth_encode(person_fields);
    int code=request(path,current ? "PATCH" : "POST",raw,reply);
    create_rejected_=!current && code>=400 && code<500 && code!=408;
    if (code!=200) return false;
    auto j=parse(reply);
    if (!unpack(j.get(),saved) || saved.deleted || (current && saved.source_id!=current->source_id) ||
        (!current && saved.marker!=marker)) { error_="Invalid Contacts save response"; return false; }
    return true;
}
bool GoogleContactsClient::remove(const GoogleContact &c) {
    // People API provides source-etag checks for updateContact, but no
    // documented conditional delete. Re-read immediately before deletion.
    GoogleContact latest; bool found;
    if (!get(c.resource,latest,found)) return false;
    if (!found) return true;
    if (latest.source_id!=c.source_id || latest.source_etag!=c.source_etag) {
        error_="Contact changed before deletion. Sync again."; return false;
    }
    std::string reply; int code=request(resource_path(latest.resource)+":deleteContact","DELETE","",reply);
    if (code==404 || code==410) { error_.clear(); return true; }
    return code==200 || code==204;
}
