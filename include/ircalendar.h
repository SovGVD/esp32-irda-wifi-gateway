#pragma once
#include <Arduino.h>
#include "config.h"
#include "calendarsync.h"

struct IrCalendarSettings {
    bool enabled = false, cyrillic = false;
    String calendar_id = "primary";
    unsigned past_days = CALENDAR_PAST_DAYS, future_days = CALENDAR_FUTURE_DAYS;
};
void ircalendar_init();
const IrCalendarSettings &ircalendar_settings();
bool ircalendar_store_settings(const IrCalendarSettings &settings);
bool ircalendar_ready();
String ircalendar_last_result();
String ircalendar_check_login();
void ircalendar_sync(CalendarSyncResult &result);
