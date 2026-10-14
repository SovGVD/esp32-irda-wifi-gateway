#include "irmail.h"
#include "irdebug.h"
#include "hotsync.h"
#include "mailsync.h"
#include "irtasks.h"
#include "mailoauth.h"
#include <WiFi.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <time.h>
#include "esp_tls.h"
#include "esp_crt_bundle.h"

#define MAIL_TASK_STACK       16384         // Room for a TLS handshake
#define MAIL_STREAM_SIZE      2048          // HotSync bytes queued in each direction
#define CLOCK_VALID_AFTER     1700000000    // Any later time means the clock has been set

static IrMailSettings s_settings;
// Written by the conduit task, read by the portal: plain characters, so that a
// read during a write can at worst show a mixed line
static char s_last_result[160] = "";

// HotSync bytes between the IrDA loop and the conduit task
static StreamBufferHandle_t s_from_palm = nullptr;
static StreamBufferHandle_t s_to_palm = nullptr;
static volatile bool s_running = false; // The conduit task exists
static volatile bool s_stop = false;    // The infrared link is gone: give up

// ------------------------------------------------------------------------------
// Settings
// ------------------------------------------------------------------------------

static void load_settings() {
    Preferences prefs;
    prefs.begin("irmail", true);
    s_settings.enabled = prefs.getUChar("on", 0) != 0;
    s_settings.address = prefs.getString("addr", "");
    s_settings.password = prefs.getString("pass", "");
    s_settings.name = prefs.getString("name", "");
    s_settings.login = prefs.getString("login", "");
    s_settings.imap_host = prefs.getString("imap", MAIL_DEFAULT_IMAP_SERVER);
    s_settings.smtp_host = prefs.getString("smtp", MAIL_DEFAULT_SMTP_SERVER);
    s_settings.cyrillic = prefs.getUChar("cset", 0) != 0;
    s_settings.google_client_id = prefs.getString("gid", "");
    s_settings.google_client_secret = prefs.getString("gsecret", "");
    s_settings.google_token = prefs.getString("gtoken", "");
    // Existing Gmail authorizations predate Tasks and only grant mail.
    s_settings.google_scopes = prefs.getString("gscope", s_settings.google() ? "https://mail.google.com/" : "");
    prefs.end();
}

const IrMailSettings &irmail_settings() {
    return s_settings;
}

void irmail_forget_position() {
    Preferences prefs;
    prefs.begin("irmail", false);
    prefs.remove("uidv");
    prefs.remove("uid");
    prefs.remove("links");
    prefs.end();
}

// Not called while a sync runs: the portal and HotSync cannot share the Palm's infrared link
void irmail_store_settings(const IrMailSettings &new_settings) {
    IrMailSettings settings = new_settings;
    bool other_mailbox = settings.address != s_settings.address || settings.login != s_settings.login ||
                         settings.imap_host != s_settings.imap_host;
    if (other_mailbox && settings.google_token == s_settings.google_token) {
        settings.google_token = "";
        settings.google_scopes = ""; // The Google sign-in was for the other account
    }
    s_settings = settings;

    Preferences prefs;
    prefs.begin("irmail", false);
    prefs.putUChar("on", settings.enabled ? 1 : 0);
    prefs.putString("addr", settings.address);
    prefs.putString("pass", settings.password);
    prefs.putString("name", settings.name);
    prefs.putString("login", settings.login);
    prefs.putString("imap", settings.imap_host);
    prefs.putString("smtp", settings.smtp_host);
    prefs.putUChar("cset", settings.cyrillic ? 1 : 0);
    prefs.putString("gid", settings.google_client_id);
    prefs.putString("gsecret", settings.google_client_secret);
    prefs.putString("gtoken", settings.google_token);
    prefs.putString("gscope", settings.google_scopes);
    prefs.end();
    if (other_mailbox) {
        irmail_forget_position();
    }
}

bool irmail_enabled() {
    return s_settings.enabled && s_settings.address.length() > 0 && (s_settings.password.length() > 0 || s_settings.google());
}

bool irmail_ready() {
    return s_from_palm != nullptr && (irmail_enabled() || irtasks_ready());
}

