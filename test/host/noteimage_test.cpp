#include "notetext.h"
#include <zlib.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <new>
#include <cstdio>
#include "data/notes_png_fixtures.h"

// Exercise the miniz decoder under a contiguous-allocation limit, and detect
// compressed-data copies made by C++ strings independently of its workspace.
static bool measuring=false;
static size_t largest_cpp=0,largest_workspace=0,workspace_limit=32768,attempt=0,fail_at=0;
extern "C" void *__real_malloc(size_t);
extern "C" void *__real_calloc(size_t,size_t);
extern "C" void __real_free(void *);
extern "C" void *__wrap_malloc(size_t n) {
    if (measuring) {
        largest_workspace=std::max(largest_workspace,n);
        if (++attempt==fail_at || n>workspace_limit) return nullptr;
    }
    return __real_malloc(n);
}
extern "C" void *__wrap_calloc(size_t n,size_t size) {
    if (measuring) {
        largest_workspace=std::max(largest_workspace,n*size);
        if (++attempt==fail_at || n*size>workspace_limit) return nullptr;
    }
    return __real_calloc(n,size);
}
extern "C" void __wrap_free(void *p) { __real_free(p); }
void *operator new(size_t n) {
    if (measuring) largest_cpp=std::max(largest_cpp,n);
    if (auto *p=__real_malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { __real_free(p); }
void operator delete[](void *p) noexcept { __real_free(p); }
void operator delete(void *p,size_t) noexcept { __real_free(p); }
void operator delete[](void *p,size_t) noexcept { __real_free(p); }
static bool decode(const std::string &png,PalmNote &out,std::string &error,size_t limit=32768,size_t fail=0) {
    largest_cpp=largest_workspace=attempt=0; workspace_limit=limit; fail_at=fail;
    measuring=true; bool ok=note_read_png(png,out,error); measuring=false;
    return ok;
}
static void be32(std::string &s,uint32_t n) { for (int i=24;i>=0;i-=8) s+=(char)(n>>i); }
static void chunk(std::string &s,const char *tag,const std::string &data) {
    be32(s,data.size()); size_t at=s.size(); s.append(tag,4); s+=data;
    be32(s,crc32(0,(const Bytef *)s.data()+at,4+data.size()));
}
static std::string unhex(const char *p) {
    std::string s; for (size_t i=0;p[i];i+=2) { char b[3]={p[i],p[i+1],0}; s+=(char)strtoul(b,nullptr,16); } return s;
}
static std::string image(unsigned width,unsigned height,unsigned type,const std::string &raw,size_t split=65536,int level=9,int corruption=0) {
    std::string png("\x89PNG\r\n\x1a\n",8),head;
    be32(head,width); be32(head,height); head+=(char)8; head+=(char)type; head.append(3,0); chunk(png,"IHDR",head);
    uLongf bytes=compressBound(raw.size()); std::string compressed(bytes,0);
    assert(compress2((Bytef *)&compressed[0],&bytes,(const Bytef *)raw.data(),raw.size(),level)==Z_OK); compressed.resize(bytes);
    if (corruption==1) compressed.back()^=1; // bad Adler, valid PNG CRC
    if (corruption==2) compressed.pop_back();
    if (corruption==3) compressed+='x';
    chunk(png,"IDAT","");
    for (size_t at=0;at<compressed.size();at+=split) chunk(png,"IDAT",compressed.substr(at,split));
    chunk(png,"IDAT",""); chunk(png,"IEND",""); return png;
}
static std::string raster(unsigned width,unsigned height,unsigned channels,std::string &pixels) {
    std::string raw; pixels.assign((width+7)/8*height,0); uint32_t random=12345;
    for (unsigned y=0;y<height;y++) {
        raw+=(char)0;
        for (unsigned x=0;x<width;x++) {
            random^=random<<13; random^=random>>17; random^=random<<5;
            uint8_t gray=random;
            for (unsigned c=0;c<channels;c++) raw+=(char)gray;
            if (gray<128) pixels[y*((width+7)/8)+x/8]|=(char)(0x80>>(x%8));
        }
    }
    return raw;
}
int main() {
    for (const auto &fixture:png_fixtures) {
        PalmNote out; std::string error;
        assert(decode(unhex(fixture.png),out,error,16000) && out.pixels==unhex(fixture.pixels));
        assert(largest_workspace<16000);
    }
    std::string pixels; auto raw=raster(160,160,1,pixels);
    auto gray=image(160,160,0,raw); assert(gray.size()>25000);
    PalmNote out; std::string error;
    assert(decode(gray,out,error,26000) && out.pixels==pixels);
    assert(largest_workspace==raw.size() && largest_cpp<=pixels.size()+1);
    // No mandatory 32 KiB allocation even for a dense 8-bit grayscale image.
    for (size_t split:{size_t(1),size_t(7),size_t(65536)}) {
        auto small_raw=raster(32,20,1,pixels); auto png=image(32,20,0,small_raw,split);
        assert(decode(png,out,error,16000) && out.pixels==pixels && largest_workspace<16000);
    }
    for (size_t fail:{size_t(1),size_t(2)}) {
        assert(!decode(gray,out,error,32768,fail) && error.find("Not enough memory")!=std::string::npos);
        assert(error.find("160x160, 8-bit, 1 channels")!=std::string::npos);
    }
    PalmNote mono; mono.pixels.assign(3200,0); auto mono_png=note_png(mono);
    assert(!decode(mono_png,out,error,32768,2) && error.find("Not enough memory")!=std::string::npos && error.find("Try a 1-bit")==std::string::npos);
    auto exact_window=raster(127,256,1,pixels);
    assert(exact_window.size()==32768);
    assert(decode(image(127,256,0,exact_window,997),out,error) && out.pixels==pixels && largest_workspace==32768);
    auto reported=raster(160,352,1,pixels);
    auto reported_png=image(160,352,0,reported,65536,0);
    assert(reported_png.size()>56000 && reported_png.size()<PALM_NOTE_PNG_MAX);
    assert(decode(reported_png,out,error) && out.pixels==pixels && out.height==352 && largest_workspace==32768);
    // Large filtered rasters wrap the window, including across IDAT splits.
    for (unsigned channels:{1u,3u}) {
        unsigned height=channels==1 ? 240 : 480;
        auto tall_raw=raster(160,height,channels,pixels);
        auto png=image(160,height,channels==1 ? 0 : 2,tall_raw,997);
        // Random grayscale at 480 rows can exceed the compressed file limit;
        // use a deterministic repeating raster for the long-window case.
        if (png.size()>PALM_NOTE_PNG_MAX) {
            tall_raw.clear(); pixels.assign(20*height,0);
            for (unsigned y=0;y<height;y++) {
                tall_raw+=(char)0;
                for (unsigned x=0;x<160;x++) {
                    uint8_t value=((x+y)%17)*15;
                    tall_raw.append(channels,(char)value);
                    if (value<128) pixels[y*20+x/8]|=(char)(0x80>>(x%8));
                }
            }
            png=image(160,height,channels==1 ? 0 : 2,tall_raw,7);
        }
        assert(decode(png,out,error) && out.pixels==pixels && out.height==height && largest_workspace==32768);
    }
    for (unsigned height:{160u,320u,480u,1600u}) {
        PalmNote source; source.height=height; source.pixels.assign(20*height,0);
        source.pixels[0]=(char)0x80; source.pixels.back()=1;
        assert(decode(note_png(source),out,error) && out.pixels==source.pixels);
    }
    auto small_raw=raster(8,3,1,pixels);
    for (int corruption:{1,2,3}) assert(!decode(image(8,3,0,small_raw,1,9,corruption),out,error));
    assert(!decode(image(8,2,0,small_raw,1),out,error)); // excess decoded rows
    assert(!decode(image(8,4,0,small_raw,1),out,error)); // missing row
    auto bad_filter=small_raw; bad_filter[0]=5;
    assert(!decode(image(8,3,0,bad_filter),out,error));
    puts("noteimage: miniz path passed (bounded allocations, gray/RGB, split IDAT, window wrapping, checksums and failures)");
}
