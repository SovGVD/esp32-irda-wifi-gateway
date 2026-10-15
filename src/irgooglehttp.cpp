#include "irgooglehttp.h"
#include "hotsync.h"
#include "irdebug.h"
#include "irsyncdiag.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <climits>
#include <cstdlib>

// Receive without reallocating an ever larger string while TLS owns its heap.
// Every block allocation can fail cleanly; the final contiguous allocation is
// deferred until esp_http_client_cleanup releases TLS and SDK buffers.
class GoogleReplyBlocks {
    struct Block { Block *next; size_t used; char bytes[1024]; };
    Block *first_=nullptr,*last_=nullptr;
    size_t size_=0;
public:
    GoogleReplyBlocks()=default;
    GoogleReplyBlocks(const GoogleReplyBlocks &)=delete;
    GoogleReplyBlocks &operator=(const GoogleReplyBlocks &)=delete;
    ~GoogleReplyBlocks() { clear(); }
    size_t size() const { return size_; }
    bool append(const char *p,size_t n) {
        while (n) {
            if (!last_ || last_->used==sizeof(last_->bytes)) {
                if (!irsyncdiag_memory_available(sizeof(Block),8192)) return false;
                auto *block=(Block *)malloc(sizeof(Block));
                if (!block) return false;
                block->next=nullptr; block->used=0;
                if (last_) last_->next=block; else first_=block;
                last_=block;
            }
            size_t count=std::min(n,sizeof(last_->bytes)-last_->used);
            memcpy(last_->bytes+last_->used,p,count); last_->used+=count;
            size_+=count; p+=count; n-=count;
        }
        return true;
    }
    bool copy_to(std::string &out) const {
        if (size_>out.capacity()) {
            if (!irsyncdiag_memory_available(size_+1,8192)) return false;
            out.reserve(size_);
        }
        for (auto *block=first_;block;block=block->next) out.append(block->bytes,block->used);
        return true;
    }
    void clear() {
        while (first_) { auto *next=first_->next; free(first_); first_=next; }
        last_=nullptr; size_=0;
    }
};

