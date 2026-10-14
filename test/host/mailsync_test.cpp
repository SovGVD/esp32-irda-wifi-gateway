// A whole mail sync on a computer: the conduit talks HotSync to fake_palm.py
// over a pseudo terminal, and IMAP / SMTP to scripted servers. run.sh compares
// what the simulated Palm holds afterwards with the expected contents.
//
//   mailsync_test <terminal> normal|bad-imap|offline|oauth <outbox record files...>
#include "hotsync.h"
#include "mailoauth.h"
#include "mailsync.h"
#include "scripted_server.h"
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int s_fd = -1;

static int port_read(uint8_t *dest, size_t max_len, uint32_t timeout_ms) {
    struct pollfd wait = {s_fd, POLLIN, 0};
    if (poll(&wait, 1, (int)timeout_ms) <= 0) {
        return 0;
    }
    ssize_t n = read(s_fd, dest, max_len);
    return (n > 0) ? (int)n : -1;
}

static bool port_write(const uint8_t *data, size_t len) {
    return write(s_fd, data, len) == (ssize_t)len;
}

static uint32_t port_millis() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

// One connection object, as on the ESP32; the server it reaches depends on the name
class Servers : public MailConnection {
public:
    Servers(ScriptedServer &imap, ScriptedServer &smtp, ScriptedServer &google) : imap_(imap), smtp_(smtp), google_(google) {}

    bool connect(const char *host, uint16_t port) override {
        current_ = (strncmp(host, "imap", 4) == 0) ? &imap_ : (strncmp(host, "oauth2", 6) == 0) ? &google_ : &smtp_;
        return current_->connect(host, port);
    }
    int read(uint8_t *dest, size_t max_len, uint32_t timeout_ms) override { return current_->read(dest, max_len, timeout_ms); }
    bool write(const uint8_t *data, size_t len) override { return current_->write(data, len); }
    void close() override { current_->close(); }

private:
    ScriptedServer &imap_;
    ScriptedServer &smtp_;
    ScriptedServer &google_;
    ScriptedServer *current_ = nullptr;
};

class TestHost : public MailSyncHost {
public:
    TestHost(MailConnection &connection, bool online) : connection_(connection), online_(online) {}

    MailConnection &connection() override { return connection_; }
    bool online() override { return online_; }
    int64_t now() override { return 1791736200; } // 2026-10-11 16:30:00 UTC
    uint32_t random() override { return 0x12345678; }
    void load_position(uint32_t &uid_validity, uint32_t &last_uid) override {
        uid_validity = uid_validity_;
        last_uid = last_uid_;
    }
    void store_position(uint32_t uid_validity, uint32_t last_uid) override {
        uid_validity_ = uid_validity;
        last_uid_ = last_uid;
        stored_ = true;
    }
    void trace(const std::string &text) override { printf("trace: %s\n", text.c_str()); }

    uint32_t uid_validity_ = 1700000001;
    uint32_t last_uid_ = 205;
    bool stored_ = false;

private:
    MailConnection &connection_;
    bool online_;
};

static std::string literal(const std::string &data) {
    char size[16];
    snprintf(size, sizeof(size), "{%zu}\r\n", data.length());
    return size + data;
}

// What the conduit must hand to the SMTP server for an Outbox record
static std::string expected_message(const char *file, const MailIdentity &sender, std::vector<std::string> &recipients) {
    uint8_t data[4096];
    FILE *in = fopen(file, "rb");
    size_t len = in ? fread(data, 1, sizeof(data), in) : 0;
    if (in) {
        fclose(in);
    }
    MailMessage message;
    mail_unpack_record(data, len, message);
    mail_extract_addresses(message.to, recipients);
    // The simulated Palm's clock shows 18:30, two hours ahead of the network's
    return mail_compose(message, sender, PALM_WESTERN, "Sent from my Palm IIIx", 1791736200, 120, "6acbb988.12345678@gmail.com");
}

