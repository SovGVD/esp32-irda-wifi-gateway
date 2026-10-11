#pragma once

#include <stdint.h>
#include <string>
#include "mailnet.h"

// Google sign-in (OAuth 2.0) for the mail account, instead of an app password.
// The owner signs in once from a PC browser; the adapter keeps the refresh
// token it gets for that, and trades it for a short-lived access token at the
// start of each sync. The access token is the login for IMAP and SMTP (SASL
// XOAUTH2). Talks to Google through an abstract connection like the mail
// clients, so nothing here depends on the ESP32.
//
// Google sends the browser back to a loopback address after the consent (the
// way for installed apps), which does not load: the address, with the code in
// it, is then copied from the browser into the adapter's page.

#define OAUTH_CONSENT_URL   "https://accounts.google.com/o/oauth2/v2/auth"
#define OAUTH_TOKEN_HOST    "oauth2.googleapis.com"
#define OAUTH_TOKEN_PORT    443
#define OAUTH_REDIRECT_URI  "http://127.0.0.1"
#define OAUTH_REPLY_MAX     8192  // Longest answer read from the token server

struct OAuthTokens {
    std::string access_token;
    std::string refresh_token;  // Only when a code is exchanged
    uint32_t expires_in = 0;    // Seconds the access token is valid
};

// SASL XOAUTH2 initial response, base64-encoded, for IMAP AUTHENTICATE and SMTP AUTH
std::string mail_xoauth2(const std::string &user, const std::string &access_token);

// Percent-encoding for addresses and forms: everything but A-Z a-z 0-9 - . _ ~
std::string oauth_encode(const std::string &text);

// PKCE S256 challenge of a verifier: BASE64URL(SHA-256(verifier)), no padding
std::string oauth_pkce_challenge(const std::string &verifier);

// The Google page that asks the owner to allow access. `challenge` is the PKCE
// S256 challenge of the verifier later passed to exchange_code().
std::string oauth_consent_url(const std::string &client_id, const std::string &scope, const std::string &challenge,
                              const std::string &state, const std::string &login_hint);

// The code from what the owner pasted: the whole address the browser was sent
// back to, or the bare code. `state` must match when the address carries one.
// On failure `problem` says why, in a line for the page.
bool oauth_code_from_reply(const std::string &pasted, const std::string &state, std::string &code, std::string &problem);

// A string or number field of a flat JSON object
bool oauth_json_field(const std::string &json, const char *name, std::string &value);

class OAuthClient {
public:
    OAuthClient(MailConnection &conn, void (*idle)()) : stream_(conn, idle) {}

    // After the consent: the code for a refresh token (and a first access token)
    bool exchange_code(const std::string &client_id, const std::string &client_secret, const std::string &code,
                       const std::string &verifier, OAuthTokens &tokens);
    // Before a sync: the refresh token for a new access token
    bool refresh(const std::string &client_id, const std::string &client_secret, const std::string &refresh_token,
                 OAuthTokens &tokens);
    const std::string &error() const { return error_; } // Short, for the HotSync log

private:
    bool request(const std::string &form, OAuthTokens &tokens);
    bool read_body(bool chunked, size_t length, std::string &body);

    MailStream stream_;
    std::string error_;
};
