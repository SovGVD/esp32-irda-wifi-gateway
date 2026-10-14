#include "calendartext.h"
#include "mailtext.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <map>

static unsigned get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static void put16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
bool calendar_date(const std::string &s, int &y, unsigned &m, unsigned &d) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (unsigned i = 0; i < 10; i++) if (i != 4 && i != 7 && !isdigit((unsigned char)s[i])) return false;
    if (sscanf(s.c_str(), "%4d-%2u-%2u", &y, &m, &d) != 3 || y < 1 || m < 1 || m > 12) return false;
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return d >= 1 && d <= days[m-1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
bool calendar_palm_date(const std::string &s) {
    int y; unsigned m,d;
    return calendar_date(s,y,m,d) && y >= 1904 && y <= 2031;
}
static std::string date_string(int y, unsigned m, unsigned d) {
    char out[16]; snprintf(out,sizeof(out),"%04d-%02u-%02u",y,m,d); return out;
}
std::string calendar_utc(int64_t seconds) {
    // civil_from_days: Gregorian eras, no libc TZ or 32-bit time_t.
    int64_t days = seconds / 86400;
    int64_t remainder = seconds % 86400;
    if (remainder < 0) { remainder += 86400; --days; }
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096)/365;
    int y = (int)yoe + (int)era*400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy+2)/153;
    unsigned d = doy - (153*mp+2)/5+1;
    unsigned m = mp < 10 ? mp+3 : mp-9;
    y += m <= 2;
    char out[32];
    snprintf(out,sizeof(out),"%04d-%02u-%02uT%02u:%02u:%02uZ",y,m,d,
             (unsigned)(remainder/3600),(unsigned)(remainder/60%60),(unsigned)(remainder%60));
    return out;
}
std::string calendar_add_days(const std::string &s, int days) {
    int y; unsigned m,d;
    if (!calendar_date(s,y,m,d)) return "";
    return calendar_utc(mail_seconds(y,m,d,0,0,0) + (int64_t)days*86400).substr(0,10);
}
bool calendar_timestamp(const std::string &s, int64_t &utc) {
    int y; unsigned m,d,h,mi,sec;
    if (s.size() < 20 || !calendar_date(s.substr(0,10),y,m,d) || s[10]!='T' ||
        s[13]!=':' || s[16]!=':' || sscanf(s.c_str()+11,"%2u:%2u:%2u",&h,&mi,&sec)!=3 ||
        h > 23 || mi > 59 || sec > 59) return false;
    for (unsigned i : {11u,12u,14u,15u,17u,18u}) if (!isdigit((unsigned char)s[i])) return false;
    size_t at = 19;
    if (s[at] == '.') {
        size_t first = ++at;
        while (at < s.size() && isdigit((unsigned char)s[at])) at++;
        if (at == first) return false;
    }
    int offset = 0;
    if (at+1 == s.size() && s[at]=='Z') {}
    else {
        unsigned oh,om;
        if (at+6!=s.size() || (s[at]!='+' && s[at]!='-') || s[at+3]!=':' ||
            sscanf(s.c_str()+at+1,"%2u:%2u",&oh,&om)!=2 || oh>23 || om>59) return false;
        for (size_t i : {at+1,at+2,at+4,at+5}) if (!isdigit((unsigned char)s[i])) return false;
        offset = (int)(oh*60+om)*(s[at]=='-' ? -1 : 1);
    }
    utc = mail_seconds(y,m,d,h,mi,sec)-(int64_t)offset*60;
    return true;
}
static std::string unpack_date(unsigned packed) {
    return date_string(1904+(packed>>9),(packed>>5)&15,packed&31);
}
static unsigned pack_date(const std::string &s) {
    int y; unsigned m,d;
    calendar_date(s,y,m,d);
    return ((y-1904)<<9)|(m<<5)|d;
}
static bool alarm_parts(int minutes, unsigned &advance, unsigned &units) {
    if (minutes < 0) return false;
    if (minutes <= 255) { advance = minutes; units = 0; return true; }
    if (minutes%60==0 && minutes/60<=255) { advance=minutes/60; units=1; return true; }
    if (minutes%1440==0 && minutes/1440<=255) { advance=minutes/1440; units=2; return true; }
    return false;
}
static bool valid(const PalmAppointment &a) {
    unsigned advance,units;
    return calendar_palm_date(a.date) &&
        ((a.start_minute==-1 && a.end_minute==-1) ||
         (a.start_minute>=0 && a.start_minute<1440 && a.end_minute>=a.start_minute && a.end_minute<1440)) &&
        (a.alarm_minutes==-1 || alarm_parts(a.alarm_minutes,advance,units)) &&
        a.title.size()<=CALENDAR_TITLE_MAX && a.notes.size()<=CALENDAR_NOTE_MAX &&
        a.title.find('\0')==std::string::npos && a.notes.find('\0')==std::string::npos &&
        a.repeat<=CALENDAR_YEARLY && a.interval>=1 && a.interval<=255 && a.week_start<=6 &&
        (a.repeat_until.empty() || (calendar_palm_date(a.repeat_until) && a.repeat_until>=a.date)) &&
        (a.repeat!=CALENDAR_WEEKLY || (a.repeat_on>0 && a.repeat_on<128)) &&
        (a.repeat!=CALENDAR_MONTHLY_DAY || a.repeat_on<35) &&
        (a.repeat!=CALENDAR_ONCE || (a.repeat_until.empty() && a.exceptions.empty())) &&
        a.exceptions.size()<=CALENDAR_EXCEPTIONS_MAX &&
        std::all_of(a.exceptions.begin(),a.exceptions.end(),calendar_palm_date);
}
bool calendar_unpack_record(const uint8_t *p, size_t len, PalmAppointment &a) {
    a = PalmAppointment();
    if (len<8 || (p[6]&~0x7c)) return false;
    a.date=unpack_date(get16(p+4));
    if (get16(p)!=0xffff) {
        if (p[0]>23 || p[1]>59 || p[2]>23 || p[3]>59) return false;
        a.start_minute=p[0]*60+p[1]; a.end_minute=p[2]*60+p[3];
    } else if (get16(p+2)!=0xffff) return false;
    size_t at=8;
    if (p[6]&0x40) {
        if (len-at<2 || p[at+1]>2) return false;
        a.alarm_minutes=p[at]*(p[at+1]==0 ? 1 : p[at+1]==1 ? 60 : 1440); at+=2;
    }
    if (p[6]&0x20) {
        if (len-at<8 || p[at]<1 || p[at]>5) return false;
        a.repeat=(CalendarRepeat)p[at];
        if (get16(p+at+2)!=0xffff) a.repeat_until=unpack_date(get16(p+at+2));
        a.interval=p[at+4]; a.repeat_on=p[at+5]; a.week_start=p[at+6]; at+=8;
    }
    if (p[6]&0x08) {
        if (len-at<2) return false;
        unsigned n=get16(p+at); at+=2;
        if (n>CALENDAR_EXCEPTIONS_MAX || len-at<n*2) return false;
        for (unsigned i=0;i<n;i++,at+=2) a.exceptions.push_back(unpack_date(get16(p+at)));
    }
    const std::pair<int,std::string *> text_fields[]={{0x04,&a.title},{0x10,&a.notes}};
    for (const auto &field:text_fields) {
        if (!(p[6]&field.first)) continue;
        const uint8_t *end=(const uint8_t *)memchr(p+at,0,len-at);
        if (!end) return false;
        field.second->assign((const char *)p+at,end-p-at); at=(end-p)+1;
    }
    return at==len && valid(a);
}
size_t calendar_pack_record(const PalmAppointment &a, uint8_t *p, size_t max) {
    if (!valid(a)) return 0;
    size_t len=8+(a.alarm_minutes>=0 ? 2 : 0)+(a.repeat ? 8 : 0)+
        (a.exceptions.empty() ? 0 : 2+a.exceptions.size()*2)+
        (a.title.empty() ? 0 : a.title.size()+1)+(a.notes.empty() ? 0 : a.notes.size()+1);
    if (len>max) return 0;
    memset(p,0,len);
    if (a.start_minute<0) memset(p,0xff,4);
    else {
        p[0]=a.start_minute/60; p[1]=a.start_minute%60;
        p[2]=a.end_minute/60; p[3]=a.end_minute%60;
    }
    put16(p+4,pack_date(a.date)); size_t at=8;
    if (a.alarm_minutes>=0) {
        unsigned advance,units; alarm_parts(a.alarm_minutes,advance,units);
        p[6]|=0x40; p[at++]=advance; p[at++]=units;
    }
    if (a.repeat) {
        p[6]|=0x20; p[at]=a.repeat;
        put16(p+at+2,a.repeat_until.empty() ? 0xffff : pack_date(a.repeat_until));
        p[at+4]=a.interval; p[at+5]=a.repeat_on; p[at+6]=a.week_start; at+=8;
    }
    if (!a.exceptions.empty()) {
        p[6]|=0x08; put16(p+at,a.exceptions.size()); at+=2;
        for (const auto &date:a.exceptions) { put16(p+at,pack_date(date)); at+=2; }
    }
    if (!a.title.empty()) { p[6]|=0x04; memcpy(p+at,a.title.c_str(),a.title.size()+1); at+=a.title.size()+1; }
    if (!a.notes.empty()) { p[6]|=0x10; memcpy(p+at,a.notes.c_str(),a.notes.size()+1); }
    return len;
}
static const char *weekdays[]={"SU","MO","TU","WE","TH","FR","SA"};
static std::string compact(const std::string &date) {
    std::string s=date; s.erase(7,1); s.erase(4,1); return s;
}
static std::string expanded(const std::string &s) {
    if (s.size()<8) return "";
    return s.substr(0,4)+"-"+s.substr(4,2)+"-"+s.substr(6,2);
}
std::vector<std::string> calendar_recurrence(const PalmAppointment &a) {
    if (!a.repeat) return {};
    const char *freq[]={"","DAILY","WEEKLY","MONTHLY","MONTHLY","YEARLY"};
    std::string r=std::string("RRULE:FREQ=")+freq[a.repeat]+";INTERVAL="+std::to_string(a.interval);
    if (a.repeat==CALENDAR_WEEKLY) {
        r+=";BYDAY="; bool first=true;
        for (unsigned n=0;n<7;n++) if (a.repeat_on&(1u<<n)) { if (!first) r+=","; r+=weekdays[n]; first=false; }
    } else if (a.repeat==CALENDAR_MONTHLY_DAY) {
        unsigned week=a.repeat_on/7;
        r+=";BYDAY="+std::string(week==4 ? "-1" : std::to_string(week+1))+weekdays[a.repeat_on%7];
    } else if (a.repeat==CALENDAR_MONTHLY_DATE) r+=";BYMONTHDAY="+std::to_string(std::stoi(a.date.substr(8,2)));
    r+=";WKST="+std::string(weekdays[a.week_start]);
    // Timed UNTIL is resolved through Calendar instances in the REST client,
    // using the calendar's actual time-zone offsets at that boundary.
    if (!a.repeat_until.empty() && a.start_minute<0) r+=";UNTIL="+compact(a.repeat_until);
    std::vector<std::string> out{r};
    for (const auto &date:a.exceptions) {
        std::string ex="EXDATE";
        if (a.start_minute<0) ex+=";VALUE=DATE:"+compact(date);
        else {
            char time[32]; snprintf(time,sizeof(time),"T%02d%02d00",a.start_minute/60,a.start_minute%60);
            ex+=":"+compact(date)+time;
        }
        out.push_back(ex);
    }
    return out;
}
static std::vector<std::string> split(const std::string &s, char separator) {
    std::vector<std::string> out; size_t at=0;
    do {
        size_t end=s.find(separator,at); out.push_back(s.substr(at,end==std::string::npos ? end : end-at));
        if (end==std::string::npos) break;
        at=end+1;
    } while (at<=s.size());
    return out;
}
static int weekday(const std::string &s) {
    for (int n=0;n<7;n++) if (s==weekdays[n]) return n;
    return -1;
}
bool calendar_read_recurrence(const std::vector<std::string> &rules, PalmAppointment &a,
                              std::string &utc_until, std::string &problem) {
    if (!calendar_palm_date(a.date)) { problem="invalid recurrence start date"; return false; }
    a.repeat=CALENDAR_ONCE; a.interval=1; a.week_start=1; a.repeat_until.clear(); a.exceptions.clear();
    utc_until.clear();
    bool seen=false;
    auto fail=[&]() { problem="recurrence cannot be represented by Palm Date Book"; return false; };
    for (const auto &rule:rules) {
        if (rule.compare(0,6,"RRULE:")==0) {
            if (seen) return fail();
            seen=true; std::map<std::string,std::string> values;
            for (const auto &part:split(rule.substr(6),';')) {
                size_t eq=part.find('=');
                if (eq==std::string::npos || !values.emplace(part.substr(0,eq),part.substr(eq+1)).second) return fail();
            }
            for (const auto &v:values)
                if (v.first!="FREQ" && v.first!="INTERVAL" && v.first!="BYDAY" && v.first!="BYMONTHDAY" &&
                    v.first!="BYMONTH" && v.first!="UNTIL" && v.first!="WKST") return fail();
            if (values.count("INTERVAL")) {
                const auto &v=values["INTERVAL"];
                if (v.empty() || v.size()>3 || v.find_first_not_of("0123456789")!=std::string::npos) return fail();
                a.interval=std::stoul(v); if (!a.interval || a.interval>255) return fail();
            }
            if (values.count("WKST")) {
                int d=weekday(values["WKST"]); if (d<0) return fail(); a.week_start=d;
            }
            const auto &freq=values["FREQ"];
            const auto &day=values["BYDAY"];
            if (freq=="DAILY" && day.empty() && !values.count("BYMONTHDAY") && !values.count("BYMONTH")) a.repeat=CALENDAR_DAILY;
            else if (freq=="WEEKLY" && !values.count("BYMONTHDAY") && !values.count("BYMONTH")) {
                a.repeat=CALENDAR_WEEKLY; a.repeat_on=0;
                if (day.empty()) {
                    int y; unsigned m,d; calendar_date(a.date,y,m,d);
                    int64_t n=mail_seconds(y,m,d,0,0,0)/86400;
                    a.repeat_on=1u<<((n+4)%7+7)%7;
                } else for (const auto &v:split(day,',')) {
                    int d=weekday(v); if (d<0) return fail(); a.repeat_on|=1u<<d;
                }
            } else if (freq=="MONTHLY" && !values.count("BYMONTH")) {
                if (!day.empty()) {
                    int ordinal=0; int d=weekday(day.size()>=3 ? day.substr(day.size()-2) : "");
                    std::string n=day.size()>=3 ? day.substr(0,day.size()-2) : "";
                    if (n=="-1") ordinal=5;
                    else if (n.size()==1 && n[0]>='1' && n[0]<='4') ordinal=n[0]-'0';
                    if (!ordinal || d<0 || values.count("BYMONTHDAY")) return fail();
                    a.repeat=CALENDAR_MONTHLY_DAY; a.repeat_on=(ordinal==5 ? 4 : ordinal-1)*7+d;
                } else {
                    a.repeat=CALENDAR_MONTHLY_DATE;
                    if (values.count("BYMONTHDAY") && values["BYMONTHDAY"]!=std::to_string(std::stoi(a.date.substr(8,2)))) return fail();
                }
            } else if (freq=="YEARLY" && day.empty()) {
                a.repeat=CALENDAR_YEARLY;
                if ((values.count("BYMONTHDAY") && values["BYMONTHDAY"]!=std::to_string(std::stoi(a.date.substr(8,2)))) ||
                    (values.count("BYMONTH") && values["BYMONTH"]!=std::to_string(std::stoi(a.date.substr(5,2))))) return fail();
            } else return fail();
            if (values.count("UNTIL")) {
                const std::string &until=values["UNTIL"];
                if (until.size()==8 && a.start_minute<0) a.repeat_until=expanded(until);
                else if (until.size()==16 && until[8]=='T' && until[15]=='Z' && a.start_minute>=0) {
                    utc_until=expanded(until)+"T"+until.substr(9,2)+":"+until.substr(11,2)+":"+until.substr(13,2)+"Z";
                    int64_t seconds; if (!calendar_timestamp(utc_until,seconds)) return fail();
                } else return fail();
            }
        } else if (rule.compare(0,6,"EXDATE")==0) {
            size_t colon=rule.find(':'); if (colon==std::string::npos) return fail();
            std::string params=rule.substr(6,colon-6);
            if (a.start_minute<0 && params!=";VALUE=DATE" && !params.empty()) return fail();
            if (a.start_minute>=0 && !params.empty() && params.compare(0,6,";TZID=")!=0) return fail();
            for (const auto &ex:split(rule.substr(colon+1),',')) {
                if (a.start_minute<0 && ex.size()!=8) return fail();
                if (a.start_minute>=0) {
                    // Offset/UTC exclusions need time-zone conversion; skip
                    // rather than remove the wrong local recurrence date.
                    char time[32]; snprintf(time,sizeof(time),"T%02d%02d00",a.start_minute/60,a.start_minute%60);
                    if (ex.size()!=15 || ex.substr(8)!=time) return fail();
                }
                a.exceptions.push_back(expanded(ex));
            }
        } else return fail();
    }
    if (!rules.empty() && !seen) return fail();
    if (!seen) a.week_start=0;
    if (!valid(a)) return fail();
    std::sort(a.exceptions.begin(),a.exceptions.end());
    a.exceptions.erase(std::unique(a.exceptions.begin(),a.exceptions.end()),a.exceptions.end());
    return true;
}
