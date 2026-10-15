// Run the actual ESP32 HTTPS adapter with an SDK shim. Its request-line limit
// models ESP-IDF 4.x http_client_prepare_first_line and header chunk sizing.
#include "irgooglehttp.h"
#include "irsyncdiag.h"
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
static size_t available_heap=1000000,largest_heap=1000000;
static bool client_active=false,large_live_allocation=false;
void irsyncdiag_checkpoint(const char *) {}
bool irsyncdiag_memory_available(size_t allocation,size_t reserve) {
    if (client_active && allocation>2048) large_live_allocation=true;
    return available_heap>=reserve && allocation<=available_heap-reserve && largest_heap>=allocation;
}
static bool low_receive_memory=false,fragmented_receive=false,fail_assembly=false;
static bool force_default_buffer=false,fail_init=false,fail_header=false;
static bool fail_read=false,short_body=false,partial_writes=false;
static bool force_default_receive=false;
static esp_err_t perform_error=ESP_OK;
static int status=200,socket_error=0,keepalives=0,inits=0,performs=0,cleanups=0;
static size_t reply_index=0;
static std::vector<std::string> replies,urls;
static std::map<std::string,std::string> last_headers;
static GoogleHeaders response_headers;
static std::string last_written;
static void reset(const std::vector<std::string> &script={"ok"}) {
    available_heap=largest_heap=1000000; low_receive_memory=fragmented_receive=fail_assembly=false;
    client_active=large_live_allocation=false;
    force_default_buffer=fail_init=fail_header=false; perform_error=ESP_OK; status=200; socket_error=0;
    fail_read=short_body=partial_writes=false;
    force_default_receive=false;
    inits=performs=cleanups=keepalives=0; reply_index=0; replies=script; urls.clear(); last_headers.clear();
    response_headers.clear(); last_written.clear();
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
    client_active=true;
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
    for (const auto &h:response_headers) if (c->config.event_handler) {
        // IDF 4.4 dispatches the first value fragment, then frees its key.
        unsigned receive=force_default_receive || !c->config.buffer_size ? 512 : c->config.buffer_size;
        std::string value=h.second.substr(0,receive-96);
        esp_http_client_event_t event={HTTP_EVENT_ON_HEADER,c->config.user_data,nullptr,0,h.first.c_str(),value.c_str()};
        c->config.event_handler(&event);
    }
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
    if (low_receive_memory) available_heap=9000;
    if (fragmented_receive) { available_heap=70000-c->read_at; largest_heap=2048; }
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
    last_written=c->written;
    cleanups++; client_active=false;
    if (fragmented_receive) { available_heap=120000; largest_heap=fail_assembly ? 2048 : 64000; }
    delete c; return ESP_OK;
}

