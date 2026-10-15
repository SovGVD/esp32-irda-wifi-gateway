#pragma once
#include "googlehttp.h"
#include <stddef.h>

class EspGoogleHttp : public GoogleHttp {
public:
    EspGoogleHttp(bool keep_alive, unsigned timeout_ms, size_t response_max)
        : keep_alive_(keep_alive), timeout_ms_(timeout_ms), response_max_(response_max) {}
    int request(const std::string &url, const char *method, const std::string &auth,
                const char *type, const std::string &body, std::string &reply) override;
    int request_if_match(const std::string &url, const char *method, const std::string &auth,
                         const char *type, const std::string &body, const std::string &etag,
                         std::string &reply) override;
    std::string error() const override { return error_; }
    int transfer(const std::string &url,const char *method,const std::string &auth,const char *type,
                 size_t size,const GoogleRead &read,const GoogleHeaders &headers,std::string &reply,
                 GoogleHeaders &response_headers) override;
    int download(const std::string &url,const std::string &auth,const GoogleHeaders &headers,
                 size_t limit,std::string &reply,GoogleHeaders &response_headers) override;
private:
    int transfer_bounded(const std::string &url,const char *method,const std::string &auth,const char *type,
                         size_t size,const GoogleRead &read,const GoogleHeaders &headers,std::string &reply,
                         GoogleHeaders &response_headers,size_t response_limit);
    struct Response;
    bool keep_alive_;
    unsigned timeout_ms_;
    size_t response_max_;
    std::string error_;
};
