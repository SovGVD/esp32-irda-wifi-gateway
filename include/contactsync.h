#pragma once
#include "googlecontacts.h"

struct ContactSyncMap {
    uint32_t palm_id=0;
    std::string source_id,resource,marker;
    std::array<uint32_t,CONTACT_SYNC_FIELDS> fields = {};
    // 0: linked; 1: prepared but never sent; 2: create sent, awaiting identity.
    uint8_t pending=0;
};
bool contactsync_encode_map(const std::vector<ContactSyncMap> &map,std::string &data);
bool contactsync_decode_map(const std::string &data,unsigned max,std::vector<ContactSyncMap> &map);
class ContactSyncHost {
public:
    virtual ~ContactSyncHost() {}
    virtual GoogleHttp &http()=0;
    virtual bool online()=0;
    virtual uint32_t random()=0;
    virtual bool access_token(std::string &token,std::string &problem)=0;
    virtual bool bind_account(const std::string &account,uint32_t palm)=0;
    virtual bool load_map(std::vector<ContactSyncMap> &map)=0;
    virtual bool store_map(const std::vector<ContactSyncMap> &map)=0;
};
struct ContactSyncResult {
    unsigned uploaded=0,downloaded=0,deleted=0,skipped=0;
    std::string problem;
    bool link_lost=false;
};
void contactsync_run(const GoogleContactsSetup &setup,PalmCharset charset,ContactSyncHost &host,ContactSyncResult &result);
std::string contactsync_summary(const ContactSyncResult &result);