int main() {
    {
        EspGoogleHttp http(true,12000,64000); std::string body;
        reset(); available_heap=40000;
        assert(http.request("https://example.test/","GET","","text/plain","",body)==-1);
        assert(http.error().find("memory")!=std::string::npos && inits==0);
        reset(); largest_heap=8000;
        assert(http.request("https://example.test/","GET","","text/plain","",body)==-1 && inits==0);
        reset({std::string(5000,'x')}); low_receive_memory=true;
        assert(http.request("https://example.test/","GET","","text/plain","",body)==-1);
        assert(http.error().find("memory")!=std::string::npos && cleanups==1 && performs==1);
    }

    {
        // A fragmented TLS heap can satisfy 1 KiB blocks but cannot grow a
        // 32,000-byte string. A large allocation becomes possible only on cleanup.
        EspGoogleHttp http(true,12000,64000); std::string reply;
        std::string complete; for (unsigned i=0;i<32000;i++) complete+=(char)('a'+i%26);
        reset({complete}); fragmented_receive=true;
        assert(http.request("https://example.test/large","GET","","text/plain","",reply)==200);
        assert(reply==complete && !large_live_allocation && performs==1 && cleanups==1 && http.error().empty());
        reset({complete}); fragmented_receive=fail_assembly=true;
        assert(http.request("https://example.test/write","POST","Bearer secret","text/plain","payload",reply)==-1);
        assert(reply.empty() && http.error().find("assemble")!=std::string::npos && http.error().find("32000")!=std::string::npos &&
               http.error().find("secret")==std::string::npos && performs==1 && cleanups==1 && last_written=="payload");
        reset({"ok"});
        assert(http.request("https://example.test/retry","GET","","text/plain","",reply)==200 && reply=="ok" && http.error().empty());
    }

    std::string token="test-token";
    {
        // A grayscale PNG may exceed the JSON cap. Download it once without
        // an accumulated file buffer beside subsequent TLS handshakes.
        EspGoogleHttp binary(true,12000,32000); std::string reply; GoogleHeaders out;
        const std::string image(56729,'p');
        reset({image}); fragmented_receive=true;
        assert(binary.download("https://example.test/image.png","Bearer token",{},image.size(),reply,out)==200);
        assert(reply==image && !large_live_allocation && performs==1 && cleanups==1);
        reset({image});
        assert(binary.download("https://example.test/image.png","Bearer token",{},image.size()-1,reply,out)==-1);
        assert(reply.empty() && binary.error().find(std::to_string(image.size()-1))!=std::string::npos && performs==1);
        reset({image});
        assert(binary.request("https://example.test/json","GET","","application/json","",reply)==-1);
        assert(reply.empty() && binary.error().find("32000-byte limit")!=std::string::npos);
        reset({image}); fail_read=true;
        assert(binary.download("https://example.test/image.png","Bearer token",{},image.size(),reply,out)==-1 && reply.empty() && performs==1);
    }
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
    assert(last_written=="{\"x\":123}");
    reset({"{}"}); partial_writes=true;
    response_headers={{"Location","https://www.googleapis.com/upload/drive/v3/session"},{"ETag","\"file-v1\""}};
    size_t produced=0; GoogleHeaders returned;
    assert(http.transfer("https://www.googleapis.com/upload/drive/v3/session","PUT","Bearer "+token,"application/octet-stream",300000,
        [&](uint8_t *p,size_t n) { assert(n<=1024); memset(p,'b',n); produced+=n; return true; },
        {{"Content-Range","bytes 0-299999/300000"}},reply,returned)==200);
    assert(produced==300000 && last_written==std::string(300000,'b') && returned.size()==2 && returned[0].first=="location" &&
           returned[1].second=="\"file-v1\"" && last_headers["Content-Range"]=="bytes 0-299999/300000");
    const std::string session="https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&upload_id="+std::string(2100,'u');
    reset({""}); force_default_receive=true; response_headers={{"Location",session}};
    assert(http.transfer("https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable","POST","Bearer "+token,"application/json",0,{}, {},reply,returned)==200);
    assert(returned.size()==1 && returned[0].second!=session);
    reset({""}); response_headers={{"Location",session}};
    assert(http.transfer("https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable","POST","Bearer "+token,"application/json",0,{}, {},reply,returned)==200);
    assert(returned.size()==1 && returned[0].second==session);
    reset({"{}"});
    assert(http.request(session,"PUT","Bearer "+token,"application/octet-stream","payload",reply)==200 && urls[0]==session && last_written=="payload");
    reset();
    assert(http.transfer("https://www.googleapis.com/upload/drive/v3/session","PUT","Bearer "+token,"application/octet-stream",12,
        [&](uint8_t *,size_t) { return false; },{},reply,returned)<0 && http.error().find("upload source")!=std::string::npos);
    reset({"{\"id\":\"list-id\"}","{\"kind\":\"tasks#tasks\",\"items\":[]}"});
    GoogleTasksSetup tasks_setup; GoogleTasksClient tasks(http,tasks_setup,token); std::string id; std::vector<GoogleTask> todo;
    assert(tasks.resolve_list(id) && tasks.list(todo));
    puts("googlehttp: all passed (long URLs, unmasked HTTP errors, complete streaming, bounded responses, no redirects/retries, Tasks)");
}
