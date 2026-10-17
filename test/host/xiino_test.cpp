#include "xiino.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <new>
#include "data/xiino_fixtures.h"
#include "data/xiino_codec_fixtures.h"

// Measure C++ working storage after the retained page/fixtures are prepared.
// Ordinary tests otherwise run with an unconstrained host heap, hiding the
// extra page copies which compete with TLS and decoder allocations on C3.
static struct { void *p; size_t bytes; } allocations[4096];
static bool measure_allocations=false;
static size_t live_allocations=0,peak_allocations=0,largest_allocation=0;
void *operator new(size_t n) {
    void *p=std::malloc(n ? n : 1); if (!p) throw std::bad_alloc();
    if (measure_allocations) {
        bool added=false;
        for (auto &entry : allocations) if (!entry.p) { entry={p,n}; added=true; break; }
        assert(added); live_allocations+=n;
        peak_allocations=std::max(peak_allocations,live_allocations);
        largest_allocation=std::max(largest_allocation,n);
    }
    return p;
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (p) for (auto &entry : allocations) if (entry.p==p) { live_allocations-=entry.bytes; entry={}; break; }
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p,size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p,size_t) noexcept { ::operator delete(p); }

int main() {
    // Compare the fast cube lookup against an independent exhaustive scan,
    // including every channel level and midpoint/tie boundary and all grays.
    #include "../../src/xiino_palette.inc"
    auto check_palette=[&](unsigned r,unsigned g,unsigned b) {
        unsigned best=0; int closest=3*256*256;
        for (unsigned i=0;i<sizeof(xiino_palette)/sizeof(xiino_palette[0]);++i) {
            int dr=int(r)-xiino_palette[i][0],dg=int(g)-xiino_palette[i][1],db=int(b)-xiino_palette[i][2];
            int distance=dr*dr+dg*dg+db*db;
            if (distance<closest) { best=i; closest=distance; }
        }
        assert(xiino_palette_nearest(r,g,b)==best);
    };
    const unsigned samples[]={0,1,23,24,25,48,74,75,76,101,127,128,129,154,180,181,182,207,230,231,232,254,255};
    for (unsigned r : samples) for (unsigned g : samples) for (unsigned b : samples) check_palette(r,g,b);
    for (unsigned c=0;c<256;++c) check_palette(c,c,c);
    XiinoLimits limits;
    auto request=xiino_request("/g4/w153/deISO-8859-1/?https://example.test/page?q=a&b=c");
    assert(request.dataserver && request.url=="https://example.test/page?q=a&b=c");
    assert(request.grayscale_depth=="4" && request.width=="153" && request.encoding=="ISO-8859-1");
    auto colour=xiino_request("http://pds.mobirus.com/c8/w306/dUTF-8/?http://about/");
    assert(colour.dataserver && colour.colour_depth=="8" && colour.width=="306" && colour.encoding=="UTF-8");
    assert(!xiino_request("/nets").dataserver && !xiino_request("/?ftp://example.test/").dataserver);
    assert(xiino_request("/?http://wifi.setup/pass?p=a%26b").url=="http://wifi.setup/pass?p=a%26b");
    assert(xiino_padding()==std::string(12,'\0')+"\r\n\r\n");
    assert(xiino_resolve("https://example.test/dir/page?q=old","../next?q=new#top")=="https://example.test/next?q=new#top");
    assert(xiino_resolve("https://example.test/dir/page","//other.test/img.png")=="https://other.test/img.png");
    assert(xiino_resolve("http://example.test/dir/page?q=x","#top")=="http://example.test/dir/page?q=x#top");
    assert(xiino_resolve("http://example.test/dir/page?q=x","?q=y")=="http://example.test/dir/page?q=y");
    assert(xiino_resolve("http://example.test/dir/page","mailto:hello@example.test")=="mailto:hello@example.test");
    for (const auto &fixture : html_fixtures) {
        XiinoResource page; page.body=fixture.input; page.url="https://example.test/dir/page";
        page.content_type="text/html; charset=UTF-8";
        std::string html,problem;
        bool ok=xiino_convert(page,[](const std::string &,size_t,XiinoResource &,std::string &) { assert(false); return false; },limits,html,problem);
        if (!ok || html!=fixture.output) {
            std::cerr<<"HTML fixture mismatch\n"<<fixture.input<<"\nExpected: "<<fixture.output<<"\nActual: "<<html<<"\n";
            return 1;
        }
    }
    for (const auto &fixture : mode_fixtures) {
        std::vector<uint8_t> pixels(fixture.pixels,fixture.pixels+fixture.width*fixture.height);
        assert(xiino_mode9(pixels,fixture.width,fixture.height)==std::string(fixture.encoded,fixture.encoded_size));
    }
    for (const auto &fixture : image_fixtures) {
        std::string img,ebd,problem;
        assert(xiino_image(std::string(fixture.bytes,fixture.size),1,limits,img,ebd,problem));
        if (img!=fixture.img || ebd!=fixture.ebd) {
            std::cerr<<"Image fixture mismatch\nExpected: "<<fixture.img<<"\n"<<fixture.ebd<<"\nActual: "<<img<<"\n"<<ebd<<"\n";
            return 1;
        }
    }
    for (const auto &fixture : codec_fixtures) {
        std::string img,ebd,problem;
        if (!xiino_image(std::string(fixture.bytes,fixture.size),1,limits,img,ebd,problem)) {
            std::cerr<<fixture.name<<": "<<problem<<"\n"; return 1;
        }
        assert(img.find("EBD=\"#1\"")!=img.npos && ebd.find("<EBDIMAGE MODE=\"9\"")!=ebd.npos);
        assert(xiino_image_diagnostics().find("live 0")!=std::string::npos);
        std::cout<<fixture.name<<": "<<xiino_image_diagnostics()<<"\n";
        for (size_t length : {size_t(1),fixture.size/2,fixture.size-8}) {
            if (xiino_image(std::string(fixture.bytes,length),1,limits,img,ebd,problem)) {
                // JPEG decoders can accept missing EOI after a complete scan.
                assert(std::string(fixture.name)=="JPEG");
            }
            assert(xiino_image_diagnostics().find("live 0")!=std::string::npos);
        }
    }
#ifdef XIINO_TEST_HEAP
    // Reproduce the device's fragmented heap: the old combined PNG object
    // cannot fit its largest block, and an RGB raster breaches the reserve.
    for (const auto &fixture : codec_fixtures) if (std::strstr(fixture.name,"PNG")) {
        std::string img,ebd,problem;
        xiino_image_test_heap(83*1024,38912);
        assert(xiino_image(std::string(fixture.bytes,fixture.size),1,limits,img,ebd,problem));
        assert(xiino_image_diagnostics().find("compact PNG")!=std::string::npos);
        assert(xiino_image_diagnostics().find("live 0")!=std::string::npos && !ebd.empty());
        std::cout<<"Constrained "<<fixture.name<<": "<<xiino_image_diagnostics()<<"\n";
        xiino_image_test_heap(48*1024,38912);
        assert(!xiino_image(std::string(fixture.bytes,fixture.size),1,limits,img,ebd,problem));
        assert(problem.find("memory")!=problem.npos && xiino_image_diagnostics().find("live 0")!=std::string::npos);
    }
    xiino_image_test_heap(1024*1024,1024*1024);
#endif
    for (const auto &fixture : html_fixtures) {
        XiinoResource source; source.url="https://example.test/dir/page"; source.body=fixture.input;
        std::string rendered,problem;
        assert(xiino_convert_stream(source,XiinoFetch(),limits,[&](const char *bytes,size_t size) {
            assert(size<=512); rendered.append(bytes,size); return true;
        },problem));
        assert(rendered==fixture.output);
    }
    XiinoResource page; page.url="https://example.test/dir/page";
    page.body="<body><img src=\"../a.png\"><img src=\"b.png\"></body>";
    unsigned requests=0;
    XiinoFetch fetch=[&](const std::string &url,size_t cap,XiinoResource &image,std::string &) {
        assert(cap==limits.image_bytes);
        assert(url==(requests ? "https://example.test/dir/b.png" : "https://example.test/a.png"));
        ++requests; image.body.assign(image_fixtures[0].bytes,image_fixtures[0].size); return true;
    };
    std::string html,problem;
    assert(xiino_convert(page,fetch,limits,html,problem) && requests==2);
    assert(html.find("EBD=\"#1\"")!=html.npos && html.find("EBD=\"#2\"")!=html.npos);
    assert(html.find("</BODY>")<html.find("<EBDIMAGE MODE=\"9\" NAME=\"1\""));
    limits.page_bytes=50;
    requests=0;
    assert(!xiino_convert(page,fetch,limits,html,problem) && !problem.empty());
    limits=XiinoLimits();
    page.body="<p>Caf\xe9</p>"; page.content_type="text/html";
    assert(xiino_convert(page,fetch,limits,html,problem) && html.find("Caf\xe9")!=html.npos);
    std::string img,ebd;
    assert(!xiino_image("bad image",1,limits,img,ebd,problem));
    limits.image_bytes=1;
    assert(!xiino_image(std::string(image_fixtures[0].bytes,image_fixtures[0].size),1,limits,img,ebd,problem));
    limits=XiinoLimits();
    const std::string large_source="<html><head><style>"+std::string(70000,'c')+"</style><script>"+
        std::string(70000,'j')+"</script><!--"+std::string(70000,'!')+
        "--></head><body class='modern' data-long='unused'><p>Readable text</p><pre> x\n  y </pre>"
        "<a class='button' href='../next?q=one&amp;x=two'>Next</a><img src='photo.png' width='80'></body></html>";
    std::string baseline;
    for (size_t split : {size_t(1),size_t(7),size_t(512),size_t(4093)}) {
        XiinoResource retained;
        XiinoSourceFilter filter(retained,limits.page_bytes);
        for (size_t at=0;at<large_source.size();at+=split) filter.feed(large_source.data()+at,std::min(split,large_source.size()-at));
        filter.finish();
        assert(!retained.truncated && retained.body.size()<500);
        assert(retained.body.find("Readable text")!=retained.body.npos && retained.body.find(" x\n  y ")!=retained.body.npos);
        assert(retained.body.find("href='../next?q=one&amp;x=two'")!=retained.body.npos);
        assert(retained.body.find("class=")==retained.body.npos && retained.body.find("data-long")==retained.body.npos);
        if (baseline.empty()) baseline=retained.body;
        else assert(retained.body==baseline);
    }
    XiinoResource retained;
    XiinoSourceFilter filter(retained,limits.page_bytes);
    std::string paragraph="<body><p>"+std::string(100000,'r')+"</p></body>";
    filter.feed(paragraph.data(),paragraph.size()); filter.finish();
    assert(retained.truncated && retained.body.size()<=limits.page_bytes);
    assert(xiino_convert(retained,fetch,limits,html,problem));
    assert(html.size()<=limits.page_bytes && html.find("rrrrrrrr")!=html.npos);
    assert(html.find("[Page shortened to fit the Palm]")!=html.npos);
    limits.page_bytes=300;
    page.body="<body><img src='a.png'><p>"+std::string(1000,'r')+"</p></body>";
    XiinoFetch single_image=[](const std::string &,size_t,XiinoResource &image,std::string &) {
        image.body.assign(image_fixtures[0].bytes,image_fixtures[0].size); return true;
    };
    assert(xiino_convert(page,single_image,limits,html,problem) && html.size()<=limits.page_bytes);
    assert(html.find("[Page shortened to fit the Palm]")!=html.npos);
    assert(html.find("EBD=\"#1\"")!=html.npos && html.find("<EBDIMAGE MODE=\"9\" NAME=\"1\"")!=html.npos);
    assert(html.find("</EBDIMAGE>")!=html.npos);
    limits=XiinoLimits();
    retained=XiinoResource();
    XiinoSourceFilter bounded_tag(retained,limits.page_bytes);
    std::string huge_tag="<p class='"+std::string(80000,'x')+"'>Kept</p><a href='/next'>Link</a>";
    bounded_tag.feed(huge_tag.data(),huge_tag.size()); bounded_tag.finish();
    assert(!retained.truncated && retained.body=="<p>Kept</p><a href='/next'>Link</a>");
    for (const char *name : {"about", "about2", "github", "deviceinfo"}) {
        auto r=xiino_request(std::string("/g4/w153/dISO-8859-1/?http://")+name+"/");
        std::string builtin=xiino_builtin(r,"Host: <unsafe>&\r\n");
        assert(!builtin.empty());
        if (!std::strcmp(name,"deviceinfo")) {
            assert(builtin.find("4-bit grayscale")!=builtin.npos && builtin.find("153px")!=builtin.npos);
            assert(builtin.find("ISO-8859-1")!=builtin.npos && builtin.find("&lt;unsafe&gt;&amp;")!=builtin.npos);
        }
    }
    // Lazy source decoding preserves the previous Latin-1 output for both
    // text and attributes, including references and redirected anchor URLs.
    for (const char *type : {"text/html", "text/html; charset=\"ISO-8859-1\"", "text/html; charset=windows-1252", "text/html; charset=UTF-8"}) {
        XiinoResource encoded; encoded.url="https://example.test/start"; encoded.content_type=type;
        std::string e=std::strstr(type,"UTF-8") ? "\xc3\xa9" : "\xe9";
        encoded.body="<a href='/caf"+e+"'>  Caf"+e+" &amp; &#233; \n</a>";
        std::string expected="<A HREF=\"http://example.test/caf\xe9\">\nCaf\xe9 & \xe9\n</A>\n";
        assert(xiino_convert(encoded,XiinoFetch(),XiinoLimits(),html,problem) && html==expected);
        std::string streamed;
        assert(xiino_convert_stream(encoded,XiinoFetch(),XiinoLimits(),[&](const char *p,size_t n) {
            streamed.append(p,n); return true;
        },problem) && streamed==expected);
    }
    // Wikipedia currently sends text/html with no charset. Exercise that
    // default encoding and mixed-case raw tags with a nearly full source.
    XiinoResource memory_page; memory_page.url="https://example.test/";
    memory_page.body="<body><StYlE></sTyLe>";
    for (unsigned i=0;i<1800;++i) memory_page.body+="<p>Caf\xe9 &amp; tea</p>";
    memory_page.body+="<img src='logo.png'></body>";
    assert(memory_page.body.size()>32000 && memory_page.body.size()<limits.page_bytes);
    for (const char *type : {"text/html","text/html; charset=windows-1252","text/html; charset=UTF-8"}) {
        memory_page.content_type=type;
        size_t sent=0;
        XiinoFetch tiny_logo=[](const std::string &,size_t,XiinoResource &image,std::string &) {
            image.body.assign(image_fixtures[0].bytes,image_fixtures[0].size); return true;
        };
        live_allocations=peak_allocations=largest_allocation=0;
        measure_allocations=true;
        bool ok=xiino_convert_stream(memory_page,tiny_logo,XiinoLimits(),[&](const char *,size_t n) { sent+=n; return true; },problem);
        measure_allocations=false;
        std::cout<<"Charset working storage ("<<type<<"): peak "<<peak_allocations<<", largest "<<largest_allocation<<" bytes\n";
        assert(ok && sent>20000 && live_allocations==0);
        assert(peak_allocations<8192 && largest_allocation<2048);
    }
    std::cout<<"Xiino: request/padding, upstream HTML + mode9 + PNG/JPEG/GIF/BMP fixtures, images and bounds passed\n";
}
