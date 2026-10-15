#pragma once
#include <Arduino.h>
#include "notesync.h"
struct IrNotesSettings {
    bool enabled=false,cyrillic=false;
    uint8_t conflicts=0;
};
void irnotes_init();
const IrNotesSettings &irnotes_settings(bool drawing);
bool irnotes_store_settings(bool drawing,const IrNotesSettings &settings);
bool irnotes_ready(bool drawing);
String irnotes_check_login(bool drawing);
String irnotes_last_result(bool drawing);
String irnotes_folder(bool drawing);
bool irnotes_retry_creations(bool drawing);
void irnotes_sync(bool drawing,NoteSyncResult &result,const std::function<bool(size_t)> &reserve={},
                  const std::function<bool()> &release={});
