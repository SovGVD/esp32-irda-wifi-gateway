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
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
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
    uint32_t started=0,finished=0;
    uint32_t page_ms=0,image_ms=0,send_ms=0,first_byte_ms=0;
    bool running=false,has_output=false;
    size_t raw_bytes=0,output_bytes=0,images=0;
    std::string stage,last_image,first_image;
};
XiinoRun last_run;
#ifdef ESP_PLATFORM
RTC_NOINIT_ATTR SyncDiagnosticState retained_run;
SyncDiagnosticState previous_run;
#endif
bool diagnostics_initialized=false;
void checkpoint(const char *stage) {
#ifdef ESP_PLATFORM
    multi_heap_info_t heap={}; heap_caps_get_info(&heap,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    syncdiag_event(retained_run,stage,heap.total_free_bytes,heap.largest_free_block,
        uxTaskGetStackHighWaterMark(nullptr),now_ms());
#else
    (void)stage;
#endif
}
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
    if (compact_page) {
        // Allocate once before TLS consumes heap. Growing std::string with a
        // live TLS connection briefly needs both the old and new buffers;
        // the per-read 1 KiB admission check cannot protect that larger jump.
        if (!irsyncdiag_memory_available(limit+1,50*1024)) {
            problem="Not enough memory for Xiino page buffer"; return false;
        }
        checkpoint("Reserving Xiino page buffer");
        resource.body.reserve(limit);
    }
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
        checkpoint(compact_page ? "Opening page connection" : "Opening image connection");
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
        checkpoint(compact_page ? "Receiving/filtering page" : "Receiving image");
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
        if (err!=ESP_OK) {
            problem=now_ms()-started>=timeout ? "Xiino resource time limit" : "Xiino HTTP/HTTPS transfer failed ("+std::to_string(err)+")";
            return false;
        }
        if (!problem.empty()) return false;
        if (compact_page) {
            filter.finish();
            // TLS is closed now. Return unused source capacity to the heap
            // before starting sibling image connections/decoders.
            if (resource.body.capacity()>resource.body.size()+4096 &&
                irsyncdiag_memory_available(resource.body.size()+1,24*1024)) {
                std::string compact(resource.body); resource.body.swap(compact);
            }
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
    uint32_t elapsed=last_run.image_ms; // IR delivery/CPU time is not image download time.
    if (elapsed>=XIINO_IMAGES_TIMEOUT_MS || last_run.images>=XIINO_MAX_IMAGES) {
        problem=elapsed>=XIINO_IMAGES_TIMEOUT_MS ? "Optional image download time budget" : "Optional image count budget";
        last_run.last_image=problem; return false;
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
    uint32_t started=now_ms();
    bool ok=fetch_resource(url,limit,resource,problem,false,
        std::min((uint32_t)XIINO_IMAGE_TIMEOUT_MS,(uint32_t)XIINO_IMAGES_TIMEOUT_MS-elapsed));
    last_run.image_ms+=now_ms()-started;
    if (!ok) {
        last_run.last_image=problem;
        if (last_run.first_image.empty()) last_run.first_image=problem;
    }
    last_run.stage="Converting page";
    return ok;
}
}

void irxiino_init() {
    if (diagnostics_initialized) return;
    diagnostics_initialized=true;
#ifdef ESP_PLATFORM
    auto reason=esp_reset_reason();
    if (reason!=ESP_RST_POWERON && reason!=ESP_RST_BROWNOUT && syncdiag_valid(retained_run)) previous_run=retained_run;
    syncdiag_begin(retained_run); syncdiag_finish(retained_run,"No Xiino request since boot",false);
#endif
}
std::string irxiino_previous_diagnostics() {
    irxiino_init();
#ifdef ESP_PLATFORM
    if (syncdiag_valid(previous_run) && previous_run.active && previous_run.count) {
        const auto &last=previous_run.events[(previous_run.next+7)%8];
        return "Xiino interrupted before reset: "+std::string(last.stage)+"; heap "+std::to_string(last.free_heap)+
            ", largest "+std::to_string(last.largest_block)+", stack free "+std::to_string(last.stack_free)+" bytes";
    }
#endif
    return "";
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
    irxiino_init();
#ifdef ESP_PLATFORM
    syncdiag_begin(retained_run); syncdiag_service(retained_run,"Xiino");
#endif
    checkpoint("Starting Xiino request");
    auto finish=[&](bool ok) {
#ifdef ESP_PLATFORM
        syncdiag_finish(retained_run,ok ? "Complete" : "Request failed",!ok);
#else
        (void)ok;
#endif
    };
    last_run=XiinoRun(); last_run.started=now_ms(); last_run.running=true; last_run.stage="Fetching page";
    auto deliver=[&](const char *bytes,size_t size) {
        last_run.stage="Sending page";
        checkpoint("Sending Xiino response");
        uint32_t started=now_ms();
        bool ok=sink(bytes,size);
        last_run.send_ms+=now_ms()-started;
        if (ok && size) {
            if (!last_run.has_output) last_run.first_byte_ms=now_ms()-last_run.started;
            last_run.has_output=true; last_run.output_bytes+=size;
        }
        last_run.stage="Converting page";
        return ok;
    };
    auto start_response=[&]() {
        uint32_t started=now_ms();
        bool ok=begin();
        last_run.send_ms+=now_ms()-started;
        if (!ok) problem="Palm disconnected before Xiino response";
        return ok;
    };
    const std::string builtin=xiino_builtin(request,headers);
    if (!builtin.empty()) {
        bool ok=start_response() && deliver(builtin.data(),builtin.size());
        last_run.finished=now_ms(); last_run.running=false;
        last_run.stage=ok ? "Complete" : "Palm disconnected";
        finish(ok); return ok;
    }
    irdbg_printf("Xiino: fetching %s",request.url.c_str());
    XiinoLimits limits;
    limits.page_bytes=PROXY_MAX_PAGE_BYTES;
    limits.image_bytes=XIINO_MAX_IMAGE_BYTES;
    limits.image_pixels=XIINO_MAX_IMAGE_PIXELS;
    limits.checkpoint=checkpoint;
    XiinoResource page;
    bool ok=fetch_resource(request.url,limits.page_bytes,page,problem,true,XIINO_PAGE_TIMEOUT_MS);
    last_run.page_ms=now_ms()-last_run.started;
    if (ok && (ok=start_response())) {
        last_run.stage="Converting page";
        checkpoint("Converting Xiino HTML");
        ok=xiino_convert_stream(page,fetch,limits,deliver,problem);
    }
    last_run.finished=now_ms(); last_run.running=false; last_run.stage=ok ? "Complete" : problem;
    finish(ok);
    irdbg_printf("Xiino: %s, %lu ms, source %u, sent %u, images %u",last_run.stage.c_str(),
        (unsigned long)(last_run.finished-last_run.started),(unsigned)last_run.raw_bytes,
        (unsigned)last_run.output_bytes,(unsigned)last_run.images);
    irdbg_printf("Xiino ms: origin %lu, images %lu, socket %lu, first body %lu",
        (unsigned long)last_run.page_ms,(unsigned long)last_run.image_ms,
        (unsigned long)last_run.send_ms,(unsigned long)last_run.first_byte_ms);
    return ok;
}
std::string irxiino_diagnostics() {
    if (last_run.stage.empty()) return "Xiino: idle";
    uint32_t duration=(last_run.running ? now_ms() : last_run.finished)-last_run.started;
    uint32_t processing=duration-last_run.page_ms-last_run.image_ms-last_run.send_ms;
    return "Xiino: "+last_run.stage+"; "+std::to_string(duration)+" ms, source "+
        std::to_string(last_run.raw_bytes)+", sent "+std::to_string(last_run.output_bytes)+", images "+std::to_string(last_run.images)+
        "; origin "+std::to_string(last_run.page_ms)+" ms, images "+std::to_string(last_run.image_ms)+
        " ms, processing "+std::to_string(processing)+" ms, socket "+std::to_string(last_run.send_ms)+
        " ms, first body "+(last_run.has_output ? std::to_string(last_run.first_byte_ms)+" ms" : "pending")+
        (last_run.first_image.empty() ? "" : "; first image error: "+last_run.first_image)+
        (last_run.last_image.empty() || last_run.last_image==last_run.first_image ? "" : "; "+last_run.last_image);
}
