// Runs the IMAP and SMTP clients against scripted servers: each step names
// what the client must send and what the server then answers. The answers
// follow what Gmail sends, and arrive a few bytes at a time.
#include "mailnet.h"
#include "scripted_server.h"
#include <stdio.h>
#include <string.h>

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

static int s_idle_calls = 0;
static void idle() {
    s_idle_calls++;
}

static const char k_headers[] =
    "From: =?UTF-8?Q?Ann_M=C3=BCller?= <ann@example.com>\r\n"
    "To: me@gmail.com\r\n"
    "Subject: Lunch (tomorrow) \"at\" {12}\r\n"
    "Date: Sun, 11 Oct 2026 18:30:05 +0200\r\n"
    "\r\n";

static std::string literal(const std::string &data) {
    char size[16];
    snprintf(size, sizeof(size), "{%zu}\r\n", data.length());
    return size + data;
}

static void test_imap(size_t piece) {
    const std::string fetch_items = " (INTERNALDATE BODYSTRUCTURE BODY.PEEK[HEADER.FIELDS (FROM TO CC REPLY-TO SUBJECT DATE X-PRIORITY IMPORTANCE)])\r\n";
    std::vector<Step> steps = {
        {"", "* OK Gimap ready for requests from 203.0.113.7 x1mb12345\r\n"},
        {"A1 LOGIN \"me@gmail.com\" \"pass \\\"word\\\\\"\r\n",
         "* CAPABILITY IMAP4rev1 UNSELECT IDLE NAMESPACE QUOTA ID XLIST CHILDREN X-GM-EXT-1 UIDPLUS COMPRESS=DEFLATE ENABLE MOVE CONDSTORE ESEARCH UTF8=ACCEPT\r\n"
         "A1 OK me@gmail.com authenticated (Success)\r\n"},
        {"A2 EXAMINE INBOX\r\n",
         "* FLAGS (\\Answered \\Flagged \\Draft \\Deleted \\Seen $NotPhishing $Phishing)\r\n"
         "* OK [PERMANENTFLAGS ()] Flags permitted.\r\n"
         "* OK [UIDVALIDITY 1700000001] UIDs valid.\r\n"
         "* 7 EXISTS\r\n"
         "* 0 RECENT\r\n"
         "* OK [UIDNEXT 213] Predicted next UID.\r\n"
         "* OK [HIGHESTMODSEQ 99123]\r\n"
         "A2 OK [READ-ONLY] INBOX selected. (Success)\r\n"},
        {"A3 FETCH 4:7 (UID FLAGS)\r\n",
         "* 4 FETCH (UID 205 FLAGS (\\Seen))\r\n"
         "* 5 FETCH (FLAGS (\\Deleted \\Seen) UID 207)\r\n"
         "* 6 FETCH (UID 210 FLAGS ())\r\n"
         "* 8 EXISTS\r\n"
         "* 7 FETCH (UID 212 MODSEQ (99124) FLAGS (\\Answered $NotPhishing))\r\n"
         "A3 OK Success\r\n"},
        // Plain and HTML alternatives: the plain one is taken
        {"A4 UID FETCH 210" + fetch_items,
         "* 6 FETCH (UID 210 INTERNALDATE \"11-Oct-2026 16:30:07 +0000\" BODYSTRUCTURE ((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"UTF-8\" \"FORMAT\" \"flowed\") NIL NIL \"QUOTED-PRINTABLE\" 31 2 NIL NIL NIL)"
         "(\"TEXT\" \"HTML\" (\"CHARSET\" \"UTF-8\") NIL NIL \"QUOTED-PRINTABLE\" 120 4 NIL NIL NIL) \"ALTERNATIVE\" (\"BOUNDARY\" \"000000000000a1b2c3\") NIL NIL) "
         "BODY[HEADER.FIELDS (FROM TO CC REPLY-TO SUBJECT DATE X-PRIORITY IMPORTANCE)] " + literal(k_headers) + ")\r\n"
         "A4 OK Success\r\n"},
        {"A5 UID FETCH 210 (BODY.PEEK[1]<0.2000>)\r\n",
         "* 6 FETCH (UID 210 BODY[1]<0> " + literal("Caf=C3=A9 at noon?\r\n) not the end\r\n") + ")\r\n"
         "* 6 FETCH (FLAGS (\\Seen))\r\n"
         "A5 OK Success\r\n"},
        // An attachment before the text, which sits inside a nested multipart; an attached message after it
        {"A6 UID FETCH 212" + fetch_items,
         "* 7 FETCH (BODY[HEADER.FIELDS (FROM TO CC REPLY-TO SUBJECT DATE X-PRIORITY IMPORTANCE)] \"Subject: short\" UID 212 BODYSTRUCTURE "
         "((\"IMAGE\" \"PNG\" (\"NAME\" \"a (1).png\") \"<id@x>\" NIL \"BASE64\" 99999 NIL (\"ATTACHMENT\" (\"FILENAME\" \"a (1).png\")) NIL)"
         "((\"TEXT\" \"HTML\" (\"CHARSET\" \"windows-1251\") NIL NIL \"BASE64\" 800 12 NIL NIL NIL)(\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 0 0) \"ALTERNATIVE\")"
         "(\"MESSAGE\" \"RFC822\" NIL NIL NIL \"7BIT\" 500 (\"Sun, 11 Oct 2026 10:00:00 +0000\" \"inner (subject)\" ((\"A\" NIL \"a\" \"x.com\")) NIL NIL NIL NIL NIL NIL \"<m@x>\") (\"TEXT\" \"PLAIN\" (\"CHARSET\" \"US-ASCII\") NIL NIL \"7BIT\" 20 1) 12)"
         " \"MIXED\" (\"BOUNDARY\" \"b\") NIL NIL) INTERNALDATE \" 1-Oct-2026 09:00:00 +0200\")\r\n"
         "A6 OK Success\r\n"},
        {"A7 UID FETCH 212 (BODY.PEEK[2.2]<0.500>)\r\n", "* 7 FETCH (UID 212 BODY[2.2]<0> \"tiny \\\"quoted\\\" body\")\r\nA7 OK Success\r\n"},
        // No text at all; then a message that vanished meanwhile
        {"A8 UID FETCH 205" + fetch_items,
         "* 4 FETCH (UID 205 INTERNALDATE \"11-Oct-2026 16:30:07 +0000\" BODYSTRUCTURE (\"APPLICATION\" \"PDF\" NIL NIL NIL \"BASE64\" 5000) "
         "BODY[HEADER.FIELDS (FROM TO CC REPLY-TO SUBJECT DATE X-PRIORITY IMPORTANCE)] NIL)\r\nA8 OK Success\r\n"},
        {"A9 UID FETCH 207 (BODY.PEEK[1]<0.100>)\r\n", "* 5 EXPUNGE\r\nA9 OK Success\r\n"},
        {"A10 UID FETCH 999 (BODY.PEEK[1]<0.100>)\r\n", "A10 NO [NONEXISTENT] Unknown Mailbox: gone (now in authenticated state) (Failure)\r\n"},
        {"A11 LOGOUT\r\n", "* BYE LOGOUT Requested\r\nA11 OK 73 good day (Success)\r\n"},
    };
    ScriptedServer server(steps, piece);
    ImapClient imap(server, idle);

    expect_true("imap open", imap.open("imap.gmail.com", 993, "me@gmail.com", "pass \"word\\"));
    expect_true("imap host", server.host_ == "imap.gmail.com" && server.port_ == 993);
    uint32_t validity = 0, exists = 0;
    expect_true("imap examine", imap.examine_inbox(&validity, &exists) && validity == 1700000001 && exists == 7);

    std::vector<ImapMessage> list;
    expect_true("imap list", imap.list_newest(exists, 4, list));
    expect_true("imap list contents", list.size() == 3 && list[0].uid == 205 && list[0].seen && list[1].uid == 210 &&
                                      !list[1].seen && list[2].uid == 212 && !list[2].seen);

    std::string headers, date;
    ImapPart part;
    expect_true("imap summary", imap.fetch_summary(210, headers, date, part));
    expect("imap headers", headers, k_headers);
    expect("imap date", date, "11-Oct-2026 16:30:07 +0000");
    expect_true("imap part", part.section == "1" && !part.html && part.charset == "UTF-8" && part.encoding == "QUOTED-PRINTABLE" && part.size == 31);
    MailTextDecoder text(part.charset, part.encoding, part.html, PALM_WESTERN, 4000);
    expect_true("imap text", imap.fetch_text(210, part, 2000, text));
    text.finish();
    expect("imap text contents", text.text(), "Caf\xE9 at noon?\n) not the end");

    expect_true("imap nested summary", imap.fetch_summary(212, headers, date, part));
    expect("imap quoted headers", headers, "Subject: short");
    expect_true("imap nested part", part.section == "2.2" && !part.html && part.charset.empty() && part.encoding == "7BIT");
    MailTextDecoder tiny(part.charset, part.encoding, part.html, PALM_WESTERN, 4000);
    expect_true("imap quoted text", imap.fetch_text(212, part, 500, tiny));
    expect("imap quoted text contents", tiny.text(), "tiny \"quoted\" body");

    expect_true("imap no text", imap.fetch_summary(205, headers, date, part) && part.section.empty() && headers.empty());
    ImapPart first;
    first.section = "1";
    MailTextDecoder none("", "", false, PALM_WESTERN, 100);
    expect_true("imap vanished message", imap.fetch_text(207, first, 100, none) && none.text().empty());
    expect_true("imap refusal", !imap.fetch_text(999, first, 100, none));
    expect("imap refusal text", imap.error(), "IMAP fetch text: [NONEXISTENT] Unknown Mailbox: gone (now in authenticated state) (Fai");
    imap.close();
    expect_true("imap script", server.finished("imap") && server.closed_);
}

