#pragma once

#include <Arduino.h>
#include "config.h"

// Mail conduit: when the Palm starts a HotSync and no desktop takes it, the
// adapter answers it itself and synchronises the Palm's Mail application with
// a mail account over Wi-Fi (Outbox to SMTP, the newest IMAP inbox messages to
// the Inbox). Set up from the portal.

struct IrMailSettings {
    bool enabled = false;
    String address;    // E-mail address; also the login name unless `login` is set
    String password;
    String name;       // Sender name shown to recipients (may be empty)
    String login;
    String imap_host;  // "host" or "host:port"
    String smtp_host;
    bool cyrillic = false; // The Palm shows Windows-1251 text instead of Western
    // Google sign-in instead of the password (Gmail): the owner's own OAuth
    // client, and the refresh token got when signing in from a PC
    String google_client_id;
    String google_client_secret;
    String google_token;
    bool google() const { return google_token.length() > 0; }
};

void irmail_init();

// Settings (portal). Changing the account also forgets which messages were fetched.
const IrMailSettings &irmail_settings();
void irmail_store_settings(const IrMailSettings &settings);
void irmail_forget_position();  // The next sync fetches the newest messages again
bool irmail_ready();            // Switched on, with an account to use
String irmail_last_result();    // Outcome of the last sync, for the portal
// Tries to log in to both servers; one line of text for each
void irmail_check_login(String &receiving, String &sending);

// Google sign-in, done once from a PC browser (portal). The first call gives
// the address of Google's consent page; after the consent the owner pastes
// the address the browser was sent back to, and the second call trades the
// code in it for a refresh token and stores the account. `message` is a line
// for the page either way.
String irmail_google_begin(const String &address, const String &client_id, const String &client_secret);
bool irmail_google_finish(const String &pasted, String &message);
void irmail_google_sign_out();

// A HotSync session handled by the conduit. `opening` is what the Palm sent
// before the session was handed over. Returns false while an earlier sync is
// still winding up: try again.
bool irmail_start(const uint8_t *opening, size_t len);
void irmail_stop();

// Raw HotSync byte stream: from the Palm into the conduit, and back
size_t irmail_input_space();
void irmail_input(const uint8_t *data, size_t len);
size_t irmail_read_output(uint8_t *dest, size_t max_len);
