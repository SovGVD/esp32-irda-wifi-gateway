#include "mailnet.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STREAM_SLICE_MS     250   // Between calls of the idle function
#define ERROR_TEXT_MAX      70    // Server text quoted in an error (it ends up in the HotSync log)
#define IMAP_HEADERS_MAX    4096
#define IMAP_STRING_MAX     1024
#define IMAP_NESTING_MAX    8     // Multipart inside multipart ...

static std::string shortened(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.length() && out.length() < ERROR_TEXT_MAX; i++) {
        if ((unsigned char)text[i] >= 0x20 && (unsigned char)text[i] < 0x7F) {
            out += text[i];
        }
    }
    size_t first = out.find_first_not_of(' ');
    return (first == std::string::npos) ? std::string() : out.substr(first);
}

static std::string number(uint32_t value) {
    char text[12];
    snprintf(text, sizeof(text), "%lu", (unsigned long)value);
    return text;
}

// ------------------------------------------------------------------------------
// Stream
// ------------------------------------------------------------------------------

bool MailStream::connect(const std::string &host, uint16_t port) {
    len_ = 0;
    pos_ = 0;
    if (idle_ != nullptr) {
        idle_();
    }
    return conn_.connect(host.c_str(), port);
}

void MailStream::close() {
    conn_.close();
}

int MailStream::peek() {
    for (uint32_t waited = 0; pos_ >= len_; waited += STREAM_SLICE_MS) {
        if (waited >= MAILNET_TIMEOUT_MS) {
            return -1;
        }
        if (idle_ != nullptr) {
            idle_();
        }
        int n = conn_.read(buf_, sizeof(buf_), STREAM_SLICE_MS);
        if (n < 0) {
            return -1;
        }
        len_ = n;
        pos_ = 0;
    }
    return buf_[pos_];
}

int MailStream::get() {
    int c = peek();
    if (c >= 0) {
        pos_++;
    }
    return c;
}

bool MailStream::send(const std::string &text) {
    // In pieces, so that a long message does not keep the Palm waiting
    for (size_t at = 0; at < text.length(); at += 1024) {
        size_t n = text.length() - at < 1024 ? text.length() - at : 1024;
        if (idle_ != nullptr) {
            idle_();
        }
        if (!conn_.write((const uint8_t *)text.data() + at, n)) {
            return false;
        }
    }
    return true;
}

bool MailStream::line(std::string &text, size_t max_len) {
    text.clear();
    for (;;) {
        int c = get();
        if (c < 0) {
            return false;
        }
        if (c == '\n') {
            return true;
        }
        if (c != '\r' && text.length() < max_len) {
            text += (char)c;
        }
    }
}

// ------------------------------------------------------------------------------
// SMTP
// ------------------------------------------------------------------------------

// Code of the server's reply ("250-first", "250-more", "250 last"), or -1
int SmtpClient::reply() {
    for (;;) {
        if (!stream_.line(last_line_) || last_line_.length() < 3 || !isdigit((unsigned char)last_line_[0])) {
            return -1;
        }
        if (last_line_.length() == 3 || last_line_[3] != '-') {
            return atoi(last_line_.substr(0, 3).c_str());
        }
    }
}

// Sends a command (if any) and checks the code of the reply
bool SmtpClient::command(const std::string &text, int expected, const char *what) {
    if (!text.empty() && !stream_.send(text + "\r\n")) {
        error_ = "SMTP: connection lost";
        return false;
    }
    int code = reply();
    if (code == expected) {
        return true;
    }
    error_ = (code < 0) ? std::string("SMTP ") + what + ": no answer" : std::string("SMTP ") + what + ": " + shortened(last_line_);
    return false;
}

bool SmtpClient::open(const std::string &host, uint16_t port, const std::string &user, const std::string &password) {
    if (!stream_.connect(host, port)) {
        error_ = "cannot connect to " + shortened(host);
        return false;
    }
    if (!command("", 220, "greeting") || !command("EHLO palm.local", 250, "hello")) {
        return false;
    }
    std::string login;
    login += '\0';
    login += user;
    login += '\0';
    login += password;
    return command("AUTH PLAIN " + mail_base64(login), 235, "login");
}

