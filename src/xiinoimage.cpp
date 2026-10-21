// OpenXiino's colour-only EBDIMAGE/mode 9 path, ported from e9ef506 (MIT).
// Bounded thumbnails and integer colour conversion keep work practical on C3.
#include "xiino.h"
#include "xiinoalloc.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <memory>
#include "../third_party/pngle/pngle.h"
#include "../third_party/libwebp/src/webp/decode.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace {
// stb's intermediate allocations share a budget, including reallocation peaks.
// These are used only by the single portal task and released before returning.
struct ImageAllocation { std::max_align_t alignment; size_t size; };
size_t decoder_used=0;
size_t decoder_peak=0;
bool decoder_exhausted=false;
std::string last_image_problem;
unsigned last_width=0,last_height=0;
bool last_compact_png=false;
#ifdef XIINO_TEST_HEAP
size_t test_heap_free=1024*1024,test_heap_block=1024*1024;
#endif
const size_t decoder_budget=192*1024;
bool image_room(size_t bytes,size_t reserve) {
#ifdef ESP_PLATFORM
    size_t available=heap_caps_get_free_size(MALLOC_CAP_8BIT);
    return available>=reserve && bytes<=available-reserve;
#elif defined(XIINO_TEST_HEAP)
    size_t used=decoder_used+reserve;
    return test_heap_free>=used && bytes<=test_heap_free-used;
#else
    (void)bytes; (void)reserve; return true;
#endif
}
void *image_alloc(size_t bytes) {
    if (bytes>decoder_budget || decoder_used>decoder_budget-bytes) { decoder_exhausted=true; return nullptr; }
    if (!image_room(bytes+sizeof(ImageAllocation),24*1024)) { decoder_exhausted=true; return nullptr; }
#ifdef ESP_PLATFORM
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)<bytes+sizeof(ImageAllocation)) { decoder_exhausted=true; return nullptr; }
#elif defined(XIINO_TEST_HEAP)
    if (test_heap_block<bytes+sizeof(ImageAllocation)) { decoder_exhausted=true; return nullptr; }
#endif
    auto *block=(ImageAllocation *)std::malloc(sizeof(ImageAllocation)+bytes);
    if (!block) { decoder_exhausted=true; return nullptr; }
    block->size=bytes; decoder_used+=bytes; decoder_peak=std::max(decoder_peak,decoder_used); return block+1;
}
void image_free(void *p) {
    if (!p) return;
    auto *block=(ImageAllocation *)p-1;
    decoder_used-=block->size; std::free(block);
}
void *image_realloc(void *p,size_t bytes) {
    if (!p) return image_alloc(bytes);
    auto *block=(ImageAllocation *)p-1;
    void *next=image_alloc(bytes);
    if (!next) return nullptr;
    std::memcpy(next,p,std::min(bytes,block->size)); image_free(p); return next;
}
}
#ifdef XIINO_TEST_HEAP
void xiino_image_test_heap(size_t available,size_t largest) {
    test_heap_free=available; test_heap_block=largest;
}
#endif
extern "C" void *xiino_decoder_malloc(size_t size) { return image_alloc(size); }
extern "C" void *xiino_decoder_calloc(size_t count,size_t size) {
    if (size && count>decoder_budget/size) { decoder_exhausted=true; return nullptr; }
    size_t bytes=count*size; void *p=image_alloc(bytes); if (p) std::memset(p,0,bytes); return p;
}
extern "C" void xiino_decoder_free(void *p) { image_free(p); }
#define STBI_MALLOC image_alloc
#define STBI_REALLOC image_realloc
#define STBI_FREE image_free
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 2048
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

namespace {
#include "xiino_palette.inc"
void append_base64(const std::string &bytes,std::string &out) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i=0;i<bytes.size();i+=3) {
        unsigned a=(uint8_t)bytes[i],b=i+1<bytes.size() ? (uint8_t)bytes[i+1] : 0,c=i+2<bytes.size() ? (uint8_t)bytes[i+2] : 0;
        out+=alphabet[a>>2]; out+=alphabet[((a&3)<<4)|(b>>4)];
        out+=i+1<bytes.size() ? alphabet[((b&15)<<2)|(c>>6)] : '=';
        out+=i+2<bytes.size() ? alphabet[c&63] : '=';
    }
}
int clamp(int x) { return std::max(0,std::min(255,x)); }
}