static void test_imap_failures() {
    {
        ScriptedServer server({{"", "* OK ready\r\n"},
                               {"A1 LOGIN \"me\" \"wrong\"\r\n", "A1 NO [AUTHENTICATIONFAILED] Invalid credentials (Failure)\r\n"}}, 100);
        ImapClient imap(server, idle);
        expect_true("imap bad login", !imap.open("h", 993, "me", "wrong"));
        expect("imap bad login text", imap.error(), "IMAP login: [AUTHENTICATIONFAILED] Invalid credentials (Failure)");
    }
    {
        ScriptedServer server({{"", "* OK ready\r\n"}}, 100); // Never answers the login
        ImapClient imap(server, idle);
        s_idle_calls = 0;
        expect_true("imap silent server", !imap.open("h", 993, "me", "x"));
        expect("imap silent text", imap.error(), "IMAP login: no answer");
        expect_true("idle called while waiting", s_idle_calls >= 80);
    }
    {
        ScriptedServer server({{"", "220 this is not IMAP\r\n"}}, 100);
        ImapClient imap(server, idle);
        expect_true("imap wrong greeting", !imap.open("h", 993, "me", "x"));
    }
    {
        // A response cut off in the middle of a literal
        ScriptedServer server({{"", "* OK ready\r\n"}, {"A1 LOGIN \"me\" \"x\"\r\n", "A1 OK\r\n"},
                               {"A2 UID FETCH 5 (BODY.PEEK[1]<0.100>)\r\n", "* 1 FETCH (UID 5 BODY[1]<0> {50}\r\nonly this"}}, 100);
        ImapClient imap(server, idle);
        ImapPart part;
        part.section = "1";
        MailTextDecoder text("", "", false, PALM_WESTERN, 100);
        expect_true("imap open plain OK", imap.open("h", 993, "me", "x"));
        expect_true("imap cut-off literal", !imap.fetch_text(5, part, 100, text));
    }
}