bool SmtpClient::send(const std::string &from, const std::vector<std::string> &recipients, const std::string &message) {
    bool ok = command("MAIL FROM:<" + from + ">", 250, "sender");
    for (size_t i = 0; i < recipients.size() && ok; i++) {
        ok = command("RCPT TO:<" + recipients[i] + ">", 250, "recipient");
    }
    ok = ok && command("DATA", 354, "data");
    if (!ok) {
        // Leave the session usable for the next message
        std::string failure = error_;
        if (stream_.send("RSET\r\n")) {
            reply();
        }
        error_ = failure;
        return false;
    }

    // A line starting with "." gets a second one; a lone "." ends the message
    std::string data;
    data.reserve(message.length() + 16);
    bool line_start = true;
    for (size_t i = 0; i < message.length(); i++) {
        if (line_start && message[i] == '.') {
            data += '.';
        }
        data += message[i];
        line_start = message[i] == '\n';
    }
    if (!line_start) {
        data += "\r\n";
    }
    data += ".";
    return command(data, 250, "message");
}

void SmtpClient::close() {
    if (stream_.send("QUIT\r\n")) {
        reply();
    }
    stream_.close();
}

// ------------------------------------------------------------------------------
// IMAP: Reading Responses
// ------------------------------------------------------------------------------

// Next element of a response line. Brackets belong to the atom they are in,
// with all they contain: BODY[HEADER.FIELDS (FROM TO)], [UIDVALIDITY 7].
bool ImapClient::next(Token &token) {
    token.text.clear();
    token.length = 0;
    token.type = BROKEN;

    int c;
    do {
        c = stream_.get();
    } while (c == ' ');
    if (c == '\r') {
        c = stream_.get();
    }
    if (c < 0) {
        return false;
    }

    if (c == '\n') {
        token.type = END_OF_LINE;
    } else if (c == '(') {
        token.type = OPEN;
    } else if (c == ')') {
        token.type = CLOSE;
    } else if (c == '"') {
        for (;;) {
            c = stream_.get();
            if (c == '\\') {
                c = stream_.get();
            } else if (c == '"') {
                break;
            }
            if (c < 0) {
                return false;
            }
            if (token.text.length() < IMAP_STRING_MAX) {
                token.text += (char)c;
            }
        }
        token.type = STRING;
    } else if (c == '{') {
        // {123} and a line end, then 123 bytes of data
        while ((c = stream_.get()) >= 0 && c != '\n') {
            if (isdigit(c)) {
                token.length = token.length * 10 + (uint32_t)(c - '0');
            }
        }
        if (c < 0) {
            return false;
        }
        token.type = LITERAL;
    } else {
        int depth = 0;
        for (;;) {
            if (c == '[') {
                depth++;
            } else if (c == ']' && depth > 0) {
                depth--;
            }
            if (token.text.length() < IMAP_STRING_MAX) {
                token.text += (char)c;
            }
            c = stream_.peek();
            if (c < 0 || c == '\r' || c == '\n' || (depth == 0 && (c == ' ' || c == '(' || c == ')'))) {
                break;
            }
            stream_.get();
        }
        token.type = ATOM;
    }
    return true;
}

bool ImapClient::skip_literal(uint32_t length) {
    while (length-- > 0) {
        if (stream_.get() < 0) {
            return false;
        }
    }
    return true;
}

// A string in any of its forms (quoted, literal, NIL), at most max_len bytes of it
bool ImapClient::value(std::string &text, size_t max_len) {
    Token token;
    if (!next(token)) {
        return false;
    }
    text.clear();
    if (token.type == STRING || (token.type == ATOM && strcasecmp(token.text.c_str(), "NIL") != 0)) {
        text = token.text.substr(0, max_len);
        return true;
    }
    if (token.type == LITERAL) {
        for (uint32_t i = 0; i < token.length; i++) {
            int c = stream_.get();
            if (c < 0) {
                return false;
            }
            if (text.length() < max_len) {
                text += (char)c;
            }
        }
        return true;
    }
    return token.type == ATOM;
}