uint8_t xiino_palette_nearest(uint8_t r,uint8_t g,uint8_t b) {
    // The first 215 entries form a six-level RGB cube (black is entry 230).
    // Find its closest point per channel, then compare the 15 extra colours.
    // Match a full palette scan exactly, including its first-entry tie break.
    static const auto channel=[]() {
        std::array<uint8_t,256> result{};
        static const int levels[]={255,207,154,101,48,0};
        for (int c=0;c<256;++c) {
            int distance=256;
            for (unsigned i=0;i<6;++i) if (std::abs(c-levels[i])<distance) {
                distance=std::abs(c-levels[i]); result[c]=i;
            }
        }
        return result;
    }();
    unsigned ri=channel[r],gi=channel[g],bi=channel[b];
    unsigned best=(bi/3)*108+ri*18+(bi%3)*6+gi;
    if (best==215) best=230;
    auto distance=[&](unsigned i) {
        int dr=int(r)-xiino_palette[i][0],dg=int(g)-xiino_palette[i][1],db=int(b)-xiino_palette[i][2];
        return dr*dr+dg*dg+db*db;
    };
    int closest=distance(best);
    for (unsigned i=215;i<230;++i) {
        int d=distance(i);
        if (d<closest || (d==closest && i<best)) { best=i; closest=d; }
    }
    return (uint8_t)best;
}

