// Run the actual ESP32 HTTPS adapter with an SDK shim. Its request-line limit
// models ESP-IDF 4.x http_client_prepare_first_line and header chunk sizing.
#include "irgooglehttp.h"
#include "googlecalendar.h"
#include "googlecontacts.h"
#include "googletasks.h"
#include "esp_http_client.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include <algorithm>
#include <cstring>

struct MockHttpClient {
    esp_http_client_config_t config;
    esp_http_client_method_t method=HTTP_METHOD_GET;
    std::string url,body,response,written;
    size_t read_at=0,announced=0;
    std::map<std::string,std::string> headers;
};
static bool force_default_buffer=false,fail_init=false,fail_header=false;
static bool fail_read=false,short_body=false,partial_writes=false;
static esp_err_t perform_error=ESP_OK;
static int status=200,socket_error=0,keepalives=0,inits=0,performs=0,cleanups=0;
static size_t reply_index=0;
static std::vector<std::string> replies,urls;
static std::map<std::string,std::string> last_headers;
static void reset(const std::vector<std::string> &script={"ok"}) {
    force_default_buffer=fail_init=fail_header=false; perform_error=ESP_OK; status=200; socket_error=0;
    fail_read=short_body=partial_writes=false;
    inits=performs=cleanups=keepalives=0; reply_index=0; replies=script; urls.clear(); last_headers.clear();
}
const char *esp_err_to_name(esp_err_t e) {
    if (e==ESP_ERR_HTTP_FETCH_HEADER) return "ESP_ERR_HTTP_FETCH_HEADER";
    if (e==ESP_ERR_HTTP_MAX_REDIRECT) return "ESP_ERR_HTTP_MAX_REDIRECT";
    return e==ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : e==ESP_ERR_HTTP_CONNECT ? "ESP_ERR_HTTP_CONNECT" : "ESP_FAIL";
}
esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }
void hotsync_keep_alive() { keepalives++; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    inits++; if (fail_init) return nullptr;
    assert(config->crt_bundle_attach && config->disable_auto_redirect && config->timeout_ms==12000);
    auto *c=new MockHttpClient; c->config=*config; c->url=config->url; return c;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c,esp_http_client_method_t method) { c->method=method; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value) {
    if (fail_header) return ESP_ERR_NO_MEM;
    c->headers[key]=value; return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c,const char *data,int len) { c->body.assign(data,len); return ESP_OK; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c,int) {
    performs++; urls.push_back(c->url); last_headers=c->headers;
    if (perform_error!=ESP_OK) return perform_error;
    auto at=c->url.find('/',c->url.find("://")+3);
    assert(at!=std::string::npos);
    const char *methods[]={"GET","POST","PUT","PATCH","DELETE"};
    std::string first_line=std::string(methods[c->method])+" "+c->url.substr(at)+" HTTP/1.1\r\n";
    int buffer=force_default_buffer ? 512 : c->config.buffer_size_tx;
    // The request line has to fit, with a terminator and at least the first
    // Host header; otherwise the SDK either rejects it or sends no first line.
    if (first_line.size()+1>= (size_t)buffer || first_line.size()+32>= (size_t)buffer) return ESP_FAIL;
    for (const auto &header:c->headers) assert(header.first.size()+header.second.size()+7 < (size_t)buffer);
    assert(reply_index<replies.size()); c->response=replies[reply_index++];
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c,const char *data,int len) {
    int n=partial_writes ? std::min(len,3) : len; c->written.append(data,n); return n;
}
int esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    // The parser can dispatch body bytes while fetching headers. Those bytes
    // are then returned again from its cache by read(), but only once to users.
    c->announced=std::min((size_t)128,c->response.size());
    if (c->announced && c->config.event_handler) {
        esp_http_client_event_t event={HTTP_EVENT_ON_DATA,c->config.user_data,&c->response[0],(int)c->announced};
        c->config.event_handler(&event);
    }
    return (int)c->response.size();
}
int esp_http_client_read(esp_http_client_handle_t c,char *data,int len) {
    if (fail_read) return -1;
    if (short_body) return 0;
    size_t n=std::min((size_t)len,c->response.size()-c->read_at);
    memcpy(data,c->response.data()+c->read_at,n);
    size_t fresh=std::max(c->announced,c->read_at);
    if (fresh<c->read_at+n && c->config.event_handler) {
        esp_http_client_event_t event={HTTP_EVENT_ON_DATA,c->config.user_data,&c->response[fresh],(int)(c->read_at+n-fresh)};
        c->config.event_handler(&event);
    }
    c->read_at+=n; return (int)n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return c->read_at==c->response.size(); }
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    auto err=esp_http_client_open(c,0); if (err!=ESP_OK) return err;
    esp_http_client_fetch_headers(c);
    // This is the installed SDK's esp_http_check_response behavior: the
    // disabled-redirect check precedes its HTTP status switch.
    if (c->config.disable_auto_redirect && (status<200 || status>=300)) return ESP_ERR_HTTP_MAX_REDIRECT;
    return ESP_OK;
}
int esp_http_client_get_status_code(esp_http_client_handle_t) { return status; }
int esp_http_client_get_errno(esp_http_client_handle_t) { return socket_error; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    if (partial_writes) assert(c->body==c->written);
    cleanups++; delete c; return ESP_OK;
}