// Passes over what belongs to a token that was just read
bool ImapClient::skip_value(const Token &token) {
    if (token.type == OPEN) {
        return skip_to_close();
    }
    if (token.type == LITERAL) {
        return skip_literal(token.length);
    }
    return token.type != BROKEN;
}

// To the parenthesis that closes the list we are in
bool ImapClient::skip_to_close() {
    int depth = 1;
    Token token;
    while (depth > 0) {
        if (!next(token) || token.type == END_OF_LINE) {
            return false;
        }
        if (token.type == OPEN) {
            depth++;
        } else if (token.type == CLOSE) {
            depth--;
        } else if (token.type == LITERAL && !skip_literal(token.length)) {
            return false;
        }
    }
    return true;
}

bool ImapClient::skip_line() {
    Token token;
    for (;;) {
        if (!next(token)) {
            return false;
        }
        if (token.type == END_OF_LINE) {
            return true;
        }
        if (token.type == LITERAL && !skip_literal(token.length)) {
            return false;
        }
    }
}

// Sends a command and reads up to its tagged completion. Each untagged
// response ("* ...") in between is given to the handler, which must read it
// to the end of its line.
bool ImapClient::command(const std::string &text, Handler handler, const char *what) {
    std::string tag = "A" + number(++tag_);
    if (!stream_.send(tag + " " + text + "\r\n")) {
        error_ = "IMAP: connection lost";
        return false;
    }
    for (;;) {
        Token token;
        if (!next(token)) {
            error_ = std::string("IMAP ") + what + ": no answer";
            return false;
        }
        if (token.type == END_OF_LINE) {
            continue;
        }
        if (token.type == ATOM && token.text == tag) {
            Token status;
            std::string rest;
            bool complete = next(status) && (status.type == END_OF_LINE || stream_.line(rest));
            if (complete && status.type == ATOM && strcasecmp(status.text.c_str(), "OK") == 0) {
                return true;
            }
            error_ = std::string("IMAP ") + what + ": " + (complete ? shortened(rest) : std::string("no answer"));
            return false;
        }
        bool untagged = token.type == ATOM && token.text == "*";
        bool understood = (untagged && handler != nullptr) ? (this->*handler)() : skip_value(token) && skip_line();
        if (!understood) {
            error_ = std::string("IMAP ") + what + ": answer not understood";
            return false;
        }
    }
}

// ------------------------------------------------------------------------------
// IMAP: Commands
// ------------------------------------------------------------------------------

static std::string quoted(const std::string &text) {
    std::string out = "\"";
    for (size_t i = 0; i < text.length(); i++) {
        if (text[i] == '"' || text[i] == '\\') {
            out += '\\';
        }
        out += text[i];
    }
    return out + "\"";
}

bool ImapClient::open(const std::string &host, uint16_t port, const std::string &user, const std::string &password) {
    if (!stream_.connect(host, port)) {
        error_ = "cannot connect to " + shortened(host);
        return false;
    }
    std::string greeting;
    if (!stream_.line(greeting) || greeting.compare(0, 4, "* OK") != 0) {
        error_ = "IMAP greeting: " + (greeting.empty() ? std::string("no answer") : shortened(greeting));
        return false;
    }
    return command("LOGIN " + quoted(user) + " " + quoted(password), nullptr, "login");
}

// "* 23 EXISTS", "* OK [UIDVALIDITY 3857529045] UIDs valid"
bool ImapClient::on_examine() {
    Token first, second;
    if (!next(first) || first.type == END_OF_LINE) {
        return first.type == END_OF_LINE;
    }
    if (!skip_value(first) || !next(second) || second.type == END_OF_LINE) {
        return second.type == END_OF_LINE;
    }
    if (first.type == ATOM && second.type == ATOM) {
        if (strcasecmp(second.text.c_str(), "EXISTS") == 0) {
            exists_ = (uint32_t)strtoul(first.text.c_str(), nullptr, 10);
        } else if (strncasecmp(second.text.c_str(), "[UIDVALIDITY ", 13) == 0) {
            uid_validity_ = (uint32_t)strtoul(second.text.c_str() + 13, nullptr, 10);
        }
    }
    return skip_value(second) && skip_line();
}

