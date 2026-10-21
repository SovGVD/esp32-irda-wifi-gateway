#pragma once
#include <array>
#include <string>
#include <stdint.h>
#include <stddef.h>

// Classic Palm AddressDB v1. Phone labels: Work, Home, Fax, Other,
// E-mail, Main, Pager, Mobile. The four custom labels stay in Palm app-info.
enum ContactField { CONTACT_LAST, CONTACT_FIRST, CONTACT_COMPANY, CONTACT_PHONE1,
    CONTACT_PHONE2, CONTACT_PHONE3, CONTACT_PHONE4, CONTACT_PHONE5, CONTACT_STREET,
    CONTACT_CITY, CONTACT_REGION, CONTACT_ZIP, CONTACT_COUNTRY, CONTACT_TITLE,
    CONTACT_CUSTOM1, CONTACT_CUSTOM2, CONTACT_CUSTOM3, CONTACT_CUSTOM4, CONTACT_NOTE };
#define CONTACT_NAME_MAX 80
#define CONTACT_FIELD_MAX 255
#define CONTACT_NOTE_MAX 4095
#define CONTACT_RECORD_MAX (2 * CONTACT_NAME_MAX + 16 * CONTACT_FIELD_MAX + CONTACT_NOTE_MAX + 28)
#define CONTACT_SYNC_FIELDS 15
struct PalmContact {
    std::array<std::string,19> text;
    std::array<uint8_t,5> labels = {{0,1,2,7,4}};
    uint8_t show_phone = 0;
};
size_t contact_field_limit(unsigned index);
// The optional problem identifies format/size failures without contact text.
bool contact_unpack_record(const uint8_t *data,size_t len,PalmContact &contact,std::string *problem=nullptr);
size_t contact_pack_record(const PalmContact &contact,uint8_t *data,size_t max_len);
// Per-field baselines, with the five phone/email slots treated as one group.
std::array<uint32_t,CONTACT_SYNC_FIELDS> contact_fields(const PalmContact &contact);
