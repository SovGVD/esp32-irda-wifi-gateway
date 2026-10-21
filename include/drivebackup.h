#pragma once
#include "googledrive.h"
#include "palmbackup.h"
#include "mailtext.h"

struct DriveBackupState {
    std::string account,username,backup_folder,palm_folder,user_folder,recovery_folder;
    uint32_t palm_id=0;
    // u: backup upload; s: saving previous Palm version; r: Palm installation.
    char pending=0;
    std::string pending_file,pending_db,recovery_file;
};
struct DriveBackupSetup {
    unsigned max_databases=512,max_files=512;
    size_t folder_memory=64000;
    PalmCharset charset=PALM_WESTERN;
    std::string username; // Optional folder name for a Palm without a user name
    bool restore_saved=false,retry_interrupted=false;
    bool restore_only=false,backup_only=false;
};
struct DriveBackupResult {
    unsigned uploaded=0,restored=0,unchanged=0,retained=0,ignored=0;
    std::string username,problem;
    bool link_lost=false,reset_needed=false;
};
class DriveBackupHost {
public:
    virtual ~DriveBackupHost() {}
    virtual GoogleHttp &http()=0;
    virtual bool online()=0;
    virtual int64_t now()=0;
    virtual bool access_token(std::string &token,std::string &problem)=0;
    virtual bool load(DriveBackupState &state)=0;
    virtual bool store(const DriveBackupState &state)=0;
    // Called before creating/deleting a Palm database, including rollback.
    virtual bool reserve_record_buffer(size_t bytes) {
        size_t room=0; return hotsync_record_buffer(&room) && room>=bytes;
    }
    virtual void trace(const std::string &text) { (void)text; }
};
void drivebackup_run(const DriveBackupSetup &setup,DriveBackupHost &host,DriveBackupResult &result);
std::string drivebackup_summary(const DriveBackupResult &result);
