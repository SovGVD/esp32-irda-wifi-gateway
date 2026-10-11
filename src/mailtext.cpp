#include "mailtext.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------------------
// Character Sets
// ------------------------------------------------------------------------------

// Unicode for bytes 0x80 - 0x9F of Windows-1252 (0 = unassigned)
static const uint16_t k_cp1252_high[32] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
};

// Unicode for bytes 0x80 - 0xBF of Windows-1251; 0xC0 - 0xFF are U+0410 - U+044F
static const uint16_t k_cp1251_high[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

// Unicode for bytes 0xC0 - 0xFF of KOI8-R
static const uint16_t k_koi8_letters[64] = {
    0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433,
    0x0445, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E,
    0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432,
    0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A,
    0x042E, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413,
    0x0425, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E,
    0x041F, 0x042F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412,
    0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427, 0x042A,
};

// Nearest ASCII letter for U+00C0 - U+017F, for accented letters the Palm cannot show
static const char k_latin_fold[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNn?NnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

enum SourceCharset { SOURCE_UTF8, SOURCE_LATIN, SOURCE_CP1251, SOURCE_KOI8, SOURCE_ISO5 };

static std::string lower(const std::string &text) {
    std::string out = text;
    for (size_t i = 0; i < out.length(); i++) {
        out[i] = (char)tolower((unsigned char)out[i]);
    }
    return out;
}

static SourceCharset source_charset(const std::string &name) {
    std::string n = lower(name);
    if (n == "windows-1251" || n == "cp1251") return SOURCE_CP1251;
    if (n == "koi8-r" || n == "koi8-u") return SOURCE_KOI8;
    if (n == "iso-8859-5") return SOURCE_ISO5;
    if (n.compare(0, 9, "iso-8859-") == 0 || n == "windows-1252" || n == "cp1252" || n == "latin1") return SOURCE_LATIN;
    return SOURCE_UTF8; // Also right for US-ASCII, and the best guess for anything unknown
}

static uint32_t latin_to_unicode(uint8_t b) {
    if (b < 0x80 || b >= 0xA0) {
        return b;
    }
    return k_cp1252_high[b - 0x80] != 0 ? k_cp1252_high[b - 0x80] : '?';
}

static uint32_t cp1251_to_unicode(uint8_t b) {
    if (b < 0x80) {
        return b;
    }
    if (b >= 0xC0) {
        return 0x0410 + (b - 0xC0);
    }
    return k_cp1251_high[b - 0x80] != 0 ? k_cp1251_high[b - 0x80] : '?';
}

// One byte of a single-byte character set
static uint32_t byte_to_unicode(SourceCharset source, uint8_t b) {
    switch (source) {
    case SOURCE_CP1251:
        return cp1251_to_unicode(b);
    case SOURCE_KOI8:
        if (b < 0x80) return b;
        if (b >= 0xC0) return k_koi8_letters[b - 0xC0];
        return (b == 0xA3) ? 0x0451 : (b == 0xB3) ? 0x0401 : '?';
    case SOURCE_ISO5:
        if (b <= 0xA0 || b == 0xAD) return b;
        return (b == 0xF0) ? 0x2116 : (b == 0xFD) ? 0x00A7 : 0x0400 + (b - 0xA0);
    default:
        return latin_to_unicode(b);
    }
}

static uint32_t palm_to_unicode(uint8_t b, PalmCharset charset) {
    return (charset == PALM_CYRILLIC) ? cp1251_to_unicode(b) : latin_to_unicode(b);
}

// The Palm's byte for a character: -1 if it has no place in text at all
// (invisible marks, emoji), '?' if nothing comes close
static int palm_char(uint32_t code, PalmCharset target) {
    if (code < 0x80) {
        return (code >= 0x20 || code == '\n' || code == '\t') ? (int)code : -1;
    }
    if (code == 0x00A0) return ' ';
    if (code == 0x00AD) return -1;

    if (target == PALM_CYRILLIC) {
        if (code >= 0x0410 && code <= 0x044F) {
            return 0xC0 + (code - 0x0410);
        }
        for (int i = 0; i < 64; i++) {
            if (k_cp1251_high[i] == code) return 0x80 + i;
        }
    } else {
        if (code <= 0xFF) {
            return (int)code;
        }
        for (int i = 0; i < 32; i++) {
            if (k_cp1252_high[i] == code) return 0x80 + i;
        }
    }
    if (code >= 0x00C0 && code <= 0x017F) {
        return k_latin_fold[code - 0x00C0];
    }

    if ((code >= 0x2000 && code <= 0x200A) || code == 0x202F || code == 0x205F || code == 0x3000) return ' ';
    if ((code >= 0x2010 && code <= 0x2015) || code == 0x2212) return '-';
    if (code == 0x2018 || code == 0x2019 || code == 0x2032) return '\'';
    if (code == 0x201C || code == 0x201D || code == 0x2033) return '"';
    if (code == 0x2022) return '*';
    if (code == 0x00AB || code == 0x00BB) return '"';
    if ((code >= 0x200B && code <= 0x200F) || (code >= 0x2028 && code <= 0x202E) || (code >= 0x2060 && code <= 0x206F) ||
        (code >= 0xFE00 && code <= 0xFE0F) || code == 0xFEFF || code >= 0x1F000) {
        return -1;
    }
    return '?';
}

static void append_utf8(std::string &out, uint32_t code) {
    if (code < 0x80) {
        out += (char)code;
    } else if (code < 0x800) {
        out += (char)(0xC0 | (code >> 6));
        out += (char)(0x80 | (code & 0x3F));
    } else {
        out += (char)(0xE0 | (code >> 12));
        out += (char)(0x80 | ((code >> 6) & 0x3F));
        out += (char)(0x80 | (code & 0x3F));
    }
}

static std::string palm_to_utf8(const std::string &text, PalmCharset charset) {
    std::string out;
    for (size_t i = 0; i < text.length(); i++) {
        append_utf8(out, palm_to_unicode((uint8_t)text[i], charset));
    }
    return out;
}

int MailUtf8Reader::flush(uint32_t *out) {
    int n = held_count_;
    for (int i = 0; i < n; i++) {
        out[i] = latin_to_unicode(held_[i]);
    }
    held_count_ = 0;
    pending_ = 0;
    return n;
}

int MailUtf8Reader::push(uint8_t b, uint32_t *out) {
    int n = 0;
    if (pending_ > 0) {
        if ((b & 0xC0) == 0x80) {
            code_ = (code_ << 6) | (b & 0x3F);
            if (--pending_ > 0) {
                held_[held_count_++] = b;
                return 0;
            }
            held_count_ = 0;
            out[0] = code_;
            return 1;
        }
        n = flush(out); // Not UTF-8 after all: the bytes as they are
    }
    if (b < 0x80) {
        out[n++] = b;
    } else if (b >= 0xC2 && b <= 0xF4) {
        pending_ = (b >= 0xF0) ? 3 : (b >= 0xE0) ? 2 : 1;
        code_ = b & (0x3F >> pending_);
        held_[0] = b;
        held_count_ = 1;
    } else {
        out[n++] = latin_to_unicode(b);
    }
    return n;
}

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string mail_base64(const std::string &data) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.length(); i += 3) {
        uint32_t group = (uint8_t)data[i] << 16;
        if (i + 1 < data.length()) group |= (uint8_t)data[i + 1] << 8;
        if (i + 2 < data.length()) group |= (uint8_t)data[i + 2];
        out += alphabet[(group >> 18) & 0x3F];
        out += alphabet[(group >> 12) & 0x3F];
        out += (i + 1 < data.length()) ? alphabet[(group >> 6) & 0x3F] : '=';
        out += (i + 2 < data.length()) ? alphabet[group & 0x3F] : '=';
    }
    return out;
}

