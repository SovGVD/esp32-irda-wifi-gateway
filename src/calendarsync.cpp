#include "calendarsync.h"
#include "hotsync.h"
#include <algorithm>
#include <cstdio>

static uint32_t hash(const std::string &s) {
    uint32_t h=2166136261u;
    for (unsigned char c:s) { h^=c; h*=16777619u; }
    return h;
}
static std::array<uint32_t,5> fields(const PalmAppointment &a) {
    std::string recurrence=std::to_string(a.repeat)+"/"+std::to_string(a.interval)+"/"+
        std::to_string(a.repeat_on)+"/"+std::to_string(a.week_start)+"/"+a.repeat_until;
    for (const auto &date:a.exceptions) recurrence+="/"+date;
    return {{hash(a.title),hash(a.notes),hash(a.date+"/"+std::to_string(a.start_minute)+"/"+
             std::to_string(a.end_minute)),hash(recurrence),hash(std::to_string(a.alarm_minutes))}};
}
static void encode32(std::string &out,uint32_t value) {
    for (int n=3;n>=0;--n) out+=(char)(value>>(n*8));
}
static uint32_t decode32(const std::string &data,size_t &at) {
    uint32_t n=0;
    for (unsigned i=0;i<4;i++) n=(n<<8)|(uint8_t)data[at++];
    return n;
}
bool calendarsync_encode_map(const std::vector<CalendarSyncMap> &map,std::string &out) {
    out="CAL1";
    if (map.size()>65535) return false;
    out+=(char)(map.size()>>8); out+=(char)map.size();
    for (const auto &m:map) {
        if (!m.palm_id || m.palm_id>0xffffff || m.google_id.empty() || m.google_id.size()>1024) return false;
        encode32(out,m.palm_id);
        out+=(char)(m.pending_create|(m.suppressed<<1));
        out+=(char)(m.google_id.size()>>8); out+=(char)m.google_id.size();
        for (auto field:m.fields) encode32(out,field);
        out+=m.google_id;
    }
    return true;
}
bool calendarsync_decode_map(const std::string &data,unsigned max,std::vector<CalendarSyncMap> &map) {
    map.clear();
    if (data.size()<6 || data.compare(0,4,"CAL1")) return false;
    unsigned count=((uint8_t)data[4]<<8)|(uint8_t)data[5];
    if (count>max) return false;
    size_t at=6;
    std::vector<CalendarSyncMap> decoded;
    for (unsigned n=0;n<count;n++) {
        if (data.size()-at<27) return false;
        CalendarSyncMap m;
        m.palm_id=decode32(data,at);
        uint8_t flags=(uint8_t)data[at++];
        unsigned length=((uint8_t)data[at]<<8)|(uint8_t)data[at+1]; at+=2;
        if (!m.palm_id || m.palm_id>0xffffff || flags>3 || !length || length>1024 ||
            data.size()-at<20+length) return false;
        m.pending_create=flags&1; m.suppressed=flags&2;
        for (auto &field:m.fields) field=decode32(data,at);
        m.google_id=data.substr(at,length); at+=length;
        if (m.google_id.find('\0')!=std::string::npos) return false;
        for (const auto &old:decoded) if (old.palm_id==m.palm_id || old.google_id==m.google_id) return false;
        decoded.push_back(m);
    }
    if (at!=data.size()) return false;
    map.swap(decoded);
    return true;
}

