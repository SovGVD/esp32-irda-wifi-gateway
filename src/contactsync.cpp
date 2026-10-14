#include "contactsync.h"
#include "hotsync.h"
#include <algorithm>
#include <cstdio>

static void put32(std::string &out,uint32_t n) { for (int i=3;i>=0;--i) out+=(char)(n>>(8*i)); }
static uint32_t get32(const std::string &s,size_t &at) {
    uint32_t n=0; for (unsigned i=0;i<4;i++) n=(n<<8)|(uint8_t)s[at++]; return n;
}
static bool string_ok(const std::string &s,size_t max) { return s.size()<=max && s.find('\0')==std::string::npos; }
static bool valid_map(const ContactSyncMap &m) {
    return m.palm_id && m.palm_id<=0xffffff && m.pending<=2 && string_ok(m.source_id,256) &&
        string_ok(m.resource,256) && string_ok(m.marker,128) &&
        (m.pending ? (m.source_id.empty() && m.resource.empty() && !m.marker.empty()) :
         (!m.source_id.empty() && m.resource.size()>7 && m.resource.compare(0,7,"people/")==0));
}
bool contactsync_encode_map(const std::vector<ContactSyncMap> &map,std::string &out) {
    out="CTC1";
    if (map.size()>65535) return false;
    out+=(char)(map.size()>>8); out+=(char)map.size();
    for (const auto &m:map) {
        if (!valid_map(m)) return false;
        put32(out,m.palm_id); out+=(char)m.pending;
        for (auto f:m.fields) put32(out,f);
        const std::string *strings[]={&m.source_id,&m.resource,&m.marker};
        for (auto *s:strings) { out+=(char)(s->size()>>8); out+=(char)s->size(); out+=*s; }
    }
    return true;
}
bool contactsync_decode_map(const std::string &data,unsigned max,std::vector<ContactSyncMap> &map) {
    map.clear();
    if (data.size()<6 || data.compare(0,4,"CTC1")) return false;
    unsigned count=((uint8_t)data[4]<<8)|(uint8_t)data[5];
    if (count>max) return false;
    size_t at=6; std::vector<ContactSyncMap> decoded;
    for (unsigned n=0;n<count;n++) {
        if (data.size()-at<5+4*CONTACT_SYNC_FIELDS) return false;
        ContactSyncMap m; m.palm_id=get32(data,at); m.pending=(uint8_t)data[at++];
        for (auto &f:m.fields) f=get32(data,at);
        std::string *strings[]={&m.source_id,&m.resource,&m.marker};
        for (auto *s:strings) {
            if (data.size()-at<2) return false;
            unsigned length=((uint8_t)data[at]<<8)|(uint8_t)data[at+1]; at+=2;
            if (data.size()-at<length) return false;
            *s=data.substr(at,length); at+=length;
        }
        if (!valid_map(m)) return false;
        for (const auto &old:decoded)
            if (old.palm_id==m.palm_id || (!m.source_id.empty() && old.source_id==m.source_id) ||
                (!m.marker.empty() && old.marker==m.marker)) return false;
        decoded.push_back(m);
    }
    if (at!=data.size()) return false;
    map.swap(decoded); return true;
}
struct LocalContact { uint32_t id; uint8_t attr,category; PalmContact contact; };
static bool write_palm(uint8_t db,const LocalContact &local,const PalmContact &p,uint32_t &id) {
    size_t room; auto *data=hotsync_record_buffer(&room);
    size_t len=contact_pack_record(p,data,room);
    return len && hotsync_write_record(db,local.id,local.attr&~HOTSYNC_ATTR_DIRTY,local.category,len,&id);
}
void contactsync_run(const GoogleContactsSetup &setup,PalmCharset charset,ContactSyncHost &host,ContactSyncResult &r) {
    r=ContactSyncResult();
    if (!setup.max_contacts || setup.max_contacts>65534) { r.problem="Invalid Contacts sync limit"; return; }
    if (!host.online()) { r.problem="Wi-Fi is not connected"; return; }
    std::vector<ContactSyncMap> map;
    if (!host.load_map(map) || map.size()>setup.max_contacts) { r.problem="Contacts sync state could not be loaded"; return; }
    std::string token;
    if (!host.access_token(token,r.problem)) return;
    GoogleContactsClient google(host.http(),setup,token);
    std::string account; std::vector<GoogleContact> remote;
    if (!google.identity(account) || !google.list(remote)) { r.problem=google.error(); return; }
    uint32_t palm;
    if (!hotsync_read_user_id(&palm)) { r.problem="Could not identify the Palm"; r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK; return; }
    if (!host.bind_account(account,palm)) { r.problem="Google account or Palm identity does not match Contacts sync state"; return; }
    auto checkpoint=[&]() {
        if (host.store_map(map)) return true;
        r.problem="Could not save Contacts sync state. No further contacts were changed."; return false;
    };
    // A full list can lag after a write. Resolve markers, never assume an
    // uncertain POST failed because it is still absent from that list.
    for (auto &m:map) if (m.pending) {
        auto match=remote.end();
        for (auto it=remote.begin();it!=remote.end();++it) if (!it->deleted && it->marker==m.marker) {
            if (match!=remote.end()) { r.problem="Multiple contacts have the pending creation marker. Resolve them in Google Contacts."; return; }
            match=it;
        }
        if (match!=remote.end()) {
            m.source_id=match->source_id; m.resource=match->resource; m.pending=0; m.marker.clear();
        } else if (m.pending==2) {
            r.problem="A contact creation is awaiting confirmation. Sync again after Google updates, or use Retry unconfirmed creations on the Contacts page.";
            return;
        }
    }
    size_t bytes=0;
    for (const auto &c:remote) bytes+=c.json.size();
    // Confirm missing identities individually: snapshot propagation delay or
    // changed merged resource names must never remove a Palm contact.
    for (auto &m:map) {
        if (m.pending) continue;
        auto found=std::find_if(remote.begin(),remote.end(),[&](const GoogleContact &c) { return c.source_id==m.source_id; });
        if (found!=remote.end()) { m.resource=found->resource; continue; }
        GoogleContact c; bool exists;
        if (!google.get(m.resource,c,exists)) { r.problem=google.error(); return; }
        if (exists && c.source_id!=m.source_id) { r.problem="A linked contact's Google identity changed unexpectedly"; return; }
        if (!exists) { c.source_id=m.source_id; c.resource=m.resource; c.deleted=true; }
        bytes+=c.json.size(); remote.push_back(c);
    }
    if (bytes>64000) { r.problem="Tracked Contacts exceed the sync memory limit"; return; }
    if (!hotsync_open_conduit()) { r.problem="Could not open the Contacts conduit"; r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK; return; }
    uint8_t db;
    if (!hotsync_open_db("AddressDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE,&db)) {
        r.problem="Could not open AddressDB"; r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK; return;
    }
    auto work=[&]() {
        std::vector<LocalContact> local; bool complete=false;
        for (unsigned index=0;index<=65535;index++) {
            HotSync_Record rec;
            if (!hotsync_read_record_by_index(db,index,&rec)) {
                if (hotsync_error()==HOTSYNC_DLP_NOT_FOUND) { complete=true; break; }
                r.problem="Could not read Address Book"; return;
            }
            bool linked=std::any_of(map.begin(),map.end(),[&](const ContactSyncMap &m) { return m.palm_id==rec.id; });
            if (!linked && (rec.attr&(HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED))) { r.skipped++; continue; }
            LocalContact c; c.id=rec.id; c.attr=rec.attr; c.category=rec.category;
            if (!(rec.attr&(HOTSYNC_ATTR_DELETED|HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED))) {
                std::string problem;
                if (rec.truncated || !contact_unpack_record(rec.data,rec.len,c.contact,&problem)) {
                    char context[112];
                    snprintf(context,sizeof(context),"Address Book record %u (ID %06lx, %u bytes): ",index,
                        (unsigned long)rec.id,(unsigned)rec.len);
                    r.problem=context;
                    r.problem+=rec.truncated ? "HotSync response was truncated" : problem;
                    return;
                }
                bytes+=rec.len;
                if (bytes>64000) { r.problem="Contacts and Address Book exceed the sync memory limit"; return; }
            }
            if (local.size()>=setup.max_contacts) { r.problem="Palm Address Book exceeds the configured contact limit"; return; }
            local.push_back(c);
        }
        if (!complete) { r.problem="Address Book has too many records to scan"; return; }
        map.erase(std::remove_if(map.begin(),map.end(),[&](const ContactSyncMap &m) {
            return std::none_of(local.begin(),local.end(),[&](const LocalContact &c) { return c.id==m.palm_id; });
        }),map.end());
        size_t projected=std::count_if(remote.begin(),remote.end(),[](const GoogleContact &c) { return !c.deleted; });
        for (const auto &c:local) if (!(c.attr&(HOTSYNC_ATTR_DELETED|HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED))) {
            auto m=std::find_if(map.begin(),map.end(),[&](const ContactSyncMap &m) { return m.palm_id==c.id; });
            if (m==map.end() || m->pending) projected++;
        }
        if (projected>setup.max_contacts) { r.problem="Combined Address Book exceeds the configured contact limit"; return; }
        for (auto &c:local) {
            auto m=std::find_if(map.begin(),map.end(),[&](const ContactSyncMap &m) { return m.palm_id==c.id; });
            if (c.attr&(HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED)) { r.skipped++; continue; }
            auto g=remote.end();
            if (m!=map.end() && !m->pending)
                g=std::find_if(remote.begin(),remote.end(),[&](const GoogleContact &g) { return g.source_id==m->source_id; });
            if (c.attr&HOTSYNC_ATTR_DELETED) {
                if (g!=remote.end() && !g->deleted) {
                    if (!google.remove(*g)) { r.problem=google.error(); return; }
                    g->deleted=true;
                }
                if (!hotsync_delete_record(db,c.id)) { r.problem="Could not remove a deleted Palm contact"; return; }
                if (m!=map.end()) map.erase(m);
                if (!checkpoint()) return;
                r.deleted++; continue;
            }
            bool create=m==map.end() || m->pending;
            if (!create && (g==remote.end() || g->deleted)) {
                if (!hotsync_delete_record(db,c.id)) { r.problem="Could not remove a deleted Google contact from Palm"; return; }
                map.erase(m); if (!checkpoint()) return; r.deleted++; continue;
            }
            PalmContact cloud;
            if (!create && !google.display(*g,charset,cloud)) { r.problem=google.error(); return; }
            auto baseline=contact_fields(c.contact);
            std::array<bool,CONTACT_SYNC_FIELDS> changes; bool changed=create;
            for (unsigned n=0;n<changes.size();n++) { changes[n]=create || baseline[n]!=m->fields[n]; changed=changed||changes[n]; }
            // People fields lack immutable per-number IDs. A concurrent slot
            // change is ambiguous: preserve both sides and ask for resolution.
            if (!create && changes[4] && contact_fields(cloud)[4]!=m->fields[4]) {
                r.problem="Phone/email slots changed on both sides. Resolve this contact in Google Contacts or restore its Palm slots, then sync."; return;
            }
            GoogleContact saved;
            if (changed) {
                if (create) {
                    if (m==map.end()) {
                        if (map.size()>=setup.max_contacts) { r.problem="Contacts sync map is full"; return; }
                        char marker[48]; snprintf(marker,sizeof(marker),"p%08lx%08lx%08lx%08lx",(unsigned long)palm,
                            (unsigned long)c.id,(unsigned long)host.random(),(unsigned long)host.random());
                        ContactSyncMap entry; entry.palm_id=c.id; entry.marker=marker; entry.pending=1;
                        entry.fields=baseline; map.push_back(entry); m=map.end()-1;
                        if (!checkpoint()) return;
                    }
                    m->pending=2; m->fields=baseline;
                    if (!checkpoint()) return;
                }
                if (!google.save(create ? nullptr : &*g,create ? m->marker : "",c.contact,changes,charset,saved)) {
                    std::string problem=google.error();
                    if (create && google.create_rejected()) { m->pending=1; if (!checkpoint()) return; }
                    r.problem=problem; return;
                }
                m->source_id=saved.source_id; m->resource=saved.resource; m->pending=0; m->marker.clear();
                if (!checkpoint()) return;
                r.uploaded++;
            } else saved=*g;
            PalmContact target;
            if (!google.display(saved,charset,target)) { r.problem=google.error(); return; }
            target.show_phone=c.contact.show_phone;
            // Empty slot label preferences belong to the Palm UI.
            for (unsigned n=0;n<5;n++) if (target.text[CONTACT_PHONE1+n].empty()) target.labels[n]=c.contact.labels[n];
            uint32_t id=c.id;
            if (contact_fields(target)!=baseline || (c.attr&HOTSYNC_ATTR_DIRTY)) {
                if (!write_palm(db,c,target,id)) { r.problem="Could not write a Palm contact"; return; }
                r.downloaded++;
            }
            m->fields=contact_fields(target); if (!checkpoint()) return;
        }
        for (const auto &g:remote) {
            if (g.deleted || std::any_of(map.begin(),map.end(),[&](const ContactSyncMap &m) { return m.source_id==g.source_id; })) continue;
            if (map.size()>=setup.max_contacts) { r.problem="Contacts sync map is full"; return; }
            PalmContact p;
            if (!google.display(g,charset,p)) { r.problem=google.error(); return; }
            LocalContact fresh={}; uint32_t id;
            if (!write_palm(db,fresh,p,id)) { r.problem="Could not add a Palm contact"; return; }
            ContactSyncMap m; m.palm_id=id; m.source_id=g.source_id; m.resource=g.resource; m.fields=contact_fields(p);
            map.push_back(m);
            if (!checkpoint()) { hotsync_delete_record(db,id); return; }
            r.downloaded++;
        }
        // Persist pruned mappings even when both books are empty.
        checkpoint();
    };
    work(); r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK;
    if (!r.link_lost && !hotsync_close_db(db) && r.problem.empty()) r.problem="Could not close AddressDB";
    r.link_lost=r.link_lost||hotsync_error()==HOTSYNC_ERR_LINK;
}
std::string contactsync_summary(const ContactSyncResult &r) {
    std::string s=std::to_string(r.uploaded)+" uploaded, "+std::to_string(r.downloaded)+" downloaded, "+std::to_string(r.deleted)+" deleted";
    if (r.skipped) s+=", "+std::to_string(r.skipped)+" private/archived skipped";
    if (!r.problem.empty()) s+=". "+r.problem;
    return s;
}