// ------------------------------------------------------------------------------
// MailDB Records
// ------------------------------------------------------------------------------

bool mail_unpack_record(const uint8_t *data, size_t len, MailMessage &msg) {
    if (len < 6) {
        return false;
    }
    uint16_t date = (uint16_t)((data[0] << 8) | data[1]);
    msg.dated = date != 0;
    msg.year = 1904 + (date >> 9);
    msg.month = (date >> 5) & 0x0F;
    msg.day = date & 0x1F;
    msg.hour = data[2];
    msg.minute = data[3];

    uint8_t flags = data[4];
    msg.read = (flags & 0x80) != 0;
    msg.signature = (flags & 0x40) != 0;
    msg.confirm_read = (flags & 0x20) != 0;
    msg.confirm_delivery = (flags & 0x10) != 0;
    msg.priority = (flags >> 2) & 0x03;
    msg.addressing = flags & 0x03;

    // Eight strings, each ended by a zero byte
    std::string *fields[8] = {&msg.subject, &msg.from, &msg.to, &msg.cc, &msg.bcc, &msg.reply_to, &msg.sent_to, &msg.body};
    size_t at = 6;
    for (int i = 0; i < 8; i++) {
        size_t end = at;
        while (end < len && data[end] != 0) {
            end++;
        }
        fields[i]->assign((const char *)data + at, end - at);
        at = (end < len) ? end + 1 : len;
    }
    return true;
}

