#include "irxiino.h"
#include "config.h"
#include "irsyncdiag.h"
#include "irdebug.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <algorithm>
#include <list>
#include <cstring>
#include <strings.h>
#ifdef IRXIINO_TEST_CLOCK
extern uint32_t irxiino_test_clock();
#elif !defined(ESP_PLATFORM)
#include <chrono>
#endif

namespace {
uint32_t now_ms() {
#ifdef IRXIINO_TEST_CLOCK
    return irxiino_test_clock();
#elif defined(ESP_PLATFORM)
    return millis();
#else
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
struct XiinoRun {
    uint32_t started=0,finished=0,images_started=0;
    size_t raw_bytes=0,output_bytes=0,images=0;
    std::string stage,last_image;
};
XiinoRun last_run;
struct Headers {
    std::string location,type,encoding;
    static esp_err_t event(esp_http_client_event_t *event) {
        if (event->event_id!=HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value) return ESP_OK;
        auto &headers=*(Headers *)event->user_data;
        if (!strcasecmp(event->header_key,"Location")) headers.location=event->header_value;
        if (!strcasecmp(event->header_key,"Content-Type")) headers.type=event->header_value;
        if (!strcasecmp(event->header_key,"Content-Encoding")) headers.encoding=event->header_value;
        return ESP_OK;
    }
};
bool fetch_resource(const std::string &first,size_t limit,XiinoResource &resource,std::string &problem,bool compact_page,uint32_t timeout) {
    std::string url=first;
    const uint32_t started=now_ms();
    resource=XiinoResource();
    for (unsigned hop=0;hop<=PROXY_MAX_REDIRECTS;++hop) {
        if (now_ms()-started>=timeout) { problem="Xiino resource time limit"; return false; }
        if (url.compare(0,7,"http://") && url.compare(0,8,"https://")) {
            problem="Unsupported Xiino resource address"; return false;
        }
        if (url.size()>4096 || url.find_first_of("\r\n")!=url.npos) {
            problem="Invalid or excessive Xiino resource address"; return false;
        }
        if (!irsyncdiag_memory_available(16384,50*1024)) {
            problem="Not enough memory for Xiino HTTP/HTTPS"; return false;
        }
        Headers headers;
        esp_http_client_config_t config={};
        config.url=url.c_str();
        config.crt_bundle_attach=esp_crt_bundle_attach;
        config.timeout_ms=std::max(uint32_t(1),timeout-std::min(timeout,uint32_t(now_ms()-started)));
        config.buffer_size=1024;
        config.buffer_size_tx=std::max(size_t(1024),url.size()+512);
        config.disable_auto_redirect=true;
        config.event_handler=Headers::event;
        config.user_data=&headers;
        esp_http_client_handle_t client=esp_http_client_init(&config);
        if (!client) { problem="Could not initialize Xiino HTTP/HTTPS"; return false; }
        esp_http_client_set_header(client,"User-Agent","OpenXiino/1.0 (http://github.com/nicl83/openxiino) python-requests/2.27.1");
        esp_http_client_set_header(client,"Accept","*/*");
        esp_http_client_set_header(client,"Accept-Encoding","identity");
        esp_err_t err=esp_http_client_open(client,0);
        if (err==ESP_OK) esp_http_client_set_timeout_ms(client,std::max(uint32_t(1),timeout-std::min(timeout,uint32_t(now_ms()-started))));
        if (err==ESP_OK && esp_http_client_fetch_headers(client)<0) err=ESP_ERR_HTTP_FETCH_HEADER;
        int code=err==ESP_OK ? esp_http_client_get_status_code(client) : -1;
        if ((code==301 || code==302 || code==303 || code==307 || code==308) && !headers.location.empty()) {
            esp_http_client_cleanup(client);
            url=xiino_resolve(url,headers.location); continue;
        }
        std::list<std::string> blocks; size_t received=0;
        XiinoSourceFilter filter(resource,limit);
        const size_t input_limit=compact_page ? XIINO_MAX_SOURCE_BYTES : limit;
        char buffer[512];
        if (!headers.encoding.empty() && strcasecmp(headers.encoding.c_str(),"identity")) {
            problem="Origin ignored Xiino's uncompressed transfer request";
            esp_http_client_cleanup(client); return false;
        }
        while (err==ESP_OK) {
            uint32_t elapsed=now_ms()-started;
            if (elapsed>=timeout) { problem="Xiino resource time limit"; break; }
            esp_http_client_set_timeout_ms(client,timeout-elapsed);
            int n=esp_http_client_read(client,buffer,sizeof(buffer));
            if (n<0 || (!n && !esp_http_client_is_complete_data_received(client))) { err=ESP_ERR_HTTP_FETCH_HEADER; break; }
            if (!n) break;
            if (!irsyncdiag_memory_available(1024,12*1024)) { problem="Not enough memory to receive Xiino resource"; break; }
            if (compact_page) {
                size_t take=std::min((size_t)n,input_limit-received);
                filter.feed(buffer,take); received+=take;
                if (filter.full() || take!=(size_t)n) { filter.finish(true); break; }
            } else {
                if ((size_t)n>limit-received) {
                    problem="Xiino image exceeds the "+std::to_string(limit)+"-byte download limit: "+url; break;
                }
                blocks.emplace_back(buffer,n); received+=n;
            }
        }
        // Do not retain a live TLS connection while decoding images or fetching
        // their sibling resources. Chunk framing is consumed by ESP-IDF.
        esp_http_client_cleanup(client);
        if (err!=ESP_OK) { problem="Xiino HTTP/HTTPS transfer failed"; return false; }
        if (!problem.empty()) return false;
        if (compact_page) {
            filter.finish();
        } else {
            if (!irsyncdiag_memory_available(received+1,8*1024)) {
                problem="Not enough memory to assemble Xiino image"; return false;
            }
            resource.body.reserve(received);
            for (const auto &block : blocks) resource.body+=block;
        }
        resource.url=url; resource.content_type=headers.type;
        if (compact_page) last_run.raw_bytes=received;
        irdbg_printf("Xiino: read %u bytes, retained %u%s",(unsigned)received,(unsigned)resource.body.size(),resource.truncated ? " (shortened)" : "");
        return true; // Like requests.get(), retain ordinary HTTP error bodies.
    }
    problem="Too many Xiino redirects"; return false;
}
bool fetch(const std::string &url,size_t limit,XiinoResource &resource,std::string &problem) {
    if (!last_run.images) last_run.images_started=now_ms();
    uint32_t elapsed=now_ms()-last_run.images_started;
    if (elapsed>=XIINO_IMAGES_TIMEOUT_MS || last_run.images>=XIINO_MAX_IMAGES) {
        problem="Optional image time/count budget"; last_run.last_image=problem; return false;
    }
    ++last_run.images;
    if (url.compare(0,5,"data:")==0) {
        size_t comma=url.find(',');
        if (comma==url.npos || comma>128 || url.compare(5,6,"image/")) { problem="Invalid inline image"; return false; }
        resource=XiinoResource(); resource.content_type=url.substr(5,comma-5);
        bool encoded=resource.content_type.find(";base64")!=std::string::npos;
        const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        unsigned held=0,bits=0; bool padding=false;
        auto digit=[](char c)->int { if (c>='0' && c<='9') return c-'0'; if (c>='a' && c<='f') return c-'a'+10; if (c>='A' && c<='F') return c-'A'+10; return -1; };
        for (size_t at=comma+1;at<url.size();++at) {
            char c=url[at];
            if (encoded) {
                if (c==' ' || c=='\r' || c=='\n' || c=='\t') continue;
                if (c=='=') { padding=true; continue; }
                size_t value=alphabet.find(c);
                if (padding || value==alphabet.npos) { problem="Invalid inline base64 image"; return false; }
                held=(held<<6)|(unsigned)value; bits+=6;
                if (bits<8) continue;
                bits-=8; c=(char)(held>>bits);
            } else if (c=='%') {
                if (at+2>=url.size() || digit(url[at+1])<0 || digit(url[at+2])<0) { problem="Invalid inline image encoding"; return false; }
                c=(char)(digit(url[at+1])*16+digit(url[at+2])); at+=2;
            }
            if (resource.body.size()==limit) { problem="Inline image exceeds the download limit"; return false; }
            resource.body+=c;
        }
        if (encoded && (bits==6 || (bits && (held&((1u<<bits)-1))))) { problem="Incomplete inline base64 image"; return false; }
        return true;
    }
    last_run.stage="Fetching image";
    bool ok=fetch_resource(url,limit,resource,problem,false,
        std::min((uint32_t)XIINO_IMAGE_TIMEOUT_MS,(uint32_t)XIINO_IMAGES_TIMEOUT_MS-elapsed));
    if (!ok) last_run.last_image=problem;
    last_run.stage="Converting page";
    return ok;
}
}

bool irxiino_page(const XiinoRequest &request,const std::string &headers,
                  std::string &html,std::string &problem) {
    html.clear();
    return irxiino_stream(request,headers,[]() { return true; },
        [&](const char *bytes,size_t n) { html.append(bytes,n); return true; },problem);
}
bool irxiino_stream(const XiinoRequest &request,const std::string &headers,
                    const std::function<bool()> &begin,const XiinoSink &sink,std::string &problem) {
    problem.clear();
    last_run=XiinoRun(); last_run.started=now_ms(); last_run.stage="Fetching page";
    const std::string builtin=xiino_builtin(request,headers);
    if (!builtin.empty()) {
        bool ok=begin() && sink(builtin.data(),builtin.size());
        last_run.output_bytes=ok ? builtin.size() : 0; last_run.finished=now_ms();
        last_run.stage=ok ? "Complete" : "Palm disconnected";
        return ok;
    }
    irdbg_printf("Xiino: fetching %s",request.url.c_str());
    XiinoLimits limits;
    limits.page_bytes=PROXY_MAX_PAGE_BYTES;
    limits.image_bytes=XIINO_MAX_IMAGE_BYTES;
    limits.image_pixels=XIINO_MAX_IMAGE_PIXELS;
    XiinoResource page;
    bool ok=fetch_resource(request.url,limits.page_bytes,page,problem,true,XIINO_PAGE_TIMEOUT_MS);
    if (ok && (ok=begin())) {
        last_run.stage="Converting page";
        ok=xiino_convert_stream(page,fetch,limits,[&](const char *bytes,size_t size) {
            if (!sink(bytes,size)) return false;
            last_run.output_bytes+=size; return true;
        },problem);
    }
    last_run.finished=now_ms(); last_run.stage=ok ? "Complete" : problem;
    return ok;
}
std::string irxiino_diagnostics() {
    return "Xiino: "+last_run.stage+"; "+std::to_string(last_run.finished-last_run.started)+" ms, source "+
        std::to_string(last_run.raw_bytes)+", sent "+std::to_string(last_run.output_bytes)+", images "+std::to_string(last_run.images)+
        (last_run.last_image.empty() ? "" : "; "+last_run.last_image);
}
