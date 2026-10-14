#include "irgooglehttp.h"
#include "hotsync.h"
#include "irdebug.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include <algorithm>
#include <cstring>

struct EspGoogleHttp::Response {
    EspGoogleHttp *http;
    bool too_large = false;
    explicit Response(EspGoogleHttp *owner) : http(owner) {}
    static esp_err_t event(esp_http_client_event_t *e) {
        auto *r = (Response *)e->user_data;
        if (r->http->keep_alive_) hotsync_keep_alive();
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
    reply.clear(); error_.clear();
    // ESP-IDF 4.x needs the complete request line in its TX buffer. Google
    // pagination tokens and field masks can exceed its default 512 bytes.
    // Also leave room for headers: a nearly-full first line can prevent the
    // first header chunk (and the request line itself) from being sent.
    const size_t overhead=256;
    const size_t max_request=16384;
    size_t tx_size=overhead;
    const size_t pieces[]={url.size(),strlen(method),auth.size(),strlen(type),etag.size()};
    for (size_t size:pieces) {
        if (size>max_request-tx_size) { error_="Google request URL/headers exceed the 16 KB limit"; return -1; }
        tx_size+=size;
    }
    Response response(this);
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = timeout_ms_;
    config.buffer_size_tx = (int)std::max((size_t)1024,tx_size);
    config.disable_auto_redirect = true;
    config.event_handler = Response::event;
    config.user_data = &response;
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
    if (err==ESP_OK && !etag.empty()) err=esp_http_client_set_header(client,"If-Match",etag.c_str());
    if (err==ESP_OK && !body.empty()) err=esp_http_client_set_post_field(client,body.data(),body.size());
    if (err!=ESP_OK) {
        error_=std::string("Could not prepare Google HTTPS request: ")+esp_err_to_name(err);
        esp_http_client_cleanup(client); return -1;
    }
    if (keep_alive_) hotsync_keep_alive();
    // ESP-IDF 4.x perform() reports MAX_REDIRECT for *every* non-2xx
    // response when redirects are disabled, including 400/401/403/404/412.
    // Use the streaming API to retain HTTP status and the complete error body
    // without redirecting or resending authenticated writes.
    err=esp_http_client_open(client,(int)body.size());
    size_t sent=0;
    while (err==ESP_OK && sent<body.size()) {
        if (keep_alive_) hotsync_keep_alive();
        int n=esp_http_client_write(client,body.data()+sent,(int)(body.size()-sent));
        if (n<=0) err=ESP_ERR_HTTP_WRITE_DATA;
        else sent+=n;
    }
    if (err==ESP_OK && esp_http_client_fetch_headers(client)<0) err=ESP_ERR_HTTP_FETCH_HEADER;
    int code=err==ESP_OK ? esp_http_client_get_status_code(client) : -1;
    char chunk[512];
    while (err==ESP_OK) {
        if (keep_alive_) hotsync_keep_alive();
        int n=esp_http_client_read(client,chunk,sizeof(chunk));
        if (n<0 || (!n && !esp_http_client_is_complete_data_received(client))) {
            err=ESP_ERR_HTTP_FETCH_HEADER; break;
        }
        if (!n) break;
        if (reply.size()+(size_t)n>response_max_) { response.too_large=true; break; }
        reply.append(chunk,n);
    }
    if (err!=ESP_OK || response.too_large) code=-1;
    if (response.too_large) error_="Google response exceeds the "+std::to_string(response_max_)+"-byte limit";
    else if (err!=ESP_OK) {
        error_=std::string("HTTPS request failed: ")+esp_err_to_name(err);
        int socket_error=esp_http_client_get_errno(client);
        if (socket_error) error_+=" (socket errno "+std::to_string(socket_error)+")";
    }
    if (!error_.empty()) irdbg_printf("Google HTTPS: %s",error_.c_str());
    esp_http_client_cleanup(client);
    if (keep_alive_) hotsync_keep_alive();
    return code;
}