size_t mail_pack_record(const MailMessage &msg, uint8_t *dest, size_t max_len) {
    const std::string *fields[8] = {&msg.subject, &msg.from, &msg.to, &msg.cc, &msg.bcc, &msg.reply_to, &msg.sent_to, &msg.body};
    size_t needed = 6;
    for (int i = 0; i < 8; i++) {
        needed += strlen(fields[i]->c_str()) + 1;
    }
    if (needed > max_len) {
        return 0;
    }

    uint16_t date = 0;
    if (msg.dated && msg.year >= 1904 && msg.year <= 2031) {
        date = (uint16_t)(((msg.year - 1904) << 9) | ((msg.month & 0x0F) << 5) | (msg.day & 0x1F));
    }
    dest[0] = (uint8_t)(date >> 8);
    dest[1] = (uint8_t)date;
    dest[2] = (date != 0) ? msg.hour : 0;
    dest[3] = (date != 0) ? msg.minute : 0;
    dest[4] = (msg.read ? 0x80 : 0) | (msg.signature ? 0x40 : 0) | (msg.confirm_read ? 0x20 : 0) |
              (msg.confirm_delivery ? 0x10 : 0) | ((msg.priority & 0x03) << 2) | (msg.addressing & 0x03);
    dest[5] = 0;

    size_t at = 6;
    for (int i = 0; i < 8; i++) {
        size_t n = strlen(fields[i]->c_str()); // Text ends at a zero byte, should one have slipped in
        memcpy(dest + at, fields[i]->c_str(), n + 1);
        at += n + 1;
    }
    return at;
}

void mail_unpack_sync_options(const uint8_t *data, size_t len, MailSyncOptions &options) {
    options = MailSyncOptions();
    if (len < 6) {
        return;
    }
    options.type = data[0];
    options.get_high_priority = data[1] != 0;
    options.retrieve_matching = data[2] != 0;
    uint16_t truncate = (uint16_t)((data[4] << 8) | data[5]);
    if (truncate > 0) {
        options.truncate = truncate;
    }

    std::string *fields[3] = {&options.to, &options.from, &options.subject};
    size_t at = 6;
    for (int i = 0; i < 3 && at < len; i++) {
        size_t end = at;
        while (end < len && data[end] != 0) {
            end++;
        }
        fields[i]->assign((const char *)data + at, end - at);
        at = end + 1;
    }
}

static bool contains(const std::string &text, const std::string &part) {
    return !part.empty() && lower(text).find(lower(part)) != std::string::npos;
}

bool mail_filter_accepts(const MailSyncOptions &options, const std::string &from, const std::string &to,
                         const std::string &subject, bool high_priority) {
    if (options.type != MAIL_SYNC_FILTER) {
        return true;
    }
    if (options.get_high_priority && high_priority) {
        return true;
    }
    bool matches = contains(to, options.to) || contains(from, options.from) || contains(subject, options.subject);
    return options.retrieve_matching ? matches : !matches;
}

// ------------------------------------------------------------------------------
// Incoming Text
// ------------------------------------------------------------------------------

MailTextDecoder::MailTextDecoder(const std::string &charset, const std::string &encoding, bool html,
                                 PalmCharset target, size_t max_chars)
    : html_(html), target_(target), max_chars_(max_chars) {
    std::string name = lower(encoding);
    encoding_ = (name == "base64") ? BASE64 : (name == "quoted-printable") ? QUOTED : PLAIN;
    switch (source_charset(charset)) {
    case SOURCE_LATIN:  source_ = LATIN; break;
    case SOURCE_CP1251: source_ = CP1251; break;
    case SOURCE_KOI8:   source_ = KOI8; break;
    case SOURCE_ISO5:   source_ = ISO5; break;
    default:            source_ = UTF8; break;
    }
}

