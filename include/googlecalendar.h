#pragma once
#include "googlehttp.h"
#include "calendartext.h"
#include <string>
#include <vector>
#include <array>
#include <stdint.h>

struct GoogleCalendarSetup {
    std::string calendar_id = "primary";
    unsigned max_events = 64, past_days = 30, future_days = 90;
};
struct GoogleCalendarInfo {
    std::string id, zone, role;
    std::vector<std::pair<std::string,int>> default_reminders;
};
struct GoogleCalendarEvent {
    std::string id, etag, title, notes, start, end, zone, status, event_type;
    std::string recurring_id, original_date, palm_marker;
    std::vector<std::string> recurrence;
    std::vector<std::pair<std::string,int>> reminders;
    bool all_day = false, writable = true, use_default_reminders = true;
};
class GoogleCalendarClient {
public:
    GoogleCalendarClient(GoogleHttp &http, const GoogleCalendarSetup &setup, const std::string &token)
        : http_(http), setup_(setup), token_(token) {}
    bool open(GoogleCalendarInfo &info);
    bool list(int64_t now, std::vector<GoogleCalendarEvent> &events);
    // Missing is a verified 404/410, never an absent item in a date window.
    bool get(const std::string &id, GoogleCalendarEvent &event, bool &found);
    bool display(const GoogleCalendarEvent &event, PalmAppointment &appointment, std::string &reason);
    // fields: title, notes, date/times, recurrence, alarm. Other event fields survive.
    bool save(const std::string &id, bool create, const std::string &marker, const std::string &etag,
              const PalmAppointment &appointment, const std::array<bool,5> &fields,
              const GoogleCalendarEvent *current, GoogleCalendarEvent &saved);
    bool remove(const std::string &id, const std::string &etag);
    const std::string &error() const { return error_; }
    const GoogleCalendarInfo &info() const { return info_; }
private:
    int request(const std::string &path, const char *method, const std::string &body,
                const std::string &etag, std::string &response);
    bool boundary(const std::string &id, const std::string &local_date, bool last_before,
                  std::string &date, std::string &utc);
    GoogleHttp &http_;
    const GoogleCalendarSetup &setup_;
    std::string token_, error_;
    GoogleCalendarInfo info_;
};
