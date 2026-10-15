#include "notetext.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <functional>
#ifdef ESP_PLATFORM
#include "esp32c3/rom/miniz.h"
#elif defined(NOTE_PNG_MINIZ)
// Host regression build of the same streaming path using SDK miniz sources.
#include <miniz.h>
#else
#include <zlib.h>
#endif

static uint32_t u32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3]; }
static void p32(std::string &s,uint32_t n) { for (int shift=24;shift>=0;shift-=8) s+=(char)(n>>shift); }
static uint32_t crc(const uint8_t *p,size_t n) { uint32_t c=~0u; while (n--) { c^=*p++; for (int i=0;i<8;i++) c=(c>>1)^(0xedb88320u&-(c&1)); } return ~c; }
static void chunk(std::string &png,const char *type,const std::string &data) {
    p32(png,data.size()); size_t at=png.size(); png.append(type,4); png+=data;
    p32(png,crc((const uint8_t *)png.data()+at,data.size()+4));
}
std::string note_png(const PalmNote &n) {
    if (n.width<8 || n.width>160 || n.width%8 || !n.height || n.height>PALM_NOTE_BITMAP_MAX/(n.width/8) || n.pixels.size()!=n.width/8*n.height) return "";
    std::string png("\x89PNG\r\n\x1a\n",8),head;
    p32(head,n.width); p32(head,n.height); head.append("\x01\x00\x00\x00\x00",5); chunk(png,"IHDR",head);
    // A small standards-compliant zlib stream with one stored DEFLATE block.
    // It needs no compressor workspace and is readily editable by PNG tools.
    std::string z("\x78\x01\x01",3); unsigned length=(n.width/8+1)*n.height;
    z.reserve(length+11); png.reserve(length+80);
    z+=(char)length; z+=(char)(length>>8); z+=(char)~length; z+=(char)(~length>>8);
    uint32_t a=1,b=0;
    auto byte=[&](uint8_t c) { z+=(char)c; a=(a+c)%65521; b=(b+a)%65521; };
    for (unsigned y=0;y<n.height;y++) { byte(0); for (unsigned x=0;x<n.width/8;x++) byte((uint8_t)~n.pixels[y*n.width/8+x]); }
    p32(z,(b<<16)|a);
    chunk(png,"IDAT",z); chunk(png,"IEND",""); return png;
}
// All chunks have already passed bounds, order and CRC validation. Feed IDAT
// payloads directly from the PNG instead of retaining a second compressed copy.
struct PngInput {
    const uint8_t *png,*bytes=nullptr;
    size_t next,end,available=0,remaining;
    PngInput(const uint8_t *p,size_t first,size_t last,size_t size) : png(p),next(first),end(last),remaining(size) {}
    void span() {
        while (!available && next<end) {
            available=u32(png+next); bytes=png+next+8; next+=available+12;
        }
    }
    void consume(size_t count) { bytes+=count; available-=count; remaining-=count; }
};
static bool inflate_rows(PngInput data,size_t expected,const std::function<bool(const uint8_t *,size_t)> &sink,bool &low_memory) {
    low_memory=false;
#if defined(ESP_PLATFORM) || defined(NOTE_PNG_MINIZ)
    // Small images need only their full filtered raster, not a mandatory 32 KiB
    // dictionary. Larger rasters use the bounded DEFLATE sliding window.
    const size_t window=std::min(expected,size_t(TINFL_LZ_DICT_SIZE));
    const bool wrapping=expected>window;
    // Allocate the largest block first, before the Huffman state fragments it.
    std::unique_ptr<uint8_t,decltype(&free)> dictionary((uint8_t *)malloc(window),free);
    if (!dictionary) { low_memory=true; return false; }
    std::unique_ptr<tinfl_decompressor,decltype(&free)> state((tinfl_decompressor *)calloc(1,sizeof(tinfl_decompressor)),free);
    if (!state) { low_memory=true; return false; }
    tinfl_init(state.get()); size_t out=0,total=0;
    for (;;) {
        data.span(); size_t input=data.available,output=window-out;
        unsigned flags=TINFL_FLAG_PARSE_ZLIB_HEADER;
        if (!wrapping) flags|=TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if (data.remaining>input) flags|=TINFL_FLAG_HAS_MORE_INPUT;
        auto status=tinfl_decompress(state.get(),data.bytes,&input,dictionary.get(),dictionary.get()+out,&output,flags);
        data.consume(input); total+=output;
        if (total>expected || (output && !sink(dictionary.get()+out,output))) return false;
        if (status==TINFL_STATUS_DONE) return !data.remaining && total==expected;
        if ((status!=TINFL_STATUS_HAS_MORE_OUTPUT && status!=TINFL_STATUS_NEEDS_MORE_INPUT) || (!input && !output)) return false;
        out+=output; if (wrapping && out==window) out=0;
    }
#else
    z_stream z={}; int init=inflateInit(&z); if (init!=Z_OK) { low_memory=init==Z_MEM_ERROR; return false; }
    uint8_t out[1024]; bool ok=false; size_t total=0;
    for (;;) {
        data.span(); z.next_in=(Bytef *)data.bytes; z.avail_in=data.available;
        z.next_out=out; z.avail_out=sizeof(out);
        int status=inflate(&z,Z_NO_FLUSH);
        size_t input=data.available-z.avail_in,output=sizeof(out)-z.avail_out;
        data.consume(input); total+=output; low_memory=status==Z_MEM_ERROR;
        if (total>expected || (output && !sink(out,output))) break;
        if (status==Z_STREAM_END) { ok=!data.remaining && total==expected; break; }
        if (status!=Z_OK || (!input && !output)) break;
    }
    inflateEnd(&z); return ok;
#endif
}
static unsigned paeth(unsigned a,unsigned b,unsigned c) {
    int p=(int)a+(int)b-(int)c,pa=abs(p-(int)a),pb=abs(p-(int)b),pc=abs(p-(int)c);
    return pa<=pb && pa<=pc ? a : pb<=pc ? b : c;
}
bool note_read_png(const std::string &png,PalmNote &n,std::string &problem) {
    auto fail=[&](const char *s) { problem=s; return false; };
    if (png.size()>PALM_NOTE_PNG_MAX || png.size()<45 || memcmp(png.data(),"\x89PNG\r\n\x1a\n",8)) return fail("Expected a PNG image of at most 64 KiB");
    const auto *p=(const uint8_t *)png.data();
    unsigned width=0,height=0,depth=0,type=0; bool head=false,end=false,seen_data=false,after_data=false;
    size_t data_first=0,data_end=0,data_size=0;
    std::string palette,alpha;
    for (size_t at=8;at<png.size();) {
        if (png.size()-at<12) return fail("Truncated PNG chunk");
        uint32_t size=u32(p+at); if (size>png.size()-at-12) return fail("PNG chunk exceeds file bounds");
        const uint8_t *name=p+at+4,*body=name+4;
        if (crc(name,size+4)!=u32(body+size)) return fail("PNG checksum mismatch");
        bool idat=!memcmp(name,"IDAT",4);
        if (!head && memcmp(name,"IHDR",4)) return fail("PNG header must be first");
        if (!memcmp(name,"IHDR",4)) {
            if (head || size!=13) return fail("Invalid PNG header");
            head=true; width=u32(body); height=u32(body+4); depth=body[8]; type=body[9];
            bool supported=(type==0 || type==3) ? (depth==1 || depth==2 || depth==4 || depth==8) : ((type==2 || type==4 || type==6) && depth==8);
            if (!width || width>160 || !height || height>PALM_NOTE_BITMAP_MAX/((width+7)/8) || !supported || body[10] || body[11] || body[12]) return fail("PNG must be non-interlaced, at most 160 pixels wide and 32000 decoded bitmap bytes, with supported gray/palette or RGB/RGBA pixels");
        } else if (!memcmp(name,"PLTE",4)) {
            if (seen_data || !palette.empty() || !size || size>768 || size%3) return fail("Invalid PNG palette");
            palette.assign((const char *)body,size);
        } else if (!memcmp(name,"tRNS",4)) {
            if (seen_data || !alpha.empty() || size>256 || (type==0 && size!=2) || (type==2 && size!=6) || (type!=0 && type!=2 && type!=3)) return fail("Invalid PNG transparency");
            alpha.assign((const char *)body,size);
        } else if (idat) {
            if (after_data || data_size+size>PALM_NOTE_PNG_MAX) return fail("Invalid PNG data sequence");
            if (!seen_data) data_first=at;
            seen_data=true; data_end=at+12+size; data_size+=size;
        } else if (!memcmp(name,"IEND",4)) {
            if (size || !seen_data || at+12!=png.size()) return fail("Invalid PNG end");
            end=true;
        } else if (!(name[0]&32)) return fail("Unsupported critical PNG chunk");
        if (seen_data && !idat) after_data=true;
        at+=12+size;
    }
    if (!end || !data_size || (type==3 && (palette.empty() || alpha.size()>palette.size()/3))) return fail("Incomplete PNG image");
    unsigned channels=type==2 ? 3 : type==4 ? 2 : type==6 ? 4 : 1;
    size_t row_bytes=(width*depth*channels+7)/8,bpp=std::max(1u,(channels*depth+7)/8),pos=0; unsigned row=0;
    std::string scan(row_bytes+1,0),previous(row_bytes,0);
    n.pixels.assign(std::max(1u,(width+7)/8)*height,0); n.width=std::max(8u,(width+7)/8*8); n.height=height;
    bool valid=true;
    auto sink=[&](const uint8_t *bytes,size_t count) {
        while (count--) {
            if (row>=height) return false;
            scan[pos++]=(char)*bytes++;
            if (pos!=scan.size()) continue;
            unsigned filter=(uint8_t)scan[0]; if (filter>4) return false;
            auto *current=(uint8_t *)&scan[1];
            for (size_t i=0;i<row_bytes;i++) {
                unsigned a=i>=bpp ? current[i-bpp] : 0,b=(uint8_t)previous[i],c=i>=bpp ? (uint8_t)previous[i-bpp] : 0;
                current[i]+=(uint8_t)(filter==1 ? a : filter==2 ? b : filter==3 ? (a+b)/2 : filter==4 ? paeth(a,b,c) : 0);
            }
            for (unsigned x=0;x<width;x++) {
                unsigned red,green,blue,opacity=255;
                if (type==0 || type==3) {
                    unsigned mask=(1u<<depth)-1,sample=(current[x*depth/8]>>(8-depth-(x*depth%8)))&mask;
                    if (type==3) {
                        if (sample>=palette.size()/3) return false;
                        red=(uint8_t)palette[sample*3]; green=(uint8_t)palette[sample*3+1]; blue=(uint8_t)palette[sample*3+2];
                        if (sample<alpha.size()) opacity=(uint8_t)alpha[sample];
                    } else { red=green=blue=sample*255/mask; if (alpha.size()==2 && sample==(((unsigned)(uint8_t)alpha[0]<<8)|(uint8_t)alpha[1])) opacity=0; }
                } else {
                    auto *pixel=current+x*channels; red=pixel[0]; green=type==4 ? red : pixel[1]; blue=type==4 ? red : pixel[2];
                    if (type==4 || type==6) opacity=pixel[channels-1];
                    if (type==2 && alpha.size()==6 && !alpha[0] && !alpha[2] && !alpha[4] && red==(uint8_t)alpha[1] && green==(uint8_t)alpha[3] && blue==(uint8_t)alpha[5]) opacity=0;
                }
                unsigned gray=(red*299+green*587+blue*114)/1000;
                if ((gray*opacity+255*(255-opacity))/255<128) n.pixels[row*(n.width/8)+x/8]|=(char)(0x80>>(x%8));
            }
            previous.assign((const char *)current,row_bytes); pos=0; row++;
        }
        return true;
    };
    bool low_memory=false; valid=inflate_rows(PngInput(p,data_first,data_end,data_size),(row_bytes+1)*height,sink,low_memory);
    if (low_memory) {
        problem="Not enough memory to decode the PNG image ("+std::to_string(width)+"x"+std::to_string(height)+
            ", "+std::to_string(depth)+"-bit, "+std::to_string(channels)+" channels). Try a 1-bit monochrome PNG";
        return false;
    }
    if (!valid || row!=height || pos) return fail("Invalid, truncated or oversized PNG image stream");
    return true;
}