namespace {
// Area reduction for full-size JPEG/BMP rasters; PNG/GIF/WebP arrive already
// resized. Composite and diffuse using integers: C3 has no floating-point unit.
std::vector<uint8_t> palette_image(const uint8_t *pixels,unsigned sw,unsigned sh,
                                  unsigned width,unsigned height,unsigned channels,bool alpha) {
    std::vector<uint8_t> result(width*height);
    std::vector<int> errors((width+2)*3,0),next(errors.size(),0);
    for (unsigned y=0;y<height;++y) {
        for (unsigned x=0;x<width;++x) {
            unsigned rgb[3]={0,0,0};
            if (sw==width && sh==height) {
                const uint8_t *p=pixels+((size_t)y*sw+x)*channels;
                unsigned a=alpha ? p[channels-1] : 255;
                for (unsigned c=0;c<3;++c) rgb[c]=(p[channels<3 ? 0 : c]*a+255*(255-a)+127)/255;
            } else {
                // Coordinates measured in destination pixels give exact integer
                // coverage weights, including fractional edges of odd sizes.
                unsigned left=x*sw,right=(x+1)*sw,top=y*sh,bottom=(y+1)*sh;
                uint64_t sum[3]={0,0,0};
                for (unsigned py=top/height;py<(bottom+height-1)/height;++py) {
                    unsigned wy=std::min(bottom,(py+1)*height)-std::max(top,py*height);
                    for (unsigned px=left/width;px<(right+width-1)/width;++px) {
                        unsigned wx=std::min(right,(px+1)*width)-std::max(left,px*width);
                        const uint8_t *p=pixels+((size_t)py*sw+px)*channels;
                        unsigned a=alpha ? p[channels-1] : 255;
                        for (unsigned c=0;c<3;++c)
                            sum[c]+=uint64_t((p[channels<3 ? 0 : c]*a+255*(255-a)+127)/255)*wx*wy;
                    }
                }
                unsigned weight=sw*sh;
                for (unsigned c=0;c<3;++c) rgb[c]=(sum[c]+weight/2)/weight;
            }
            int corrected[3];
            for (unsigned c=0;c<3;++c) corrected[c]=clamp((int(rgb[c])*16+errors[(x+1)*3+c]+8)/16);
            unsigned best=xiino_palette_nearest(corrected[0],corrected[1],corrected[2]);
            result[y*width+x]=(uint8_t)best;
            for (unsigned c=0;c<3;++c) {
                int e=corrected[c]-xiino_palette[best][c];
                errors[(x+2)*3+c]+=e*7;
                next[x*3+c]+=e*3; next[(x+1)*3+c]+=e*5; next[(x+2)*3+c]+=e;
            }
        }
        errors.swap(next); std::fill(next.begin(),next.end(),0);
#ifdef ESP_PLATFORM
        if (!(y&15)) vTaskDelay(1);
#endif
    }
    return result;
}
struct PngRaster {
    uint8_t *pixels;
    unsigned sw,sh,w,h;
    bool complete,indexed;
};
void png_draw(pngle_t *png,uint32_t x,uint32_t y,uint32_t rw,uint32_t rh,const uint8_t rgba[4]) {
#ifdef ESP_PLATFORM
    if (!x && !(y&31)) vTaskDelay(1);
#endif
    auto &r=*(PngRaster *)pngle_get_user_data(png);
    unsigned left=((uint64_t)2*x*r.w+r.sw-1)/(2*r.sw);
    unsigned right=std::min(r.w,(unsigned)(((uint64_t)2*(x+rw)*r.w+r.sw-1)/(2*r.sw)));
    unsigned top=((uint64_t)2*y*r.h+r.sh-1)/(2*r.sh);
    unsigned bottom=std::min(r.h,(unsigned)(((uint64_t)2*(y+rh)*r.h+r.sh-1)/(2*r.sh)));
    for (unsigned py=top;py<bottom;++py) for (unsigned px=left;px<right;++px) {
        unsigned rgb[3];
        for (unsigned c=0;c<3;++c) rgb[c]=(rgba[c]*rgba[3]+255*(255-rgba[3])+127)/255;
        size_t at=(size_t)py*r.w+px;
        if (r.indexed) r.pixels[at]=xiino_palette_nearest(rgb[0],rgb[1],rgb[2]);
        else for (unsigned c=0;c<3;++c) r.pixels[at*3+c]=rgb[c];
    }
}
bool decode_png(const std::string &file,PngRaster &r,pngle_t *png,std::string &problem) {
    pngle_set_user_data(png,&r);
    pngle_set_draw_callback(png,png_draw);
    pngle_set_done_callback(png,[](pngle_t *p) { ((PngRaster *)pngle_get_user_data(p))->complete=true; });
    size_t at=0;
    while (at<file.size()) {
        int n=pngle_feed(png,file.data()+at,file.size()-at);
        if (n<=0) { problem=decoder_exhausted ? "PNG decoder memory limit" : "Invalid or incomplete PNG"; return false; }
        at+=(size_t)n;
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#endif
    }
    if (!r.complete) problem="Incomplete PNG";
    return r.complete;
}
// Decode the first GIF frame into the thumbnail, without stb's full-frame
// history/background copies. LZW dictionary storage is fixed at 16 KiB.
bool decode_gif(const std::string &file,PngRaster &r,std::string &problem) {
    const auto *p=(const uint8_t *)file.data(); size_t at=13;
    auto fail=[&]() { problem="Invalid or incomplete GIF"; return false; };
    auto u16=[&](size_t pos) { return unsigned(p[pos])|(unsigned(p[pos+1])<<8); };
    if (file.size()<13) return fail();
    const uint8_t *palette=nullptr; unsigned colours=0,transparent=256;
    if (p[10]&128) {
        colours=2u<<(p[10]&7);
        if (colours*3>file.size()-at) return fail();
        palette=p+at; at+=colours*3;
    }
    auto blocks=[&]() {
        while (at<file.size()) { unsigned n=p[at++]; if (!n) return true; if (n>file.size()-at) return false; at+=n; }
        return false;
    };
    while (at<file.size()) {
        unsigned marker=p[at++];
        if (marker==0x21) {
            if (at==file.size()) return fail();
            unsigned type=p[at++];
            if (type==0xf9) {
                if (file.size()-at<6 || p[at]!=4 || p[at+5]) return fail();
                transparent=p[at+1]&1 ? p[at+4] : 256; at+=6;
            } else if (!blocks()) return fail();
            continue;
        }
        if (marker!=0x2c || file.size()-at<9) return fail();
        unsigned left=u16(at),top=u16(at+2),width=u16(at+4),height=u16(at+6),flags=p[at+8]; at+=9;
        if (!width || !height || left+width>r.sw || top+height>r.sh) return fail();
        if (flags&128) {
            colours=2u<<(flags&7);
            if (colours*3>file.size()-at) return fail();
            palette=p+at; at+=colours*3;
        }
        if (!palette || at==file.size()) return fail();
        unsigned minimum=p[at++]; if (minimum<2 || minimum>8) return fail();
        struct Dictionary { uint16_t prefix[4096]; uint8_t suffix[4096],stack[4096]; };
        std::unique_ptr<Dictionary,decltype(&image_free)> dict((Dictionary *)image_alloc(sizeof(Dictionary)),image_free);
        if (!dict) { problem="GIF decoder memory limit"; return false; }
        unsigned clear=1u<<minimum,end=clear+1,next=end+1,bits=minimum+1;
        for (unsigned i=0;i<clear;++i) dict->suffix[i]=i;
        unsigned block=0,held=0,bit_count=0,x=0,y=0,pass=0,drawn=0,first=0;
        int old=-1;
        auto code=[&]() -> int {
            while (bit_count<bits) {
                if (!block) { if (at==file.size()) return -1; block=p[at++]; if (!block || block>file.size()-at) return -1; }
                held|=unsigned(p[at++])<<bit_count; bit_count+=8; --block;
            }
            unsigned value=held&((1u<<bits)-1); held>>=bits; bit_count-=bits; return (int)value;
        };
        auto pixel=[&](unsigned index) {
            if (index>=colours || drawn>=(size_t)width*height) return false;
            if (index!=transparent) {
                uint8_t rgba[4]={palette[index*3],palette[index*3+1],palette[index*3+2],255};
                unsigned dx=((uint64_t)2*(left+x)*r.w+r.sw-1)/(2*r.sw);
                unsigned dx_end=std::min(r.w,(unsigned)(((uint64_t)2*(left+x+1)*r.w+r.sw-1)/(2*r.sw)));
                unsigned dy=((uint64_t)2*(top+y)*r.h+r.sh-1)/(2*r.sh);
                unsigned dy_end=std::min(r.h,(unsigned)(((uint64_t)2*(top+y+1)*r.h+r.sh-1)/(2*r.sh)));
                for (unsigned py=dy;py<dy_end;++py) for (unsigned px=dx;px<dx_end;++px)
                    std::memcpy(r.pixels+((size_t)py*r.w+px)*3,rgba,3);
            }
            ++drawn;
            if (++x==width) {
                x=0;
#ifdef ESP_PLATFORM
                if (!(y&31)) vTaskDelay(1);
#endif
                if (flags&64) {
                    static const unsigned steps[]={8,8,4,2},starts[]={0,4,2,1};
                    y+=steps[pass];
                    while (y>=height && pass<3) y=starts[++pass];
                } else ++y;
            }
            return true;
        };
        for (;;) {
            int current=code(); if (current<0) return fail();
            if ((unsigned)current==clear) { next=end+1; bits=minimum+1; old=-1; continue; }
            if ((unsigned)current==end) return drawn==(size_t)width*height ? true : fail();
            if (old<0) {
                if ((unsigned)current>=clear || !pixel(current)) return fail();
                first=current; old=current; continue;
            }
            unsigned original=current,count=0;
            if ((unsigned)current==next) { dict->stack[count++]=first; current=old; }
            else if ((unsigned)current>next) return fail();
            while ((unsigned)current>=clear) {
                if ((unsigned)current>=next || count==4096) return fail();
                dict->stack[count++]=dict->suffix[current]; current=dict->prefix[current];
            }
            if (count==4096) return fail();
            first=dict->suffix[current]; dict->stack[count++]=first;
            while (count) if (!pixel(dict->stack[--count])) return fail();
            if (next<4096) {
                dict->prefix[next]=old; dict->suffix[next++]=first;
                if (next==(1u<<bits) && bits<12) ++bits;
            }
            old=original;
        }
    }
    return fail();
}
}

