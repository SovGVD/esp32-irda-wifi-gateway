#pragma once
#include <string>
#include <stdint.h>
#include <vector>
#include <utility>
#include <functional>

using GoogleHeaders=std::vector<std::pair<std::string,std::string>>;
// Fill exactly the requested bytes, in order. No body-sized allocation.
using GoogleRead=std::function<bool(uint8_t *,size_t)>;

// Shared HTTPS interface for Google REST clients, also implemented in tests.
class GoogleHttp {
public:
    virtual ~GoogleHttp() {}
    virtual int request(const std::string &url, const char *method,
                        const std::string &authorization, const char *content_type,
                        const std::string &body, std::string &response) = 0;
    // Detail for a negative transport result, without credentials or payloads.
    virtual std::string error() const { return ""; }
    virtual int transfer(const std::string &url,const char *method,const std::string &authorization,
                         const char *type,size_t size,const GoogleRead &read,const GoogleHeaders &headers,
                         std::string &response,GoogleHeaders &response_headers) {
        (void)url; (void)method; (void)authorization; (void)type; (void)size; (void)read;
        (void)headers; (void)response; (void)response_headers; return -1;
    }
    // A single binary GET with an explicit response cap. Ordinary API requests
    // retain their configured limit. Adapters may collect blocks while TLS is
    // active and assemble the file only after closing the connection.
    virtual int download(const std::string &url,const std::string &authorization,
                         const GoogleHeaders &headers,size_t limit,std::string &response,GoogleHeaders &out) {
        int code=transfer(url,"GET",authorization,"application/octet-stream",0,{},headers,response,out);
        if (response.size()>limit) { std::string().swap(response); return -1; }
        return code;
    }
    // Calendar updates use an ETag to detect concurrent edits.
    virtual int request_if_match(const std::string &url, const char *method,
                                 const std::string &authorization, const char *content_type,
                                 const std::string &body, const std::string &etag, std::string &response) {
        (void)etag;
        return request(url, method, authorization, content_type, body, response);
    }
};
