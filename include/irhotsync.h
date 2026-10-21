#pragma once
#include <Arduino.h>
#include "hotsyncuser.h"
struct IrHotSyncSettings {
    String username;
    bool cyrillic=false;
};
void irhotsync_init();
const IrHotSyncSettings &irhotsync_settings();
bool irhotsync_store_settings(const IrHotSyncSettings &settings,String &problem);
bool irhotsync_prepare(String &problem);
String irhotsync_last_result();
