#pragma once

// A mail server that follows a script: each step names exactly what the client
// must send, and what the server then answers. Answers arrive in pieces of a
// chosen size, to exercise the clients' parsing across packet boundaries.
#include "mailnet.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

struct Step {
    std::string client; // Exactly what the client must send (empty: on connecting)
    std::string server;
};

class ScriptedServer : public MailConnection {
public:
    ScriptedServer(const std::vector<Step> &steps, size_t piece) : steps_(steps), piece_(piece) {}

    bool connect(const char *host, uint16_t port) override {
        host_ = host;
        port_ = port;
        advance();
        return true;
    }

    int read(uint8_t *dest, size_t max_len, uint32_t) override {
        size_t n = pending_.length() < piece_ ? pending_.length() : piece_;
        if (n > max_len) {
            n = max_len;
        }
        memcpy(dest, pending_.data(), n);
        pending_.erase(0, n);
        return (int)n;
    }

    bool write(const uint8_t *data, size_t len) override {
        sent_.append((const char *)data, len);
        advance();
        return true;
    }

    void close() override { closed_ = true; }

    // Everything was said as scripted, and nothing else
    bool finished(const char *name) {
        if (at_ == steps_.size() && sent_.empty()) {
            return true;
        }
        printf("  %s: stopped at step %zu of %zu; unmatched client data: [%s]\n", name, at_, steps_.size(), sent_.c_str());
        return false;
    }

    std::string host_;
    uint16_t port_ = 0;
    bool closed_ = false;

private:
    void advance() {
        while (at_ < steps_.size() && sent_.compare(0, steps_[at_].client.length(), steps_[at_].client) == 0 &&
               sent_.length() >= steps_[at_].client.length()) {
            sent_.erase(0, steps_[at_].client.length());
            pending_ += steps_[at_].server;
            at_++;
        }
    }

    std::vector<Step> steps_;
    size_t piece_;
    size_t at_ = 0;
    std::string sent_;
    std::string pending_;
};
