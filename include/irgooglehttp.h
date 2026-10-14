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
private:
    struct Response;
    bool keep_alive_;
    unsigned timeout_ms_;
    size_t response_max_;
    std::string error_;
};