struct LocalAppointment {
    uint32_t id;
    uint8_t attr,category;
    PalmAppointment appointment;
};
static bool write_palm(uint8_t db,uint32_t id,uint8_t attr,uint8_t category,const PalmAppointment &a,uint32_t &saved) {
    size_t room; uint8_t *p=hotsync_record_buffer(&room);
    size_t len=calendar_pack_record(a,p,room);
    return len && hotsync_write_record(db,id,attr&~HOTSYNC_ATTR_DIRTY,category,len,&saved);
}
void calendarsync_run(const GoogleCalendarSetup &setup,PalmCharset charset,CalendarSyncHost &host,CalendarSyncResult &r) {
    r=CalendarSyncResult();
    if (!setup.max_events || setup.max_events>65534 || !setup.future_days || setup.past_days>3650 || setup.future_days>3650) {
        r.problem="Invalid Calendar sync limits"; return;
    }
    if (!host.online()) { r.problem="Wi-Fi is not connected"; return; }
    int64_t now=host.now();
    if (now<=0) { r.problem="The network clock is not set yet. Wait and sync again."; return; }
    std::vector<CalendarSyncMap> map;
    if (!host.load_map(map) || map.size()>setup.max_events) { r.problem="Calendar sync state could not be loaded"; return; }
    std::string token;
    if (!host.access_token(token,r.problem)) return;
    GoogleCalendarClient google(host.http(),setup,token);
    GoogleCalendarInfo info;
    std::vector<GoogleCalendarEvent> remote;
    if (!google.open(info) || !google.list(now,remote)) { r.problem=google.error(); return; }
    uint32_t palm_user;
    if (!hotsync_read_user_id(&palm_user)) { r.problem="Could not identify the Palm"; r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK; return; }
    if (!host.bind_calendar(info.id,palm_user)) { r.problem="Calendar or Palm identity does not match the saved sync state"; return; }
    if (!hotsync_open_conduit()) { r.problem="Could not open the Calendar conduit"; r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK; return; }
    uint8_t db;
    if (!hotsync_open_db("DatebookDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE,&db)) {
        r.problem="Could not open DatebookDB"; r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK; return;
    }
    auto skip=[&](const std::string &reason) {
        r.skipped++;
        if (r.skipped_reason.empty()) r.skipped_reason=reason;
    };
    auto checkpoint=[&]() {
        if (host.store_map(map)) return true;
        r.problem="Could not save Calendar sync state. No further events were changed.";
        return false;
    };
    auto display=[&](const GoogleCalendarEvent &g,PalmAppointment &a) {
        std::string reason;
        if (!google.display(g,a,reason)) {
            if (!google.error().empty()) r.problem=google.error();
            else skip(reason);
            return false;
        }
        a.title=palm_from_utf8(g.title,charset,CALENDAR_TITLE_MAX);
        a.notes=palm_from_utf8(g.notes,charset,CALENDAR_NOTE_MAX);
        return true;
    };
    auto unsupported_series=[&](const GoogleCalendarEvent &g) {
        // A modified occurrence cannot fit inside a Palm recurrence rule.
        // Skip its series too, rather than showing the original time twice.
        return !g.recurring_id.empty() || std::any_of(remote.begin(),remote.end(),
            [&](const GoogleCalendarEvent &instance) { return instance.recurring_id==g.id; });
    };
    auto work=[&]() {
        std::vector<LocalAppointment> local;
        size_t bytes=0;
        bool complete=false;
        const std::string first_date=calendar_utc(now-(int64_t)setup.past_days*86400).substr(0,10);
        const std::string last_date=calendar_utc(now+(int64_t)setup.future_days*86400).substr(0,10);
        for (unsigned index=0;index<=65535;index++) {
            HotSync_Record rec;
            if (!hotsync_read_record_by_index(db,index,&rec)) {
                if (hotsync_error()==HOTSYNC_DLP_NOT_FOUND) { complete=true; break; }
                r.problem="Could not read Date Book"; return;
            }
            LocalAppointment a; a.id=rec.id; a.attr=rec.attr; a.category=rec.category;
            bool linked=std::any_of(map.begin(),map.end(),[&](const CalendarSyncMap &m) { return m.palm_id==a.id; });
            if (!linked && (rec.attr&(HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED))) continue;
            if (!(rec.attr&(HOTSYNC_ATTR_DELETED|HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED))) {
                if (rec.truncated || !calendar_unpack_record(rec.data,rec.len,a.appointment)) {
                    r.problem="A Date Book record is too large or invalid"; return;
                }
                if (!linked && (a.appointment.date>last_date ||
                    (a.appointment.repeat==CALENDAR_ONCE && a.appointment.date<first_date) ||
                    (!a.appointment.repeat_until.empty() && a.appointment.repeat_until<first_date))) {
                    skip("appointment outside the chosen date window");
                    continue;
                }
                bytes+=rec.len;
                if (bytes>48000) { r.problem="Date Book text exceeds the sync memory limit"; return; }
            }
            if (local.size()>=setup.max_events) { r.problem="Active Date Book events exceed the configured limit"; return; }
            local.push_back(a);
        }
        if (!complete) { r.problem="Date Book has too many records to scan"; return; }
        // Missing local records may have been purged by a desktop; only
        // explicit Palm deletion tombstones are sent to Google.
        map.erase(std::remove_if(map.begin(),map.end(),[&](const CalendarSyncMap &m) {
            return !m.suppressed && std::none_of(local.begin(),local.end(),
                [&](const LocalAppointment &a) { return a.id==m.palm_id; });
        }),map.end());
        for (const auto &event:remote) bytes+=event.title.size()+event.notes.size();
        if (bytes>48000) { r.problem="Calendar and Date Book text exceed the sync memory limit"; return; }
        // Fetch every tracked event individually if it is absent from the
        // rolling window. Absence from a window never proves deletion.
        for (const auto &m:map) {
            if (std::any_of(remote.begin(),remote.end(),[&](const GoogleCalendarEvent &e) { return e.id==m.google_id; })) continue;
            GoogleCalendarEvent event; bool found;
            if (!google.get(m.google_id,event,found)) { r.problem=google.error(); return; }
            if (found) {
                if (bytes+event.title.size()+event.notes.size()>48000) { r.problem="Tracked Calendar events exceed the sync memory limit"; return; }
                bytes+=event.title.size()+event.notes.size();
                remote.push_back(event);
            } else {
                event.id=m.google_id; event.status="cancelled"; remote.push_back(event);
            }
        }
        std::vector<std::string> handled;
        unsigned added=0;
        for (auto &a:local) {
            auto it=std::find_if(map.begin(),map.end(),[&](const CalendarSyncMap &m) { return m.palm_id==a.id; });
            bool fresh=it==map.end();
            if (a.attr&(HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_ARCHIVED)) continue;
            auto g=remote.end();
            if (!fresh) g=std::find_if(remote.begin(),remote.end(),[&](const GoogleCalendarEvent &e) { return e.id==it->google_id; });
            if (a.attr&HOTSYNC_ATTR_DELETED) {
                if (!fresh && g!=remote.end() && g->status!="cancelled" && !g->writable) {
                    it->suppressed=true;
                    if (!checkpoint()) return;
                    if (!hotsync_delete_record(db,a.id)) { r.problem="Could not remove a local invitation"; return; }
                    skip("invitation removed from Palm only"); continue;
                }
                if (!fresh) {
                    if (g!=remote.end() && g->status!="cancelled" &&
                        !google.remove(it->google_id,g->etag)) { r.problem=google.error(); return; }
                    handled.push_back(it->google_id);
                    map.erase(it);
                    if (!checkpoint()) return;
                }
                if (!hotsync_delete_record(db,a.id)) { r.problem="Could not remove a deleted appointment"; return; }
                r.deleted++; continue;
            }
            if (!fresh && it->suppressed) continue;
            bool pending=!fresh && it->pending_create;
            if (!fresh && !pending && (g==remote.end() || g->status=="cancelled")) {
                handled.push_back(it->google_id);
                if (!hotsync_delete_record(db,a.id)) { r.problem="Could not remove a cancelled appointment"; return; }
                map.erase(it);
                if (!checkpoint()) return;
                r.deleted++; continue;
            }
            if (!pending && g!=remote.end() && g->status!="cancelled" && unsupported_series(*g)) {
                skip("series with modified or cancelled occurrences"); continue;
            }
            PalmAppointment cloud;
            // A pending creation resends every Palm field and verifies the
            // saved private marker. Its intermediate recurrence cutoff need
            // not have a complete Palm projection yet.
            if (!pending && g!=remote.end() && g->status!="cancelled" && !display(*g,cloud)) {
                if (!r.problem.empty()) return;
                continue;
            }
            auto baseline=fields(a.appointment);
            std::array<bool,5> changes;
            bool changed=fresh||pending;
            for (unsigned n=0;n<5;n++) { changes[n]=fresh||pending||baseline[n]!=it->fields[n]; changed=changed||changes[n]; }
            if (a.appointment.repeat && changes[2]) changes[3]=true;
            if (changed && g!=remote.end() && g->status!="cancelled" && !g->writable) {
                skip("read-only invitation edited on Palm"); continue;
            }
            GoogleCalendarEvent saved;
            PalmAppointment target=a.appointment;
            if (changed) {
                if (fresh) {
                    if (map.size()>=setup.max_events) { r.problem="Calendar sync map is full"; return; }
                    char id[48];
                    snprintf(id,sizeof(id),"p%08lx%08lx%08lx%08lx",(unsigned long)palm_user,(unsigned long)a.id,
                             (unsigned long)host.random(),(unsigned long)host.random());
                    CalendarSyncMap entry; entry.palm_id=a.id; entry.google_id=id; entry.pending_create=true;
                    map.push_back(entry); it=map.end()-1; pending=true;
                    // Persist the chosen Google ID before POST: a retry uses
                    // the same ID and verifies its private Palm marker.
                    if (!checkpoint()) return;
                }
                // Merge only locally edited fields with the current cloud
                // projection. Unchanged Google text and metadata are omitted.
                PalmAppointment merged=a.appointment;
                if (g!=remote.end() && g->status!="cancelled") {
                    if (!changes[0]) merged.title=cloud.title;
                    if (!changes[1]) merged.notes=cloud.notes;
                    if (!changes[2]) {
                        merged.date=cloud.date; merged.start_minute=cloud.start_minute; merged.end_minute=cloud.end_minute;
                    }
                    if (!changes[3]) {
                        merged.repeat=cloud.repeat; merged.interval=cloud.interval; merged.repeat_on=cloud.repeat_on;
                        merged.week_start=cloud.week_start; merged.repeat_until=cloud.repeat_until; merged.exceptions=cloud.exceptions;
                    }
                    if (!changes[4]) merged.alarm_minutes=cloud.alarm_minutes;
                }
                if (changes[0]) merged.title=palm_to_utf8(merged.title,charset);
                if (changes[1]) merged.notes=palm_to_utf8(merged.notes,charset);
                std::string marker=std::to_string(palm_user)+":"+std::to_string(a.id);
                if (!google.save(it->google_id,pending,marker,
                                 g!=remote.end() ? g->etag : "",merged,changes,
                                 g!=remote.end() ? &*g : nullptr,saved)) { r.problem=google.error(); return; }
                // A local title may be longer in UTF-8; display converts it
                // back to Palm bytes before packing and recording baselines.
                if (!display(saved,target)) {
                    if (r.problem.empty()) r.problem="A saved event cannot be represented by Palm Date Book";
                    return;
                }
                it->pending_create=false;
                if (!checkpoint()) return;
                r.uploaded++;
            } else {
                saved=*g; target=cloud;
            }
            handled.push_back(saved.id);
            if (fields(target)!=baseline || (a.attr&HOTSYNC_ATTR_DIRTY)) {
                uint32_t id;
                if (!write_palm(db,a.id,a.attr,a.category,target,id)) { r.problem="Could not write an appointment"; return; }
                r.downloaded++;
            }
            it->fields=fields(target);
            if (!checkpoint()) return;
        }
        for (const auto &g:remote) {
            if (g.status=="cancelled" || std::find(handled.begin(),handled.end(),g.id)!=handled.end() ||
                std::any_of(map.begin(),map.end(),[&](const CalendarSyncMap &m) { return m.google_id==g.id; })) continue;
            if (unsupported_series(g)) { skip("series with modified or cancelled occurrences"); continue; }
            PalmAppointment a;
            if (!display(g,a)) { if (!r.problem.empty()) return; continue; }
            if (map.size()>=setup.max_events || local.size()+added>=setup.max_events) {
                r.problem="Calendar exceeds the configured Palm event limit"; return;
            }
            uint32_t id;
            if (!write_palm(db,0,0,0,a,id)) { r.problem="Could not add an appointment"; return; }
            CalendarSyncMap m; m.palm_id=id; m.google_id=g.id; m.fields=fields(a);
            map.push_back(m);
            if (!checkpoint()) {
                // A known storage failure can be compensated while the Palm
                // is still connected. Power loss in this gap is not atomic.
                hotsync_delete_record(db,id);
                return;
            }
            r.downloaded++; added++;
        }
    };
    work();
    r.link_lost=hotsync_error()==HOTSYNC_ERR_LINK;
    if (!r.link_lost && !hotsync_close_db(db) && r.problem.empty()) r.problem="Could not close DatebookDB";
    r.link_lost=r.link_lost||hotsync_error()==HOTSYNC_ERR_LINK;
}
std::string calendarsync_summary(const CalendarSyncResult &r) {
    std::string s=std::to_string(r.uploaded)+" uploaded, "+std::to_string(r.downloaded)+
        " downloaded, "+std::to_string(r.deleted)+" deleted";
    if (r.skipped) s+=", "+std::to_string(r.skipped)+" skipped ("+r.skipped_reason+")";
    if (!r.problem.empty()) s+=". "+r.problem;
    return s;
}
