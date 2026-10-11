#pragma once

#include <stdint.h>
#include <string>
#include "mailnet.h"
#include "mailtext.h"

// The mail conduit itself: what happens between the HotSync handshake and the
// end of the sync. Sends the Palm's Outbox through SMTP, copies the messages
// that arrived since the last sync from the IMAP inbox to the Palm's Inbox,
// and tidies the database as a desktop sync would. Uses the HotSync session
// set up by hotsync_accept(). Nothing here depends on the ESP32.

#define MAILSYNC_FIELD_MAX   255   // Longest From / To / CC / Subject passed to the Palm
#define MAILSYNC_BODY_MAX    8000  // Longest body the Mail application takes
// Largest record the conduit writes: the HotSync working memory must hold it
#define MAILSYNC_RECORD_MAX  (MAILSYNC_BODY_MAX + 5 * MAILSYNC_FIELD_MAX + 32)

struct MailSyncSetup {
    std::string address;     // Sender address
    std::string name;        // Sender name; Palm character set; may be empty
    std::string login;
    std::string password;
    std::string imap_host;
    uint16_t imap_port = 993;
    std::string smtp_host;
    uint16_t smtp_port = 465;
    PalmCharset charset = PALM_WESTERN;
    unsigned max_fetch = 15;          // Newest inbox messages considered per sync
    unsigned max_send = 10;           // Outbox messages sent per sync
    uint32_t html_fetch_max = 96000;  // Bytes read of a message that only exists as HTML
};

// What the conduit needs from its surroundings
class MailSyncHost {
public:
    virtual ~MailSyncHost() {}
    // Used for one server at a time
    virtual MailConnection &connection() = 0;
    virtual bool online() = 0;
    // Seconds since 1970 (UTC); 0 while the clock is not set
    virtual int64_t now() = 0;
    virtual uint32_t random() = 0;
    // Where the last sync stopped: inbox messages up to this UID have been dealt with
    virtual void load_position(uint32_t &uid_validity, uint32_t &last_uid) = 0;
    virtual void store_position(uint32_t uid_validity, uint32_t last_uid) = 0;
    virtual void trace(const std::string &text) = 0;
};

struct MailSyncResult {
    unsigned sent = 0;
    unsigned not_sent = 0;
    unsigned received = 0;
    std::string problem;     // The first thing that went wrong
    bool link_lost = false;  // The Palm is gone: nothing more can be said to it
};

void mailsync_run(const MailSyncSetup &setup, MailSyncHost &host, MailSyncResult &result);
// "2 sent, 5 received", with what went wrong if anything did
std::string mailsync_summary(const MailSyncResult &result);
