#pragma once
#include "hotsync.h"
#include "googlehttp.h"
#include <string>
#include <functional>

#define PALM_BACKUP_RECORD_MAX 65489
using PalmBackupRead=std::function<bool(uint32_t,uint8_t *,size_t)>;
// Database calendar dates are Palm local wall time, unlike Drive timestamps.
int64_t palm_backup_time(const uint8_t date[8],int timezone_seconds);
void palm_backup_modified(HotSync_DBInfo &info,int64_t utc,int timezone_seconds);
// A standard PDB/PRC header and validated offset table, read without loading
// the database/file or its complete record table into ESP32 RAM.
struct PalmBackupImage {
    HotSync_DBInfo info={};
    uint32_t size=0,app_offset=0,app_size=0,sort_offset=0,sort_size=0,data_offset=0;
    uint16_t count=0;
    size_t record_buffer_size=0; // Largest app/sort/record/resource block
    struct Entry { uint32_t offset=0,size=0,id=0,type=0; uint16_t resource_id=0; uint8_t attr=0,category=0; };
    bool open(uint32_t bytes,const PalmBackupRead &read,std::string &problem);
    bool entry(unsigned index,const PalmBackupRead &read,Entry &entry,std::string &problem) const;
};
class PalmBackupExport {
public:
    explicit PalmBackupExport(const HotSync_DBInfo &info) : info_(info) {}
    ~PalmBackupExport();
    bool prepare();
    bool read(uint8_t *data,size_t size);
    bool rewind(); // reopen and validate the same snapshot before retrying a 404 session
    bool close();
    uint32_t size() const { return size_; }
    size_t record_buffer_size() const { return record_buffer_size_; }
    std::string signature() const;
    const std::string &error() const { return problem_; }
private:
    bool meta(unsigned index,PalmBackupImage::Entry &entry,uint32_t &hash);
    bool block_size(bool sort,uint32_t &size);
    bool finish();
    bool fail(const char *message);
    HotSync_DBInfo info_;
    uint8_t handle_=0;
    bool opened_=false,finished_=false;
    uint16_t raw_count_=0,count_=0;
    uint32_t size_=0,app_size_=0,sort_size_=0,data_offset_=0,sent_=0;
    uint32_t hash_=2166136261u,table_hash_=2166136261u,body_hash_=2166136261u;
    unsigned stage_=0,index_=0; uint32_t offset_=0,table_offset_=0;
    PalmBackupImage::Entry current_;
    bool have_current_=false;
    std::string piece_,problem_;
    size_t piece_at_=0;
    size_t record_buffer_size_=0;
};
bool palm_backup_install(const PalmBackupImage &image,const PalmBackupRead &read,std::string &problem);
