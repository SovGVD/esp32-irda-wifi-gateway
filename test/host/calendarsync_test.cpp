// Date Book sync using the real HotSync stack and an in-memory Calendar API.
#include "calendarsync.h"
#include "hotsync.h"
#include "mailoauth.h"
#include "cJSON.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static int fd=-1;
static int port_read(uint8_t *p,size_t max,uint32_t ms) {
    pollfd wait={fd,POLLIN,0};
    if (poll(&wait,1,ms)<=0) return 0;
    ssize_t n=read(fd,p,max); return n>0 ? (int)n : -1;
}
static bool port_write(const uint8_t *p,size_t n) { return write(fd,p,n)==(ssize_t)n; }
static uint32_t port_millis() { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000; }
static std::string field(cJSON *j,const char *name) {
    auto *v=cJSON_GetObjectItemCaseSensitive(j,name); return cJSON_IsString(v) ? v->valuestring : "";
}
static std::string json(cJSON *j) {
    char *p=cJSON_PrintUnformatted(j); assert(p);
    std::string s(p); free(p); cJSON_Delete(j); return s;
}
static std::string decode(std::string s) {
    std::string out;
    for (size_t n=0;n<s.size();n++) {
        if (s[n]=='%' && n+2<s.size()) { out+=(char)strtol(s.substr(n+1,2).c_str(),nullptr,16); n+=2; }
        else out+=s[n];
    }
    return out;
}
static GoogleCalendarEvent event(const std::string &id,const std::string &date,bool all_day=false) {
    GoogleCalendarEvent e; e.id=id; e.etag="\"1\""; e.title=id; e.status="confirmed"; e.event_type="default";
    e.all_day=all_day; e.zone="Pacific/Auckland";
    e.start=all_day ? date : date+"T09:00:00+13:00";
    e.end=all_day ? calendar_add_days(date,1) : date+"T10:00:00+13:00";
    return e;
}
static cJSON *event_json(const GoogleCalendarEvent &e) {
    cJSON *j=cJSON_CreateObject(); cJSON_AddStringToObject(j,"kind","calendar#event");
    for (const auto &f:{std::make_pair("id",e.id),{"etag",e.etag},{"summary",e.title},
        {"description",e.notes},{"status",e.status},{"eventType",e.event_type}}) cJSON_AddStringToObject(j,f.first,f.second.c_str());
    if (e.status=="cancelled") return j;
    for (const auto &f:{std::make_pair("start",e.start),{"end",e.end}}) {
        cJSON *v=cJSON_AddObjectToObject(j,f.first);
        cJSON_AddStringToObject(v,e.all_day ? "date" : "dateTime",f.second.c_str());
        if (!e.all_day) cJSON_AddStringToObject(v,"timeZone",e.zone.c_str());
    }
    cJSON *organizer=cJSON_AddObjectToObject(j,"organizer");
    cJSON_AddBoolToObject(organizer,"self",e.writable);
    if (!e.recurring_id.empty()) {
        cJSON_AddStringToObject(j,"recurringEventId",e.recurring_id.c_str());
        cJSON_AddStringToObject(cJSON_AddObjectToObject(j,"originalStartTime"),"dateTime",
                               (e.original_date+"T09:00:00+13:00").c_str());
    }
    if (!e.palm_marker.empty())
        cJSON_AddStringToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(j,"extendedProperties"),"private"),
                               "palmGateway",e.palm_marker.c_str());
    if (!e.recurrence.empty()) {
        cJSON *a=cJSON_AddArrayToObject(j,"recurrence");
        for (const auto &r:e.recurrence) cJSON_AddItemToArray(a,cJSON_CreateString(r.c_str()));
    }
    cJSON *rem=cJSON_AddObjectToObject(j,"reminders");
    cJSON_AddBoolToObject(rem,"useDefault",e.use_default_reminders);
    cJSON *a=cJSON_AddArrayToObject(rem,"overrides");
    for (const auto &r:e.reminders) {
        cJSON *item=cJSON_CreateObject(); cJSON_AddStringToObject(item,"method",r.first.c_str());
        cJSON_AddNumberToObject(item,"minutes",r.second); cJSON_AddItemToArray(a,item);
    }
    // Fields the Palm does not model must survive partial updates.
    cJSON_AddStringToObject(j,"location","Boardroom");
    cJSON_AddArrayToObject(j,"attendees");
    return j;
}
class Server : public GoogleHttp {
public:
    std::map<std::string,GoogleCalendarEvent> events;
    std::vector<GoogleCalendarEvent> instances;
    std::string calendar_id="owner@example.com",zone="Pacific/Auckland",role="owner";
    std::string last_patch,last_if_match;
    unsigned posts=0,patches=0,deletes=0,gets=0;
    bool pages=true,bad_page=false,malformed=false,auth_failure=false,fail_patch=false,
         conflict=false,lose_insert=false,hide_from_window=false,fail_recurrence_patch_once=false;
    int request(const std::string &url,const char *method,const std::string &auth,
                const char *type,const std::string &body,std::string &reply) override {
        return request_if_match(url,method,auth,type,body,"",reply);
    }
    int request_if_match(const std::string &url,const char *method,const std::string &auth,
                        const char *,const std::string &body,const std::string &etag,std::string &reply) override {
        assert(auth=="Bearer token"); assert(url.find("oauth2.googleapis.com")==std::string::npos);
        if (auth_failure) { reply="{}"; return 403; }
        last_if_match=etag;
        if (url.find("/users/me/calendarList/")!=std::string::npos) {
            cJSON *j=cJSON_CreateObject();
            cJSON_AddStringToObject(j,"kind","calendar#calendarListEntry");
            cJSON_AddStringToObject(j,"id",calendar_id.c_str()); cJSON_AddStringToObject(j,"timeZone",zone.c_str());
            cJSON_AddStringToObject(j,"accessRole",role.c_str());
            cJSON *a=cJSON_AddArrayToObject(j,"defaultReminders"); cJSON *r=cJSON_CreateObject();
            cJSON_AddStringToObject(r,"method","popup"); cJSON_AddNumberToObject(r,"minutes",15);
            cJSON_AddItemToArray(a,r); reply=json(j); return 200;
        }
        auto at=url.find("/events"); assert(at!=std::string::npos);
        std::string suffix=url.substr(at+7);
        std::string path=suffix.substr(0,suffix.find('?'));
        if (path.empty() && !strcmp(method,"GET")) {
            assert(url.find("singleEvents=false")!=std::string::npos && url.find("showDeleted=true")!=std::string::npos);
            assert(url.find("timeMin=")!=std::string::npos && url.find("timeMax=")!=std::string::npos);
            assert(url.find("timeZone=Pacific%2FAuckland")!=std::string::npos);
            bool next=url.find("pageToken=")!=std::string::npos;
            if (next) assert(url.find("pageToken=next%2B%26")!=std::string::npos);
            if (bad_page && next) { reply="{}"; return 503; }
            if (malformed) { reply="{}"; return 200; }
            cJSON *j=cJSON_CreateObject(); cJSON_AddStringToObject(j,"kind","calendar#events");
            cJSON *a=cJSON_AddArrayToObject(j,"items"); unsigned n=0;
            for (const auto &e:events) {
                if (!hide_from_window && (!pages || (next ? n>=1 : n<1))) cJSON_AddItemToArray(a,event_json(e.second));
                n++;
            }
            if (pages && !next) cJSON_AddStringToObject(j,"nextPageToken","next+&");
            reply=json(j); return 200;
        }
        if (path.find("/instances")!=std::string::npos) {
            cJSON *j=cJSON_CreateObject(); cJSON_AddStringToObject(j,"kind","calendar#events");
            cJSON *a=cJSON_AddArrayToObject(j,"items");
            for (const auto &e:instances) cJSON_AddItemToArray(a,event_json(e));
            reply=json(j); return 200;
        }
        std::string id=path.empty() ? "" : decode(path.substr(1));
        if (!strcmp(method,"GET")) {
            gets++;
            if (!events.count(id)) { reply="{}"; return 404; }
            reply=json(event_json(events[id])); return 200;
        }
        assert(url.find("sendUpdates=none")!=std::string::npos);
        if (!strcmp(method,"DELETE")) {
            deletes++;
            if (!events.count(id)) return 404;
            if (conflict || (!etag.empty() && etag!=events[id].etag)) return 412;
            events[id].status="cancelled"; reply.clear(); return 204;
        }
        cJSON *j=cJSON_Parse(body.c_str()); assert(j);
        bool post=!strcmp(method,"POST");
        if (post) {
            posts++; id=field(j,"id"); assert(!id.empty());
            if (events.count(id)) { cJSON_Delete(j); reply="{}"; return 409; }
            events[id]=event(id,"2026-10-14");
        } else {
            patches++; last_patch=body;
            if (fail_recurrence_patch_once && cJSON_GetObjectItemCaseSensitive(j,"recurrence")) {
                fail_recurrence_patch_once=false; cJSON_Delete(j); reply="{}"; return 503;
            }
            if (fail_patch || conflict || etag!=events[id].etag) {
                cJSON_Delete(j); reply="{}"; return conflict ? 412 : fail_patch ? 503 : 412;
            }
        }
        auto &e=events[id];
        if (cJSON_GetObjectItemCaseSensitive(j,"summary")) e.title=field(j,"summary");
        if (cJSON_GetObjectItemCaseSensitive(j,"description")) e.notes=field(j,"description");
        auto *start=cJSON_GetObjectItemCaseSensitive(j,"start"),*end=cJSON_GetObjectItemCaseSensitive(j,"end");
        if (start) {
            e.all_day=!field(start,"date").empty();
            e.start=field(start,e.all_day ? "date" : "dateTime");
            e.end=field(end,e.all_day ? "date" : "dateTime");
            if (!e.all_day) { e.zone=field(start,"timeZone"); e.start+="+13:00"; e.end+="+13:00"; }
        }
        auto *rules=cJSON_GetObjectItemCaseSensitive(j,"recurrence");
        if (rules) {
            e.recurrence.clear(); const cJSON *r;
            cJSON_ArrayForEach(r,rules) e.recurrence.emplace_back(r->valuestring);
        }
        auto *rem=cJSON_GetObjectItemCaseSensitive(j,"reminders");
        if (rem) {
            e.use_default_reminders=false; e.reminders.clear();
            const cJSON *r;
            cJSON_ArrayForEach(r,cJSON_GetObjectItemCaseSensitive(rem,"overrides"))
                e.reminders.emplace_back(field((cJSON *)r,"method"),cJSON_GetObjectItemCaseSensitive(r,"minutes")->valueint);
        }
        auto *props=cJSON_GetObjectItemCaseSensitive(j,"extendedProperties");
        if (props) e.palm_marker=field(cJSON_GetObjectItemCaseSensitive(props,"private"),"palmGateway");
        e.etag="\""+std::to_string(posts+patches+2)+"\""; reply=json(event_json(e)); cJSON_Delete(j);
        if (post && lose_insert) { lose_insert=false; reply.clear(); return -1; }
        return post ? 201 : 200;
    }
};
class Host : public CalendarSyncHost {
public:
    Server server;
    std::vector<CalendarSyncMap> map;
    std::string bound;
    bool is_online=true,writable=true,bad_auth=false;
    uint32_t next_random=1;
    GoogleHttp &http() override { return server; }
    bool online() override { return is_online; }
    int64_t now() override { return 1791936000; }
    uint32_t random() override { return next_random++; }
    bool access_token(std::string &token,std::string &problem) override {
        if (bad_auth) { problem="Google sign-in expired"; return false; }
        token="token"; return true;
    }
    bool bind_calendar(const std::string &id,uint32_t palm) override {
        assert(palm==1);
        if (!bound.empty() && bound!=id) return false;
        bound=id; return true;
    }
    bool load_map(std::vector<CalendarSyncMap> &out) override {
        std::string data;
        return calendarsync_encode_map(map,data) && calendarsync_decode_map(data,64,out);
    }
    bool store_map(const std::vector<CalendarSyncMap> &in) override {
        if (!writable) return false;
        std::string data;
        return calendarsync_encode_map(in,data) && calendarsync_decode_map(data,64,map);
    }
};
static uint32_t put(const PalmAppointment &a,uint32_t id=0,uint8_t attr=HOTSYNC_ATTR_DIRTY,uint8_t category=0) {
    uint8_t db; assert(hotsync_open_db("DatebookDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE,&db));
    size_t max; uint8_t *p=hotsync_record_buffer(&max); size_t len=calendar_pack_record(a,p,max); assert(len);
    uint32_t saved; assert(hotsync_write_record(db,id,attr,category,len,&saved));
    assert(hotsync_close_db(db)); return saved;
}
static PalmAppointment get(uint32_t id,uint8_t *category=nullptr) {
    uint8_t db; assert(hotsync_open_db("DatebookDB",HOTSYNC_MODE_READ|HOTSYNC_MODE_WRITE,&db));
    HotSync_Record record; assert(hotsync_read_record(db,id,&record));
    PalmAppointment a; assert(calendar_unpack_record(record.data,record.len,a));
    if (category) *category=record.category;
    assert(hotsync_close_db(db)); return a;
}
static uint32_t mapped(const Host &h,const std::string &id) {
    for (const auto &m:h.map) if (m.google_id==id) return m.palm_id;
    assert(false); return 0;
}
static CalendarSyncResult sync(Host &h,const GoogleCalendarSetup &s,bool success=true) {
    CalendarSyncResult r; calendarsync_run(s,PALM_WESTERN,h,r);
    printf("Calendar: %s\n",calendarsync_summary(r).c_str()); fflush(stdout);
    assert(!r.link_lost); assert(r.problem.empty()==success); return r;
}
static void codec() {
    // Independent Palm v1 fixture: 09:30-10:15, 14 Oct 2026, description only.
    const uint8_t fixture[]={9,30,10,15,0xf5,0x4e,4,0,'M','e','e','t',0};
    PalmAppointment a; assert(calendar_unpack_record(fixture,sizeof(fixture),a));
    assert(a.date=="2026-10-14" && a.title=="Meet" && a.start_minute==570 && a.end_minute==615);
    uint8_t p[CALENDAR_RECORD_MAX]; size_t n=calendar_pack_record(a,p,sizeof(p));
    assert(n==sizeof(fixture) && !memcmp(p,fixture,n));
    for (size_t len=0;len<sizeof(fixture);len++) assert(!calendar_unpack_record(fixture,len,a));
    a=PalmAppointment(); a.date="2028-02-29"; a.title="Leap day"; a.notes="line\n\n\tend ";
    a.alarm_minutes=1440; a.repeat=CALENDAR_WEEKLY; a.interval=2; a.repeat_on=3;
    a.repeat_until="2028-05-30"; a.exceptions={"2028-03-13","2028-03-27"};
    n=calendar_pack_record(a,p,sizeof(p)); assert(n);
    PalmAppointment b; assert(calendar_unpack_record(p,n,b));
    assert(b.date==a.date && b.notes==a.notes && b.alarm_minutes==1440 && b.exceptions==a.exceptions &&
           b.repeat==a.repeat && b.repeat_until==a.repeat_until);
    auto rules=calendar_recurrence(a); std::string until,reason;
    assert(calendar_read_recurrence(rules,b,until,reason) && until.empty() && b.repeat_until==a.repeat_until);
    assert(!calendar_read_recurrence({"RRULE:FREQ=DAILY;COUNT=10"},b,until,reason));
    assert(!calendar_read_recurrence({"RRULE:FREQ=MONTHLY;BYDAY=5MO"},b,until,reason));
    assert(calendar_read_recurrence({"RRULE:FREQ=MONTHLY;BYDAY=-1MO"},b,until,reason) && b.repeat_on==29);
    a.date="2027-02-29"; assert(!calendar_pack_record(a,p,sizeof(p)));
    assert(calendar_add_days("2028-02-28",1)=="2028-02-29" && calendar_add_days("2028-02-29",1)=="2028-03-01");
    int64_t epoch; assert(calendar_timestamp("2026-10-14T09:30:00+13:00",epoch));
    assert(calendar_utc(epoch)=="2026-10-13T20:30:00Z");
    assert(!calendar_timestamp("2026-02-30T09:30:00Z",epoch) && !calendar_timestamp("2026-10-14T24:00:00Z",epoch));

    // Maximum-sized Palm record fits the advertised transport buffer.
    a=PalmAppointment(); a.date="2026-10-14"; a.title=std::string(CALENDAR_TITLE_MAX,'t');
    a.notes=std::string(CALENDAR_NOTE_MAX,'n'); a.alarm_minutes=15; a.repeat=CALENDAR_DAILY;
    a.exceptions.assign(CALENDAR_EXCEPTIONS_MAX,"2026-10-15");
    n=calendar_pack_record(a,p,sizeof(p)); assert(n==sizeof(p) && calendar_unpack_record(p,n,b));

    // Flash state round trips flags, IDs and unsigned hashes, and rejects
    // malformed/truncated snapshots rather than silently starting over.
    CalendarSyncMap m; m.palm_id=0xffffff; m.google_id="google-event-id";
    m.fields={{0,1,0x80000000u,0xffffffffu,12345}}; m.pending_create=true; m.suppressed=true;
    std::string data; std::vector<CalendarSyncMap> decoded;
    assert(calendarsync_encode_map({m},data) && calendarsync_decode_map(data,64,decoded));
    assert(decoded.size()==1 && decoded[0].palm_id==m.palm_id && decoded[0].google_id==m.google_id &&
           decoded[0].fields==m.fields && decoded[0].pending_create && decoded[0].suppressed);
    for (size_t len=0;len<data.size();len++) assert(!calendarsync_decode_map(data.substr(0,len),64,decoded));
    assert(!calendarsync_decode_map(data,0,decoded) && !calendarsync_decode_map(data+"x",64,decoded));
    auto bad=data; bad[0]='X'; assert(!calendarsync_decode_map(bad,64,decoded));
    bad=data; bad[10]=4; assert(!calendarsync_decode_map(bad,64,decoded));
    assert(calendarsync_encode_map({m,m},bad) && !calendarsync_decode_map(bad,64,decoded));
    m.palm_id=0; assert(!calendarsync_encode_map({m},bad));
    assert(calendarsync_encode_map({},data) && calendarsync_decode_map(data,64,decoded) && decoded.empty());
}
static void projection() {
    Server server; GoogleCalendarSetup setup;
    GoogleCalendarClient client(server,setup,"token"); GoogleCalendarInfo info;
    assert(client.open(info));
    auto e=event("bounded","2026-04-10");
    e.start="2026-04-10T00:30:00+12:00"; e.end="2026-04-10T01:30:00+12:00";
    e.recurrence={"RRULE:FREQ=DAILY;UNTIL=20261013T113000Z"};
    auto final=event("final","2026-10-14");
    final.start="2026-10-14T00:30:00+13:00"; final.end="2026-10-14T01:30:00+13:00";
    server.instances={final}; PalmAppointment a; std::string reason;
    assert(client.display(e,a,reason) && a.repeat_until=="2026-10-14");
    // The original offset suggests Oct 13, but the actual final instance is
    // Oct 14 after DST. Never invent a cutoff when instances cannot prove it.
    server.instances.clear();
    assert(!client.display(e,a,reason) && client.error().empty() && !reason.empty());
    e.recurrence={"RRULE:FREQ=DAILY","EXDATE;TZID=Europe/London:20261014T003000"};
    assert(!client.display(e,a,reason) && reason=="recurrence exclusion uses a different time zone");
}
int main(int argc,char **argv) {
    codec(); projection(); assert(argc==2);
    fd=open(argv[1],O_RDWR|O_NOCTTY); assert(fd>=0);
    uint8_t memory[HOTSYNC_BUFFER_SIZE(CALENDAR_RECORD_MAX)];
    HotSync_Io io={port_read,port_write,port_millis}; assert(hotsync_accept(&io,memory,sizeof(memory),10000));
    Host h; GoogleCalendarSetup setup;
    auto cloud=event("cloud","2026-10-14"); cloud.title="Caf\xc3\xa9"; cloud.notes=std::string(5000,'n');
    cloud.use_default_reminders=false; cloud.reminders={{"popup",15},{"email",120}};
    h.server.events["cloud"]=cloud;
    h.server.events["holiday"]=event("holiday","2026-10-15",true);
    auto yearly=event("yearly","2026-10-16",true); yearly.recurrence={"RRULE:FREQ=YEARLY"};
    h.server.events[yearly.id]=yearly;
    auto count=event("count","2026-10-17"); count.recurrence={"RRULE:FREQ=DAILY;COUNT=10"};
    h.server.events[count.id]=count;
    auto multiple=event("multi","2026-10-18",true); multiple.end="2026-10-20";
    h.server.events[multiple.id]=multiple;
    auto invite=event("invite","2026-10-19"); invite.writable=false; h.server.events[invite.id]=invite;
    PalmAppointment local; local.date="2026-10-14"; local.start_minute=660; local.end_minute=720;
    local.title="Local"; local.notes="Palm note"; local.alarm_minutes=30;
    uint32_t local_id=put(local,0,HOTSYNC_ATTR_DIRTY,3);
    auto private_event=local; private_event.title="Private";
    uint32_t private_id=put(private_event,0,HOTSYNC_ATTR_SECRET|HOTSYNC_ATTR_DIRTY);
    put(private_event,0,HOTSYNC_ATTR_ARCHIVED|HOTSYNC_ATTR_DIRTY);
    auto old=local; old.date="2025-01-01"; old.title="Outside window"; uint32_t old_id=put(old);
    auto r=sync(h,setup); assert(r.uploaded==1 && r.downloaded==5 && r.skipped==3 && h.map.size()==5);
    assert(get(old_id).title=="Outside window");
    uint8_t category; assert(get(local_id,&category).title=="Local" && category==3);
    uint32_t cloud_id=mapped(h,"cloud"); auto palm=get(cloud_id);
    assert(palm.title==std::string("Caf\xe9") && palm.start_minute==540 && palm.alarm_minutes==15 && palm.notes.size()==4095);
    assert(get(mapped(h,"holiday")).start_minute==-1 && get(mapped(h,"yearly")).repeat==CALENDAR_YEARLY);
    r=sync(h,setup); assert(!r.uploaded && !r.downloaded && h.server.posts==1);

    // A time edit preserves a long cloud note and concurrently changed title.
    palm.start_minute=570; palm.end_minute=630; put(palm,cloud_id);
    h.server.events["cloud"].title="Cloud renamed";
    r=sync(h,setup); assert(r.uploaded==1 && h.server.events["cloud"].notes==cloud.notes);
    cJSON *patch=cJSON_Parse(h.server.last_patch.c_str()); assert(patch);
    assert(!cJSON_GetObjectItemCaseSensitive(patch,"description") && !cJSON_GetObjectItemCaseSensitive(patch,"summary") &&
           !cJSON_GetObjectItemCaseSensitive(patch,"attendees") && !cJSON_GetObjectItemCaseSensitive(patch,"location"));
    cJSON_Delete(patch); assert(get(cloud_id).title=="Cloud renamed");

    // An alarm edit retains the separate Google email reminder.
    palm=get(cloud_id); palm.alarm_minutes=60; put(palm,cloud_id);
    sync(h,setup); assert(h.server.events["cloud"].reminders.size()==2 &&
                         std::find(h.server.events["cloud"].reminders.begin(),h.server.events["cloud"].reminders.end(),
                                   std::make_pair(std::string("email"),120))!=h.server.events["cloud"].reminders.end());

    // A mapped event absent from the date window is checked, never deleted.
    h.server.hide_from_window=true; unsigned gets=h.server.gets;
    r=sync(h,setup); assert(!r.deleted && h.server.gets>gets && get(cloud_id).title=="Cloud renamed");
    h.server.hide_from_window=false;

    // Failed or malformed pages cause no writes.
    unsigned writes=h.server.posts+h.server.patches+h.server.deletes;
    h.server.bad_page=true; sync(h,setup,false); h.server.bad_page=false;
    h.server.malformed=true; sync(h,setup,false); h.server.malformed=false;
    assert(writes==h.server.posts+h.server.patches+h.server.deletes);
    h.is_online=false; sync(h,setup,false); h.is_online=true;
    h.bad_auth=true; sync(h,setup,false); h.bad_auth=false;
    h.server.role="reader"; sync(h,setup,false); h.server.role="owner";
    h.server.calendar_id="other-account@example.com"; sync(h,setup,false); h.server.calendar_id="owner@example.com";

    // A mapping-store failure prevents a new Google event from being sent.
    local.date="2026-10-20"; local.title="Storage retry"; put(local);
    h.writable=false; writes=h.server.posts+h.server.patches+h.server.deletes;
    sync(h,setup,false); assert(writes==h.server.posts+h.server.patches+h.server.deletes);
    h.writable=true; sync(h,setup);

    // Modified recurrence instances prevent flattening their parent series.
    auto series=event("modified-series","2026-10-20"); series.recurrence={"RRULE:FREQ=DAILY"};
    auto moved=event("moved-instance","2026-10-21"); moved.recurring_id=series.id;
    h.server.events[series.id]=series; h.server.events[moved.id]=moved;
    size_t links=h.map.size(); r=sync(h,setup); assert(h.map.size()==links);
    assert(std::none_of(h.map.begin(),h.map.end(),[&](const CalendarSyncMap &m) { return m.google_id==series.id || m.google_id==moved.id; }));

    // A local edit to a read-only invitation neither patches Google nor
    // overwrites the user's pending edit on the Palm.
    uint32_t invitation=mapped(h,"invite"); palm=get(invitation); palm.title="Local invitation edit"; put(palm,invitation);
    writes=h.server.posts+h.server.patches+h.server.deletes; sync(h,setup);
    assert(writes==h.server.posts+h.server.patches+h.server.deletes && get(invitation).title=="Local invitation edit");

    // Concurrent edits are detected by If-Match and leave the Palm edit intact.
    palm=get(cloud_id); palm.title="Palm edit"; put(palm,cloud_id);
    h.server.conflict=true; sync(h,setup,false); assert(get(cloud_id).title=="Palm edit");
    h.server.conflict=false; sync(h,setup); assert(h.server.events["cloud"].title=="Palm edit");

    // A POST accepted by Google but whose reply is lost retries the saved ID.
    local.title="Retry insert"; local.date="2026-10-21"; uint32_t retry_id=put(local);
    h.server.lose_insert=true; sync(h,setup,false);
    auto pending=std::find_if(h.map.begin(),h.map.end(),[&](const CalendarSyncMap &m) { return m.palm_id==retry_id; });
    assert(pending!=h.map.end() && pending->pending_create);
    std::string saved_id=pending->google_id; size_t server_count=h.server.events.size();
    sync(h,setup); assert(h.server.events.size()==server_count && h.server.events[saved_id].title=="Retry insert");
    Host restarted=h; r=sync(restarted,setup); assert(!r.uploaded && !r.downloaded);

    // All-day events have an exclusive next-day end.
    local=PalmAppointment(); local.date="2026-10-22"; local.title="Palm all-day"; uint32_t all_day_id=put(local);
    sync(h,setup);
    auto allmap=std::find_if(h.map.begin(),h.map.end(),[&](const CalendarSyncMap &m) { return m.palm_id==all_day_id; });
    assert(allmap!=h.map.end() && h.server.events[allmap->google_id].end=="2026-10-23");

    // A bounded repeating timed event uses a real DST-boundary instance to
    // compute UTC UNTIL; 09:00 Auckland in April is 21:00 UTC the day before.
    local.date="2026-10-23"; local.start_minute=540; local.end_minute=600; local.title="Weekly";
    local.repeat=CALENDAR_WEEKLY; local.repeat_on=16; local.repeat_until="2027-04-08";
    auto instance=event("lastinstance","2027-04-08"); instance.start="2027-04-08T09:00:00+12:00";
    instance.end="2027-04-08T10:00:00+12:00"; h.server.instances={instance};
    uint32_t repeat_id=put(local); h.server.fail_recurrence_patch_once=true; sync(h,setup,false);
    auto incomplete=std::find_if(h.map.begin(),h.map.end(),[&](const CalendarSyncMap &m) { return m.palm_id==repeat_id; });
    assert(incomplete!=h.map.end() && incomplete->pending_create);
    assert(h.server.events[incomplete->google_id].recurrence[0].find("UNTIL=20270409T120000Z")!=std::string::npos);
    size_t count_before_retry=h.server.events.size(); sync(h,setup);
    assert(h.server.events.size()==count_before_retry);
    auto repeatmap=std::find_if(h.map.begin(),h.map.end(),[&](const CalendarSyncMap &m) { return m.palm_id==repeat_id; });
    assert(repeatmap!=h.map.end());
    auto &rules=h.server.events[repeatmap->google_id].recurrence;
    assert(rules[0].find("UNTIL=20270407T210000Z")!=std::string::npos && get(repeat_id).repeat_until=="2027-04-08");

    // Palm deletes owned events, but deleting an invitation only hides its
    // local copy and never sends a Google DELETE.
    palm=get(cloud_id); put(palm,cloud_id,HOTSYNC_ATTR_DELETED|HOTSYNC_ATTR_DIRTY);
    r=sync(h,setup); assert(r.deleted==1 && h.server.events["cloud"].status=="cancelled");
    palm=get(invitation); put(palm,invitation,HOTSYNC_ATTR_DELETED);
    unsigned deletes=h.server.deletes; sync(h,setup); assert(h.server.deletes==deletes);
    r=sync(h,setup); assert(!r.downloaded);
    assert(get(private_id).title=="Private");
    h.server.events["holiday"].status="cancelled"; r=sync(h,setup); assert(r.deleted==1);
    assert(hotsync_end(0)); close(fd); puts("done");
}
