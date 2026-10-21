#pragma once
#include <Arduino.h>
#include "drivebackup.h"
struct IrDriveSettings {
    bool enabled=false,cyrillic=false,restore_saved=false,retry_interrupted=false;
    String username;
};
void irdrive_init();
const IrDriveSettings &irdrive_settings();
bool irdrive_store_settings(const IrDriveSettings &settings);
bool irdrive_ready();
String irdrive_check_login();
String irdrive_last_result();
void irdrive_record_result(const DriveBackupResult &result);
String irdrive_folder();
bool irdrive_restore_once();
bool irdrive_retry_interrupted();
String irdrive_interrupted_name();
void irdrive_sync(DriveBackupResult &result);
void irdrive_restore(DriveBackupResult &result,const std::function<bool(size_t)> &reserve);