String irmail_last_result() {
    return s_last_result;
}

// "host" or "host:port"
static void split_server(const String &server, uint16_t default_port, std::string &host, uint16_t &port) {
    int colon = server.lastIndexOf(':');
    port = default_port;
    if (colon > 0 && server.substring(colon + 1).toInt() > 0) {
        port = (uint16_t)server.substring(colon + 1).toInt();
        host = server.substring(0, colon).c_str();
    } else {
        host = server.c_str();
    }
}

static std::string login_name() {
    return (s_settings.login.length() > 0 ? s_settings.login : s_settings.address).c_str();
}

// ------------------------------------------------------------------------------
// Connection to a Mail Server (TLS, certificate checked)
// ------------------------------------------------------------------------------

class TlsConnection : public MailConnection {
public:
    ~TlsConnection() override { close(); }

    bool connect(const char *host, uint16_t port) override {
        close();
        esp_tls_cfg_t config = {};
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.timeout_ms = MAIL_CONNECT_TIMEOUT_MS;
        tls_ = esp_tls_init();
        if (tls_ == nullptr) {
            return false;
        }
        irdbg_printf("Mail: connecting to %s:%u", host, port);
        if (esp_tls_conn_new_sync(host, strlen(host), port, &config, tls_) != 1) {
            irdbg_printf("Mail: could not connect to %s (free memory %u)", host, (unsigned)ESP.getFreeHeap());
            close();
            return false;
        }
        return true;
    }

    int read(uint8_t *dest, size_t max_len, uint32_t timeout_ms) override {
        if (tls_ == nullptr || s_stop) {
            return -1;
        }
        // Wait on the socket, unless decrypted data is already there
        if (esp_tls_get_bytes_avail(tls_) <= 0) {
            int sock = -1;
            if (esp_tls_get_conn_sockfd(tls_, &sock) != ESP_OK || sock < 0) {
                return -1;
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(sock, &readable);
            struct timeval wait = {(time_t)(timeout_ms / 1000), (suseconds_t)((timeout_ms % 1000) * 1000)};
            if (select(sock + 1, &readable, NULL, NULL, &wait) <= 0) {
                return 0;
            }
        }
        ssize_t n = esp_tls_conn_read(tls_, dest, max_len);
        if (n > 0) {
            return (int)n;
        }
        return (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) ? 0 : -1;
    }

    bool write(const uint8_t *data, size_t len) override {
        while (len > 0 && tls_ != nullptr && !s_stop) {
            ssize_t n = esp_tls_conn_write(tls_, data, len);
            if (n > 0) {
                data += n;
                len -= n;
            } else if (n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE) {
                return false;
            }
        }
        return len == 0;
    }

    void close() override {
        if (tls_ != nullptr) {
            esp_tls_conn_destroy(tls_);
            tls_ = nullptr;
        }
    }

private:
    esp_tls_t *tls_ = nullptr;
};

void irmail_check_login(String &receiving, String &sending) {
    if (s_running) {
        receiving = sending = "A mail sync is running.";
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        receiving = sending = "Wi-Fi is not connected.";
        return;
    }
    s_stop = false;
    std::string host;
    uint16_t port;
    std::string password = s_settings.password.c_str();
    if (s_settings.google()) {
        TlsConnection connection;
        OAuthClient google(connection, nullptr);
        OAuthTokens tokens;
        if (!google.refresh(s_settings.google_client_id.c_str(), s_settings.google_client_secret.c_str(),
                            s_settings.google_token.c_str(), tokens)) {
            receiving = sending = google.error().c_str();
            return;
        }
        password = tokens.access_token;
    }
    {
        TlsConnection connection;
        ImapClient imap(connection, nullptr);
        uint32_t validity = 0, exists = 0;
        split_server(s_settings.imap_host, MAIL_IMAP_PORT, host, port);
        if (imap.open(host, port, login_name(), password, s_settings.google()) && imap.examine_inbox(&validity, &exists)) {
            receiving = "OK, " + String((unsigned long)exists) + " messages in the inbox.";
            imap.close();
        } else {
            receiving = imap.error().c_str();
        }
    }
    {
        TlsConnection connection;
        SmtpClient smtp(connection, nullptr);
        split_server(s_settings.smtp_host, MAIL_SMTP_PORT, host, port);
        if (smtp.open(host, port, login_name(), password, s_settings.google())) {
            sending = "OK.";
            smtp.close();
        } else {
            sending = smtp.error().c_str();
        }
    }
}

// ------------------------------------------------------------------------------
// Shared Google Sign-In (Mail and Tasks, from a PC browser)
// ------------------------------------------------------------------------------

// The sign-in in progress: what was entered, and the PKCE verifier whose
// challenge went into the consent address
static String s_pending_address;
static String s_pending_client_id;
static String s_pending_client_secret;
static std::string s_pending_verifier;
static std::string s_pending_state;

// Characters allowed in a PKCE verifier (and used for the state as well)
static std::string random_text(size_t len) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::string text;
    for (size_t i = 0; i < len; i++) {
        text += chars[esp_random() % (sizeof(chars) - 1)];
    }
    return text;
}

