#include "contacttext.h"
#include <cstring>

size_t contact_field_limit(unsigned i) {
    return i<2 ? CONTACT_NAME_MAX : i==CONTACT_NOTE ? CONTACT_NOTE_MAX : CONTACT_FIELD_MAX;
}
static bool valid(const PalmContact &c) {
    if (c.show_phone>4) return false;
    for (unsigned n=0;n<5;n++) if (c.labels[n]>7) return false;
    for (unsigned n=0;n<19;n++)
        if (c.text[n].size()>contact_field_limit(n) || c.text[n].find('\0')!=std::string::npos) return false;
    return true;
}
bool contact_unpack_record(const uint8_t *p,size_t len,PalmContact &c,std::string *problem) {
    c=PalmContact();
    if (problem) problem->clear();
    auto fail=[&](const std::string &why) { if (problem) *problem=why; return false; };
    if (!p || len<9) return fail("Header is shorter than 9 bytes");
    // Byte 0 is gapfill, and the field mask has only 19 meaningful bits.
    // Like pilot-link's unpack_Address, do not validate unused bits as data.
    // https://github.com/desrod/pilot-link/blob/main/libpisock/address.c
    uint32_t labels=((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
    c.show_phone=labels>>20;
    if (c.show_phone>4) return fail("Displayed phone slot is outside 1-5");
    for (unsigned n=0;n<5;n++) {
        c.labels[n]=(labels>>(n*4))&15;
        if (c.labels[n]>7) return fail("Phone "+std::to_string(n+1)+" has an unsupported label");
    }
    uint32_t fields=((uint32_t)p[4]<<24)|((uint32_t)p[5]<<16)|((uint32_t)p[6]<<8)|p[7];
    static const char *names[]={"Last name","First name","Company","Phone 1","Phone 2","Phone 3","Phone 4","Phone 5",
        "Street","City","Region","Postcode","Country","Title","Custom 1","Custom 2","Custom 3","Custom 4","Note"};
    size_t at=9;
    for (unsigned n=0;n<19;n++) {
        if (!(fields&(1u<<n))) continue;
        const auto *end=(const uint8_t *)memchr(p+at,0,len-at);
        if (!end) return fail(std::string(names[n])+" has no terminating zero");
        size_t size=end-(p+at),limit=contact_field_limit(n);
        if (size>limit) return fail(std::string(names[n])+" is "+std::to_string(size)+" bytes; limit is "+std::to_string(limit));
        c.text[n].assign((const char *)p+at,size); at=end-p+1;
    }
    // Zero padding after the strings is harmless. Unknown nonzero data is
    // still rejected so rewriting cannot silently discard an extension.
    for (size_t n=at;n<len;n++) if (p[n])
        return fail("Unrecognized trailing data after the Address Book fields");
    return true;
}
size_t contact_pack_record(const PalmContact &c,uint8_t *p,size_t max) {
    if (!valid(c)) return 0;
    size_t len=9;
    for (const auto &s:c.text) if (!s.empty()) len+=s.size()+1;
    if (len>max) return 0;
    memset(p,0,len);
    uint32_t labels=(uint32_t)c.show_phone<<20,fields=0;
    for (unsigned n=0;n<5;n++) labels|=(uint32_t)c.labels[n]<<(n*4);
    p[1]=labels>>16; p[2]=labels>>8; p[3]=labels;
    size_t at=9;
    for (unsigned n=0;n<19;n++) {
        if (c.text[n].empty()) continue;
        fields|=1u<<n;
        if (n==CONTACT_COMPANY) p[8]=at-8;
        memcpy(p+at,c.text[n].c_str(),c.text[n].size()+1); at+=c.text[n].size()+1;
    }
    p[4]=fields>>24; p[5]=fields>>16; p[6]=fields>>8; p[7]=fields;
    return len;
}
static uint32_t hash(const std::string &s) {
    uint32_t h=2166136261u;
    for (unsigned char c:s) { h^=c; h*=16777619u; }
    return h;
}
std::array<uint32_t,CONTACT_SYNC_FIELDS> contact_fields(const PalmContact &c) {
    std::array<uint32_t,CONTACT_SYNC_FIELDS> out;
    for (unsigned n=0;n<3;n++) out[n]=hash(c.text[n]);
    out[3]=hash(c.text[CONTACT_TITLE]);
    std::string phones;
    for (unsigned n=0;n<5;n++) {
        phones+=c.text[CONTACT_PHONE1+n]; phones+='\0';
        phones+=(char)(c.text[CONTACT_PHONE1+n].empty() ? 0 : c.labels[n]);
    }
    out[4]=hash(phones);
    for (unsigned n=0;n<5;n++) out[5+n]=hash(c.text[CONTACT_STREET+n]);
    for (unsigned n=0;n<4;n++) out[10+n]=hash(c.text[CONTACT_CUSTOM1+n]);
    out[14]=hash(c.text[CONTACT_NOTE]);
    return out;
}