static void test_smtp(size_t piece) {
    const std::string message = "Subject: Hi\r\n\r\nLine\r\n.\r\n..two\r\nlast";
    std::vector<Step> steps = {
        {"", "220 smtp.gmail.com ESMTP x1-20020a05 - gsmtp\r\n"},
        {"EHLO palm.local\r\n",
         "250-smtp.gmail.com at your service, [203.0.113.7]\r\n250-SIZE 35882577\r\n250-8BITMIME\r\n"
         "250-AUTH LOGIN PLAIN XOAUTH2 PLAIN-CLIENTTOKEN OAUTHBEARER XOAUTH\r\n250-ENHANCEDSTATUSCODES\r\n250 SMTPUTF8\r\n"},
        {"AUTH PLAIN AG1lQGdtYWlsLmNvbQBzZWNyZXQ=\r\n", "235 2.7.0 Accepted\r\n"},
        {"MAIL FROM:<me@gmail.com>\r\n", "250 2.1.0 OK x1 - gsmtp\r\n"},
        {"RCPT TO:<ann@example.com>\r\n", "250 2.1.5 OK x1 - gsmtp\r\n"},
        {"RCPT TO:<bob@example.org>\r\n", "250 2.1.5 OK x1 - gsmtp\r\n"},
        {"DATA\r\n", "354 Go ahead x1 - gsmtp\r\n"},
        {"Subject: Hi\r\n\r\nLine\r\n..\r\n...two\r\nlast\r\n.\r\n", "250 2.0.0 OK 1791736205 x1 - gsmtp\r\n"},
        // A refused recipient must not end the session
        {"MAIL FROM:<me@gmail.com>\r\n", "250 2.1.0 OK\r\n"},
        {"RCPT TO:<bad@example>\r\n", "553-5.1.3 The recipient address <bad@example> is not a valid RFC 5321\r\n553 5.1.3 address. x1 - gsmtp\r\n"},
        {"RSET\r\n", "250 2.1.5 Flushed x1 - gsmtp\r\n"},
        {"MAIL FROM:<me@gmail.com>\r\n", "250 2.1.0 OK\r\n"},
        {"RCPT TO:<ann@example.com>\r\n", "250 2.1.5 OK\r\n"},
        {"DATA\r\n", "354 Go ahead\r\n"},
        {"x\r\n.\r\n", "250 2.0.0 OK\r\n"},
        {"QUIT\r\n", "221 2.0.0 closing connection x1 - gsmtp\r\n"},
    };
    ScriptedServer server(steps, piece);
    SmtpClient smtp(server, idle);
    expect_true("smtp open", smtp.open("smtp.gmail.com", 465, "me@gmail.com", "secret"));
    expect_true("smtp send", smtp.send("me@gmail.com", {"ann@example.com", "bob@example.org"}, message));
    expect_true("smtp refused recipient", !smtp.send("me@gmail.com", {"bad@example"}, "x\r\n"));
    expect("smtp refusal text", smtp.error(), "SMTP recipient: 553 5.1.3 address. x1 - gsmtp");
    expect_true("smtp send after refusal", smtp.send("me@gmail.com", {"ann@example.com"}, "x\r\n"));
    smtp.close();
    expect_true("smtp script", server.finished("smtp") && server.closed_);

    ScriptedServer refusing({{"", "220 ready\r\n"}, {"EHLO palm.local\r\n", "250 ok\r\n"},
                             {"AUTH PLAIN AG1lAHdyb25n\r\n", "535-5.7.8 Username and Password not accepted. For more information, go to\r\n535 5.7.8  https://support.google.com/mail/?p=BadCredentials x1 - gsmtp\r\n"}}, piece);
    SmtpClient bad(refusing, idle);
    expect_true("smtp bad login", !bad.open("smtp.gmail.com", 465, "me", "wrong"));
    expect("smtp bad login text", bad.error(), "SMTP login: 535 5.7.8  https://support.google.com/mail/?p=BadCredentials x1 - gsmt");
}