int main() {
    std::string token="test-token";
    const std::string page=std::string(1200,'p');
    const std::vector<std::string> calendar_replies={
        "{\"kind\":\"calendar#calendarListEntry\",\"id\":\"calendar@example.com\",\"timeZone\":\"Pacific/Auckland\",\"accessRole\":\"owner\"}",
        "{\"kind\":\"calendar#events\",\"items\":[],\"nextPageToken\":\""+page+"\"}",
        "{\"kind\":\"calendar#events\",\"items\":[]}"};
    GoogleCalendarSetup calendar_setup; GoogleCalendarInfo info; std::vector<GoogleCalendarEvent> events;
    EspGoogleHttp http(false,12000,32000);
    GoogleCalendarClient calendar(http,calendar_setup,token);
    reset(calendar_replies); force_default_buffer=true;
    assert(calendar.open(info) && !calendar.list(1791936000,events));
    assert(calendar.error().find("ESP_FAIL")!=std::string::npos);
    reset(calendar_replies);
    assert(calendar.open(info) && calendar.list(1791936000,events) && events.empty());
    assert(urls.back().size()>512 && !keepalives && cleanups==3);

    const std::vector<std::string> contacts_replies={
        "{\"resourceName\":\"people/account1\"}",
        "{\"connections\":[],\"totalItems\":0,\"nextPageToken\":\""+page+"\"}",
        "{\"totalItems\":0}"};
    GoogleContactsSetup contacts_setup; GoogleContactsClient contacts(http,contacts_setup,token);
    std::string account; std::vector<GoogleContact> people;
    reset(contacts_replies); force_default_buffer=true;
    assert(contacts.identity(account) && !contacts.list(people));
    assert(contacts.error().find("ESP_FAIL")!=std::string::npos);
    reset(contacts_replies);
    assert(contacts.identity(account) && contacts.list(people) && people.empty());
    assert(urls.back().size()>512 && cleanups==3);

    // Failures are distinct from HTTP status responses, and details clear on
    // the next request. Never disclose the bearer credential in an error.
    reset(); perform_error=ESP_ERR_HTTP_CONNECT; socket_error=111;
    assert(!contacts.identity(account));
    assert(contacts.error().find("ESP_ERR_HTTP_CONNECT")!=std::string::npos && contacts.error().find("111")!=std::string::npos &&
           contacts.error().find(token)==std::string::npos && cleanups==1);
    reset(contacts_replies); assert(contacts.identity(account) && http.error().empty());
    reset(); fail_header=true; std::string reply;
    assert(http.request("https://people.googleapis.com/v1/people/me","GET","Bearer "+token,"application/json","",reply)<0);
    assert(!performs && cleanups==1 && http.error().find("ESP_ERR_NO_MEM")!=std::string::npos);
    reset(); fail_init=true;
    assert(http.request("https://people.googleapis.com/v1/people/me","GET","Bearer "+token,"application/json","",reply)<0 && !performs);
    assert(http.error().find("memory")!=std::string::npos);
    reset({"12345"}); EspGoogleHttp bounded(true,12000,4);
    assert(bounded.request("https://people.googleapis.com/v1/people/me","GET","Bearer "+token,"application/json","",reply)<0);
    assert(reply.size()<=4 && bounded.error().find("4-byte limit")!=std::string::npos && keepalives>=3 && cleanups==1);
    reset();
    assert(http.request("https://people.googleapis.com/"+std::string(16384,'x'),"GET","","application/json","",reply)<0 && !inits);
    assert(http.error().find("16 KB")!=std::string::npos);
    reset();
    assert(http.request_if_match("https://www.googleapis.com/calendar/v3/calendars/primary/events/id","PATCH",
                                "Bearer "+std::string(900,'t'),"application/json","{}","\"etag\"",reply)==200);
    assert(last_headers["If-Match"]=="\"etag\"" && http.error().empty());
    reset(); status=403; assert(!contacts.identity(account));
    assert(contacts.error().find("HTTP 403")!=std::string::npos && http.error().empty());
    // SDK perform() masks an ordinary Google 403 with MAX_REDIRECT. The real
    // adapter must retain that status and the full body using streamed reads.
    const std::string disabled="{\"error\":{\"message\":\"People API has not been used or is disabled\",\"details\":[{\"reason\":\"SERVICE_DISABLED\"}],\"padding\":\""+std::string(1500,'x')+"\"}}";
    reset({disabled}); status=403;
    esp_http_client_config_t cfg={}; cfg.url="https://people.googleapis.com/v1/people/me";
    cfg.timeout_ms=12000; cfg.buffer_size_tx=1024; cfg.disable_auto_redirect=true; cfg.crt_bundle_attach=esp_crt_bundle_attach;
    auto masked=esp_http_client_init(&cfg); assert(esp_http_client_perform(masked)==ESP_ERR_HTTP_MAX_REDIRECT); esp_http_client_cleanup(masked);
    reset({disabled}); status=403;
    assert(!contacts.identity(account) && contacts.error().find("Enable People API")!=std::string::npos &&
           contacts.error().find("MAX_REDIRECT")==std::string::npos && performs==1 && http.error().empty());
    for (int code:{400,401,403,404,410,412,429,500,302,307,308}) {
        reset({disabled}); status=code;
        assert(http.request("https://people.googleapis.com/v1/people/me","GET","Bearer "+token,"application/json","",reply)==code);
        assert(reply==disabled && performs==1 && cleanups==1 && http.error().empty());
    }
    reset({"{\"error\":{\"message\":\"Insufficient scopes\",\"details\":[{\"reason\":\"ACCESS_TOKEN_SCOPE_INSUFFICIENT\"}]}}"}); status=403;
    account="stale-account";
    assert(!contacts.identity(account) && account.empty() && contacts.error().find("basic profile permission")!=std::string::npos &&
           contacts.error().find("Sign in again")!=std::string::npos && performs==1);
    const std::string legacy_scope_error="{\"error\":{\"message\":\"Insufficient Permission\",\"errors\":[{\"reason\":\"insufficientPermissions\"}]}}";
    reset({legacy_scope_error}); status=403;
    assert(!contacts.identity(account) && contacts.error().find("basic profile permission")!=std::string::npos);
    reset({legacy_scope_error}); status=403;
    assert(!contacts.list(people) && contacts.error().find("Contacts permission")!=std::string::npos &&
           contacts.error().find("basic profile")==std::string::npos);
    reset({"{\"error\":{\"message\":\"Request had insufficient authentication scopes.\",\"status\":\"PERMISSION_DENIED\"}}"}); status=403;
    assert(!contacts.identity(account) && contacts.error().find("basic profile permission")!=std::string::npos);
    reset({"{\"error\":{\"message\":\"Request quota exhausted\"}}"}); status=429;
    assert(!contacts.identity(account) && contacts.error().find("Request quota exhausted")!=std::string::npos);
    reset({"abcdef"}); short_body=true;
    assert(http.request("https://people.googleapis.com/v1/people/me","GET","Bearer "+token,"application/json","",reply)<0 &&
           http.error().find("ESP_ERR_HTTP_FETCH_HEADER")!=std::string::npos);
    reset({"{}"}); partial_writes=true;
    assert(http.request("https://people.googleapis.com/v1/people:createContact","POST","Bearer "+token,"application/json","{\"x\":123}",reply)==200);
    assert(reply=="{}" && performs==1);
    reset({"{\"id\":\"list-id\"}","{\"kind\":\"tasks#tasks\",\"items\":[]}"});
    GoogleTasksSetup tasks_setup; GoogleTasksClient tasks(http,tasks_setup,token); std::string id; std::vector<GoogleTask> todo;
    assert(tasks.resolve_list(id) && tasks.list(todo));
    puts("googlehttp: all passed (long URLs, unmasked HTTP errors, complete streaming, bounded responses, no redirects/retries, Tasks)");
}
