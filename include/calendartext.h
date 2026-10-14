#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include <stddef.h>

#define CALENDAR_TITLE_MAX 255
#define CALENDAR_NOTE_MAX 4095
#define CALENDAR_EXCEPTIONS_MAX 64
#define CALENDAR_RECORD_MAX (CALENDAR_TITLE_MAX + CALENDAR_NOTE_MAX + 22 + 2 * CALENDAR_EXCEPTIONS_MAX)

enum CalendarRepeat : uint8_t {
    CALENDAR_ONCE, CALENDAR_DAILY, CALENDAR_WEEKLY, CALENDAR_MONTHLY_DAY,
    CALENDAR_MONTHLY_DATE, CALENDAR_YEARLY
};
struct PalmAppointment {
    std::string title, notes, date; // YYYY-MM-DD, local calendar date
    int start_minute = -1, end_minute = -1; // -1: untimed/all-day
    int alarm_minutes = -1; // -1: no alarm
    CalendarRepeat repeat = CALENDAR_ONCE;
    unsigned interval = 1;
    unsigned repeat_on = 0; // Weekly day mask, or monthly ordinal*7+weekday
    unsigned week_start = 0; // 0 = Sunday
    std::string repeat_until; // Inclusive local date; empty means forever
    std::vector<std::string> exceptions;
};
bool calendar_unpack_record(const uint8_t *data, size_t len, PalmAppointment &appointment);
size_t calendar_pack_record(const PalmAppointment &appointment, uint8_t *data, size_t max_len);
// Gregorian helpers independent of the host/ESP32 process time zone.
bool calendar_date(const std::string &text, int &year, unsigned &month, unsigned &day);
bool calendar_palm_date(const std::string &text);
std::string calendar_add_days(const std::string &date, int days);
std::string calendar_utc(int64_t seconds);
bool calendar_timestamp(const std::string &text, int64_t &utc); // RFC3339 with offset
// RRULE/EXDATE conversion; unsupported recurrence is reported rather than flattened.
std::vector<std::string> calendar_recurrence(const PalmAppointment &appointment);
bool calendar_read_recurrence(const std::vector<std::string> &rules, PalmAppointment &appointment,
                              std::string &utc_until, std::string &problem);
