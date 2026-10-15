#include "notetext.h"
#include <cstring>
#include <algorithm>

static unsigned u16(const uint8_t *p) { return (p[0]<<8)|p[1]; }
static uint32_t u32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3]; }
static void p16(uint8_t *p,unsigned n) { p[0]=n>>8; p[1]=n; }
static void p32(uint8_t *p,uint32_t n) { p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n; }
static uint32_t hash(const std::string &s) { uint32_t h=2166136261u; for (unsigned char c:s) { h^=c; h*=16777619u; } return h; }
std::array<uint32_t,2> note_fields(const PalmNote &n,bool drawing) {
    return {{hash(drawing ? n.title : n.text),drawing ? hash(n.pixels+":"+std::to_string(n.width)+":"+std::to_string(n.height)) : 0}};
}
bool memo_unpack(const uint8_t *p,size_t size,PalmNote &n,std::string &problem) {
    n=PalmNote(); if (!size) { problem="Memo has no NUL terminator"; return false; }
    auto *end=(const uint8_t *)memchr(p,0,size);
    if (!end || end-p>4095) { problem="Memo is unterminated or exceeds 4095 bytes"; return false; }
    for (auto *q=end;q<p+size;q++) if (*q) { problem="Memo has nonzero trailing data"; return false; }
    for (auto *q=p;q<end;q++) {
        if (*q<32 && *q!=9 && *q!=10 && *q!=13) { problem="Memo has control characters unsupported by Google Docs"; return false; }
        if (*q==13) { n.text+='\n'; if (q+1<end && q[1]==10) q++; }
        else n.text+=(char)*q;
    }
    return true;
}
size_t memo_pack(const PalmNote &n,uint8_t *p,size_t room) {
    if (n.text.size()>4095 || n.text.size()+1>room || n.text.find('\0')!=std::string::npos) return 0;
    memcpy(p,n.text.c_str(),n.text.size()+1); return n.text.size()+1;
}
bool drawing_unpack(const uint8_t *p,size_t size,PalmNote &n,std::string &problem) {
    n=PalmNote(); problem.clear();
    auto fail=[&](const char *s) { problem=s; return false; };
    if (size<30) return fail("Truncated Note Pad header");
    unsigned flags=u16(p+28); if (flags&~7u) return fail("Unsupported Note Pad flags");
    size_t at=30+(flags&4 ? 14 : 0);
    if (at>size) return fail("Truncated Note Pad alarm");
    n.header.assign((const char *)p,at);
    if (flags&2) {
        auto *end=(const uint8_t *)memchr(p+at,0,size-at);
        if (!end || end-(p+at)>255) return fail("Invalid Note Pad title");
        n.title.assign((const char *)p+at,end-p-at);
        at+=(n.title.size()+2)&~size_t(1);
    }
    if (!(flags&1)) {
        if (at>size) return fail("Truncated Note Pad name padding");
        for (size_t i=at;i<size;i++) if (p[i]) return fail("Unexpected data after an empty Note Pad record");
        n.pixels.assign(3200,0); return true;
    }
    if (at>size || size-at<24) return fail("Truncated Note Pad drawing header");
    uint32_t width=u32(p+at+4),height=u32(p+at+8),type=u32(p+at+16),length=u32(p+at+20);
    n.body_word=u32(p+at+12);
    if (width>152 || (width+8)%8 || !height) return fail("Invalid Note Pad canvas width or height");
    if (type>1) { problem="Unsupported Note Pad compression type "+std::to_string(type)+" (expected raw or RLE)"; return false; }
    if (length>size-at-24) return fail("Truncated Note Pad bitmap payload");
    if (height>PALM_NOTE_BITMAP_MAX/((width+8)/8)) return fail("Note Pad bitmap exceeds the 32000-byte decoded limit");
    n.width=width+8; n.height=height; size_t expected=n.width/8*n.height;
    const uint8_t *body=p+at+24;
    auto bitmap_fail=[&](const char *message) {
        problem=std::string(message)+" ("+std::to_string(n.width)+"x"+std::to_string(n.height)+
            ", payload "+std::to_string(length)+" bytes, decoded "+std::to_string(n.pixels.size())+
            "/"+std::to_string(expected)+" bytes)";
        return false;
    };
    if (!type) { if (length!=expected) return fail("Wrong uncompressed Note Pad bitmap size"); n.pixels.assign((const char *)body,length); }
    else {
        // pilot-read-notepad processes dataLen/2 complete pairs. A lone final
        // byte is not another run; accept it only after the full canvas has
        // been decoded. A zero repeat count is a no-op, as in that exporter.
        for (size_t i=0;i+1<length;i+=2) {
            if (body[i]>expected-n.pixels.size()) return bitmap_fail("Note Pad RLE exceeds drawing bounds");
            n.pixels.append(body[i],(char)body[i+1]);
        }
        if (n.pixels.size()!=expected) return bitmap_fail(length%2 ? "Truncated Note Pad RLE pair" : "Incomplete Note Pad bitmap");
    }
    for (size_t i=at+24+length;i<size;i++) if (p[i]) return fail("Unexpected Note Pad trailing data");
    return true;
}
static void date(uint8_t *p,const HotSync_Time &t) {
    const unsigned fields[]={t.second,t.minute,t.hour,t.day,t.month,t.year,0};
    for (unsigned i=0;i<7;i++) p16(p+i*2,fields[i]);
}
size_t drawing_record_size(const PalmNote &n) {
    if (n.width<8 || n.width>160 || n.width%8 || !n.height || n.height>PALM_NOTE_BITMAP_MAX/(n.width/8) || n.pixels.size()!=n.width/8*n.height ||
        n.title.size()>255 || n.title.find('\0')!=std::string::npos || (!n.header.empty() && n.header.size()!=30 && n.header.size()!=44)) return 0;
    size_t rle=0;
    for (size_t at=0;at<n.pixels.size();) {
        size_t end=at+1; while (end<n.pixels.size() && end-at<255 && n.pixels[end]==n.pixels[at]) end++;
        rle+=2; at=end;
    }
    size_t total=(n.header.empty() ? 30 : n.header.size())+((n.title.size()+2)&~size_t(1))+24+rle;
    return total<=PALM_NOTE_RECORD_MAX ? total : 0;
}
size_t drawing_pack(const PalmNote &n,const HotSync_Time &now,uint8_t *p,size_t room) {
    size_t total=drawing_record_size(n);
    if (!total || total>room || !p) return 0;
    size_t head=n.header.empty() ? 30 : n.header.size(),name=(n.title.size()+2)&~size_t(1),length=total-head-name-24;
    memset(p,0,total);
    if (n.header.empty()) date(p,now); else memcpy(p,n.header.data(),head);
    date(p+14,now); p16(p+28,3|(head==44 ? 4 : 0));
    memcpy(p+head,n.title.c_str(),n.title.size()+1);
    uint8_t *b=p+head+name;
    p32(b,24+length); p32(b+4,n.width-8); p32(b+8,n.height); p32(b+12,n.body_word);
    p32(b+16,1); p32(b+20,length);
    size_t offset=24;
    for (size_t at=0;at<n.pixels.size();) {
        size_t end=at+1; while (end<n.pixels.size() && end-at<255 && n.pixels[end]==n.pixels[at]) end++;
        b[offset++]=end-at; b[offset++]=(uint8_t)n.pixels[at]; at=end;
    }
    return total;
}