String irmail_google_begin(const String &address, const String &client_id, const String &client_secret) {
    s_pending_address = address;
    s_pending_client_id = client_id;
    s_pending_client_secret = client_secret;
    s_pending_verifier = random_text(64);
    s_pending_state = random_text(16);
    return oauth_consent_url(client_id.c_str(), GOOGLE_SCOPES, oauth_pkce_challenge(s_pending_verifier), s_pending_state,
                             address.c_str()).c_str();
}

bool irmail_google_finish(const String &pasted, String &message) {
    if (s_pending_verifier.empty()) {
        message = "No sign-in was started (or the adapter restarted since): start again.";
        return false;
    }
    if (s_running) {
        message = "A mail sync is running: try again when it is done.";
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        message = "Wi-Fi is not connected.";
        return false;
    }
    std::string code, problem;
    if (!oauth_code_from_reply(pasted.c_str(), s_pending_state, code, problem)) {
        message = problem.c_str();
        return false;
    }

    s_stop = false;
    TlsConnection connection;
    OAuthClient google(connection, nullptr);
    OAuthTokens tokens;
    if (!google.exchange_code(s_pending_client_id.c_str(), s_pending_client_secret.c_str(), code, s_pending_verifier, tokens)) {
        message = google.error().c_str();
        return false;
    }
    if (tokens.refresh_token.empty()) {
        message = "Google gave no refresh token. Remove the adapter's access in your Google account and sign in again.";
        return false;
    }
    s_pending_verifier.clear(); // A code works only once

    IrMailSettings settings = s_settings;
    settings.address = s_pending_address;
    settings.login = "";
    settings.google_client_id = s_pending_client_id;
    settings.google_client_secret = s_pending_client_secret;
    settings.google_token = tokens.refresh_token.c_str();
    settings.google_scopes = tokens.scope.empty() ? GOOGLE_SCOPES : tokens.scope.c_str();
    settings.enabled = true;
    irmail_store_settings(settings);
    message = "Signed in as " + s_pending_address + ". Mail sync is on.";
    return true;
}

void irmail_google_sign_out() {
    IrMailSettings settings = s_settings;
    settings.google_token = "";
    settings.google_scopes = "";
    settings.enabled = settings.password.length() > 0;
    irmail_store_settings(settings);
}

// Reuse the same TLS transport, OAuth client and stored Google credentials
// for Tasks. The portal uses no HotSync keep-alive; the conduit does.
bool irmail_google_access_token(std::string &token, std::string &problem, bool keep_alive) {
    token.clear();
    if (!keep_alive && s_running) { problem = "A HotSync is running. Try again when it is done."; return false; }
    if (!s_settings.google()) { problem = "Sign in with Google first."; return false; }
    if (WiFi.status() != WL_CONNECTED) { problem = "Wi-Fi is not connected."; return false; }
    if (!keep_alive) s_stop = false;
    TlsConnection connection;
    OAuthClient google(connection, keep_alive ? hotsync_keep_alive : nullptr);
    OAuthTokens tokens;
    if (!google.refresh(s_settings.google_client_id.c_str(), s_settings.google_client_secret.c_str(),
                        s_settings.google_token.c_str(), tokens)) {
        problem = google.error();
        return false;
    }
    token = tokens.access_token;
    return true;
}