void MailTextDecoder::feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len && !full(); i++) {
        uint8_t b = data[i];
        switch (encoding_) {
        case PLAIN:
            decoded_byte(b);
            break;

        case BASE64: {
            int value = base64_value((char)b);
            if (value < 0) {
                break; // Line ends and padding
            }
            bits_ = (bits_ << 6) | (uint32_t)value;
            bit_count_ += 6;
            if (bit_count_ >= 8) {
                bit_count_ -= 8;
                decoded_byte((uint8_t)(bits_ >> bit_count_));
            }
            break;
        }

        case QUOTED:
            if (quoted_left_ == 0) {
                if (b == '=') {
                    quoted_left_ = 2;
                } else {
                    decoded_byte(b);
                }
            } else if (quoted_left_ == 2) {
                if (b == '\n') {
                    quoted_left_ = 0; // "=" at the end of a line joins it to the next
                } else {
                    quoted_first_ = b;
                    quoted_left_ = 1;
                }
            } else {
                quoted_left_ = 0;
                if (quoted_first_ == '\r') {
                    if (b != '\n') {
                        decoded_byte(b);
                    }
                } else if (hex_value((char)quoted_first_) >= 0 && hex_value((char)b) >= 0) {
                    decoded_byte((uint8_t)(hex_value((char)quoted_first_) * 16 + hex_value((char)b)));
                } else {
                    decoded_byte('='); // Not an escape after all
                    decoded_byte(quoted_first_);
                    decoded_byte(b);
                }
            }
            break;
        }
    }
}

void MailTextDecoder::decoded_byte(uint8_t b) {
    if (source_ != UTF8) {
        static const SourceCharset sources[] = {SOURCE_UTF8, SOURCE_LATIN, SOURCE_CP1251, SOURCE_KOI8, SOURCE_ISO5};
        character(byte_to_unicode(sources[source_], b));
        return;
    }
    uint32_t codes[4];
    int n = utf8_.push(b, codes);
    for (int i = 0; i < n; i++) {
        character(codes[i]);
    }
}

void MailTextDecoder::character(uint32_t code) {
    if (html_) {
        markup(code);
    } else if (code == '\n') {
        // At most one blank line in a row; spaces at the end of a line are dropped
        if (newlines_ < 2) {
            emit('\n');
            newlines_++;
        }
        spaces_ = 0;
    } else if (code == ' ' || code == '\t' || code == 0x00A0) {
        if (spaces_ < 40) {
            spaces_++;
        }
    } else if (code != '\r') {
        visible(code);
    }
}

// A character that shows: written after the spaces that came before it
void MailTextDecoder::visible(uint32_t code) {
    int c = palm_char(code, target_);
    if (c < 0) {
        return;
    }
    // In HTML, space at the start of a line is layout, not text
    for (; spaces_ > 0; spaces_--) {
        if (!html_ || newlines_ == 0) {
            emit(' ');
        }
    }
    emit((char)c);
    newlines_ = 0;
}

void MailTextDecoder::emit(char c) {
    if (out_.length() < max_chars_) {
        out_ += c;
    }
}

void MailTextDecoder::line_break(uint8_t count) {
    while (newlines_ < count) {
        emit('\n');
        newlines_++;
    }
    spaces_ = 0;
}

void MailTextDecoder::markup(uint32_t code) {
    switch (state_) {
    case TEXT:
        if (code == '<') {
            state_ = TAG;
            held_.clear();
        } else if (code == '&') {
            state_ = ENTITY;
            held_.clear();
        } else if (code == ' ' || code == '\t' || code == '\r' || code == '\n' || code == 0x00A0) {
            spaces_ = 1;
        } else {
            visible(code);
        }
        break;

    case TAG:
        if (code < 0x80 && (isalnum((int)code) || code == '/' || code == '!' || code == '-') && held_.length() < 12) {
            held_ += (char)tolower((int)code);
            if (held_ == "!--") {
                state_ = COMMENT;
                matched_ = 0;
            }
            break;
        }
        if (held_.empty()) {
            // "a < b": not a tag
            state_ = TEXT;
            visible('<');
            markup(code);
            break;
        }
        end_tag_name();
        markup(code);
        break;

    case TAG_REST: // To the end of the tag; a '>' inside a quoted attribute does not count
        if (quote_ != 0) {
            if (code == (uint32_t)quote_) {
                quote_ = 0;
            }
        } else if (code == '"' || code == '\'') {
            quote_ = (char)code;
        } else if (code == '>') {
            state_ = TEXT;
        }
        break;

    case ENTITY:
        if (code < 0x80 && (isalnum((int)code) || code == '#') && held_.length() < 10) {
            held_ += (char)code;
            break;
        }
        state_ = TEXT;
        end_entity(code == ';');
        if (code != ';') {
            markup(code);
        }
        break;

    case SKIP_ELEMENT: // Until "</script" (any case), then to the end of that tag
        if (code < 0x80 && tolower((int)code) == closing_[matched_]) {
            if (++matched_ == closing_.length()) {
                state_ = TAG_REST;
                quote_ = 0;
            }
        } else {
            matched_ = (code == '<') ? 1 : 0;
        }
        break;

    case COMMENT: // Until "-->"
        if (code == '-') {
            matched_ = (matched_ < 2) ? matched_ + 1 : 2;
        } else if (code == '>' && matched_ == 2) {
            state_ = TEXT;
        } else {
            matched_ = 0;
        }
        break;
    }
}