// Google sign-in: the access token as SASL XOAUTH2, accepted and refused
static void test_xoauth2(size_t piece) {
    const std::string token = "dXNlcj1tZUBnbWFpbC5jb20BYXV0aD1CZWFyZXIgeWEyOS50b2sBAQ==";
    const std::string reason = "eyJzdGF0dXMiOiI0MDAiLCJzY2hlbWVzIjoiQmVhcmVyIiwic2NvcGUiOiJodHRwczovL21haWwuZ29vZ2xlLmNvbS8ifQ==";
    {
        ScriptedServer server({{"", "* OK Gimap ready\r\n"},
                               {"A1 AUTHENTICATE XOAUTH2 " + token + "\r\n",
                                "* CAPABILITY IMAP4rev1 UNSELECT IDLE\r\nA1 OK me@gmail.com authenticated (Success)\r\n"}}, piece);
        ImapClient imap(server, idle);
        expect_true("imap xoauth2", imap.open("imap.gmail.com", 993, "me@gmail.com", "ya29.tok", true));
        expect_true("imap xoauth2 script", server.finished("imap xoauth2"));
    }
    {
        ScriptedServer server({{"", "* OK Gimap ready\r\n"},
                               {"A1 AUTHENTICATE XOAUTH2 " + token + "\r\n", "+ " + reason + "\r\n"},
                               {"\r\n", "A1 NO [AUTHENTICATIONFAILED] Invalid credentials (Failure)\r\n"}}, piece);
        ImapClient imap(server, idle);
        expect_true("imap xoauth2 refused", !imap.open("imap.gmail.com", 993, "me@gmail.com", "ya29.tok", true));
        expect("imap xoauth2 refused text", imap.error(), "IMAP login: [AUTHENTICATIONFAILED] Invalid credentials (Failure)");
        expect_true("imap xoauth2 refused script", server.finished("imap xoauth2 refused"));
    }
    {
        ScriptedServer server({{"", "220 smtp.gmail.com ESMTP\r\n"},
                               {"EHLO palm.local\r\n", "250-smtp.gmail.com\r\n250 AUTH LOGIN PLAIN XOAUTH2\r\n"},
                               {"AUTH XOAUTH2 " + token + "\r\n", "235 2.7.0 Accepted\r\n"},
                               {"QUIT\r\n", "221 2.0.0 closing connection\r\n"}}, piece);
        SmtpClient smtp(server, idle);
        expect_true("smtp xoauth2", smtp.open("smtp.gmail.com", 465, "me@gmail.com", "ya29.tok", true));
        smtp.close();
        expect_true("smtp xoauth2 script", server.finished("smtp xoauth2"));
    }
    {
        ScriptedServer server({{"", "220 smtp.gmail.com ESMTP\r\n"},
                               {"EHLO palm.local\r\n", "250 smtp.gmail.com\r\n"},
                               {"AUTH XOAUTH2 " + token + "\r\n", "334 " + reason + "\r\n"},
                               {"\r\n", "535-5.7.8 Username and Password not accepted.\r\n535 5.7.8 https://support.google.com/mail/?p=BadCredentials\r\n"}}, piece);
        SmtpClient smtp(server, idle);
        expect_true("smtp xoauth2 refused", !smtp.open("smtp.gmail.com", 465, "me@gmail.com", "ya29.tok", true));
        expect("smtp xoauth2 refused text", smtp.error(), "SMTP login: 535 5.7.8 https://support.google.com/mail/?p=BadCredentials");
        expect_true("smtp xoauth2 refused script", server.finished("smtp xoauth2 refused"));
    }
}

int main() {
    // The same conversations with the answers arriving whole, and in small pieces
    static const size_t pieces[] = {4096, 7, 1};
    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
        test_imap(pieces[i]);
        test_smtp(pieces[i]);
        test_xoauth2(pieces[i]);
    }
    test_imap_failures();
    printf(s_failures == 0 ? "mailnet: all passed\n" : "mailnet: %d FAILED\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