bool ImapClient::examine_inbox(uint32_t *uid_validity, uint32_t *exists) {
    uid_validity_ = 0;
    exists_ = 0;
    if (!command("EXAMINE INBOX", &ImapClient::on_examine, "inbox")) {
        return false;
    }
    *uid_validity = uid_validity_;
    *exists = exists_;
    return true;
}

// "* 12 FETCH (UID 100 FLAGS (\Seen))"
bool ImapClient::on_list() {
    Token token;
    if (!next(token) || token.type == END_OF_LINE) {
        return token.type == END_OF_LINE;
    }
    Token kind;
    if (!skip_value(token) || !next(kind) || kind.type == END_OF_LINE) {
        return kind.type == END_OF_LINE;
    }
    if (kind.type != ATOM || strcasecmp(kind.text.c_str(), "FETCH") != 0 || !next(token) || token.type != OPEN) {
        return skip_value(kind) && (token.type == END_OF_LINE || (skip_value(token) && skip_line()));
    }

    ImapMessage message = {0, false};
    bool deleted = false;
    for (;;) {
        Token name;
        if (!next(name) || name.type == END_OF_LINE) {
            return false;
        }
        if (name.type == CLOSE) {
            break;
        }
        if (!next(token)) {
            return false;
        }
        if (name.type == ATOM && strcasecmp(name.text.c_str(), "UID") == 0 && token.type == ATOM) {
            message.uid = (uint32_t)strtoul(token.text.c_str(), nullptr, 10);
        } else if (name.type == ATOM && strcasecmp(name.text.c_str(), "FLAGS") == 0 && token.type == OPEN) {
            while (next(token) && token.type == ATOM) {
                message.seen = message.seen || strcasecmp(token.text.c_str(), "\\Seen") == 0;
                deleted = deleted || strcasecmp(token.text.c_str(), "\\Deleted") == 0;
            }
            if (token.type != CLOSE) {
                return false;
            }
        } else if (!skip_value(token)) {
            return false;
        }
    }
    if (message.uid != 0 && !deleted) {
        list_->push_back(message);
    }
    return skip_line();
}

bool ImapClient::list_newest(uint32_t exists, uint32_t count, std::vector<ImapMessage> &out) {
    out.clear();
    if (exists == 0 || count == 0) {
        return true;
    }
    list_ = &out;
    uint32_t first = (exists > count) ? exists - count + 1 : 1;
    return command("FETCH " + number(first) + ":" + number(exists) + " (UID FLAGS)", &ImapClient::on_list, "list");
}

// One body of a BODYSTRUCTURE, whose opening parenthesis was just read. Notes
// the first text/plain part in part_, or failing that the first text/html one.
bool ImapClient::parse_body(const std::string &path, int depth) {
    Token token;
    if (depth > IMAP_NESTING_MAX || !next(token)) {
        return false;
    }

    if (token.type == OPEN) {
        // Multipart: its parts one after another, then the subtype and extras
        for (unsigned index = 1; token.type == OPEN; index++) {
            if (!parse_body(path.empty() ? number(index) : path + "." + number(index), depth + 1) || !next(token)) {
                return false;
            }
        }
        return token.type == CLOSE || (skip_value(token) && skip_to_close());
    }

    // Single part: type, subtype, (parameters), id, description, encoding, size ...
    std::string type = token.text;
    std::string subtype, charset, encoding, ignored, size;
    if (token.type == CLOSE || !skip_value(token) || !value(subtype, 40) || !next(token)) {
        return false;
    }
    if (token.type == OPEN) {
        for (;;) {
            std::string parameter, setting;
            if (!next(token)) {
                return false;
            }
            if (token.type == CLOSE) {
                break;
            }
            parameter = token.text;
            if (!skip_value(token) || !value(setting, 40)) {
                return false;
            }
            if (strcasecmp(parameter.c_str(), "charset") == 0) {
                charset = setting;
            }
        }
    }
    if (!value(ignored, 0) || !value(ignored, 0) || !value(encoding, 40) || !value(size, 12) || !skip_to_close()) {
        return false;
    }

    bool plain = strcasecmp(subtype.c_str(), "plain") == 0;
    bool html = strcasecmp(subtype.c_str(), "html") == 0;
    if (strcasecmp(type.c_str(), "text") == 0 && (plain || html) && (part_->section.empty() || (part_->html && plain))) {
        part_->section = path.empty() ? "1" : path;
        part_->charset = charset;
        part_->encoding = encoding;
        part_->html = html;
        part_->size = (uint32_t)strtoul(size.c_str(), nullptr, 10);
    }
    return true;
}