// The tag's name is known: what it means for the text
void MailTextDecoder::end_tag_name() {
    std::string name = held_;
    while (!name.empty() && name[name.length() - 1] == '/') {
        name.erase(name.length() - 1);
    }
    state_ = TAG_REST;
    quote_ = 0;

    if (name == "script" || name == "style" || name == "head" || name == "title") {
        closing_ = "</" + name;
        matched_ = 0;
        state_ = SKIP_ELEMENT;
        return;
    }
    bool closing = !name.empty() && name[0] == '/';
    if (closing) {
        name.erase(0, 1);
    }

    static const char *const blocks[] = {"div", "tr", "table", "ul", "ol", "li", "blockquote", "pre", "hr",
                                         "h1", "h2", "h3", "h4", "h5", "h6"};
    if (name == "br") {
        // A second <br> in a row makes a blank line
        if (newlines_ < 2) {
            emit('\n');
            newlines_++;
        }
        spaces_ = 0;
    } else if (name == "p") {
        line_break(2);
    } else if (name == "td" || name == "th") {
        spaces_ = 1;
    } else {
        for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
            if (name == blocks[i]) {
                line_break(1);
                if (name == "li" && !closing) {
                    visible('-');
                    spaces_ = 1;
                }
                break;
            }
        }
    }
}

void MailTextDecoder::end_entity(bool terminated) {
    static const struct {
        const char *name;
        uint16_t code;
    } entities[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
                    {"copy", 0x00A9}, {"reg", 0x00AE}, {"deg", 0x00B0}, {"middot", 0x00B7}, {"laquo", 0x00AB},
                    {"raquo", 0x00BB}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}, {"bull", 0x2022},
                    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"euro", 0x20AC},
                    {"trade", 0x2122}};

    uint32_t code = 0;
    if (held_.length() > 1 && held_[0] == '#') {
        bool hex = held_[1] == 'x' || held_[1] == 'X';
        code = (uint32_t)strtoul(held_.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
    } else {
        for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
            if (held_ == entities[i].name) {
                code = entities[i].code;
            }
        }
    }

    if (code == ' ' || code == 0x00A0) {
        spaces_ = 1;
    } else if (code != 0) {
        visible(code);
    } else {
        // Not an entity: the text as it stands
        visible('&');
        for (size_t i = 0; i < held_.length(); i++) {
            visible((uint8_t)held_[i]);
        }
        if (terminated) {
            visible(';');
        }
    }
}

void MailTextDecoder::finish() {
    uint32_t codes[4];
    int n = utf8_.flush(codes);
    for (int i = 0; i < n; i++) {
        character(codes[i]);
    }
    while (!out_.empty() && (out_[out_.length() - 1] == '\n' || out_[out_.length() - 1] == ' ')) {
        out_.erase(out_.length() - 1);
    }
}

// ------------------------------------------------------------------------------
// Incoming Headers
// ------------------------------------------------------------------------------

std::string mail_header_field(const std::string &headers, const char *name) {
    size_t name_len = strlen(name);
    size_t at = 0;
    while (at < headers.length()) {
        size_t end = headers.find('\n', at);
        if (end == std::string::npos) {
            end = headers.length();
        }
        if (end - at > name_len && headers[at + name_len] == ':' && strncasecmp(headers.c_str() + at, name, name_len) == 0) {
            // The value runs on over the lines that start with white space
            std::string value;
            size_t from = at + name_len + 1;
            for (;;) {
                value.append(headers, from, end - from);
                if (end + 1 >= headers.length() || (headers[end + 1] != ' ' && headers[end + 1] != '\t')) {
                    break;
                }
                from = end + 1;
                end = headers.find('\n', from);
                if (end == std::string::npos) {
                    end = headers.length();
                }
            }
            std::string unfolded;
            for (size_t i = 0; i < value.length(); i++) {
                if (value[i] != '\r' && value[i] != '\n') {
                    unfolded += (value[i] == '\t') ? ' ' : value[i];
                }
            }
            size_t first = unfolded.find_first_not_of(' ');
            size_t last = unfolded.find_last_not_of(' ');
            return (first == std::string::npos) ? std::string() : unfolded.substr(first, last - first + 1);
        }
        at = end + 1;
    }
    return std::string();
}

