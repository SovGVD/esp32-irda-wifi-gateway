#pragma once
#include <Arduino.h>
#include "contactsync.h"

struct IrContactsSettings { bool enabled=false,cyrillic=false; };
void ircontacts_init();
const IrContactsSettings &ircontacts_settings();
bool ircontacts_store_settings(const IrContactsSettings &settings);
bool ircontacts_ready();
String ircontacts_check_login();
String ircontacts_last_result();
bool ircontacts_has_pending();
bool ircontacts_retry_pending();
void ircontacts_sync(ContactSyncResult &result);
