#include "mailoauth.h"
#include "mailtext.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ERROR_TEXT_MAX  90  // Server text quoted in an error (it ends up in the HotSync log)

std::string mail_xoauth2(const std::string &user, const std::string &access_token) {
    return mail_base64("user=" + user + "\x01" + "auth=Bearer " + access_token + "\x01\x01");
}

std::string oauth_encode(const std::string &text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < text.length(); i++) {
        unsigned char c = (unsigned char)text[i];
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

bool oauth_has_scope(const std::string &granted, const std::string &scope) {
    if (scope.empty()) return false;
    size_t at = 0;
    while (at < granted.size()) {
        while (at < granted.size() && isspace((unsigned char)granted[at])) at++;
        size_t end = at;
        while (end < granted.size() && !isspace((unsigned char)granted[end])) end++;
        if (granted.compare(at, end - at, scope) == 0) return true;
        at = end;
    }
    return false;
}

static std::string decoded(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.length(); i++) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%' && i + 2 < text.length() && isxdigit((unsigned char)text[i + 1]) &&
                   isxdigit((unsigned char)text[i + 2])) {
            out += (char)strtol(text.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

// ------------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), for the PKCE challenge only
// ------------------------------------------------------------------------------

static uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t state[8], const uint8_t *block) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static std::string sha256(const std::string &data) {
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string padded = data + '\x80';
    while (padded.length() % 64 != 56) {
        padded += '\0';
    }
    uint64_t bits = (uint64_t)data.length() * 8;
    for (int i = 7; i >= 0; i--) {
        padded += (char)(bits >> (i * 8));
    }
    for (size_t at = 0; at < padded.length(); at += 64) {
        sha256_block(state, (const uint8_t *)padded.data() + at);
    }
    std::string digest;
    for (int i = 0; i < 8; i++) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            digest += (char)(state[i] >> shift);
        }
    }
    return digest;
}

std::string oauth_pkce_challenge(const std::string &verifier) {
    std::string text = mail_base64(sha256(verifier));
    for (size_t i = 0; i < text.length(); i++) {
        if (text[i] == '+') {
            text[i] = '-';
        } else if (text[i] == '/') {
            text[i] = '_';
        }
    }
    size_t padding = text.find('=');
    return (padding == std::string::npos) ? text : text.substr(0, padding);
}

// ------------------------------------------------------------------------------
// Consent
// ------------------------------------------------------------------------------

std::string oauth_consent_url(const std::string &client_id, const std::string &scope, const std::string &challenge,
                              const std::string &state, const std::string &login_hint) {
    // access_type=offline and prompt=consent: a refresh token every time, also
    // when the account allowed this client before
    std::string url = OAUTH_CONSENT_URL "?response_type=code&client_id=" + oauth_encode(client_id) +
                      "&redirect_uri=" + oauth_encode(OAUTH_REDIRECT_URI) + "&scope=" + oauth_encode(scope) +
                      "&code_challenge=" + oauth_encode(challenge) + "&code_challenge_method=S256" +
                      "&state=" + oauth_encode(state) + "&access_type=offline&prompt=consent";
    if (!login_hint.empty()) {
        url += "&login_hint=" + oauth_encode(login_hint);
    }
    return url;
}

// Value of a field in the query of an address ("...?state=x&code=4%2F0A"), decoded
static bool query_field(const std::string &query, const char *name, std::string &value) {
    size_t name_len = strlen(name);
    size_t at = 0;
    while (at < query.length()) {
        size_t end = query.find('&', at);
        if (end == std::string::npos) {
            end = query.length();
        }
        if (end - at > name_len && query.compare(at, name_len, name) == 0 && query[at + name_len] == '=') {
            value = decoded(query.substr(at + name_len + 1, end - at - name_len - 1));
            return true;
        }
        at = end + 1;
    }
    return false;
}

