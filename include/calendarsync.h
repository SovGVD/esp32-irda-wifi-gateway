#pragma once
#include "googlecalendar.h"
#include "mailtext.h"
#include <array>

struct CalendarSyncMap {
    uint32_t palm_id = 0;
    std::string google_id;
    std::array<uint32_t,5> fields = {}; // title, notes, date/times, recurrence, alarm
    bool pending_create = false, suppressed = false;
};
// Compact, versioned persistent state; independent of ESP32 NVS for tests.
bool calendarsync_encode_map(const std::vector<CalendarSyncMap> &map, std::string &data);
bool calendarsync_decode_map(const std::string &data, unsigned max_events, std::vector<CalendarSyncMap> &map);
class CalendarSyncHost {
public:
    virtual ~CalendarSyncHost() {}
    virtual GoogleHttp &http() = 0;
    virtual bool online() = 0;
    virtual int64_t now() = 0;
    virtual uint32_t random() = 0;
    virtual bool access_token(std::string &token, std::string &problem) = 0;
    virtual bool bind_calendar(const std::string &id, uint32_t palm_user) = 0;
    virtual bool load_map(std::vector<CalendarSyncMap> &map) = 0;
    virtual bool store_map(const std::vector<CalendarSyncMap> &map) = 0;
};
struct CalendarSyncResult {
    unsigned uploaded = 0, downloaded = 0, deleted = 0, skipped = 0;
    std::string problem, skipped_reason;
    bool link_lost = false;
};
void calendarsync_run(const GoogleCalendarSetup &setup, PalmCharset charset,
                      CalendarSyncHost &host, CalendarSyncResult &result);
std::string calendarsync_summary(const CalendarSyncResult &result);
