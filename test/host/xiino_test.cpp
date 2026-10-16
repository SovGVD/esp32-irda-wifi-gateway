#include "xiino.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include "data/xiino_fixtures.h"
#include "data/xiino_codec_fixtures.h"

int main() {
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
    std::cout<<"Xiino: request/padding, upstream HTML + mode9 + PNG/JPEG/GIF/BMP fixtures, images and bounds passed\n";
}
