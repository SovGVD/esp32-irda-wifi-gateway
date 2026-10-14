#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

// Messages as the Palm's Mail application stores them, and the conversions
// between that and Internet mail: character sets, MIME transfer encodings,
// headers, dates. Nothing here depends on the ESP32, so it builds for host tests.

// Text on the Palm has one byte per character
enum PalmCharset : uint8_t {
    PALM_WESTERN = 0,  // Windows-1252 (what an unmodified Palm shows)
    PALM_CYRILLIC = 1  // Windows-1251 (with a Cyrillic localisation installed)
};

// Categories of MailDB
#define MAIL_CATEGORY_INBOX   0
#define MAIL_CATEGORY_OUTBOX  1
#define MAIL_CATEGORY_DELETED 2
#define MAIL_CATEGORY_FILED   3
#define MAIL_CATEGORY_DRAFT   4

#define MAIL_PRIORITY_HIGH    0
#define MAIL_PRIORITY_NORMAL  1
#define MAIL_PRIORITY_LOW     2

// One record of MailDB. Text is in the Palm's character set.
struct MailMessage {
    bool dated = false;
    uint16_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, minute = 0;
    bool read = false;
    bool signature = false;        // Append the signature when sending
    bool confirm_read = false;
    bool confirm_delivery = false;
    uint8_t priority = MAIL_PRIORITY_NORMAL;
    uint8_t addressing = 0;
    std::string subject, from, to, cc, bcc, reply_to, sent_to, body;
};

bool mail_unpack_record(const uint8_t *data, size_t len, MailMessage &msg);
// Returns the record length, or 0 if it does not fit
size_t mail_pack_record(const MailMessage &msg, uint8_t *dest, size_t max_len);

// "HotSync Options" of the Mail application (a saved preference)
#define MAIL_SYNC_ALL         0
#define MAIL_SYNC_SEND_ONLY   1
#define MAIL_SYNC_FILTER      2
#define MAIL_SYNC_UNREAD      3
#define MAIL_DEFAULT_TRUNCATE 4000

struct MailSyncOptions {
    uint8_t type = MAIL_SYNC_ALL;
    bool get_high_priority = false;  // Filter: always take high priority messages
    bool retrieve_matching = false;  // Filter: take only matching messages (else: leave them out)
    uint16_t truncate = MAIL_DEFAULT_TRUNCATE;
    std::string to, from, subject;   // Filter: text to look for
};

void mail_unpack_sync_options(const uint8_t *data, size_t len, MailSyncOptions &options);
// Whether the filter lets a message with these (already decoded) headers through
bool mail_filter_accepts(const MailSyncOptions &options, const std::string &from, const std::string &to,
                         const std::string &subject, bool high_priority);

// ------------------------------------------------------------------------------
// Incoming mail
// ------------------------------------------------------------------------------

// UTF-8 decoding one byte at a time. Bytes that do not form UTF-8 are taken as
// Windows-1252, which is what mislabelled mail usually contains.
class MailUtf8Reader {
public:
    // Writes the characters completed by `b` to out (room for 4); returns how many
    int push(uint8_t b, uint32_t *out);
    // At the end of the text: what an unfinished sequence left behind
    int flush(uint32_t *out);

private:
    uint32_t code_ = 0;
    uint8_t pending_ = 0;
    uint8_t held_[3] = {};
    uint8_t held_count_ = 0;
};

// Turns the content of one MIME part into text for the Palm, piece by piece:
// transfer encoding, character set, HTML markup, line ends, length limit.
class MailTextDecoder {
public:
    // encoding: "base64", "quoted-printable" or anything else for none
    MailTextDecoder(const std::string &charset, const std::string &encoding, bool html, PalmCharset target, size_t max_chars);

    void feed(const uint8_t *data, size_t len);
    void finish();
    bool full() const { return out_.length() >= max_chars_; }
    const std::string &text() const { return out_; }

private:
    enum Encoding { PLAIN, BASE64, QUOTED };
    enum Source { UTF8, LATIN, CP1251, KOI8, ISO5 };
    enum Markup { TEXT, TAG, TAG_REST, ENTITY, SKIP_ELEMENT, COMMENT };

    void decoded_byte(uint8_t b);
    void character(uint32_t code);
    void markup(uint32_t code);
    void end_tag_name();
    void end_entity(bool terminated);
    void visible(uint32_t code);
    void line_break(uint8_t count);
    void emit(char c);

    Encoding encoding_;
    Source source_;
    bool html_;
    PalmCharset target_;
    size_t max_chars_;
    std::string out_;

    // Transfer decoding
    uint32_t bits_ = 0;
    uint8_t bit_count_ = 0;
    int8_t quoted_left_ = 0;   // Characters still expected after '='
    uint8_t quoted_first_ = 0;

    MailUtf8Reader utf8_;

    // HTML
    Markup state_ = TEXT;
    std::string held_;         // Tag name or entity being collected
    std::string closing_;      // Element being skipped: its end tag
    size_t matched_ = 0;
    char quote_ = 0;

    // Line ends and spaces
    uint8_t newlines_ = 2;     // Line ends written in a row (the start counts as a blank line)
    uint8_t spaces_ = 0;       // Spaces seen, written once a visible character follows
};

// Unfolded value of a header field ("Subject") from a block of header lines; empty if absent
std::string mail_header_field(const std::string &headers, const char *name);
// Header text for the Palm: encoded words (=?utf-8?B?...?=) and raw UTF-8 are converted
std::string mail_decode_header(const std::string &raw, PalmCharset target);

// "Sun, 11 Oct 2026 18:30:00 +0200" or IMAP's "11-Oct-2026 18:30:00 +0200", as seconds since 1970 (UTC)
bool mail_parse_date(const std::string &text, int64_t *utc_seconds);
// Fills the date of a record with the local time `offset_minutes` east of UTC
void mail_set_date(MailMessage &msg, int64_t utc_seconds, int offset_minutes);
// Seconds since 1970 for a calendar date and time
int64_t mail_seconds(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);

// ------------------------------------------------------------------------------
// Outgoing mail
// ------------------------------------------------------------------------------

struct MailIdentity {
    std::string address;   // user@example.com
    std::string name;      // Shown to the recipient; Palm character set; may be empty
};

// Plain addresses (user@example.com) named in a To / CC / BCC field written on the Palm
void mail_extract_addresses(const std::string &field, std::vector<std::string> &out);

// The message as Internet mail: headers and body with CRLF line ends, UTF-8.
// utc_seconds 0 leaves the Date header to the mail server.
std::string mail_compose(const MailMessage &msg, const MailIdentity &sender, PalmCharset charset,
                         const std::string &signature, int64_t utc_seconds, int offset_minutes,
                         const std::string &message_id);

std::string mail_base64(const std::string &data);

// Plain Palm text to UTF-8, shared by the Tasks conduit.
std::string palm_to_utf8(const std::string &text, PalmCharset charset);

// Convert plain UTF-8 without mail whitespace normalization.
std::string palm_from_utf8(const std::string &text, PalmCharset charset, size_t max_chars);
