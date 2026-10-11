#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include "mailtext.h"

// IMAP and SMTP clients, as far as a Palm mail conduit needs them: read the
// newest messages of the inbox, pass on what became of messages on the Palm
// (read, deleted), and submit messages. The connection is abstract (TLS on the ESP32, a script in
// the host tests), so nothing here depends on the ESP32.

#define MAILNET_TIMEOUT_MS  20000  // Longest wait for the server to say something

class MailConnection {
public:
    virtual ~MailConnection() {}
    virtual bool connect(const char *host, uint16_t port) = 0;
    // Up to max_len bytes; 0 if none arrived within timeout_ms, -1 once closed
    virtual int read(uint8_t *dest, size_t max_len, uint32_t timeout_ms) = 0;
    virtual bool write(const uint8_t *data, size_t len) = 0;
    virtual void close() = 0;
};

// Buffered reading with a time limit. `idle` is called while waiting, so the
// caller can look after the Palm (keep-alive) in the meantime.
class MailStream {
public:
    MailStream(MailConnection &conn, void (*idle)()) : conn_(conn), idle_(idle) {}

    bool connect(const std::string &host, uint16_t port);
    void close();
    int get();   // Next byte, or -1 when the server is silent or gone
    int peek();
    bool send(const std::string &text);
    bool line(std::string &text, size_t max_len = 512); // Without the line end

private:
    MailConnection &conn_;
    void (*idle_)();
    uint8_t buf_[512];
    int len_ = 0;
    int pos_ = 0;
};

class SmtpClient {
public:
    SmtpClient(MailConnection &conn, void (*idle)()) : stream_(conn, idle) {}

    // xoauth2: `password` is a Google access token (see mailoauth.h)
    bool open(const std::string &host, uint16_t port, const std::string &user, const std::string &password,
              bool xoauth2 = false);
    // `message`: headers and body with CRLF line ends, as mail_compose() builds it
    bool send(const std::string &from, const std::vector<std::string> &recipients, const std::string &message);
    void close();
    const std::string &error() const { return error_; } // Short, for the HotSync log

private:
    int reply();
    bool command(const std::string &text, int expected, const char *what);

    MailStream stream_;
    std::string error_;
    std::string last_line_;
};

struct ImapMessage {
    uint32_t uid;
    bool seen;
};

// Where the readable text of a message is
struct ImapPart {
    std::string section;   // "1", "1.2" ...; empty if the message has no text part
    std::string charset;
    std::string encoding;
    bool html = false;
    uint32_t size = 0;
};

class ImapClient {
public:
    ImapClient(MailConnection &conn, void (*idle)()) : stream_(conn, idle) {}

    // xoauth2: `password` is a Google access token (see mailoauth.h)
    bool open(const std::string &host, uint16_t port, const std::string &user, const std::string &password,
              bool xoauth2 = false);
    // Opens the inbox read-only: nothing this client does changes the mailbox
    bool examine_inbox(uint32_t *uid_validity, uint32_t *exists);
    // Opens the inbox for changes (flags, removing messages)
    bool select_inbox(uint32_t *uid_validity, uint32_t *exists);
    // The newest `count` messages, oldest first (deleted ones left out)
    bool list_newest(uint32_t exists, uint32_t count, std::vector<ImapMessage> &out);
    bool fetch_summary(uint32_t uid, std::string &headers, std::string &internal_date, ImapPart &part);
    // The first `max_bytes` of a part, passed through the decoder
    bool fetch_text(uint32_t uid, const ImapPart &part, uint32_t max_bytes, MailTextDecoder &decoder);
    // Which of these messages are still there, and whether they are read
    bool fetch_flags(const std::vector<uint32_t> &uids, std::vector<ImapMessage> &out);
    bool mark_read(const std::vector<uint32_t> &uids);
    // The mailbox deleted messages go to (Gmail: "[Gmail]/Trash", in the
    // account's language); empty if the server names none
    bool find_trash(std::string &mailbox);
    // Into `trash`, or (if empty) deleted for good
    bool remove(const std::vector<uint32_t> &uids, const std::string &trash);
    void close();
    const std::string &error() const { return error_; }

private:
    enum TokenType { END_OF_LINE, OPEN, CLOSE, ATOM, STRING, LITERAL, BROKEN };
    struct Token {
        TokenType type = BROKEN;
        std::string text;
        uint32_t length = 0; // LITERAL: bytes that follow
    };
    typedef bool (ImapClient::*Handler)();

    bool next(Token &token);
    bool value(std::string &text, size_t max_len);
    bool skip_literal(uint32_t length);
    bool skip_value(const Token &token);
    bool skip_to_close();
    bool skip_line();
    bool command(const std::string &text, Handler handler, const char *what);
    bool parse_body(const std::string &path, int depth);
    bool on_examine();
    bool on_list();
    bool on_summary();
    bool on_text();
    bool on_folder();
    bool open_inbox(const char *command, uint32_t *uid_validity, uint32_t *exists);

    MailStream stream_;
    std::string error_;
    unsigned tag_ = 0;

    // Results of the command in progress
    uint32_t uid_validity_ = 0;
    uint32_t exists_ = 0;
    std::vector<ImapMessage> *list_ = nullptr;
    std::string *headers_ = nullptr;
    std::string *internal_date_ = nullptr;
    ImapPart *part_ = nullptr;
    MailTextDecoder *decoder_ = nullptr;
    std::string *trash_ = nullptr;
};