// ------------------------------------------------------------------------------
// The Conduit
// ------------------------------------------------------------------------------

static int palm_read(uint8_t *dest, size_t max_len, uint32_t timeout_ms) {
    if (s_stop) {
        return -1;
    }
    size_t n = xStreamBufferReceive(s_from_palm, dest, max_len, pdMS_TO_TICKS(timeout_ms));
    return s_stop ? -1 : (int)n;
}

// Waits for room: the infrared link takes bytes far more slowly than we produce them
static bool palm_write(const uint8_t *data, size_t len) {
    while (len > 0) {
        if (s_stop) {
            return false;
        }
        size_t n = xStreamBufferSend(s_to_palm, data, len, pdMS_TO_TICKS(100));
        data += n;
        len -= n;
    }
    return true;
}

static uint32_t palm_millis() {
    return millis();
}

static const HotSync_Io k_palm_io = {palm_read, palm_write, palm_millis};

// What the conduit needs from the ESP32
class Esp32MailHost : public MailSyncHost {
public:
    MailConnection &connection() override { return connection_; }
    bool online() override { return WiFi.status() == WL_CONNECTED; }

    int64_t now() override {
        time_t clock = time(nullptr);
        return (clock > CLOCK_VALID_AFTER) ? (int64_t)clock : 0;
    }

    uint32_t random() override { return esp_random(); }

    void load_position(uint32_t &uid_validity, uint32_t &last_uid) override {
        Preferences prefs;
        prefs.begin("irmail", true);
        uid_validity = prefs.getULong("uidv", 0);
        last_uid = prefs.getULong("uid", 0);
        prefs.end();
    }

    void store_position(uint32_t uid_validity, uint32_t last_uid) override {
        Preferences prefs;
        prefs.begin("irmail", false);
        prefs.putULong("uidv", uid_validity);
        prefs.putULong("uid", last_uid);
        prefs.end();
    }

    // 9 bytes per message: Palm record ID, UID, read
    void load_links(std::vector<MailLink> &links) override {
        links.clear();
        Preferences prefs;
        prefs.begin("irmail", true);
        size_t len = prefs.isKey("links") ? prefs.getBytesLength("links") : 0;
        std::vector<uint8_t> data(len);
        if (len > 0 && prefs.getBytes("links", data.data(), len) == len) {
            for (size_t at = 0; at + 9 <= len; at += 9) {
                MailLink link;
                memcpy(&link.record_id, &data[at], 4);
                memcpy(&link.uid, &data[at + 4], 4);
                link.read = data[at + 8] != 0;
                links.push_back(link);
            }
        }
        prefs.end();
    }

    void store_links(const std::vector<MailLink> &links) override {
        std::vector<uint8_t> data(links.size() * 9);
        for (size_t i = 0; i < links.size(); i++) {
            memcpy(&data[i * 9], &links[i].record_id, 4);
            memcpy(&data[i * 9 + 4], &links[i].uid, 4);
            data[i * 9 + 8] = links[i].read ? 1 : 0;
        }
        Preferences prefs;
        prefs.begin("irmail", false);
        if (links.empty()) {
            prefs.remove("links");
        } else {
            prefs.putBytes("links", data.data(), data.size());
        }
        prefs.end();
    }

    void trace(const std::string &text) override { irdbg_printf("Mail: %s", text.c_str()); }

private:
    TlsConnection connection_;
};

