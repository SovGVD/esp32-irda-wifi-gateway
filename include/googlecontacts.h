#pragma once
#include "googlehttp.h"
#include "contacttext.h"
#include "mailtext.h"
#include <vector>

struct GoogleContactsSetup { unsigned max_contacts=64; };
struct GoogleContact {
    std::string resource,source_id,source_etag,marker,json;
    bool deleted=false;
};
class GoogleContactsClient {
public:
    GoogleContactsClient(GoogleHttp &http,const GoogleContactsSetup &setup,const std::string &token)
        : http_(http),setup_(setup),token_(token) {}
    bool identity(std::string &account);
    bool list(std::vector<GoogleContact> &contacts);
    bool get(const std::string &resource,GoogleContact &contact,bool &found);
    bool display(const GoogleContact &contact,PalmCharset charset,PalmContact &palm);
    bool save(const GoogleContact *current,const std::string &marker,const PalmContact &palm,
              const std::array<bool,CONTACT_SYNC_FIELDS> &changes,PalmCharset charset,GoogleContact &saved);
    bool remove(const GoogleContact &contact);
    const std::string &error() const { return error_; }
    // A rejected 4xx create can be retried after correction; a lost response
    // or server error is ambiguous and must be reconciled by its marker.
    bool create_rejected() const { return create_rejected_; }
private:
    int request(const std::string &path,const char *method,const std::string &body,std::string &reply,
                const char *permission="Contacts");
    GoogleHttp &http_;
    const GoogleContactsSetup &setup_;
    std::string token_,error_;
    bool create_rejected_=false;
};
