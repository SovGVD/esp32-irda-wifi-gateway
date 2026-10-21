#pragma once
#include "googlehttp.h"
#include <stdint.h>

struct GoogleDriveFile {
    std::string id,name,mime,revision,md5,signature,backup_md5,db_name,etag,sync_marker,version;
    std::vector<std::string> parents;
    uint32_t size=0;
    int64_t modified=0;
    bool folder=false,managed=false,trashed=false;
};
class GoogleDriveClient {
public:
    GoogleDriveClient(GoogleHttp &http,const std::string &token,unsigned max_files=512,size_t budget=64000)
        : http_(http),token_(token),max_files_(max_files),budget_(budget) {}
    bool root(std::string &id);
    bool get(const std::string &id,GoogleDriveFile &file,bool &found);
    bool list(const std::string &parent,std::vector<GoogleDriveFile> &files,const std::string &folder_name="");
    bool folder(const std::string &parent,const std::string &name,std::string &id,
                const std::function<bool()> &checkpoint);
    bool generate_id(std::string &id);
    bool upload(const GoogleDriveFile &target,bool create,const std::string &parent,const std::string &name,
                const std::string &db_name,const std::string &signature,int64_t modified,size_t size,const GoogleRead &read,GoogleDriveFile &saved,
                const std::string &mime="application/octet-stream",const std::string &sync_marker="",
                const std::function<bool()> &rewind={});
    bool read_range(const GoogleDriveFile &file,uint32_t offset,size_t length,std::string &data);
    bool download(const GoogleDriveFile &file,size_t limit,std::string &data);
    const std::string &error() const { return problem_; }
private:
    int call(const std::string &path,const char *method,const std::string &body,std::string &reply,
             const GoogleHeaders &headers={},GoogleHeaders *response_headers=nullptr,bool upload=false,size_t download_limit=0);
    bool decode(const std::string &text,GoogleDriveFile &file);
    GoogleHttp &http_; std::string token_,problem_;
    unsigned max_files_; size_t budget_;
};
// Two small independent caches keep table lookups from evicting payload data.
class GoogleDriveReader {
public:
    GoogleDriveReader(GoogleDriveClient &client,const GoogleDriveFile &file) : client_(client),file_(file) {}
    bool read(uint32_t offset,uint8_t *data,size_t length);
    void table_end(uint32_t end) { table_end_=end; }
private:
    struct Cache { uint32_t offset=0; std::string data; } table_,body_;
    GoogleDriveClient &client_; const GoogleDriveFile &file_; uint32_t table_end_=78;
};