bool oauth_code_from_reply(const std::string &pasted, const std::string &state, std::string &code, std::string &problem) {
    std::string text = pasted;
    size_t first = text.find_first_not_of(" \t\r\n");
    size_t last = text.find_last_not_of(" \t\r\n");
    text = (first == std::string::npos) ? std::string() : text.substr(first, last - first + 1);
    code.clear();
    if (text.empty()) {
        problem = "Nothing was pasted.";
        return false;
    }

    size_t query = text.find('?');
    if (query == std::string::npos && text.find('=') == std::string::npos) {
        code = text; // The bare code
        return true;
    }
    std::string fields = (query == std::string::npos) ? text : text.substr(query + 1);
    size_t hash = fields.find('#');
    if (hash != std::string::npos) {
        fields = fields.substr(0, hash);
    }

    std::string error, returned_state;
    if (query_field(fields, "error", error)) {
        problem = (error == "access_denied") ? "Access was not allowed on Google's page." : "Google answered: " + error;
        return false;
    }
    if (query_field(fields, "state", returned_state) && returned_state != state) {
        problem = "This address belongs to an older sign-in. Start again and paste the newest one.";
        return false;
    }
    if (!query_field(fields, "code", code) || code.empty()) {
        problem = "No code in the pasted address. Copy the whole address from the browser's address bar.";
        return false;
    }
    return true;
}

bool oauth_json_field(const std::string &json, const char *name, std::string &value) {
    std::string key = std::string("\"") + name + "\"";
    size_t at = 0;
    while ((at = json.find(key, at)) != std::string::npos) {
        size_t pos = json.find_first_not_of(" \t\r\n", at + key.length());
        at += key.length();
        if (pos == std::string::npos || json[pos] != ':') {
            continue; // The name appeared as a value
        }
        pos = json.find_first_not_of(" \t\r\n", pos + 1);
        if (pos == std::string::npos) {
            return false;
        }
        value.clear();
        if (json[pos] != '"') {
            // Number (or true / false / null)
            while (pos < json.length() && json[pos] != ',' && json[pos] != '}' && !isspace((unsigned char)json[pos])) {
                value += json[pos++];
            }
            return !value.empty();
        }
        for (pos++; pos < json.length() && json[pos] != '"'; pos++) {
            if (json[pos] != '\\' || pos + 1 >= json.length()) {
                value += json[pos];
                continue;
            }
            char c = json[++pos];
            if (c == 'n') {
                value += '\n';
            } else if (c == 't') {
                value += '\t';
            } else if (c == 'u' && pos + 4 < json.length()) {
                long unit = strtol(json.substr(pos + 1, 4).c_str(), nullptr, 16);
                value += (unit >= 0x20 && unit < 0x7F) ? (char)unit : '?';
                pos += 4;
            } else {
                value += c; // \" \\ \/
            }
        }
        return pos < json.length();
    }
    return false;
}

static std::string shortened(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.length() && out.length() < ERROR_TEXT_MAX; i++) {
        if ((unsigned char)text[i] >= 0x20 && (unsigned char)text[i] < 0x7F) {
            out += text[i];
        }
    }
    return out;
}

// ------------------------------------------------------------------------------
// Token Server
// ------------------------------------------------------------------------------

bool OAuthClient::exchange_code(const std::string &client_id, const std::string &client_secret, const std::string &code,
                                const std::string &verifier, OAuthTokens &tokens) {
    return request("grant_type=authorization_code&code=" + oauth_encode(code) + "&code_verifier=" + oauth_encode(verifier) +
                       "&redirect_uri=" + oauth_encode(OAUTH_REDIRECT_URI) + "&client_id=" + oauth_encode(client_id) +
                       "&client_secret=" + oauth_encode(client_secret),
                   tokens);
}

