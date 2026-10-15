#pragma once
#include "googlesheets.h"
#include "hotsync.h"

struct ExpenseSyncMap {
    uint32_t palm_id=0;
    std::string row_id;
    std::array<uint32_t,10> fields={};
    uint8_t pending=0; // 1: prepared export, 2: unconfirmed append, 3: reserved import
};
struct ExpenseSyncState {
    std::string account,username,root_folder,user_folder,spreadsheet,marker;
    uint32_t palm_id=0;
    bool creating=false,initialized=false;
    std::vector<ExpenseSyncMap> map;
};
bool expensesync_encode(const ExpenseSyncState &state,std::string &data);
bool expensesync_decode(const std::string &data,unsigned max,ExpenseSyncState &state);
bool expensesync_retry_unconfirmed(ExpenseSyncState &state);
struct ExpenseSyncSetup {
    PalmCharset charset=PALM_WESTERN;
    unsigned max_expenses=64;
    size_t content_memory=24000;
    uint8_t conflicts=0; // 0: stop, 1: prefer Palm, 2: prefer Google
};
struct ExpenseSyncResult {
    unsigned uploaded=0,downloaded=0,deleted=0,skipped=0;
    std::string problem;
    bool link_lost=false;
};
class ExpenseSyncHost {
public:
    virtual ~ExpenseSyncHost() {}
    virtual GoogleHttp &http()=0;
    virtual bool online()=0;
    virtual uint32_t random()=0;
    virtual bool access_token(std::string &token,std::string &problem)=0;
    virtual bool load(ExpenseSyncState &state)=0;
    virtual bool store(const ExpenseSyncState &state)=0;
};
void expensesync_run(const ExpenseSyncSetup &setup,ExpenseSyncHost &host,ExpenseSyncResult &result);
std::string expensesync_summary(const ExpenseSyncResult &result);
