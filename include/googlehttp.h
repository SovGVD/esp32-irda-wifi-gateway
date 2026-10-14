#pragma once
#include <string>

// Shared HTTPS interface for Google REST clients, also implemented in tests.
class GoogleHttp {
public:
    virtual ~GoogleHttp() {}
    virtual int request(const std::string &url, const char *method,
                        const std::string &authorization, const char *content_type,
                        const std::string &body, std::string &response) = 0;
    // Detail for a negative transport result, without credentials or payloads.
    virtual std::string error() const { return ""; }
    // Calendar updates use an ETag to detect concurrent edits.
    virtual int request_if_match(const std::string &url, const char *method,
                                 const std::string &authorization, const char *content_type,
                                 const std::string &body, const std::string &etag, std::string &response) {
        (void)etag;
        return request(url, method, authorization, content_type, body, response);
    }
};