std::string xiino_mode9(const std::vector<uint8_t> &pixels,unsigned width,unsigned height) {
    if (!width || width>153 || !height || pixels.size()!=(size_t)width*height) return "";
    std::string out; out.reserve(pixels.size());
    static const uint8_t run_codes[]={0,0xec,0xf0,0xf4,0xf8,0xfc,0xe8};
    static const uint8_t copy_codes[][3]={{0,0,0},{0xed,0xee,0xef},{0xf1,0xf2,0xf3},
        {0xf5,0xf6,0xf7},{0xf9,0xfa,0xfb},{0xfd,0xfe,0xff},{0xe9,0xea,0xeb}};
    for (unsigned y=0;y<height;++y) {
        for (unsigned x=0;x<width;) {
            unsigned lengths[4]={0,0,0,0};
            uint8_t pixel=pixels[y*width+x];
            if (x+1<width && pixels[y*width+x+1]==pixel) {
                while (x+lengths[0]<width && pixels[y*width+x+lengths[0]]==pixel) ++lengths[0];
            }
            if (y) for (int offset=-1;offset<=1;++offset) {
                unsigned &n=lengths[offset+2];
                while (x+n<width && (offset!=1 || x+1+n<width)) {
                    int previous=(int)(x+n)+offset;
                    // Python's negative index wraps to the previous row's end.
                    if (previous<0) previous+=(int)width;
                    if (pixels[y*width+x+n]!=pixels[(y-1)*width+previous]) break;
                    ++n;
                }
            }
            unsigned best=0;
            for (unsigned i=1;i<4;++i) if (lengths[i]>lengths[best]) best=i;
            unsigned n=lengths[best];
            if (!n) { out+=(char)pixel; ++x; }
            else if (!best) {
                out+=(char)pixel; out+=(char)run_codes[std::min(n,6u)];
                if (n>=6) out+=(char)(n-6);
                x+=n+1; // Upstream's existing run advancement is intentional.
            } else {
                out+=(char)copy_codes[std::min(n,6u)][best-1];
                if (n>=6) out+=(char)(n-6);
                x+=n;
            }
        }
    }
    return out;
}

