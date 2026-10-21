#pragma once
#include "mailtext.h"
#include "hotsync.h"
#include <array>

// Classic MemoDB and Palm OS 4 npadDB. Bitmap bits are MSB-first, 1 = ink.
// Scrolling canvases are bounded by decoded bytes, not the screen height.
constexpr size_t PALM_NOTE_BITMAP_MAX=32000;
constexpr size_t PALM_NOTE_PNG_MAX=65536;
constexpr size_t PALM_NOTE_RECORD_MAX=65489;
struct PalmNote {
    std::string text,title,pixels,header;
    unsigned width=160,height=160;
    uint32_t body_word=1;
};
bool memo_unpack(const uint8_t *data,size_t size,PalmNote &note,std::string &problem);
bool drawing_unpack(const uint8_t *data,size_t size,PalmNote &note,std::string &problem);
size_t memo_pack(const PalmNote &note,uint8_t *data,size_t room);
size_t drawing_pack(const PalmNote &note,const HotSync_Time &now,uint8_t *data,size_t room);
size_t drawing_record_size(const PalmNote &note);
std::array<uint32_t,2> note_fields(const PalmNote &note,bool drawing);
std::string note_png(const PalmNote &note);
bool note_read_png(const std::string &png,PalmNote &note,std::string &problem);