// Appends text in a named character set, converted for the Palm
static void append_for_palm(std::string &out, const std::string &bytes, SourceCharset source, PalmCharset target) {
    MailUtf8Reader utf8;
    for (size_t i = 0; i <= bytes.length(); i++) {
        uint32_t codes[4];
        int n;
        if (i == bytes.length()) {
            n = utf8.flush(codes);
        } else if (source == SOURCE_UTF8) {
            n = utf8.push((uint8_t)bytes[i], codes);
        } else {
            codes[0] = byte_to_unicode(source, (uint8_t)bytes[i]);
            n = 1;
        }
        for (int k = 0; k < n; k++) {
            int c = palm_char(codes[k] == '\n' || codes[k] == '\t' ? ' ' : codes[k], target);
            if (c >= 0) {
                out += (char)c;
            }
        }
    }
}

std::string mail_decode_header(const std::string &raw, PalmCharset target) {
    std::string out;
    std::string plain; // Text between encoded words, kept back to see what follows it
    bool after_word = false;
    size_t at = 0;

    while (at < raw.length()) {
        // An encoded word: =?charset?B-or-Q?text?=
        size_t charset_end, text_start, text_end;
        if (raw.compare(at, 2, "=?") == 0 && (charset_end = raw.find('?', at + 2)) != std::string::npos &&
            charset_end + 2 < raw.length() && raw[charset_end + 2] == '?' &&
            (text_end = raw.find("?=", text_start = charset_end + 3)) != std::string::npos) {
            char kind = (char)tolower((unsigned char)raw[charset_end + 1]);
            std::string charset = raw.substr(at + 2, charset_end - at - 2);
            size_t language = charset.find('*');
            if (language != std::string::npos) {
                charset.erase(language);
            }

            std::string bytes;
            if (kind == 'b') {
                uint32_t bits = 0;
                int count = 0;
                for (size_t i = text_start; i < text_end; i++) {
                    int value = base64_value(raw[i]);
                    if (value < 0) {
                        continue;
                    }
                    bits = (bits << 6) | (uint32_t)value;
                    count += 6;
                    if (count >= 8) {
                        count -= 8;
                        bytes += (char)(bits >> count);
                    }
                }
            } else {
                for (size_t i = text_start; i < text_end; i++) {
                    if (raw[i] == '_') {
                        bytes += ' ';
                    } else if (raw[i] == '=' && i + 2 < text_end && hex_value(raw[i + 1]) >= 0 && hex_value(raw[i + 2]) >= 0) {
                        bytes += (char)(hex_value(raw[i + 1]) * 16 + hex_value(raw[i + 2]));
                        i += 2;
                    } else {
                        bytes += raw[i];
                    }
                }
            }

            // White space between two encoded words only separates them
            if (!(after_word && plain.find_first_not_of(' ') == std::string::npos)) {
                append_for_palm(out, plain, SOURCE_UTF8, target);
            }
            plain.clear();
            append_for_palm(out, bytes, source_charset(charset), target);
            after_word = true;
            at = text_end + 2;
            continue;
        }
        plain += raw[at++];
    }
    append_for_palm(out, plain, SOURCE_UTF8, target);
    return out;
}

// ------------------------------------------------------------------------------
// Dates
// ------------------------------------------------------------------------------

// Days since 1970-01-01 for a date of the Gregorian calendar, and back
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *year, unsigned *month, unsigned *day) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    *year = (int)(yoe + era * 400) + (*month <= 2);
}