// "* 12 FETCH (UID 100 INTERNALDATE "..." BODYSTRUCTURE (...) BODY[HEADER.FIELDS (...)] {342}"
// and likewise "* 12 FETCH (UID 100 BODY[1]<0> {2000}"
bool ImapClient::on_summary() {
    Token token;
    if (!next(token) || token.type == END_OF_LINE) {
        return token.type == END_OF_LINE;
    }
    Token kind;
    if (!skip_value(token) || !next(kind) || kind.type == END_OF_LINE) {
        return kind.type == END_OF_LINE;
    }
    if (kind.type != ATOM || strcasecmp(kind.text.c_str(), "FETCH") != 0 || !next(token) || token.type != OPEN) {
        return skip_value(kind) && (token.type == END_OF_LINE || (skip_value(token) && skip_line()));
    }

    for (;;) {
        Token name;
        if (!next(name) || name.type == END_OF_LINE) {
            return false;
        }
        if (name.type == CLOSE) {
            return skip_line();
        }
        const char *item = name.text.c_str();
        if (name.type != ATOM) {
            if (!skip_value(name)) {
                return false;
            }
        } else if (internal_date_ != nullptr && strcasecmp(item, "INTERNALDATE") == 0) {
            if (!value(*internal_date_, 40)) {
                return false;
            }
        } else if (part_ != nullptr && strcasecmp(item, "BODYSTRUCTURE") == 0) {
            if (!next(token) || token.type != OPEN || !parse_body("", 0)) {
                return false;
            }
        } else if (headers_ != nullptr && strncasecmp(item, "BODY[HEADER", 11) == 0) {
            if (!value(*headers_, IMAP_HEADERS_MAX)) {
                return false;
            }
        } else if (decoder_ != nullptr && strncasecmp(item, "BODY[", 5) == 0) {
            if (!next(token)) {
                return false;
            }
            if (token.type == LITERAL) {
                // All of it has to be read, even once the decoder has enough
                uint8_t chunk[256];
                for (uint32_t left = token.length; left > 0;) {
                    size_t n = 0;
                    while (n < sizeof(chunk) && n < left) {
                        int c = stream_.get();
                        if (c < 0) {
                            return false;
                        }
                        chunk[n++] = (uint8_t)c;
                    }
                    decoder_->feed(chunk, n);
                    left -= (uint32_t)n;
                }
            } else if (token.type == STRING) {
                decoder_->feed((const uint8_t *)token.text.data(), token.text.length());
            } else if (!skip_value(token)) {
                return false;
            }
        } else if (!next(token) || !skip_value(token)) {
            return false;
        }
    }
}

bool ImapClient::fetch_summary(uint32_t uid, std::string &headers, std::string &internal_date, ImapPart &part) {
    headers.clear();
    internal_date.clear();
    part = ImapPart();
    headers_ = &headers;
    internal_date_ = &internal_date;
    part_ = &part;
    decoder_ = nullptr;
    return command("UID FETCH " + number(uid) +
                   " (INTERNALDATE BODYSTRUCTURE BODY.PEEK[HEADER.FIELDS (FROM TO CC REPLY-TO SUBJECT DATE X-PRIORITY IMPORTANCE)])",
                   &ImapClient::on_summary, "fetch");
}

bool ImapClient::fetch_text(uint32_t uid, const ImapPart &part, uint32_t max_bytes, MailTextDecoder &decoder) {
    headers_ = nullptr;
    internal_date_ = nullptr;
    part_ = nullptr;
    decoder_ = &decoder;
    return command("UID FETCH " + number(uid) + " (BODY.PEEK[" + part.section + "]<0." + number(max_bytes) + ">)",
                   &ImapClient::on_summary, "fetch text");
}

void ImapClient::close() {
    stream_.send("A" + number(++tag_) + " LOGOUT\r\n");
    stream_.close();
}
