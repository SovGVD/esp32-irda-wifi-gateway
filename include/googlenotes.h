#pragma once
#include "googledrive.h"
#include "notetext.h"
struct GoogleNote {
    GoogleDriveFile file;
    PalmNote note; // UTF-8 text/title, monochrome bitmap
    std::string revision,tab;
    unsigned end=1;
    bool plain=true;
};
class GoogleNotesClient {
public:
    GoogleNotesClient(GoogleHttp &http,const std::string &token,unsigned limit) : drive(http,token,limit,24000),http_(http),token_(token) {}
    GoogleDriveClient drive;
    bool read(const GoogleDriveFile &file,bool drawing,GoogleNote &note);
    bool create_memo(const std::string &folder,const std::string &title,const std::string &marker,GoogleDriveFile &file);
    bool write_memo(const GoogleNote &old,const std::string &text);
    bool write_drawing(const GoogleDriveFile &old,bool create,const std::string &folder,const PalmNote &note,const std::string &title,const std::string &marker,int64_t now,GoogleDriveFile &saved);
    bool trash(const GoogleDriveFile &old);
    bool rename_drawing(const GoogleDriveFile &old,const std::string &title);
    const std::string &error() const { return error_; }
private:
    bool call(const std::string &url,const char *method,const std::string &body,std::string &reply,const std::string &etag="");
    GoogleHttp &http_; std::string token_,error_;
};
