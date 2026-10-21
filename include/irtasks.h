#pragma once
#include <Arduino.h>
#include "tasksync.h"

struct IrTasksSettings {
    bool enabled = false, cyrillic = false;
    String list_id = "@default";
};
void irtasks_init();
const IrTasksSettings &irtasks_settings();
bool irtasks_store_settings(const IrTasksSettings &settings);
bool irtasks_ready();
String irtasks_last_result();
String irtasks_check_login();
// Run within the existing accepted HotSync session (does not end it).
void irtasks_sync(TaskSyncResult &result);
