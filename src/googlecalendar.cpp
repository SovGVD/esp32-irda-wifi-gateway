#include "googlecalendar.h"
#include "mailoauth.h"
#include "mailtext.h"
#include "cJSON.h"
#include <memory>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

static const char *event_fields="id,etag,summary,description,start,end,status,eventType,recurringEventId,originalStartTime,"
    "recurrence,organizer(self),guestsCanModify,reminders,extendedProperties/private/palmGateway";
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
static Json parse(const std::string &s) { return Json(cJSON_ParseWithOpts(s.c_str(),nullptr,true),cJSON_Delete); }
static std::string field(const cJSON *j,const char *key) {
    const auto *v=cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static std::string print(cJSON *j) {
    char *raw=cJSON_PrintUnformatted(j);
    if (!raw) return "";
    std::string out(raw); free(raw); return out;
}
static bool reminders(const cJSON *items,std::vector<std::pair<std::string,int>> &out) {
    out.clear();
    if (!items) return true;
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items)>5) return false;
    const cJSON *item;
    cJSON_ArrayForEach(item,items) {
        auto *minutes=cJSON_GetObjectItemCaseSensitive(item,"minutes");
        std::string method=field(item,"method");
        if ((method!="popup" && method!="email") || !cJSON_IsNumber(minutes) ||
            minutes->valuedouble<0 || minutes->valuedouble>40320 || minutes->valuedouble!=(int)minutes->valuedouble) return false;
        out.emplace_back(method,(int)minutes->valuedouble);
    }
    return true;
}
static bool unpack(const cJSON *j,GoogleCalendarEvent &e) {
    if (!cJSON_IsObject(j)) return false;
    e=GoogleCalendarEvent();
    for (const char *key:{"id","etag","summary","description","status","eventType","recurringEventId"}) {
        const auto *v=cJSON_GetObjectItemCaseSensitive(j,key);
        if (v && !cJSON_IsString(v)) return false;
    }
    e.id=field(j,"id"); e.etag=field(j,"etag"); e.title=field(j,"summary"); e.notes=field(j,"description");
    e.status=field(j,"status"); e.event_type=field(j,"eventType"); e.recurring_id=field(j,"recurringEventId");
    e.palm_marker=field(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j,"extendedProperties"),"private"),"palmGateway");
    auto *original=cJSON_GetObjectItemCaseSensitive(j,"originalStartTime");
    e.original_date=field(original,"date");
    if (e.original_date.empty()) e.original_date=field(original,"dateTime").substr(0,10);
    if (e.id.empty() || (e.status!="confirmed" && e.status!="tentative" && e.status!="cancelled")) return false;
    if (e.status=="cancelled") return true;
    const auto *start=cJSON_GetObjectItemCaseSensitive(j,"start"),*end=cJSON_GetObjectItemCaseSensitive(j,"end");
    e.start=field(start,"date"); e.end=field(end,"date");
    e.all_day=!e.start.empty();
    if (!e.all_day) { e.start=field(start,"dateTime"); e.end=field(end,"dateTime"); }
    e.zone=field(start,"timeZone");
    if (e.start.empty() || e.end.empty()) return false;
    auto *r=cJSON_GetObjectItemCaseSensitive(j,"recurrence");
    if (r && !cJSON_IsArray(r)) return false;
    const cJSON *item;
    cJSON_ArrayForEach(item,r) {
        if (!cJSON_IsString(item)) return false;
        e.recurrence.emplace_back(item->valuestring);
    }
    auto *rem=cJSON_GetObjectItemCaseSensitive(j,"reminders");
    auto *defaults=cJSON_GetObjectItemCaseSensitive(rem,"useDefault");
    if (defaults && !cJSON_IsBool(defaults)) return false;
    e.use_default_reminders=!defaults || cJSON_IsTrue(defaults);
    if (!reminders(cJSON_GetObjectItemCaseSensitive(rem,"overrides"),e.reminders)) return false;
    auto *organizer=cJSON_GetObjectItemCaseSensitive(j,"organizer");
    e.writable=!organizer || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(organizer,"self")) ||
               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"guestsCanModify"));
    return true;
}
int GoogleCalendarClient::request(const std::string &path,const char *method,const std::string &body,
                                  const std::string &etag,std::string &reply) {
    error_.clear();
    if (token_.empty() || token_.find_first_of("\r\n")!=std::string::npos ||
        etag.find_first_of("\r\n")!=std::string::npos) {
        error_="Calendar is not authorized"; return -1;
    }
    int code=http_.request_if_match("https://www.googleapis.com/calendar/v3"+path,method,
                                   "Bearer "+token_,"application/json",body,etag,reply);
    if (code<200 || code>=300) {
        error_=code==412 ? "Calendar changed during sync. Sync again." :
            "Google Calendar request failed (HTTP "+std::to_string(code)+")";
        if (code<0 && !http_.error().empty()) error_="Google Calendar: "+http_.error();
        if (code==403) error_+=". Check Calendar permission and API access.";
    }
    return code;
}
bool GoogleCalendarClient::open(GoogleCalendarInfo &info) {
    std::string reply;
    int code=request("/users/me/calendarList/"+oauth_encode(setup_.calendar_id),"GET","","",reply);
    auto j=parse(reply);
    info_=GoogleCalendarInfo();
    info_.id=field(j.get(),"id"); info_.zone=field(j.get(),"timeZone"); info_.role=field(j.get(),"accessRole");
    if (code!=200 || field(j.get(),"kind")!="calendar#calendarListEntry" || info_.id.empty() || info_.zone.empty() ||
        (info_.role!="owner" && info_.role!="writer")) {
        if (code==200) error_="Choose a writable calendar from your calendar list.";
        return false;
    }
    if (!reminders(cJSON_GetObjectItemCaseSensitive(j.get(),"defaultReminders"),info_.default_reminders)) {
        error_="Invalid Calendar defaults"; return false;
    }
    info=info_; return true;
}
bool GoogleCalendarClient::list(int64_t now,std::vector<GoogleCalendarEvent> &events) {
    events.clear(); std::string page; size_t bytes=0;
    for (unsigned n=0;n<100;n++) {
        std::string path="/calendars/"+oauth_encode(info_.id)+"/events?maxResults=3&singleEvents=false&showDeleted=true"+
            std::string("&fields=")+oauth_encode(std::string("kind,nextPageToken,items(")+event_fields+")")+
            "&timeZone="+oauth_encode(info_.zone)+
            "&timeMin="+oauth_encode(calendar_utc(now-(int64_t)setup_.past_days*86400))+
            "&timeMax="+oauth_encode(calendar_utc(now+(int64_t)setup_.future_days*86400));
        if (!page.empty()) path+="&pageToken="+oauth_encode(page);
        std::string reply;
        if (request(path,"GET","","",reply)!=200) return false;
        auto j=parse(reply);
        const auto *items=cJSON_GetObjectItemCaseSensitive(j.get(),"items");
        const auto *next=cJSON_GetObjectItemCaseSensitive(j.get(),"nextPageToken");
        if (!j || field(j.get(),"kind")!="calendar#events" || (items && !cJSON_IsArray(items)) ||
            (next && !cJSON_IsString(next))) { error_="Invalid Calendar events response"; return false; }
        const cJSON *item;
        cJSON_ArrayForEach(item,items) {
            GoogleCalendarEvent e;
            if (!unpack(item,e)) { error_="Invalid Calendar event"; return false; }
            for (const auto &old:events) if (e.id==old.id) { error_="Duplicate Calendar event ID"; return false; }
            bytes+=e.title.size()+e.notes.size()+e.id.size()+e.start.size()+e.end.size();
            for (const auto &r:e.recurrence) bytes+=r.size();
            if (bytes>48000 || events.size()>=setup_.max_events) {
                error_="Calendar date window exceeds the sync memory or event limit. Choose a smaller window."; return false;
            }
            events.push_back(e);
        }
        std::string next_page=field(j.get(),"nextPageToken");
        if (next_page.empty()) return true;
        if (next_page==page) { error_="Calendar repeated a page"; return false; }
        page=next_page;
    }
    error_="Too many Calendar pages"; return false;
}
bool GoogleCalendarClient::get(const std::string &id,GoogleCalendarEvent &e,bool &found) {
    found=false; std::string reply;
    int code=request("/calendars/"+oauth_encode(info_.id)+"/events/"+oauth_encode(id)+
                     "?fields="+oauth_encode(event_fields)+"&timeZone="+oauth_encode(info_.zone),"GET","","",reply);
    if (code==404 || code==410) { error_.clear(); return true; }
    if (code!=200) return false;
    auto j=parse(reply);
    if (!unpack(j.get(),e) || e.id!=id) { error_="Invalid Calendar event response"; return false; }
    found=true; return true;
}
static int minute(const std::string &stamp) {
    return std::stoi(stamp.substr(11,2))*60+std::stoi(stamp.substr(14,2));
}
bool GoogleCalendarClient::boundary(const std::string &id,const std::string &local_date,bool last_before,
                                    std::string &date,std::string &utc) {
    int y; unsigned m,d;
    if (!calendar_date(local_date,y,m,d)) { error_="Invalid recurrence boundary"; return false; }
    int64_t noon=mail_seconds(y,m,d,12,0,0);
    std::string page;
    // A +/- 2 day window covers every real UTC offset and DST change.
    // An outgoing series initially extends beyond the requested local date,
    // so an empty window proves the fallback lies between occurrences. For an incoming
    // bounded series, require an actual last occurrence; an empty window
    // cannot establish whether an occurrence on the cutoff date was excluded.
    date=last_before ? local_date : ""; utc=calendar_utc(noon);
    std::string best;
    for (unsigned n=0;n<16;n++) {
        std::string path="/calendars/"+oauth_encode(info_.id)+"/events/"+oauth_encode(id)+
            "/instances?maxResults=50&fields="+oauth_encode("kind,nextPageToken,items(id,status,start,end)")+"&timeZone="+oauth_encode(info_.zone)+
            "&timeMin="+oauth_encode(calendar_utc(noon-2*86400))+
            "&timeMax="+oauth_encode(calendar_utc(noon+2*86400));
        if (!page.empty()) path+="&pageToken="+oauth_encode(page);
        std::string reply;
        if (request(path,"GET","","",reply)!=200) return false;
        auto j=parse(reply);
        const auto *items=cJSON_GetObjectItemCaseSensitive(j.get(),"items");
        const auto *next=cJSON_GetObjectItemCaseSensitive(j.get(),"nextPageToken");
        if (!j || field(j.get(),"kind")!="calendar#events" || (items && !cJSON_IsArray(items)) ||
            (next && !cJSON_IsString(next))) { error_="Invalid Calendar instances response"; return false; }
        const cJSON *item;
        cJSON_ArrayForEach(item,items) {
            GoogleCalendarEvent e;
            if (!unpack(item,e)) { error_="Invalid Calendar instance"; return false; }
            if (e.status=="cancelled") continue;
            int64_t epoch;
            if (!calendar_timestamp(e.start,epoch)) { error_="Invalid Calendar instance date"; return false; }
            std::string local=e.start.substr(0,10);
            if ((!last_before || local<=local_date) && (best.empty() || e.start>best)) {
                best=e.start; date=local; utc=calendar_utc(epoch);
            }
        }
        std::string next_page=field(j.get(),"nextPageToken");
        if (next_page.empty()) return true;
        if (next_page==page) break;
        page=next_page;
    }
    error_="Too many Calendar instances"; return false;
}
bool GoogleCalendarClient::display(const GoogleCalendarEvent &e,PalmAppointment &a,std::string &reason) {
    error_.clear();
    auto unsupported=[&](const std::string &why) { reason=why; return false; };
    if (!e.event_type.empty() && e.event_type!="default") return unsupported("special Calendar event type");
    if (!e.recurrence.empty() && !e.zone.empty() && e.zone!=info_.zone) return unsupported("recurrence uses a different time zone");
    for (const auto &rule:e.recurrence) {
        if (rule.compare(0,12,"EXDATE;TZID=")==0) {
            size_t colon=rule.find(':',12);
            if (colon==std::string::npos || rule.substr(12,colon-12)!=info_.zone)
                return unsupported("recurrence exclusion uses a different time zone");
        }
    }
    if (!e.recurring_id.empty()) return unsupported("modified recurrence instance");
    if (!calendar_palm_date(e.start.substr(0,10))) return unsupported("date outside Palm range");
    a=PalmAppointment(); a.date=e.start.substr(0,10);
    if (e.all_day) {
        if (e.start.size()!=10 || e.end!=calendar_add_days(e.start,1)) return unsupported("multi-day all-day event");
    } else {
        int64_t start,end;
        if (!calendar_timestamp(e.start,start) || !calendar_timestamp(e.end,end)) return unsupported("invalid event time");
        if (e.end.substr(0,10)!=a.date || end<=start) return unsupported("multi-day or invalid timed event");
        a.start_minute=minute(e.start); a.end_minute=minute(e.end);
        if (a.end_minute<a.start_minute) return unsupported("event crosses a clock change");
    }
    a.title=palm_from_utf8(e.title,PALM_WESTERN,CALENDAR_TITLE_MAX);
    a.notes=palm_from_utf8(e.notes,PALM_WESTERN,CALENDAR_NOTE_MAX);
    const auto &rem=e.use_default_reminders ? info_.default_reminders : e.reminders;
    for (const auto &r:rem) {
        if (r.first!="popup") continue;
        int m=r.second;
        if (m<=255 || (m%60==0 && m/60<=255) || (m%1440==0 && m/1440<=255)) { a.alarm_minutes=m; break; }
    }
    std::string until;
    if (!calendar_read_recurrence(e.recurrence,a,until,reason)) return false;
    if (!until.empty()) {
        int64_t epoch; calendar_timestamp(until,epoch);
        // Google supplies the original event in the requested calendar TZ.
        // Use its offset for the provisional local cutoff date, then inspect
        // actual instances near that cutoff to handle seasonal DST changes.
        int y; unsigned m,d; calendar_date(e.start.substr(0,10),y,m,d);
        int64_t start_epoch; calendar_timestamp(e.start,start_epoch);
        int64_t offset=mail_seconds(y,m,d,minute(e.start)/60,minute(e.start)%60,
                                   std::stoi(e.start.substr(17,2)))-start_epoch;
        std::string cutoff=calendar_utc(epoch+offset).substr(0,10);
        std::string date,utc;
        if (!boundary(e.id,cutoff,false,date,utc)) { reason=error_; return false; }
        if (date.empty()) return unsupported("timed recurrence cutoff cannot be resolved near its end date");
        if (!calendar_palm_date(date) || date<a.date) return unsupported("recurrence end outside Palm range");
        a.repeat_until=date;
    }
    return true;
}
static std::string floating(const PalmAppointment &a,bool end) {
    char t[32]; int minutes=end ? a.end_minute : a.start_minute;
    snprintf(t,sizeof(t),"T%02d:%02d:00",minutes/60,minutes%60);
    return a.date+t;
}
static void set_times(cJSON *j,const PalmAppointment &a,const std::string &zone) {
    cJSON *start=cJSON_AddObjectToObject(j,"start"),*end=cJSON_AddObjectToObject(j,"end");
    if (a.start_minute<0) {
        cJSON_AddStringToObject(start,"date",a.date.c_str());
        cJSON_AddStringToObject(end,"date",calendar_add_days(a.date,1).c_str());
    } else {
        cJSON_AddStringToObject(start,"dateTime",floating(a,false).c_str());
        cJSON_AddStringToObject(end,"dateTime",floating(a,true).c_str());
        cJSON_AddStringToObject(start,"timeZone",zone.c_str()); cJSON_AddStringToObject(end,"timeZone",zone.c_str());
    }
}
static void set_recurrence(cJSON *j,const PalmAppointment &a,const std::string &zone,const std::string &utc_until="") {
    cJSON *rules=cJSON_AddArrayToObject(j,"recurrence");
    for (auto rule:calendar_recurrence(a)) {
        if (rule.compare(0,6,"EXDATE")==0 && a.start_minute>=0) rule.insert(6,";TZID="+zone);
        if (rule.compare(0,6,"RRULE:")==0 && !utc_until.empty()) {
            std::string compact;
            for (char c:utc_until) if (c!='-' && c!=':') compact+=c;
            rule+=";UNTIL="+compact;
        }
        cJSON_AddItemToArray(rules,cJSON_CreateString(rule.c_str()));
    }
}
bool GoogleCalendarClient::save(const std::string &id,bool create,const std::string &marker,const std::string &etag,
                                const PalmAppointment &a,const std::array<bool,5> &fields,
                                const GoogleCalendarEvent *current,GoogleCalendarEvent &saved) {
    if ((create || fields[2]) && a.start_minute>=0 && a.end_minute<=a.start_minute) { error_="An appointment must end after it starts."; return false; }
    if (a.alarm_minutes>40320) { error_="Calendar reminders cannot exceed 4 weeks."; return false; }
    Json body(cJSON_CreateObject(),cJSON_Delete);
    if (create) {
        cJSON_AddStringToObject(body.get(),"id",id.c_str());
        cJSON *p=cJSON_AddObjectToObject(cJSON_AddObjectToObject(body.get(),"extendedProperties"),"private");
        cJSON_AddStringToObject(p,"palmGateway",marker.c_str());
    }
    if (fields[0]) cJSON_AddStringToObject(body.get(),"summary",a.title.c_str());
    if (fields[1]) cJSON_AddStringToObject(body.get(),"description",a.notes.c_str());
    if (fields[2]) set_times(body.get(),a,info_.zone);
    if (fields[3]) {
        // Resolve the precise UTC cutoff below, but keep the first write
        // bounded if the network fails between the two requests. This initial
        // cutoff includes the requested local day at every real UTC offset.
        std::string initial_until;
        if (a.repeat && !a.repeat_until.empty() && a.start_minute>=0)
            initial_until=calendar_add_days(a.repeat_until,1)+"T12:00:00Z";
        set_recurrence(body.get(),a,info_.zone,initial_until);
    }
    if (fields[4]) {
        cJSON *r=cJSON_AddObjectToObject(body.get(),"reminders");
        cJSON_AddBoolToObject(r,"useDefault",false);
        cJSON *overrides=cJSON_AddArrayToObject(r,"overrides");
        const auto &old=current ? (current->use_default_reminders ? info_.default_reminders : current->reminders) : info_.default_reminders;
        unsigned count=0;
        for (const auto &item:old) if (item.first!="popup") {
            cJSON *rem=cJSON_CreateObject(); cJSON_AddStringToObject(rem,"method",item.first.c_str());
            cJSON_AddNumberToObject(rem,"minutes",item.second); cJSON_AddItemToArray(overrides,rem); count++;
        }
        if (a.alarm_minutes>=0) {
            if (count>=5) { error_="Calendar has too many reminders"; return false; }
            cJSON *rem=cJSON_CreateObject(); cJSON_AddStringToObject(rem,"method","popup");
            cJSON_AddNumberToObject(rem,"minutes",a.alarm_minutes); cJSON_AddItemToArray(overrides,rem);
        }
    }
    std::string raw=print(body.get()),reply;
    if (raw.empty()) { error_="Not enough memory for Calendar update"; return false; }
    std::string path="/calendars/"+oauth_encode(info_.id)+"/events";
    if (!create) path+="/"+oauth_encode(id);
    path+="?sendUpdates=none";
    int code=request(path,create ? "POST" : "PATCH",raw,create ? "" : etag,reply);
    if (create && code==409) {
        bool found;
        if (!get(id,saved,found)) return false;
        if (!found || saved.palm_marker!=marker || saved.status=="cancelled") {
            error_="Calendar event ID collision. No event was overwritten."; return false;
        }
        // Retry a journalled insert with the desired fields, preserving its identity.
        return save(id,false,marker,saved.etag,a,fields,&saved,saved);
    }
    if (code!=200 && code!=201) return false;
    auto j=parse(reply);
    if (!unpack(j.get(),saved) || saved.id!=id || saved.status=="cancelled") {
        error_="Invalid Calendar save response"; return false;
    }
    if (fields[3] && a.repeat && !a.repeat_until.empty() && a.start_minute>=0) {
        std::string date,utc;
        if (!boundary(id,a.repeat_until,true,date,utc)) return false;
        Json end_rule(cJSON_CreateObject(),cJSON_Delete);
        set_recurrence(end_rule.get(),a,info_.zone,utc);
        raw=print(end_rule.get());
        if (raw.empty() || request("/calendars/"+oauth_encode(info_.id)+"/events/"+oauth_encode(id)+"?sendUpdates=none",
                                  "PATCH",raw,saved.etag,reply)!=200) return false;
        j=parse(reply);
        if (!unpack(j.get(),saved) || saved.id!=id) { error_="Invalid Calendar recurrence response"; return false; }
    }
    return true;
}
bool GoogleCalendarClient::remove(const std::string &id,const std::string &etag) {
    std::string reply;
    int code=request("/calendars/"+oauth_encode(info_.id)+"/events/"+oauth_encode(id)+"?sendUpdates=none",
                     "DELETE","",etag,reply);
    if (code==404 || code==410) { error_.clear(); return true; }
    return code==204 || code==200;
}
