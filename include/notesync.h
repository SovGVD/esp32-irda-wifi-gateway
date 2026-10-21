#pragma once
#include "googlenotes.h"
struct NoteSyncMap {
    uint32_t palm_id=0;
    std::string google_id,marker;
    std::array<uint32_t,2> fields={};
    uint8_t pending=0; // 1: outgoing initialization, 2: unconfirmed Docs create, 3: reserved Palm import
};
struct NoteSyncState {
    std::string account,username,root_folder,user_folder,folder;
    uint32_t palm_id=0;
    std::vector<NoteSyncMap> map;
};
bool notesync_encode(const NoteSyncState &state,std::string &data);
bool notesync_decode(const std::string &data,unsigned max,NoteSyncState &state);
bool notesync_retry_unconfirmed(NoteSyncState &state);
struct NoteSyncSetup {
    bool drawing=false;
    PalmCharset charset=PALM_WESTERN;
    unsigned max_notes=64;
    size_t content_memory=64000;
    uint8_t conflicts=0; // 0: stop, 1: prefer Palm, 2: prefer Google, per changed field
};
struct NoteSyncResult {
    unsigned uploaded=0,downloaded=0,deleted=0,skipped=0;
    std::string problem;
    bool link_lost=false;
};
class NoteSyncHost {
public:
    virtual ~NoteSyncHost() {}
    virtual GoogleHttp &http()=0;
    virtual bool online()=0;
    virtual int64_t now()=0;
    virtual uint32_t random()=0;
    virtual bool access_token(std::string &token,std::string &problem)=0;
    virtual bool load(NoteSyncState &state)=0;
    virtual bool store(const NoteSyncState &state)=0;
    virtual bool reserve_record_buffer(size_t bytes) {
        size_t room=0; return hotsync_record_buffer(&room) && room>=bytes;
    }
    // Records are decoded before network work. Release extra native-record
    // capacity between operations so it does not compete with TLS allocations.
    virtual bool release_record_buffer() { return true; }
};
void notesync_run(const NoteSyncSetup &setup,NoteSyncHost &host,NoteSyncResult &result);
std::string notesync_summary(const NoteSyncResult &result);