bool OAuthClient::refresh(const std::string &client_id, const std::string &client_secret, const std::string &refresh_token,
                          OAuthTokens &tokens) {
    return request("grant_type=refresh_token&refresh_token=" + oauth_encode(refresh_token) + "&client_id=" +
                       oauth_encode(client_id) + "&client_secret=" + oauth_encode(client_secret),
                   tokens);
}

// The body of the answer: `length` bytes, in chunks, or up to the end of the connection
bool OAuthClient::read_body(bool chunked, size_t length, std::string &body) {
    body.clear();
    if (!chunked) {
        int c;
        while (body.length() < length && body.length() < OAUTH_REPLY_MAX && (c = stream_.get()) >= 0) {
            body += (char)c;
        }
        return length == (size_t)-1 || body.length() == length;
    }
    for (;;) {
        std::string size_line;
        if (!stream_.line(size_line)) {
            return false;
        }
        size_t size = strtoul(size_line.c_str(), nullptr, 16);
        if (size == 0) {
            return true;
        }
        for (size_t i = 0; i < size; i++) {
            int c = stream_.get();
            if (c < 0) {
                return false;
            }
            if (body.length() < OAUTH_REPLY_MAX) {
                body += (char)c;
            }
        }
        if (!stream_.line(size_line)) { // The line end after the chunk
            return false;
        }
    }
}

bool OAuthClient::request(const std::string &form, OAuthTokens &tokens) {
    tokens = OAuthTokens();
    if (!stream_.connect(OAUTH_TOKEN_HOST, OAUTH_TOKEN_PORT)) {
        error_ = "Google sign-in: cannot connect to " OAUTH_TOKEN_HOST;
        return false;
    }
    // HTTP/1.0: one answer, then the server closes the connection
    std::string text = "POST /token HTTP/1.0\r\nHost: " OAUTH_TOKEN_HOST "\r\n"
                       "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n"
                       "Content-Length: " + std::to_string(form.length()) + "\r\nConnection: close\r\n\r\n" + form;
    std::string status, header, body;
    bool ok = stream_.send(text) && stream_.line(status);
    bool chunked = false;
    size_t length = (size_t)-1;
    while (ok) {
        if (!stream_.line(header)) {
            ok = false;
        } else if (header.empty()) {
            break;
        } else if (strncasecmp(header.c_str(), "Content-Length:", 15) == 0) {
            length = strtoul(header.c_str() + 15, nullptr, 10);
        } else if (strncasecmp(header.c_str(), "Transfer-Encoding:", 18) == 0 && strstr(header.c_str(), "chunked") != nullptr) {
            chunked = true;
        }
    }
    ok = ok && read_body(chunked, length, body);
    stream_.close();
    if (!ok || status.compare(0, 5, "HTTP/") != 0 || status.length() < 12) {
        error_ = "Google sign-in: no answer";
        return false;
    }

    int code = atoi(status.c_str() + 9);
    std::string expires;
    if (code == 200 && oauth_json_field(body, "access_token", tokens.access_token) && !tokens.access_token.empty()) {
        oauth_json_field(body, "refresh_token", tokens.refresh_token);
        oauth_json_field(body, "scope", tokens.scope);
        if (oauth_json_field(body, "expires_in", expires)) {
            tokens.expires_in = (uint32_t)strtoul(expires.c_str(), nullptr, 10);
        }
        return true;
    }

    // {"error": "invalid_grant", "error_description": "Token has been expired or revoked."}
    std::string error, description;
    oauth_json_field(body, "error", error);
    oauth_json_field(body, "error_description", description);
    if (error.empty()) {
        error_ = "Google sign-in: " + shortened(status.substr(9));
    } else if (error == "invalid_grant") {
        // Revoked, expired (testing mode: 7 days), password changed, or a code used twice
        error_ = "Google sign-in no longer valid, sign in again from a PC" + (description.empty() ? "" : " (" + shortened(description) + ")");
    } else {
        error_ = "Google sign-in: " + shortened(error + (description.empty() ? "" : ", " + description));
    }
    return false;
}
