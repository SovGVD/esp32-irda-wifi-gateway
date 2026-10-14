#include "ircalendar.h"
#include "irmail.h"
#include "irgooglehttp.h"
#include "irdebug.h"
#include "config.h"
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <cstdlib>
#include <cstring>

static IrCalendarSettings s_settings;
static char s_result[256]="";
static GoogleCalendarSetup calendar_setup() {
    GoogleCalendarSetup s;
    s.calendar_id=s_settings.calendar_id.c_str();
    s.max_events=CALENDAR_MAX_PER_SYNC;
    s.past_days=s_settings.past_days; s.future_days=s_settings.future_days;
    return s;
}
void ircalendar_init() {
    Preferences p;
    if (!p.begin("ircalendar",true)) return;
    s_settings.enabled=p.getBool("on",false);
    s_settings.cyrillic=p.getBool("cset",false);
    s_settings.calendar_id=p.getString("cal","primary");
    s_settings.past_days=p.getUInt("past",CALENDAR_PAST_DAYS);
    s_settings.future_days=p.getUInt("future",CALENDAR_FUTURE_DAYS);
    p.end();
}
const IrCalendarSettings &ircalendar_settings() { return s_settings; }
bool ircalendar_ready() {
    const auto &google=irmail_settings();
    return ENABLE_WIFI_GATEWAY && s_settings.enabled && !s_settings.calendar_id.isEmpty() &&
        google.google_scope(GOOGLE_CALENDAR_SCOPE) &&
        !google.google_client_id.isEmpty() && !google.google_client_secret.isEmpty();
}
bool ircalendar_store_settings(const IrCalendarSettings &s) {
    if (s.calendar_id.isEmpty() || s.past_days>3650 || !s.future_days || s.future_days>3650) return false;
    Preferences p;
    if (!p.begin("ircalendar",false)) return false;
    if (s.calendar_id!=s_settings.calendar_id && (p.isKey("links") && p.getBytesLength("links")>6)) { p.end(); return false; }
    bool ok=p.putString("cal",s.calendar_id)==s.calendar_id.length() &&
        p.putUInt("past",s.past_days)==4 && p.putUInt("future",s.future_days)==4 &&
        p.putBool("on",s.enabled)==1 && p.putBool("cset",s.cyrillic)==1;
    p.end(); if (ok) s_settings=s;
    return ok;
}
class EspCalendarHost : public CalendarSyncHost {
public:
    GoogleHttp &http() override { return http_; }
    bool online() override { return WiFi.status()==WL_CONNECTED; }
    int64_t now() override { time_t t=time(nullptr); return t>1700000000 ? (int64_t)t : 0; }
    uint32_t random() override { return esp_random(); }
    bool access_token(std::string &token,std::string &problem) override {
        return irmail_google_access_token(token,problem,true);
    }
    bool bind_calendar(const std::string &id,uint32_t palm) override {
        Preferences p;
        if (!p.begin("ircalendar",false)) return false;
        String bound=p.getString("bound","");
        bool ok=(bound.isEmpty() || bound==id.c_str()) &&
            (!p.isKey("palm") || p.getULong("palm")==palm);
        if (ok) ok=p.putString("bound",id.c_str())==id.size() && p.putULong("palm",palm)==4;
        p.end(); return ok;
    }
    bool load_map(std::vector<CalendarSyncMap> &map) override {
        map.clear(); Preferences p;
        if (!p.begin("ircalendar",false)) return false;
        if (!p.isKey("links")) { p.end(); return true; }
        size_t length=p.getBytesLength("links");
        if (length<6 || length>CALENDAR_MAX_PER_SYNC*1051+6) { p.end(); return false; }
        std::string data(length,'\0');
        bool ok=p.getBytes("links",&data[0],length)==length;
        p.end();
        return ok && calendarsync_decode_map(data,CALENDAR_MAX_PER_SYNC,map);
    }
    bool store_map(const std::vector<CalendarSyncMap> &map) override {
        std::string data;
        if (!calendarsync_encode_map(map,data)) return false;
        Preferences p;
        if (!p.begin("ircalendar",false)) return false;
        bool ok=p.putBytes("links",data.data(),data.size())==data.size();
        p.end(); return ok;
    }
private:
    EspGoogleHttp http_{true,CALENDAR_HTTP_TIMEOUT_MS,CALENDAR_HTTP_RESPONSE_MAX};
};
String ircalendar_last_result() { return s_result; }
String ircalendar_check_login() {
    if (!ircalendar_ready()) return "Enable Calendar and sign in with Calendar permission first.";
    if (WiFi.status()!=WL_CONNECTED) return "Wi-Fi is not connected.";
    time_t now=time(nullptr);
    if (now<=1700000000) return "The network clock is not set yet.";
    std::string token,problem;
    if (!irmail_google_access_token(token,problem)) return problem.c_str();
    EspGoogleHttp http(false,CALENDAR_HTTP_TIMEOUT_MS,CALENDAR_HTTP_RESPONSE_MAX);
    auto setup=calendar_setup(); GoogleCalendarClient google(http,setup,token);
    GoogleCalendarInfo info; std::vector<GoogleCalendarEvent> events;
    if (!google.open(info) || !google.list(now,events)) return google.error().c_str();
    return "OK, "+String((unsigned)events.size())+" events in the date window. Time zone: "+String(info.zone.c_str())+".";
}
void ircalendar_sync(CalendarSyncResult &r) {
    EspCalendarHost host;
    calendarsync_run(calendar_setup(),s_settings.cyrillic ? PALM_CYRILLIC : PALM_WESTERN,host,r);
    std::string summary=calendarsync_summary(r);
    strlcpy(s_result,summary.c_str(),sizeof(s_result));
    irdbg_printf("Calendar: %s",s_result);
}