struct EspGoogleHttp::Response {
    EspGoogleHttp *http;
    bool too_large = false;
    GoogleHeaders &headers;
    bool bad_header=false;
    bool low_memory=false;
    Response(EspGoogleHttp *owner,GoogleHeaders &out) : http(owner),headers(out) {}
    static esp_err_t event(esp_http_client_event_t *e) {
        auto *r = (Response *)e->user_data;
        if (r->http->keep_alive_) hotsync_keep_alive();
        if (e->event_id==HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
            std::string key=e->header_key;
            std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c) { return (char)std::tolower(c); });
            if (key=="etag" || key=="location" || key=="content-range" || key=="range") {
                if (strlen(e->header_value)>4096 || r->headers.size()>=8) r->bad_header=true;
                else if (!irsyncdiag_memory_available(strlen(e->header_value)+key.size()+128,8192)) r->low_memory=true;
                else r->headers.emplace_back(key,e->header_value);
            }
        }
        // Streaming reads below collect the body, including bytes cached while
        // fetching headers. Appending here too would duplicate those bytes.
        return ESP_OK;
    }
};
int EspGoogleHttp::request(const std::string &url, const char *method, const std::string &auth,
                           const char *type, const std::string &body, std::string &reply) {
    return request_if_match(url, method, auth, type, body, "", reply);
}
int EspGoogleHttp::request_if_match(const std::string &url, const char *method, const std::string &auth,
                                    const char *type, const std::string &body, const std::string &etag,
                                    std::string &reply) {
    size_t at=0; GoogleHeaders headers,out;
    if (!etag.empty()) headers.emplace_back("If-Match",etag);
    return transfer(url,method,auth,type,body.size(),[&](uint8_t *p,size_t n) {
        if (n>body.size()-at) return false;
        memcpy(p,body.data()+at,n); at+=n; return true;
    },headers,reply,out);
}
int EspGoogleHttp::transfer(const std::string &url,const char *method,const std::string &auth,const char *type,
                            size_t body_size,const GoogleRead &read,const GoogleHeaders &headers,std::string &reply,
                            GoogleHeaders &response_headers) {
    return transfer_bounded(url,method,auth,type,body_size,read,headers,reply,response_headers,response_max_);
}
int EspGoogleHttp::download(const std::string &url,const std::string &auth,const GoogleHeaders &headers,
                            size_t limit,std::string &reply,GoogleHeaders &response_headers) {
    return transfer_bounded(url,"GET",auth,"application/octet-stream",0,{},headers,reply,response_headers,limit);
}
int EspGoogleHttp::transfer_bounded(const std::string &url,const char *method,const std::string &auth,const char *type,
                                   size_t body_size,const GoogleRead &read,const GoogleHeaders &headers,std::string &reply,
                                   GoogleHeaders &response_headers,size_t response_limit) {
    // Previous responses are no longer needed. Release their capacity before
    // establishing another TLS connection, rather than keeping it via clear().
    std::string().swap(reply); error_.clear();
    response_headers.clear();
    if (body_size>INT_MAX || (body_size && !read)) { error_="Invalid Google upload size/source"; return -1; }
    // ESP-IDF 4.x needs the complete request line in its TX buffer. Google
    // pagination tokens and field masks can exceed its default 512 bytes.
    // Also leave room for headers: a nearly-full first line can prevent the
    // first header chunk (and the request line itself) from being sent.
    const size_t overhead=256;
    const size_t max_request=16384;
    size_t tx_size=overhead;
    const size_t pieces[]={url.size(),strlen(method),auth.size(),strlen(type)};
    for (size_t size:pieces) {
        if (size>max_request-tx_size) { error_="Google request URL/headers exceed the 16 KB limit"; return -1; }
        tx_size+=size;
    }
    for (const auto &h:headers) {
        size_t size=h.first.size()+h.second.size()+8;
        if (size>max_request-tx_size || h.first.empty() || h.first.find_first_of("\r\n:")!=std::string::npos ||
            h.second.find_first_of("\r\n")!=std::string::npos) { error_="Invalid or excessive Google request headers"; return -1; }
        tx_size+=size;
    }
    Response response(this,response_headers);
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = timeout_ms_;
    config.buffer_size_tx = (int)std::max((size_t)1024,tx_size);
    // IDF 4.4 dispatches a header on the first parser value fragment, then
    // discards its key. Its default 512-byte RX buffer can therefore truncate
    // Drive's long Location/upload_id and turn a fresh session into HTTP 404.
    // Leave room for all accepted Location bytes and neighbouring headers.
    bool upload_init=(!strcmp(method,"POST") || !strcmp(method,"PATCH")) &&
        url.rfind("https://www.googleapis.com/upload/drive/v3/files",0)==0 && url.find("uploadType=resumable")!=std::string::npos;
    config.buffer_size=upload_init ? 8192 : 1024;
    config.disable_auto_redirect = true;
    config.event_handler = Response::event;
    config.user_data = &response;
    if (keep_alive_) irsyncdiag_checkpoint("Google HTTPS connect");
    // The C3 has no PSRAM. Leave room for TLS/certificates and the HTTP
    // buffers, and require a contiguous TLS input block before opening.
    if (!irsyncdiag_memory_available(std::max((size_t)16384,tx_size),
                                    48*1024+(size_t)config.buffer_size)) {
        error_="Not enough free/contiguous memory for Google HTTPS"; return -1;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { error_="Not enough memory to initialize Google HTTPS"; return -1; }
    esp_http_client_method_t m = HTTP_METHOD_GET;
    if (!strcmp(method, "POST")) m = HTTP_METHOD_POST;
    else if (!strcmp(method, "PUT")) m = HTTP_METHOD_PUT;
    else if (!strcmp(method, "PATCH")) m = HTTP_METHOD_PATCH;
    else if (!strcmp(method, "DELETE")) m = HTTP_METHOD_DELETE;
    else if (strcmp(method,"GET")) {
        error_="Unsupported Google HTTP method"; esp_http_client_cleanup(client); return -1;
    }
    esp_err_t err=esp_http_client_set_method(client,m);
    if (err==ESP_OK) err=esp_http_client_set_header(client,"Content-Type",type);
    if (err==ESP_OK && !auth.empty()) err=esp_http_client_set_header(client,"Authorization",auth.c_str());
    for (const auto &h:headers) if (err==ESP_OK) err=esp_http_client_set_header(client,h.first.c_str(),h.second.c_str());
    if (err!=ESP_OK) {
        error_=std::string("Could not prepare Google HTTPS request: ")+esp_err_to_name(err);
        esp_http_client_cleanup(client); return -1;
    }
    if (keep_alive_) hotsync_keep_alive();
    // ESP-IDF 4.x perform() reports MAX_REDIRECT for *every* non-2xx
    // response when redirects are disabled, including 400/401/403/404/412.
    // Use the streaming API to retain HTTP status and the complete error body
    // without redirecting or resending authenticated writes.
    err=esp_http_client_open(client,(int)body_size);
    if (keep_alive_) irsyncdiag_checkpoint("Google HTTPS transfer");
    size_t sent=0;
    uint8_t upload[1024]; bool source_failed=false;
    while (err==ESP_OK && sent<body_size) {
        if (keep_alive_) hotsync_keep_alive();
        size_t want=std::min(sizeof(upload),body_size-sent),at=0;
        if (!read(upload,want)) { source_failed=true; err=ESP_ERR_HTTP_WRITE_DATA; break; }
        while (err==ESP_OK && at<want) {
            int n=esp_http_client_write(client,(const char *)upload+at,(int)(want-at));
            if (n<=0) err=ESP_ERR_HTTP_WRITE_DATA;
            else { at+=n; sent+=n; }
        }
    }
    if (err==ESP_OK && esp_http_client_fetch_headers(client)<0) err=ESP_ERR_HTTP_FETCH_HEADER;
    int code=err==ESP_OK ? esp_http_client_get_status_code(client) : -1;
    GoogleReplyBlocks received;
    size_t response_bytes=0;
    char chunk[512];
    while (err==ESP_OK && !response.low_memory && !response.bad_header) {
        if (keep_alive_) hotsync_keep_alive();
        int n=esp_http_client_read(client,chunk,sizeof(chunk));
        if (n<0 || (!n && !esp_http_client_is_complete_data_received(client))) {
            err=ESP_ERR_HTTP_FETCH_HEADER; break;
        }
        if (!n) break;
        response_bytes+=(size_t)n;
        if ((size_t)n>response_limit-received.size()) { response.too_large=true; break; }
        if (!received.append(chunk,n)) { response.low_memory=true; break; }
    }
    // Preserve the transport details before destroying its handle. Complete
    // HTTP error bodies are assembled too; transport failures remain failures.
    int socket_error=err!=ESP_OK ? esp_http_client_get_errno(client) : 0;
    esp_http_client_cleanup(client);
    const size_t received_size=response_bytes;
    bool assembly_failed=false;
    if (err==ESP_OK && !response.too_large && !response.bad_header && !response.low_memory && !received.copy_to(reply)) {
        response.low_memory=true; assembly_failed=true;
    }
    received.clear();
    if (err!=ESP_OK || response.too_large || response.bad_header || response.low_memory) code=-1;
    if (response.low_memory) error_=std::string(assembly_failed ? "Not enough contiguous memory to assemble Google HTTPS response" :
        "Not enough memory to receive Google HTTPS response")+" ("+std::to_string(received_size)+" bytes received)";
    else if (response.too_large) error_="Google response exceeds the "+std::to_string(response_limit)+"-byte limit";
    else if (response.bad_header) error_="Google response headers exceed their limit";
    else if (source_failed) error_="Could not read Google upload source";
    else if (err!=ESP_OK) {
        error_=std::string("HTTPS request failed: ")+esp_err_to_name(err);
        if (socket_error) error_+=" (socket errno "+std::to_string(socket_error)+")";
    }
    if (!error_.empty()) irdbg_printf("Google HTTPS: %s",error_.c_str());
    if (keep_alive_) irsyncdiag_checkpoint("Google HTTPS closed");
    if (keep_alive_) hotsync_keep_alive();
    return code;
}