bool xiino_image(const std::string &file,unsigned slot,const XiinoLimits &limits,
                 std::string &img,std::string &ebd,std::string &problem) {
    img.clear(); ebd.clear(); problem.clear();
    decoder_peak=0; decoder_exhausted=false;
    last_image_problem.clear(); last_width=last_height=0; last_compact_png=false;
    auto fail=[&](const char *why) { problem=why; last_image_problem=why; return false; };
    if (file.empty() || file.size()>limits.image_bytes) return fail("Image exceeds the configured byte limit");
    int width=0,height=0,channels=0;
    const auto *input=(const stbi_uc *)file.data();
    bool png=file.size()>=33 && !std::memcmp(file.data(),"\x89PNG\r\n\x1a\n",8);
    bool gif=file.size()>=13 && (file.compare(0,6,"GIF87a")==0 || file.compare(0,6,"GIF89a")==0);
    bool webp=file.size()>=12 && !std::memcmp(file.data(),"RIFF",4) && !std::memcmp(file.data()+8,"WEBP",4);
    WebPBitstreamFeatures features={};
    if (webp) {
        if (WebPGetFeatures(input,file.size(),&features)!=VP8_STATUS_OK) return fail("Invalid WebP image");
        if (features.has_animation) return fail("Animated WebP is not supported");
        width=features.width; height=features.height; channels=4;
    } else if (!stbi_info_from_memory(input,(int)file.size(),&width,&height,&channels) || width<=0 || height<=0)
        return fail("Unrecognized image format");
    if (width>2048 || height>2048) return fail("Image dimensions exceed the decoder limit");
    last_width=width; last_height=height;
    if (width<=2) return fail("Image is too small"); // Same width-only prefilter as upstream.
    unsigned w=width>306 ? 153 : (width+1)/2;
    unsigned h=width>306 ? (unsigned)(((size_t)height*153+width-1)/width) : (height+1)/2;
    if ((size_t)w*h>limits.image_pixels) return fail("Resized image exceeds the configured pixel limit");
    if (limits.checkpoint) limits.checkpoint("Decoding Xiino image");
    std::unique_ptr<uint8_t,decltype(&image_free)> pixels(nullptr,image_free);
    unsigned sw=width,sh=height;
    bool alpha=channels==2 || channels==4;
    if (png || gif || webp) {
        // Prefer the dithered RGB thumbnail; use direct palette sampling if
        // that raster plus PNG workspace would breach the free-heap reserve.
        last_compact_png=png && !image_room((size_t)w*h*3+PNGLE_T_SIZE+32768+(size_t)width*8+1024,24*1024);
        std::unique_ptr<pngle_t,decltype(&pngle_destroy)> decoder(png ? pngle_new() : nullptr,pngle_destroy);
        if (png && !decoder) return fail("PNG decoder memory limit");
        channels=last_compact_png ? 1 : webp ? 4 : 3;
        pixels.reset((uint8_t *)image_alloc((size_t)w*h*channels));
        if (!pixels) return fail("Thumbnail memory limit");
        if (png || gif) {
            std::memset(pixels.get(),last_compact_png ? 0 : 255,(size_t)w*h*channels);
            PngRaster raster={pixels.get(),(unsigned)width,(unsigned)height,w,h,false,last_compact_png};
            if (!(png ? decode_png(file,raster,decoder.get(),problem) : decode_gif(file,raster,problem))) { last_image_problem=problem; return false; }
            alpha=false;
        } else {
            WebPDecoderConfig config;
            if (!WebPInitDecoderConfig(&config)) return fail("Could not initialize WebP decoder");
            config.output.colorspace=MODE_RGBA;
            config.output.is_external_memory=1;
            config.output.u.RGBA.rgba=pixels.get();
            config.output.u.RGBA.stride=w*4;
            config.output.u.RGBA.size=(size_t)w*h*4;
            config.options.use_scaling=1; config.options.scaled_width=w; config.options.scaled_height=h;
            VP8StatusCode status=WebPDecode(input,file.size(),&config);
            WebPFreeDecBuffer(&config.output);
            if (status!=VP8_STATUS_OK) return fail(decoder_exhausted || status==VP8_STATUS_OUT_OF_MEMORY ? "WebP decoder memory limit" : "Invalid WebP image");
            alpha=true;
        }
        sw=w; sh=h;
    } else {
        if ((size_t)width*height>decoder_budget/(unsigned)std::max(channels,1)) return fail("Decoded image exceeds the memory budget");
        pixels.reset(stbi_load_from_memory(input,(int)file.size(),&width,&height,&channels,0));
        if (!pixels) return fail(decoder_exhausted ? "Image decoder memory limit" : "Invalid or incomplete image");
    }
    if (limits.checkpoint) limits.checkpoint("Quantizing Xiino image");
    if (!image_room((size_t)w*h+(w+2)*3*2*sizeof(int),24*1024)) return fail("Image conversion memory limit");
    auto indexed=last_compact_png ? std::vector<uint8_t>(pixels.get(),pixels.get()+(size_t)w*h) :
        palette_image(pixels.get(),sw,sh,w,h,channels,alpha && !gif);
    pixels.reset();
    if (limits.checkpoint) limits.checkpoint("Encoding Xiino image");
    // Bound the remaining C++ storage as well as the C decoder allocations.
    // Do not retain separate packed, base64 and copied EBD payloads together.
    if (!image_room(indexed.size()*3+512,24*1024)) return fail("Image encoding memory limit");
    std::string packed=xiino_mode9(indexed,w,h);
    std::vector<uint8_t>().swap(indexed);
    std::string id=std::to_string(slot),ws=std::to_string(w),hs=std::to_string(h);
    img="<IMG ALT=\"None\" WIDTH=\""+ws+"\" HEIGHT=\""+hs+"\" EBDWIDTH=\""+ws+"\" EBDHEIGHT=\""+hs+"\" EBD=\"#"+id+"\">";
    const std::string prefix="<EBDIMAGE MODE=\"9\" NAME=\""+id+"\"><!--";
    ebd.reserve(prefix.size()+(packed.size()+2)/3*4+14);
    ebd=prefix; append_base64(packed,ebd); ebd+="--></EBDIMAGE>";
    return true;
}

std::string xiino_image_diagnostics() {
    return "Image: "+std::to_string(last_width)+"x"+std::to_string(last_height)+", decoder peak "+
        std::to_string(decoder_peak)+" bytes, live "+std::to_string(decoder_used)+(last_compact_png ? ", compact PNG" : "")+(last_image_problem.empty() ? "" : "; "+last_image_problem);
}