int64_t mail_seconds(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
    return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

static const char k_months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

bool mail_parse_date(const std::string &text, int64_t *utc_seconds) {
    const char *p = text.c_str();
    const char *comma = strchr(p, ',');
    if (comma != nullptr) {
        p = comma + 1; // Skip the day of the week
    }
    while (*p == ' ') p++;
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    char *next;
    long day = strtol(p, &next, 10);
    p = next;
    while (*p == ' ' || *p == '-') p++;

    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(p, k_months + i * 3, 3) == 0) {
            month = i + 1;
        }
    }
    if (month == 0) {
        return false;
    }
    while (isalpha((unsigned char)*p)) p++;
    while (*p == ' ' || *p == '-') p++;

    long year = strtol(p, &next, 10);
    if (next == p) {
        return false;
    }
    if (next - p <= 2) {
        year += (year < 50) ? 2000 : 1900;
    }
    p = next;
    while (*p == ' ') p++;

    long hour = strtol(p, &next, 10);
    if (next == p || *next != ':') {
        return false;
    }
    p = next + 1;
    long minute = strtol(p, &next, 10);
    long second = 0;
    p = next;
    if (*p == ':') {
        second = strtol(p + 1, &next, 10);
        p = next;
    }
    while (*p == ' ') p++;

    long offset = 0; // Minutes east of UTC
    if (*p == '+' || *p == '-') {
        long zone = strtol(p + 1, nullptr, 10);
        offset = (zone / 100) * 60 + zone % 100;
        if (*p == '-') {
            offset = -offset;
        }
    } else {
        static const struct {
            const char *name;
            int hours;
        } zones[] = {{"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5}, {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7}};
        for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
            if (strncasecmp(p, zones[i].name, 3) == 0) {
                offset = zones[i].hours * 60;
            }
        }
    }
    if (day < 1 || day > 31 || year < 1970 || year > 2100 || hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    *utc_seconds = mail_seconds((uint16_t)year, (uint8_t)month, (uint8_t)day, (uint8_t)hour, (uint8_t)minute, (uint8_t)second) -
                   offset * 60;
    return true;
}

void mail_set_date(MailMessage &msg, int64_t utc_seconds, int offset_minutes) {
    int64_t local = utc_seconds + (int64_t)offset_minutes * 60;
    int64_t days = (local >= 0 ? local : local - 86399) / 86400;
    int64_t in_day = local - days * 86400;
    int year;
    unsigned month, day;
    civil_from_days(days, &year, &month, &day);

    // The record keeps the year in seven bits, counted from 1904
    msg.dated = year >= 1904 && year <= 2031;
    msg.year = (uint16_t)year;
    msg.month = (uint8_t)month;
    msg.day = (uint8_t)day;
    msg.hour = (uint8_t)(in_day / 3600);
    msg.minute = (uint8_t)((in_day % 3600) / 60);
}

// ------------------------------------------------------------------------------
// Outgoing Mail
// ------------------------------------------------------------------------------

static bool is_address_char(char c) {
    return (unsigned char)c > 0x20 && (unsigned char)c < 0x7F && strchr("<>()[],;:\\\"", c) == nullptr;
}

void mail_extract_addresses(const std::string &field, std::vector<std::string> &out) {
    // An address is the run of address characters around each '@'
    size_t from = 0;
    for (;;) {
        size_t at = field.find('@', from);
        if (at == std::string::npos) {
            return;
        }
        size_t start = at;
        while (start > 0 && is_address_char(field[start - 1]) && field[start - 1] != '@') {
            start--;
        }
        size_t end = at + 1;
        while (end < field.length() && is_address_char(field[end]) && field[end] != '@') {
            end++;
        }
        while (end > at + 1 && field[end - 1] == '.') {
            end--; // "Write to me@example.com."
        }
        if (start < at && end > at + 1) {
            out.push_back(field.substr(start, end - start));
        }
        from = end > at ? end : at + 1;
    }
}

static bool is_ascii(const std::string &text) {
    for (size_t i = 0; i < text.length(); i++) {
        if ((unsigned char)text[i] >= 0x80) {
            return false;
        }
    }
    return true;
}

// Header text: line ends would start a new header, so they become spaces
static std::string one_line(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.length(); i++) {
        unsigned char c = (unsigned char)text[i];
        out += (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
    }
    return out;
}

// Text for a header: as it is if plain ASCII, else as encoded words of UTF-8
static std::string header_text(const std::string &palm_text, PalmCharset charset) {
    std::string utf8 = palm_to_utf8(one_line(palm_text), charset);
    if (is_ascii(utf8)) {
        return utf8;
    }
    // An encoded word may be 75 characters long: 45 bytes of text per word,
    // divided between characters
    std::string out;
    size_t at = 0;
    while (at < utf8.length()) {
        size_t end = at + 45;
        if (end >= utf8.length()) {
            end = utf8.length();
        } else {
            while (end > at && ((unsigned char)utf8[end] & 0xC0) == 0x80) {
                end--;
            }
        }
        if (!out.empty()) {
            out += "\r\n ";
        }
        out += "=?UTF-8?B?" + mail_base64(utf8.substr(at, end - at)) + "?=";
        at = end;
    }
    return out;
}

static std::string address_list(const std::string &field) {
    std::vector<std::string> addresses;
    mail_extract_addresses(field, addresses);
    std::string out;
    for (size_t i = 0; i < addresses.size(); i++) {
        if (i > 0) {
            out += ",\r\n ";
        }
        out += addresses[i];
    }
    return out;
}

// Body text as quoted-printable: any byte, in lines of at most 76 characters
static std::string quoted_printable(const std::string &text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    size_t column = 0;
    for (size_t i = 0; i < text.length(); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            out += "\r\n";
            column = 0;
            continue;
        }
        bool line_ends = (i + 1 == text.length() || text[i + 1] == '\n');
        bool literal = (c >= 33 && c <= 126 && c != '=') || ((c == ' ' || c == '\t') && !line_ends);
        size_t width = literal ? 1 : 3;
        if (column + width > 75) {
            out += "=\r\n";
            column = 0;
        }
        if (literal) {
            out += (char)c;
        } else {
            out += '=';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
        column += width;
    }
    return out;
}

std::string mail_compose(const MailMessage &msg, const MailIdentity &sender, PalmCharset charset,
                         const std::string &signature, int64_t utc_seconds, int offset_minutes,
                         const std::string &message_id) {
    std::string out;

    // From: "Name" <address>
    out += "From: ";
    if (!sender.name.empty()) {
        std::string name = header_text(sender.name, charset);
        if (name.compare(0, 2, "=?") == 0) {
            out += name + " ";
        } else {
            out += '"';
            for (size_t i = 0; i < name.length(); i++) {
                if (name[i] != '"' && name[i] != '\\') {
                    out += name[i];
                }
            }
            out += "\" ";
        }
    }
    out += "<" + sender.address + ">\r\n";

    std::string list = address_list(msg.to);
    if (!list.empty()) {
        out += "To: " + list + "\r\n";
    }
    list = address_list(msg.cc);
    if (!list.empty()) {
        out += "Cc: " + list + "\r\n";
    }
    list = address_list(msg.reply_to);
    if (!list.empty()) {
        out += "Reply-To: " + list + "\r\n";
    }
    out += "Subject: " + header_text(msg.subject, charset) + "\r\n";

    if (utc_seconds > 0) {
        MailMessage local;
        mail_set_date(local, utc_seconds, offset_minutes);
        int64_t days = (utc_seconds + (int64_t)offset_minutes * 60) / 86400;
        static const char weekdays[] = "ThuFriSatSunMonTueWed"; // 1970-01-01 was a Thursday
        int second = (int)((utc_seconds + (int64_t)offset_minutes * 60) % 60);
        int zone = abs(offset_minutes);
        char line[64];
        snprintf(line, sizeof(line), "Date: %.3s, %u %.3s %u %02u:%02u:%02d %c%02d%02d\r\n", weekdays + (days % 7) * 3,
                 local.day, k_months + (local.month - 1) * 3, local.year, local.hour, local.minute, second,
                 offset_minutes < 0 ? '-' : '+', zone / 60, zone % 60);
        out += line;
    }
    if (!message_id.empty()) {
        out += "Message-ID: <" + message_id + ">\r\n";
    }
    if (msg.priority == MAIL_PRIORITY_HIGH) {
        out += "X-Priority: 1\r\nImportance: high\r\n";
    } else if (msg.priority == MAIL_PRIORITY_LOW) {
        out += "X-Priority: 5\r\nImportance: low\r\n";
    }
    if (msg.confirm_read) {
        out += "Disposition-Notification-To: <" + sender.address + ">\r\n";
    }

    // Body: the Palm's line ends are single line feeds
    std::string body = msg.body;
    if (msg.signature && !signature.empty()) {
        body += "\n-- \n" + signature;
    }
    std::string text;
    for (size_t i = 0; i < body.length(); i++) {
        if (body[i] != '\r') {
            append_utf8(text, palm_to_unicode((uint8_t)body[i], charset));
        }
    }
    if (text.empty() || text[text.length() - 1] != '\n') {
        text += '\n';
    }

    // Plain 7-bit text is sent as it is; anything else quoted-printable
    bool plain = is_ascii(text);
    size_t line_start = 0;
    for (size_t i = 0; i < text.length() && plain; i++) {
        if (text[i] == '\n') {
            line_start = i + 1;
        } else if (i - line_start >= 76 || ((unsigned char)text[i] < 0x20 && text[i] != '\t')) {
            plain = false;
        }
    }

    out += "MIME-Version: 1.0\r\n";
    out += plain ? "Content-Type: text/plain; charset=US-ASCII\r\n" : "Content-Type: text/plain; charset=UTF-8\r\n";
    out += plain ? "Content-Transfer-Encoding: 7bit\r\n\r\n" : "Content-Transfer-Encoding: quoted-printable\r\n\r\n";
    if (plain) {
        for (size_t i = 0; i < text.length(); i++) {
            if (text[i] == '\n') {
                out += '\r';
            }
            out += text[i];
        }
    } else {
        out += quoted_printable(text);
    }
    return out;
}