int main(int argc, char **argv) {
    if (argc < 3 || (s_fd = open(argv[1], O_RDWR | O_NOCTTY)) < 0) {
        printf("usage: mailsync_test <terminal> <scenario> <outbox files>\n");
        return 2;
    }
    std::string scenario = argv[2];
    struct termios raw;
    tcgetattr(s_fd, &raw);
    cfmakeraw(&raw);
    tcsetattr(s_fd, TCSANOW, &raw);

    MailSyncSetup setup;
    setup.address = "me@gmail.com";
    setup.name = "Me";
    setup.login = "me@gmail.com";
    setup.password = "secret";
    setup.imap_host = "imap.gmail.com";
    setup.smtp_host = "smtp.gmail.com";
    setup.max_fetch = 4;
    // Google sign-in: one access token for both servers, fetched once
    bool oauth = scenario == "oauth";
    std::string smtp_login = "AUTH PLAIN AG1lQGdtYWlsLmNvbQBzZWNyZXQ=\r\n";
    std::string imap_login = "A1 LOGIN \"me@gmail.com\" \"secret\"\r\n";
    std::vector<Step> google_steps;
    if (oauth) {
        setup.password.clear();
        setup.oauth_client_id = "123-abc.apps.googleusercontent.com";
        setup.oauth_client_secret = "GOCSPX-secret";
        setup.oauth_refresh_token = "1//0g-refresh";
        std::string form = "grant_type=refresh_token&refresh_token=1%2F%2F0g-refresh&client_id=123-abc.apps.googleusercontent.com"
                           "&client_secret=GOCSPX-secret";
        std::string body = "{\n  \"access_token\": \"ya29.access\",\n  \"expires_in\": 3599,\n  \"token_type\": \"Bearer\"\n}";
        google_steps = {{"", ""},
                        {"POST /token HTTP/1.0\r\nHost: oauth2.googleapis.com\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                         "Accept: application/json\r\nContent-Length: " + std::to_string(form.length()) + "\r\nConnection: close\r\n\r\n" + form,
                         "HTTP/1.0 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                             std::to_string(body.length()) + "\r\n\r\n" + body}};
        smtp_login = "AUTH XOAUTH2 " + mail_xoauth2("me@gmail.com", "ya29.access") + "\r\n";
        imap_login = "A1 AUTHENTICATE XOAUTH2 " + mail_xoauth2("me@gmail.com", "ya29.access") + "\r\n";
    }

    // --- SMTP: one transaction per Outbox record ---
    MailIdentity sender;
    sender.address = setup.address;
    sender.name = setup.name;
    std::vector<Step> smtp_steps = {
        {"", "220 smtp.gmail.com ESMTP x1 - gsmtp\r\n"},
        {"EHLO palm.local\r\n", "250-smtp.gmail.com at your service\r\n250-8BITMIME\r\n250 SMTPUTF8\r\n"},
        {smtp_login, "235 2.7.0 Accepted\r\n"},
    };
    for (int i = 3; i < argc; i++) {
        std::vector<std::string> recipients;
        std::string message = expected_message(argv[i], sender, recipients);
        smtp_steps.push_back({"MAIL FROM:<me@gmail.com>\r\n", "250 2.1.0 OK\r\n"});
        for (size_t k = 0; k < recipients.size(); k++) {
            smtp_steps.push_back({"RCPT TO:<" + recipients[k] + ">\r\n", "250 2.1.5 OK\r\n"});
        }
        smtp_steps.push_back({"DATA\r\n", "354 Go ahead\r\n"});
        smtp_steps.push_back({message + ".\r\n", "250 2.0.0 OK\r\n"});
    }
    smtp_steps.push_back({"QUIT\r\n", "221 2.0.0 closing connection\r\n"});

    // --- IMAP: four messages listed, two of them new since the last sync ---
    const std::string items = " (INTERNALDATE BODYSTRUCTURE BODY.PEEK[HEADER.FIELDS (FROM TO CC REPLY-TO SUBJECT DATE X-PRIORITY IMPORTANCE)])\r\n";
    const std::string header_item = "BODY[HEADER.FIELDS (FROM TO CC REPLY-TO SUBJECT DATE X-PRIORITY IMPORTANCE)] ";
    std::vector<Step> imap_steps = {{"", "* OK Gimap ready for requests\r\n"}};
    if (scenario == "bad-imap") {
        imap_steps.push_back({imap_login, "A1 NO [AUTHENTICATIONFAILED] Invalid credentials (Failure)\r\n"});
    } else {
        std::vector<Step> rest = {
            {imap_login, "* CAPABILITY IMAP4rev1 UNSELECT IDLE\r\nA1 OK me@gmail.com authenticated (Success)\r\n"},
            {"A2 EXAMINE INBOX\r\n",
             "* FLAGS (\\Answered \\Flagged \\Draft \\Deleted \\Seen)\r\n* OK [PERMANENTFLAGS ()] Flags permitted.\r\n"
             "* OK [UIDVALIDITY 1700000001] UIDs valid.\r\n* 7 EXISTS\r\n* 0 RECENT\r\n* OK [UIDNEXT 213] Predicted next UID.\r\n"
             "A2 OK [READ-ONLY] INBOX selected. (Success)\r\n"},
            {"A3 FETCH 4:7 (UID FLAGS)\r\n",
             "* 4 FETCH (UID 205 FLAGS (\\Seen))\r\n* 5 FETCH (UID 207 FLAGS (\\Deleted))\r\n"
             "* 6 FETCH (UID 210 FLAGS ())\r\n* 7 FETCH (UID 212 FLAGS (\\Seen))\r\nA3 OK Success\r\n"},
            // New and unread: plain text with an HTML alternative
            {"A4 UID FETCH 210" + items,
             "* 6 FETCH (UID 210 INTERNALDATE \"11-Oct-2026 16:30:07 +0000\" BODYSTRUCTURE ((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"UTF-8\") NIL NIL \"QUOTED-PRINTABLE\" 31 2 NIL NIL NIL)"
             "(\"TEXT\" \"HTML\" (\"CHARSET\" \"UTF-8\") NIL NIL \"QUOTED-PRINTABLE\" 120 4 NIL NIL NIL) \"ALTERNATIVE\" (\"BOUNDARY\" \"b1\") NIL NIL) " +
             header_item + literal("From: =?UTF-8?Q?Ann_M=C3=BCller?= <ann@example.com>\r\nTo: me@gmail.com\r\nCc: Bob <bob@example.org>\r\n"
                                   "Subject: Lunch (tomorrow) \"at\" {12}\r\nDate: Sun, 11 Oct 2026 12:30:05 -0400\r\n\r\n") +
             ")\r\nA4 OK Success\r\n"},
            {"A5 UID FETCH 210 (BODY.PEEK[1]<0.31>)\r\n",
             "* 6 FETCH (UID 210 BODY[1]<0> " + literal("Caf=C3=A9 at noon?\r\n) not the end\r\n") + ")\r\nA5 OK Success\r\n"},
            // New but read elsewhere, urgent, HTML only, no Date header
            {"A6 UID FETCH 212" + items,
             "* 7 FETCH (UID 212 INTERNALDATE \"10-Oct-2026 23:05:00 +0000\" BODYSTRUCTURE (\"TEXT\" \"HTML\" (\"CHARSET\" \"UTF-8\") NIL NIL \"BASE64\" 110 2 NIL NIL NIL) " +
             header_item + literal("From: shop@example.com\r\nSubject: =?UTF-8?B?U2FsZSDigJMgNTAl?=\r\nX-Priority: 1\r\nReply-To: help@example.com\r\n\r\n") +
             ")\r\nA6 OK Success\r\n"},
            {"A7 UID FETCH 212 (BODY.PEEK[1]<0.110>)\r\n",
             "* 7 FETCH (UID 212 BODY[1]<0> " +
             literal("PGh0bWw+PGJvZHk+PHA+SGVsbG8gPGI+UGFsbTwvYj48L3A+PHA+U2Vjb25kJm5ic3A7cGFy\r\nYWdyYXBoPC9wPjwvYm9keT48L2h0bWw+\r\n") +
             ")\r\nA7 OK Success\r\n"},
            {"A8 LOGOUT\r\n", "* BYE\r\nA8 OK\r\n"},
        };
        imap_steps.insert(imap_steps.end(), rest.begin(), rest.end());
    }

    ScriptedServer imap(imap_steps, 11);
    ScriptedServer smtp(smtp_steps, 11);
    ScriptedServer google(google_steps, 11);
    Servers servers(imap, smtp, google);
    TestHost host(servers, scenario != "offline");

    static const HotSync_Io io = {port_read, port_write, port_millis};
    static uint8_t memory[HOTSYNC_BUFFER_SIZE(MAILSYNC_RECORD_MAX)];
    if (!hotsync_accept(&io, memory, sizeof(memory), 20000)) {
        printf("FAILED: no HotSync\n");
        return 1;
    }
    MailSyncResult result;
    mailsync_run(setup, host, result);
    std::string summary = mailsync_summary(result);
    printf("summary: %s\n", summary.c_str());
    if (result.link_lost || !hotsync_add_log(("Mail: " + summary + "\n").c_str()) || !hotsync_end(0)) {
        printf("FAILED: link lost\n");
        return 1;
    }

    bool ok = true;
    if (scenario == "offline") {
        ok = !host.stored_;
    } else {
        ok = smtp.finished("smtp") && imap.finished("imap") && google.finished("google");
        ok = ok && (scenario == "bad-imap" ? !host.stored_ : host.stored_ && host.uid_validity_ == 1700000001 && host.last_uid_ == 212);
    }
    printf(ok ? "done\n" : "FAILED: servers or position not as expected\n");
    return ok ? 0 : 1;
}
