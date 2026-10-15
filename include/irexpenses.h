#pragma once
#include <Arduino.h>
#include "expensesync.h"
struct IrExpenseSettings {
    bool enabled=false,cyrillic=false;
    uint8_t conflicts=0;
};
void irexpenses_init();
const IrExpenseSettings &irexpenses_settings();
bool irexpenses_store_settings(const IrExpenseSettings &settings);
bool irexpenses_ready();
String irexpenses_check_login();
String irexpenses_last_result();
String irexpenses_sheet_url();
bool irexpenses_retry_creations();
void irexpenses_sync(ExpenseSyncResult &result);
