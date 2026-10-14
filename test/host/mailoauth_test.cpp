// The Google sign-in pieces: what goes into the consent address, how the code
// is taken from what the owner pastes, and the token server's answers (as
// Google sends them, arriving whole and a few bytes at a time).
#include "mailoauth.h"
#include "scripted_server.h"
#include <stdio.h>

static int s_failures = 0;

static void expect(const char *what, const std::string &got, const std::string &wanted) {
    if (got != wanted) {
        s_failures++;
        printf("FAILED: %s\n  got:    [%s]\n  wanted: [%s]\n", what, got.c_str(), wanted.c_str());
    }
}

static void expect_true(const char *what, bool condition) {
    if (!condition) {
        s_failures++;
        printf("FAILED: %s\n", what);
    }
}

static void idle() {}

static void test_pieces() {
    expect("xoauth2", mail_xoauth2("me@gmail.com", "ya29.tok"), "dXNlcj1tZUBnbWFpbC5jb20BYXV0aD1CZWFyZXIgeWEyOS50b2sBAQ==");
    expect("encode", oauth_encode("a b/c+d~e.f_g-h@i"), "a%20b%2Fc%2Bd~e.f_g-h%40i");
    // RFC 7636, appendix B
    expect("pkce", oauth_pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    // Lengths around the SHA-256 padding boundary
    expect("pkce 55", oauth_pkce_challenge(std::string(55, 'a')), "n0OQ-NMMLdkuyfCVtl4rmumwqSWlJY4kHJ8ekQ9zQxg");
    expect("pkce 64", oauth_pkce_challenge(std::string(64, 'a')), "_-BU_nrgy23GXDr5th1SCfQ5hR20PQulmXM33xVGaOs");
    expect("consent", oauth_consent_url("123-abc.apps.googleusercontent.com", "https://mail.google.com/", "E9Mel", "st8", "me@gmail.com"),
           "https://accounts.google.com/o/oauth2/v2/auth?response_type=code&client_id=123-abc.apps.googleusercontent.com"
           "&redirect_uri=http%3A%2F%2F127.0.0.1&scope=https%3A%2F%2Fmail.google.com%2F&code_challenge=E9Mel"
           "&code_challenge_method=S256&state=st8&access_type=offline&prompt=consent&login_hint=me%40gmail.com");

    const std::string scopes = GOOGLE_DEFAULT_SCOPES;
    expect("planned services", scopes,
           "https://mail.google.com/ https://www.googleapis.com/auth/tasks "
           "https://www.googleapis.com/auth/contacts https://www.googleapis.com/auth/calendar "
           "https://www.googleapis.com/auth/drive https://www.googleapis.com/auth/documents "
           "https://www.googleapis.com/auth/spreadsheets https://www.googleapis.com/auth/presentations");
    std::string consent = oauth_consent_url("client", scopes, "challenge", "state", "");
    expect_true("all services in one consent request",
        consent.find("&scope=https%3A%2F%2Fmail.google.com%2F"
                     "%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Ftasks"
                     "%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fcontacts"
                     "%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fcalendar"
                     "%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdrive"
                     "%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdocuments"
                     "%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fspreadsheets"
                     "%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fpresentations&") != std::string::npos);
    expect_true("Contacts grant", oauth_has_scope(scopes, GOOGLE_CONTACTS_SCOPE));
    expect_true("Calendar grant", oauth_has_scope(scopes, GOOGLE_CALENDAR_SCOPE));
    expect_true("Drive grant", oauth_has_scope(scopes, GOOGLE_DRIVE_SCOPE));
    expect_true("Docs grant", oauth_has_scope(scopes, GOOGLE_DOCS_SCOPE));
    expect_true("Sheets grant", oauth_has_scope(scopes, GOOGLE_SHEETS_SCOPE));
    expect_true("Slides grant", oauth_has_scope(scopes, GOOGLE_SLIDES_SCOPE));
    expect_true("read-only Docs grant does not authorize changes",
        !oauth_has_scope("https://www.googleapis.com/auth/documents.readonly", GOOGLE_DOCS_SCOPE));
    expect_true("read-only Sheets grant does not authorize changes",
        !oauth_has_scope("https://www.googleapis.com/auth/spreadsheets.readonly", GOOGLE_SHEETS_SCOPE));
    expect_true("read-only Slides grant does not authorize changes",
        !oauth_has_scope("https://www.googleapis.com/auth/presentations.readonly", GOOGLE_SLIDES_SCOPE));
    expect_true("read-only grant does not authorize changes",
        !oauth_has_scope("https://www.googleapis.com/auth/contacts.readonly", GOOGLE_CONTACTS_SCOPE));
    expect_true("per-file Drive grant does not authorize all files",
        !oauth_has_scope("https://www.googleapis.com/auth/drive.file", GOOGLE_DRIVE_SCOPE));
    expect_true("separated grants", oauth_has_scope("\t https://www.googleapis.com/auth/tasks \n", GOOGLE_TASKS_SCOPE));
    expect_true("missing grant", !oauth_has_scope(GOOGLE_MAIL_SCOPE, GOOGLE_TASKS_SCOPE));
    expect_true("empty grant", !oauth_has_scope("", GOOGLE_DRIVE_SCOPE) && !oauth_has_scope(scopes, ""));

    std::string code, problem;
    expect_true("reply address", oauth_code_from_reply(" http://127.0.0.1/?state=st8&code=4%2F0AeanS0b-x_y&scope=https%3A%2F%2Fmail.google.com%2F \n",
                                                       "st8", code, problem));
    expect("reply code", code, "4/0AeanS0b-x_y");
    expect_true("reply without path", oauth_code_from_reply("127.0.0.1?code=4/0Ab&state=st8", "st8", code, problem) && code == "4/0Ab");
    expect_true("bare code", oauth_code_from_reply("4/0AeanS0b", "st8", code, problem) && code == "4/0AeanS0b");
    expect_true("other state", !oauth_code_from_reply("http://127.0.0.1/?state=old&code=4%2F0A", "st8", code, problem));
    expect("other state text", problem, "This address belongs to an older sign-in. Start again and paste the newest one.");
    expect_true("denied", !oauth_code_from_reply("http://127.0.0.1/?error=access_denied&state=st8", "st8", code, problem));
    expect("denied text", problem, "Access was not allowed on Google's page.");
    expect_true("no code", !oauth_code_from_reply("http://127.0.0.1/?state=st8", "st8", code, problem));
    expect_true("empty", !oauth_code_from_reply("  ", "st8", code, problem));

    std::string value;
    const std::string json = "{\n  \"access_token\": \"ya29.a\\/b\",\n  \"expires_in\": 3599,\n  \"note\": \"access_token\",\n"
                             "  \"text\": \"say \\\"hi\\\" \\u0041\"\n}";
    expect_true("json string", oauth_json_field(json, "access_token", value) && value == "ya29.a/b");
    expect_true("json number", oauth_json_field(json, "expires_in", value) && value == "3599");
    expect_true("json escapes", oauth_json_field(json, "text", value) && value == "say \"hi\" A");
    expect_true("json missing", !oauth_json_field(json, "refresh_token", value));
}

static const char k_exchange_form[] =
    "grant_type=authorization_code&code=4%2F0Ab&code_verifier=verifier-1&redirect_uri=http%3A%2F%2F127.0.0.1"
    "&client_id=123-abc.apps.googleusercontent.com&client_secret=GOCSPX-s3cr%2Bt";
static const char k_refresh_form[] =
    "grant_type=refresh_token&refresh_token=1%2F%2F0g-refresh&client_id=123-abc.apps.googleusercontent.com"
    "&client_secret=GOCSPX-s3cr%2Bt";

static std::string request(const std::string &form) {
    return "POST /token HTTP/1.0\r\nHost: oauth2.googleapis.com\r\nContent-Type: application/x-www-form-urlencoded\r\n"
           "Accept: application/json\r\nContent-Length: " + std::to_string(form.length()) + "\r\nConnection: close\r\n\r\n" + form;
}

static std::string answer(const char *status, const std::string &body, bool chunked = false) {
    std::string text = std::string("HTTP/1.0 ") + status + "\r\nContent-Type: application/json; charset=utf-8\r\n"
                       "Vary: Origin\r\nCache-Control: no-cache, no-store, max-age=0, must-revalidate\r\n";
    if (chunked) {
        char size[16];
        snprintf(size, sizeof(size), "%zx", body.length() - 10);
        return text + "Transfer-Encoding: chunked\r\n\r\n" + size + "\r\n" + body.substr(0, body.length() - 10) + "\r\na\r\n" +
               body.substr(body.length() - 10) + "\r\n0\r\n\r\n";
    }
    return text + "Content-Length: " + std::to_string(body.length()) + "\r\n\r\n" + body;
}

static void test_token_server(size_t piece) {
    const std::string id = "123-abc.apps.googleusercontent.com", secret = "GOCSPX-s3cr+t";
    {
        ScriptedServer server({{"", ""},
                               {request(k_exchange_form),
                                answer("200 OK", "{\n  \"access_token\": \"ya29.first\",\n  \"expires_in\": 3599,\n"
                                                 "  \"refresh_token\": \"1//0g-refresh\",\n  \"scope\": \"https://mail.google.com/\",\n"
                                                 "  \"token_type\": \"Bearer\"\n}")}}, piece);
        OAuthClient google(server, idle);
        OAuthTokens tokens;
        expect_true("exchange", google.exchange_code(id, secret, "4/0Ab", "verifier-1", tokens));
        expect_true("exchange tokens", tokens.access_token == "ya29.first" && tokens.refresh_token == "1//0g-refresh" &&
                                       tokens.expires_in == 3599 && tokens.scope == GOOGLE_MAIL_SCOPE);
        expect_true("old mail grant stays mail-only", !oauth_has_scope(tokens.scope, GOOGLE_CONTACTS_SCOPE));
        expect_true("exchange script", server.finished("exchange") && server.closed_ && server.host_ == "oauth2.googleapis.com" &&
                                       server.port_ == 443);
    }
    {
        // Retain the complete shared grant; the mail-only case above also
        // verifies that a usable token does not imply every API was granted.
        std::string body = "{\"access_token\":\"ya29.shared\",\"refresh_token\":\"refresh-shared\",\"scope\":\"" +
                           std::string(GOOGLE_DEFAULT_SCOPES) + "\",\"expires_in\":3599}";
        ScriptedServer server({{"", ""}, {request(k_exchange_form), answer("200 OK", body)}}, piece);
        OAuthClient google(server, idle);
        OAuthTokens tokens;
        expect_true("shared exchange", google.exchange_code(id, secret, "4/0Ab", "verifier-1", tokens));
        expect_true("shared scopes retained", tokens.scope == GOOGLE_DEFAULT_SCOPES &&
                    oauth_has_scope(tokens.scope, GOOGLE_CONTACTS_SCOPE) &&
                    oauth_has_scope(tokens.scope, GOOGLE_CALENDAR_SCOPE) &&
                    oauth_has_scope(tokens.scope, GOOGLE_DRIVE_SCOPE) &&
                    oauth_has_scope(tokens.scope, GOOGLE_DOCS_SCOPE) &&
                    oauth_has_scope(tokens.scope, GOOGLE_SHEETS_SCOPE) &&
                    oauth_has_scope(tokens.scope, GOOGLE_SLIDES_SCOPE));
        expect_true("shared exchange script", server.finished("shared exchange") && server.closed_);
    }
    {
        ScriptedServer server({{"", ""}, {request(k_refresh_form), answer("200 OK", "{\"access_token\":\"ya29.second\",\"expires_in\":3599}", true)}},
                              piece);
        OAuthClient google(server, idle);
        OAuthTokens tokens;
        expect_true("refresh chunked", google.refresh(id, secret, "1//0g-refresh", tokens) && tokens.access_token == "ya29.second" &&
                                       tokens.refresh_token.empty());
    }
    {
        ScriptedServer server({{"", ""},
                               {request(k_refresh_form),
                                answer("400 Bad Request", "{\n  \"error\": \"invalid_grant\",\n  \"error_description\": \"Token has been expired or revoked.\"\n}")}},
                              piece);
        OAuthClient google(server, idle);
        OAuthTokens tokens;
        expect_true("refresh revoked", !google.refresh(id, secret, "1//0g-refresh", tokens));
        expect("refresh revoked text", google.error(),
               "Google sign-in no longer valid, sign in again from a PC (Token has been expired or revoked.)");
    }
    {
        ScriptedServer server({{"", ""},
                               {request(k_refresh_form),
                                answer("401 Unauthorized", "{\n  \"error\": \"invalid_client\",\n  \"error_description\": \"The OAuth client was not found.\"\n}")}},
                              piece);
        OAuthClient google(server, idle);
        OAuthTokens tokens;
        expect_true("bad client", !google.refresh(id, secret, "1//0g-refresh", tokens));
        expect("bad client text", google.error(), "Google sign-in: invalid_client, The OAuth client was not found.");
    }
    {
        ScriptedServer server({{"", ""}, {request(k_refresh_form), "HTTP/1.0 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"}}, piece);
        OAuthClient google(server, idle);
        OAuthTokens tokens;
        expect_true("bad gateway", !google.refresh(id, secret, "1//0g-refresh", tokens));
        expect("bad gateway text", google.error(), "Google sign-in: 502 Bad Gateway");
    }
}

int main() {
    test_pieces();
    static const size_t pieces[] = {4096, 7, 1};
    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
        test_token_server(pieces[i]);
    }
    printf(s_failures == 0 ? "mailoauth: all passed\n" : "mailoauth: %d FAILED\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
