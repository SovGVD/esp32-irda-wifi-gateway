#pragma once
#include "hotsync.h"
#include "mailtext.h"

struct HotSyncUserSetup {
    std::string name;
    PalmCharset charset=PALM_WESTERN;
};
struct HotSyncUserResult {
    uint32_t id=0;
    std::string name,problem;
    bool written=false;
};
bool hotsync_user_validate(const HotSyncUserSetup &setup,std::string &palm_name,std::string &problem);
// Fill an absent name, preserving the existing numeric identity and all desktop
// sync metadata. Never rename a named Palm or allocate a replacement user ID.
bool hotsync_user_prepare(const HotSyncUserSetup &setup,HotSyncUserResult &result);