static void run_sync() {
    size_t memory_size = HOTSYNC_BUFFER_SIZE(MAILSYNC_RECORD_MAX);
    uint8_t *memory = (uint8_t *)malloc(memory_size);
    String result;

    if (memory == nullptr) {
        result = "not enough memory";
    } else if (!hotsync_accept(&k_palm_io, memory, memory_size, MAIL_ACCEPT_TIMEOUT_MS)) {
        result = "the HotSync did not start";
    } else {
        uint32_t rom = 0;
        hotsync_read_sys_info(&rom);
        irdbg_printf("Mail: HotSync started, Palm OS %u.%u, free memory %u", (unsigned)(rom >> 24), (unsigned)((rom >> 20) & 0x0F),
                     (unsigned)ESP.getFreeHeap());

        MailSyncSetup setup;
        setup.address = s_settings.address.c_str();
        setup.name = s_settings.name.c_str();
        setup.login = login_name();
        setup.password = s_settings.password.c_str();
        if (s_settings.google()) {
            setup.oauth_client_id = s_settings.google_client_id.c_str();
            setup.oauth_client_secret = s_settings.google_client_secret.c_str();
            setup.oauth_refresh_token = s_settings.google_token.c_str();
        }
        split_server(s_settings.imap_host, MAIL_IMAP_PORT, setup.imap_host, setup.imap_port);
        split_server(s_settings.smtp_host, MAIL_SMTP_PORT, setup.smtp_host, setup.smtp_port);
        setup.charset = s_settings.cyrillic ? PALM_CYRILLIC : PALM_WESTERN;
        setup.max_fetch = MAIL_MAX_PER_SYNC;
        setup.max_send = MAIL_MAX_SEND_PER_SYNC;
        setup.html_fetch_max = MAIL_HTML_FETCH_MAX;

        MailSyncResult outcome;
        if (irmail_enabled()) {
            Esp32MailHost host;
            mailsync_run(setup, host, outcome);
        }
        result = irmail_enabled() ? String(mailsync_summary(outcome).c_str()) : String("");
        bool lost = outcome.link_lost;
        if (irmail_enabled() && !lost) hotsync_add_log(("Mail: " + result + "\n").c_str());
        if (!lost && irtasks_ready()) {
            TaskSyncResult tasks;
            irtasks_sync(tasks);
            String summary = tasksync_summary(tasks).c_str();
            if (!tasks.link_lost) hotsync_add_log(("Tasks: " + summary + "\n").c_str());
            lost = tasks.link_lost;
            if (result.length()) result += "; ";
            result += "Tasks: " + summary;
        }
        if (lost) result += " (the link to the Palm was lost)";
        else hotsync_end(0);
    }
    irdbg_printf("Mail: %s", result.c_str());
    strlcpy(s_last_result, result.c_str(), sizeof(s_last_result));
    free(memory);
}

static void mail_task(void *arg) {
    run_sync(); // Everything it allocated is released by the time it returns
    s_running = false;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------------------
// Session (called from the IrDA loop)
// ------------------------------------------------------------------------------

void irmail_init() {
    if (!ENABLE_WIFI_GATEWAY) {
        return;
    }
    load_settings();
    irtasks_init();
    s_from_palm = xStreamBufferCreate(MAIL_STREAM_SIZE, 1);
    s_to_palm = xStreamBufferCreate(MAIL_STREAM_SIZE, 1);
    if (s_from_palm == nullptr || s_to_palm == nullptr) {
        s_from_palm = nullptr; // Mail stays off
        return;
    }
    // Messages carry dates: keep the clock set from the network (UTC)
    configTime(0, 0, MAIL_NTP_SERVER);
}

bool irmail_start(const uint8_t *opening, size_t len) {
    if (s_from_palm == nullptr || s_running) {
        return false;
    }
    xStreamBufferReset(s_from_palm);
    xStreamBufferReset(s_to_palm);
    xStreamBufferSend(s_from_palm, opening, len, 0);
    s_stop = false;
    s_running = true;
    if (xTaskCreate(mail_task, "mail", MAIL_TASK_STACK, NULL, 1, NULL) != pdPASS) {
        s_running = false;
        return false;
    }
    return true;
}

void irmail_stop() {
    s_stop = true;
}

size_t irmail_input_space() {
    return (s_from_palm != nullptr) ? xStreamBufferSpacesAvailable(s_from_palm) : 0;
}

void irmail_input(const uint8_t *data, size_t len) {
    if (s_from_palm != nullptr && len > 0) {
        xStreamBufferSend(s_from_palm, data, len, 0);
    }
}

size_t irmail_read_output(uint8_t *dest, size_t max_len) {
    return (s_to_palm != nullptr) ? xStreamBufferReceive(s_to_palm, dest, max_len, 0) : 0;
}
