#include "irxiino.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <fstream>
#include <iterator>
#include "data/xiino_fixtures.h"

struct Reply {
    std::string url,body;
    int code=200;
    std::map<std::string,std::string> headers;
    bool open_error=false,truncated=false;
    size_t split=512;
    unsigned delay_open=0,delay_read=0;
};
struct MockHttpClient {
    esp_http_client_config_t config;
    Reply reply;
    size_t offset=0;
    std::map<std::string,std::string> headers;
    int timeout=0;
};
static std::deque<Reply> replies;
static unsigned active=0,created=0;
static bool memory_available=true;
static size_t available_block=1024*1024;
static bool fixture_images=false;
static std::string live_logo;
static std::map<std::string,std::string> captured_images;
static size_t last_read=0;
static uint32_t clock_ms=100;
static std::string *expect_text_before_image=nullptr;
uint32_t irxiino_test_clock() { return clock_ms; }
esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }
bool irsyncdiag_memory_available(size_t allocation,size_t) { return memory_available && allocation<=available_block; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    assert(!active);
    Reply reply;
    if (replies.empty() && fixture_images) {
        reply.url=config->url;
        if (!captured_images.empty()) {
            auto asset=captured_images.find(reply.url); assert(asset!=captured_images.end()); reply.body=asset->second;
        } else reply.body=live_logo.empty() ? std::string(image_fixtures[0].bytes,image_fixtures[0].size) : live_logo;
    } else {
        assert(!replies.empty()); reply=replies.front(); replies.pop_front();
    }
    assert(reply.url==config->url && config->disable_auto_redirect);
    if (expect_text_before_image && reply.url.find("/image.png")!=reply.url.npos)
        assert(expect_text_before_image->find("Text before image")!=std::string::npos);
    assert(config->crt_bundle_attach && config->buffer_size_tx>=(int)reply.url.size()+128);
    ++active; ++created;
    auto *client=new MockHttpClient;
    client->config=*config; client->reply=std::move(reply); client->timeout=config->timeout_ms; return client;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c,int timeout) { assert(timeout>0); c->timeout=timeout; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value) {
    c->headers[key]=value; return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c,int length) {
    assert(length==0 && c->headers["Accept-Encoding"]=="identity");
    assert(c->headers["User-Agent"].find("OpenXiino/1.0")==0);
    clock_ms+=std::min((unsigned)c->timeout,c->reply.delay_open);
    if (c->reply.delay_open>=(unsigned)c->timeout) return ESP_ERR_HTTP_FETCH_HEADER;
    return c->reply.open_error ? ESP_ERR_HTTP_FETCH_HEADER : ESP_OK;
}
int esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    for (const auto &header : c->reply.headers) {
        esp_http_client_event_t event={};
        event.event_id=HTTP_EVENT_ON_HEADER; event.user_data=c->config.user_data;
        event.header_key=header.first.c_str(); event.header_value=header.second.c_str();
        assert(c->config.event_handler(&event)==ESP_OK);
    }
    return c->reply.body.size();
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->reply.code; }
int esp_http_client_read(esp_http_client_handle_t c,char *data,int length) {
    clock_ms+=std::min((unsigned)c->timeout,c->reply.delay_read);
    if (c->reply.delay_read>=(unsigned)c->timeout) return -1;
    size_t n=std::min({(size_t)length,c->reply.split,c->reply.body.size()-c->offset});
    std::memcpy(data,c->reply.body.data()+c->offset,n); c->offset+=n; return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
    return c->offset==c->reply.body.size() && !c->reply.truncated;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { assert(active==1); last_read=c->offset; --active; delete c; return ESP_OK; }
static Reply response(const std::string &url,const std::string &body) {
    Reply reply; reply.url=url; reply.body=body; reply.headers["Content-Type"]="text/html; charset=UTF-8"; return reply;
}
static bool run(const std::string &url,std::string &html,std::string &problem) {
    auto r=xiino_request("/g4/w153/dISO-8859-1/?"+url);
    bool ok=irxiino_page(r,"Host: pds.mobirus.com\r\n",html,problem);
    assert(!active && replies.empty()); return ok;
}
int main(int argc,char **argv) {
    std::string html,problem;
    assert(irxiino_diagnostics()=="Xiino: idle");
    assert(run("http://about/",html,problem) && html.find("<EBDIMAGE")!=html.npos && !created);
    Reply redirect=response("http://example.test/start","");
    redirect.code=302; redirect.headers["Location"]="https://example.test/dir/final";
    replies.push_back(redirect);
    Reply page=response("https://example.test/dir/final","<body><p>Caf\xc3\xa9</p><a href=\"../next\">Next</a><img src=\"i.png\"></body>");
    page.split=1; replies.push_back(page);
    replies.push_back(response("https://example.test/dir/i.png",std::string(image_fixtures[0].bytes,image_fixtures[0].size)));
    assert(run("http://example.test/start",html,problem));
    assert(html.find("Caf\xe9")!=html.npos && html.find("HREF=\"http://example.test/next\"")!=html.npos);
    assert(html.find("<EBDIMAGE MODE=\"9\"")!=html.npos && created==3);
    Reply missing=response("http://example.test/missing","<p>Not found</p>"); missing.code=404;
    replies.push_back(missing);
    assert(run(missing.url,html,problem) && html.find("Not found")!=html.npos);
    Reply broken=response("http://example.test/broken","<p>Incomplete"); broken.truncated=true;
    replies.push_back(broken);
    assert(!run(broken.url,html,problem) && problem.find("transfer failed")!=problem.npos);
    Reply tls=response("https://example.test/fail",""); tls.open_error=true; replies.push_back(tls);
    assert(!run(tls.url,html,problem));
    replies.push_back(response("http://example.test/large","<html><head><style>"+std::string(120000,'x')+
        "</style></head><body><p>Wikipedia-sized source works</p><a href='/english'>English</a></body></html>"));
    assert(run("http://example.test/large",html,problem) && problem.empty());
    assert(html.find("Wikipedia-sized source works")!=html.npos && html.find("English")!=html.npos && html.size()<1000);
    replies.push_back(response("http://example.test/long-text","<body><p>"+std::string(100000,'r')+"</p></body>"));
    assert(run("http://example.test/long-text",html,problem) && html.size()<=PROXY_MAX_PAGE_BYTES);
    assert(html.find("rrrrrrrr")!=html.npos && html.find("[Page shortened to fit the Palm]")!=html.npos);
    replies.push_back(response("http://example.test/endless","<script>"+std::string(XIINO_MAX_SOURCE_BYTES+1000,'x')+"</script><p>Beyond budget</p>"));
    assert(run("http://example.test/endless",html,problem) && last_read<=XIINO_MAX_SOURCE_BYTES+512);
    assert(html.find("[Page shortened to fit the Palm]")!=html.npos && html.find("Beyond budget")==html.npos);
    memory_available=false;
    unsigned before=created;
    assert(!run("https://example.test/low-memory",html,problem) && created==before);
    memory_available=true;
    // Enough for the old 16 KiB TLS guard, but not a single page allocation:
    // reject before opening TLS rather than grow a string into an OOM/reset.
    available_block=PROXY_MAX_PAGE_BYTES;
    before=created;
    assert(!run("http://example.test/page-memory",html,problem) && created==before);
    assert(problem.find("page buffer")!=problem.npos);
    available_block=1024*1024;
    Reply compressed=response("http://example.test/gzip","compressed"); compressed.headers["Content-Encoding"]="gzip";
    replies.push_back(compressed);
    assert(!run(compressed.url,html,problem));
    for (unsigned i=0;i<=PROXY_MAX_REDIRECTS;++i) {
        Reply loop=response("http://example.test/loop",""); loop.code=301; loop.headers["Location"]="/loop";
        replies.push_back(loop);
    }
    assert(!run("http://example.test/loop",html,problem) && problem.find("redirects")!=problem.npos);
    replies.push_back(response("http://example.test/image-page","<body><img src=\"/failure.png\"></body>"));
    Reply image_failure=response("http://example.test/failure.png",""); image_failure.open_error=true;
    replies.push_back(image_failure);
    assert(run("http://example.test/image-page",html,problem) && html.find("[Image unavailable:")!=html.npos);
    replies.push_back(response("http://example.test/large-image","<body><p>Before image</p><img src='/large.png'><p>After image</p></body>"));
    replies.push_back(response("http://example.test/large.png",std::string(XIINO_MAX_IMAGE_BYTES+1,'x')));
    assert(run("http://example.test/large-image",html,problem) && problem.empty());
    assert(html.find("Before image")!=html.npos && html.find("After image")!=html.npos && html.find("[Image unavailable:")!=html.npos);
    replies.push_back(response("http://example.test/unsupported","<body><img src=\"/bad.svg\"></body>"));
    replies.push_back(response("http://example.test/bad.svg","<svg/>"));
    assert(run("http://example.test/unsupported",html,problem) && html.find("Unrecognized image format")!=html.npos);
    replies.push_back(response("http://example.test/stream","<body><p>Text before image</p><img src='/image.png'><p>After</p></body>"));
    replies.push_back(response("http://example.test/image.png",std::string(image_fixtures[0].bytes,image_fixtures[0].size)));
    expect_text_before_image=&html;
    assert(run("http://example.test/stream",html,problem));
    expect_text_before_image=nullptr;
    replies.push_back(response("http://example.test/slow-images","<body><p>Available text</p><img src='/1'><img src='/2'><img src='/3'><img src='/4'></body>"));
    for (unsigned i=1;i<=(XIINO_IMAGES_TIMEOUT_MS+XIINO_IMAGE_TIMEOUT_MS-1)/XIINO_IMAGE_TIMEOUT_MS;++i) {
        Reply slow=response("http://example.test/"+std::to_string(i),""); slow.delay_open=XIINO_IMAGE_TIMEOUT_MS+1000; replies.push_back(slow);
    }
    uint32_t started=clock_ms;
    assert(run("http://example.test/slow-images",html,problem) && clock_ms-started<=XIINO_IMAGES_TIMEOUT_MS);
    assert(html.find("Available text")!=html.npos && html.find("[Image unavailable:")!=html.npos);
    assert(irxiino_diagnostics().find("Complete")!=std::string::npos);
    replies.push_back(response("http://example.test/cancel","<body><p>Before</p><img src='/never'></body>"));
    unsigned before_cancel=created;
    assert(!irxiino_stream(xiino_request("/?http://example.test/cancel"),"",[]() { return true; },
        [](const char *,size_t) { return false; },problem));
    assert(created==before_cancel+1 && !active && replies.empty());
    // Distinguish origin download, optional images, CPU and blocked socket
    // time. A slow sink must not be reported as image/network processing.
    Reply measured=response("http://example.test/measured","<body><p>Measured page</p><img src='/measured.png'></body>");
    measured.delay_open=40; replies.push_back(measured);
    Reply measured_image=response("http://example.test/measured.png",std::string(image_fixtures[0].bytes,image_fixtures[0].size));
    measured_image.delay_open=30; replies.push_back(measured_image);
    html.clear(); size_t measured_sends=0;
    assert(irxiino_stream(xiino_request("/?http://example.test/measured"),"",[]() { return true; },
        [&](const char *bytes,size_t size) { ++measured_sends; clock_ms+=10; html.append(bytes,size); return true; },problem));
    auto timing=irxiino_diagnostics();
    assert(timing.find("origin 40 ms, images 30 ms, processing 0 ms, socket "+std::to_string(measured_sends*10)+" ms, first body 50 ms")!=timing.npos);
    assert(html.find("<EBDIMAGE")!=html.npos && !active && replies.empty());
    // Long healthy IR writes between images must not consume the network
    // budget. Both pictures should still be downloaded and embedded.
    replies.push_back(response("http://example.test/slow-ir","<body><p>Before</p><img src='/one.png'><p>Between</p><img src='/two.png'></body>"));
    for (const char *name : {"one","two"}) {
        Reply image=response(std::string("http://example.test/")+name+".png",std::string(image_fixtures[0].bytes,image_fixtures[0].size));
        image.delay_open=100; replies.push_back(std::move(image));
    }
    html.clear();
    assert(irxiino_stream(xiino_request("/?http://example.test/slow-ir"),"",[]() { return true; },
        [&](const char *bytes,size_t size) { clock_ms+=12000; html.append(bytes,size); return true; },problem));
    assert(!active && replies.empty());
    assert(html.find("EBD=\"#1\"")!=html.npos && html.find("EBD=\"#2\"")!=html.npos);
    assert(html.find("Image unavailable")==html.npos && irxiino_diagnostics().find("images 200 ms")!=std::string::npos);
    // Keep the first failure after later attempts hit the shared gate.
    replies.push_back(response("http://example.test/timeouts","<body><img src='/timeout-a'><img src='/timeout-b'><img src='/skipped'></body>"));
    for (const char *name : {"timeout-a","timeout-b"}) {
        Reply image=response(std::string("http://example.test/")+name,""); image.delay_open=10000; replies.push_back(std::move(image));
    }
    assert(run("http://example.test/timeouts",html,problem));
    assert(irxiino_diagnostics().find("first image error: Xiino resource time limit")!=std::string::npos);
    assert(irxiino_diagnostics().find("Optional image download time budget")!=std::string::npos);
    std::string inline_image="data:image/gif;base64,R0lGODlhBAAEAPAAAP///wAAACwAAAAABAAEAAACBISPCQUAOw==";
    replies.push_back(response("http://example.test/inline","<body><img src='"+inline_image+"'></body>"));
    unsigned before_inline=created;
    assert(run("http://example.test/inline",html,problem) && created==before_inline+1);
    assert(html.find("<EBDIMAGE")!=html.npos);
    if (argc>5) {
        std::ifstream manifest(argv[5]); assert(manifest);
        std::string entry;
        while (std::getline(manifest,entry)) {
            size_t tab=entry.find('\t'); assert(tab!=entry.npos);
            std::ifstream asset(entry.substr(tab+1),std::ios::binary); assert(asset);
            auto &bytes=captured_images[entry.substr(0,tab)];
            bytes.assign(std::istreambuf_iterator<char>(asset),std::istreambuf_iterator<char>());
#ifdef XIINO_TEST_HEAP
            xiino_image_test_heap(83*1024,38912);
            std::string img,ebd,error;
            assert(xiino_image(bytes,1,XiinoLimits(),img,ebd,error));
            assert(xiino_image_diagnostics().find("live 0")!=std::string::npos);
            std::cout<<"Constrained captured image "<<entry.substr(0,tab)<<": "<<xiino_image_diagnostics()<<"\n";
            xiino_image_test_heap(1024*1024,1024*1024);
#endif
        }
    }
    if (argc>1) {
        std::ifstream file(argv[1],std::ios::binary);
        assert(file);
        std::string source((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        if (argc>2) {
            std::ifstream logo(argv[2],std::ios::binary); assert(logo);
            live_logo.assign(std::istreambuf_iterator<char>(logo),std::istreambuf_iterator<char>());
        }
        fixture_images=true;
        Reply redirect=response("http://wikipedia.org/",""); redirect.code=301; redirect.headers["Location"]="https://www.wikipedia.org/";
        replies.push_back(redirect);
        Reply wikipedia=response("https://www.wikipedia.org/",source);
        wikipedia.headers["Content-Type"]="text/html"; // Match the captured origin, including no charset.
        replies.push_back(std::move(wikipedia));
        assert(run("http://wikipedia.org/",html,problem));
        assert(html.find("Wikipedia")!=html.npos && html.find("English")!=html.npos);
        assert(html.find("http://en.wikipedia.org/")!=html.npos && html.size()<=PROXY_MAX_PAGE_BYTES);
        std::cout<<"Captured wikipedia.org: "<<source.size()<<" source bytes -> "<<html.size()<<" output bytes ("<<
            (html.find("[Page shortened")==html.npos ? "complete" : "shortened")<<")\n";
        size_t count=0,at=0;
        while ((at=html.find("<EBDIMAGE",at))!=html.npos) { ++count; at+=9; }
        std::cout<<"Captured Wikipedia embedded pictures: "<<count<<"\n";
        if (!live_logo.empty()) {
            bool embedded=html.find("<EBDIMAGE MODE=\"9\"")!=html.npos;
            assert(embedded);
            std::cout<<"Captured logo: "<<(embedded ? "embedded" : "placeholder within decoder budget")<<"\n";
        }
    }
    if (argc>4) {
        captured_images.clear();
        std::ifstream file(argv[3],std::ios::binary),logo(argv[4],std::ios::binary);
        assert(file && logo);
        std::string source((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        live_logo.assign(std::istreambuf_iterator<char>(logo),std::istreambuf_iterator<char>());
        fixture_images=true;
        replies.push_back(response("https://www.google.com/",source));
        assert(run("https://www.google.com/",html,problem));
        assert(html.find("Google")!=html.npos && html.find("<EBDIMAGE MODE=\"9\"")!=html.npos);
        assert(html.find("memory limit")==html.npos);
        std::cout<<"Captured Google page: "<<source.size()<<" source bytes -> "<<html.size()<<" output bytes; logo embedded\n";
    }
    std::cout<<"Xiino transport: redirects, split responses, image fetches, status/error bodies, TLS/receive failures and limits passed\n";
}
