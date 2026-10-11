// Checks of the mail text conversions (records, character sets, MIME, dates)
#include "mailtext.h"
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

static std::string decode(const char *charset, const char *encoding, bool html, const std::string &input,
                          PalmCharset target = PALM_WESTERN, size_t max_chars = 4000, size_t piece = 0) {
    MailTextDecoder decoder(charset, encoding, html, target, max_chars);
    if (piece == 0) {
        decoder.feed((const uint8_t *)input.data(), input.length());
    } else {
        for (size_t at = 0; at < input.length(); at += piece) {
            size_t n = input.length() - at < piece ? input.length() - at : piece;
            decoder.feed((const uint8_t *)input.data() + at, n);
        }
    }
    decoder.finish();
    return decoder.text();
}

int main() {
    // --- Records ---
    MailMessage msg;
    msg.subject = "Hello";
    msg.from = "Ann <ann@example.com>";
    msg.to = "me@example.com";
    msg.body = "Line one\nLine two";
    msg.read = true;
    msg.priority = MAIL_PRIORITY_HIGH;
    mail_set_date(msg, 1791736200, 120); // 2026-10-11 16:30 UTC, shown two hours east
    uint8_t record[256];
    size_t len = mail_pack_record(msg, record, sizeof(record));
    expect_true("record length", len == 6 + 6 + 22 + 15 + 4 + 18);
    // Date bits: (2026 - 1904) << 9 | 10 << 5 | 11, then hour and minute
    expect_true("record date", record[0] == 0xF5 && record[1] == 0x4B && record[2] == 18 && record[3] == 30);
    expect_true("record flags", record[4] == 0x80 && record[5] == 0);
    MailMessage back;
    expect_true("unpack", mail_unpack_record(record, len, back));
    expect("unpack subject", back.subject, "Hello");
    expect("unpack body", back.body, "Line one\nLine two");
    expect("unpack to", back.to, "me@example.com");
    expect_true("unpack date", back.dated && back.year == 2026 && back.month == 10 && back.day == 11 && back.hour == 18 && back.minute == 30);
    expect_true("unpack flags", back.read && back.priority == MAIL_PRIORITY_HIGH && !back.signature);
    expect_true("too small a buffer", mail_pack_record(msg, record, 20) == 0);
    expect_true("short record refused", !mail_unpack_record(record, 5, back));
    expect_true("cut-off record survives", mail_unpack_record(record, 14, back) && back.subject == "Hello" && back.body.empty());

    // HotSync options: filter, take high priority, only matching, truncate 250
    const uint8_t prefs[] = {2, 1, 1, 0, 0x00, 0xFA, 'b', 'o', 's', 's', 0, 0, 'u', 'r', 'g', 0};
    MailSyncOptions options;
    mail_unpack_sync_options(prefs, sizeof(prefs), options);
    expect_true("options", options.type == MAIL_SYNC_FILTER && options.truncate == 250 && options.to == "boss" &&
                           options.from.empty() && options.subject == "urg");
    expect_true("filter match", mail_filter_accepts(options, "a@b", "The Boss <x@y>", "hi", false));
    expect_true("filter subject", mail_filter_accepts(options, "a@b", "x@y", "URGENT", false));
    expect_true("filter miss", !mail_filter_accepts(options, "a@b", "x@y", "hi", false));
    expect_true("filter high priority", mail_filter_accepts(options, "a@b", "x@y", "hi", true));
    mail_unpack_sync_options(prefs, 3, options);
    expect_true("options default", options.type == MAIL_SYNC_ALL && options.truncate == MAIL_DEFAULT_TRUNCATE);

    // --- Text ---
    expect("plain text", decode("us-ascii", "7bit", false, "Hello,\r\n\r\n\r\n\r\nworld  \r\n  indented\r\n\r\n"),
           "Hello,\n\nworld\n  indented");
    expect("utf-8 to western", decode("UTF-8", "8bit", false, "caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x82\xAC" "5 \xE2\x80\x94 \xC5\x81\xC3\xB3" "d\xC5\xBA \xF0\x9F\x8E\x89" "ok \xE4\xB8\xAD"),
           "caf\xE9 \x93quoted\x94 \x80" "5 \x97 L\xF3" "dz ok ?");
    expect("utf-8 cyrillic to western", decode("utf-8", "", false, "\xD0\x9F\xD1\x80\xD0\xB8"), "???");
    expect("utf-8 to cyrillic", decode("utf-8", "", false, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD1\x91\xD0\x81 caf\xC3\xA9", PALM_CYRILLIC),
           "\xCF\xF0\xE8\xE2\xE5\xF2, \xB8\xA8 cafe");
    expect("koi8-r to cyrillic", decode("KOI8-R", "", false, "\xF0\xD2\xC9\xD7\xC5\xD4", PALM_CYRILLIC), "\xCF\xF0\xE8\xE2\xE5\xF2");
    expect("windows-1251 to cyrillic", decode("windows-1251", "", false, "\xCF\xF0\xE8\xE2\xE5\xF2", PALM_CYRILLIC), "\xCF\xF0\xE8\xE2\xE5\xF2");
    expect("iso-8859-5 to cyrillic", decode("ISO-8859-5", "", false, "\xBF\xE0\xD8\xD2\xD5\xE2", PALM_CYRILLIC), "\xCF\xF0\xE8\xE2\xE5\xF2");
    expect("latin1", decode("iso-8859-1", "", false, "na\xEFve"), "na\xEFve");
    expect("mislabelled utf-8", decode("utf-8", "", false, "na\xEFve \xE9"), "na\xEFve \xE9");
    expect("base64", decode("utf-8", "base64", false, "SGVsbG8sIHdvcmxkIQ0K\r\nU2Vjb25kIGxpbmU=\r\n"), "Hello, world!\nSecond line");
    expect("base64 in pieces", decode("utf-8", "BASE64", false, "SGVsbG8sIHdvcmxkIQ0K\r\nU2Vjb25kIGxpbmU=\r\n", PALM_WESTERN, 4000, 1),
           "Hello, world!\nSecond line");
    expect("quoted-printable", decode("utf-8", "quoted-printable", false, "caf=C3=A9 costs 5=E2=82=AC, a long line that is bro=\r\nken here; 2+2=4 = ok\r\n"),
           "caf\xE9 costs 5\x80, a long line that is broken here; 2+2=4 = ok");
    expect("quoted-printable in pieces", decode("utf-8", "quoted-printable", false, "caf=C3=A9 bro=\r\nken=\nagain", PALM_WESTERN, 4000, 1),
           "caf\xE9 brokenagain");
    expect("truncated", decode("utf-8", "", false, "1234567890", PALM_WESTERN, 4), "1234");

    const char *html =
        "<html><head><title>Ignore</title><style>p { color: red }</style></head>\r\n"
        "<body><!-- hidden --><div class=\"a>b\">Dear&nbsp;Ann,</div>\r\n"
        "<p>This is   <b>bold</b>\r\n and a <a href='http://x/?a=1&amp;b=2'>link</a>.</p>"
        "<ul><li>one</li><li>two &amp; three</li></ul>"
        "<script>if (a < b) { alert('</p>'); }</script>"
        "Price: 5&euro; &lt;tag&gt; &#169; &#x263A; &unknown; 1 < 2<br><br>Bye<br>\r\n</body></html>";
    expect("html", decode("utf-8", "", true, html),
           "Dear Ann,\n\nThis is bold and a link.\n\n- one\n- two & three\nPrice: 5\x80 <tag> \xA9 ? &unknown; 1 < 2\n\nBye");
    expect("html in pieces", decode("utf-8", "", true, html, PALM_WESTERN, 4000, 1), decode("utf-8", "", true, html));

    // --- Headers ---
    const std::string headers =
        "Received: by example\r\n"
        "Subject: =?UTF-8?B?Q2Fmw6kg4oCT?= =?UTF-8?Q?_men=C3=BC?=\r\n\t(continued)\r\n"
        "From: =?iso-8859-1?Q?J=F6rg_M=FCller?= <joerg@example.com>\r\n"
        "X-Subject: wrong one\r\n"
        "to: me@example.com\r\n"
        "Date: Sun, 11 Oct 2026 18:30:05 +0200\r\n";
    expect("subject field", mail_decode_header(mail_header_field(headers, "Subject"), PALM_WESTERN), "Caf\xE9 \x96 men\xFC (continued)");
    expect("from field", mail_decode_header(mail_header_field(headers, "From"), PALM_WESTERN), "J\xF6rg M\xFCller <joerg@example.com>");
    expect("field name case", mail_header_field(headers, "To"), "me@example.com");
    expect("absent field", mail_header_field(headers, "Cc"), "");
    expect("raw utf-8 header", mail_decode_header("Zo\xC3\xAB =?bad", PALM_WESTERN), "Zo\xEB =?bad");
    expect("cyrillic header", mail_decode_header("=?koi8-r?B?8NLJ18XU?=", PALM_CYRILLIC), "\xCF\xF0\xE8\xE2\xE5\xF2");

    // --- Dates ---
    int64_t when = 0;
    expect_true("rfc date", mail_parse_date(mail_header_field(headers, "Date"), &when) && when == 1791736205);
    expect_true("imap date", mail_parse_date(" 1-Jan-2026 00:00:00 +0000", &when) && when == 1767225600);
    expect_true("named zone", mail_parse_date("Thu, 1 Jan 2026 00:00 EST", &when) && when == 1767225600 + 5 * 3600);
    expect_true("two digit year", mail_parse_date("1 Jan 26 00:00:00 GMT", &when) && when == 1767225600);
    expect_true("bad date", !mail_parse_date("yesterday", &when));
    MailMessage dated;
    mail_set_date(dated, 1767225600, -300);
    expect_true("date west of UTC", dated.dated && dated.year == 2025 && dated.month == 12 && dated.day == 31 && dated.hour == 19 && dated.minute == 0);
    mail_set_date(dated, 2000000000, 0); // 2033: beyond what a record can hold
    expect_true("date out of range", !dated.dated);
    expect_true("seconds", mail_seconds(2026, 10, 11, 16, 30, 0) == 1791736200);

    // --- Outgoing ---
    std::vector<std::string> addresses;
    mail_extract_addresses("ann@example.com, Bob Smith <bob@example.org>;\ncarl@example.net (Carl), nobody, \"d@quoted.com\" e@example.com.", addresses);
    std::string joined;
    for (size_t i = 0; i < addresses.size(); i++) {
        joined += addresses[i] + "|";
    }
    expect("addresses", joined, "ann@example.com|bob@example.org|carl@example.net|d@quoted.com|e@example.com|");

    MailMessage outgoing;
    outgoing.subject = "Plain subject";
    outgoing.to = "Ann <ann@example.com>, bob@example.org";
    outgoing.cc = "carl@example.net";
    outgoing.bcc = "hidden@example.com";
    outgoing.body = "Hi Ann,\n.\nSee you.";
    MailIdentity me;
    me.address = "me@example.com";
    me.name = "Me \"Myself\"";
    expect("compose plain", mail_compose(outgoing, me, PALM_WESTERN, "Sig", 1791736205, 120, "1.2@example.com"),
           "From: \"Me Myself\" <me@example.com>\r\n"
           "To: ann@example.com,\r\n bob@example.org\r\n"
           "Cc: carl@example.net\r\n"
           "Subject: Plain subject\r\n"
           "Date: Sun, 11 Oct 2026 18:30:05 +0200\r\n"
           "Message-ID: <1.2@example.com>\r\n"
           "MIME-Version: 1.0\r\n"
           "Content-Type: text/plain; charset=US-ASCII\r\n"
           "Content-Transfer-Encoding: 7bit\r\n"
           "\r\n"
           "Hi Ann,\r\n.\r\nSee you.\r\n");

    outgoing.subject = "Caf\xE9\r\nBcc: injected@example.com";
    outgoing.cc = "";
    outgoing.signature = true;
    outgoing.priority = MAIL_PRIORITY_HIGH;
    outgoing.confirm_read = true;
    outgoing.body = "Gr\xFC\xDF" "e = 100% \x80\nA line that is longer than seventy-six characters has to be folded by the encoding  \nend";
    me.name = "J\xF6rg";
    expect("compose encoded", mail_compose(outgoing, me, PALM_WESTERN, "Sig", 0, 0, ""),
           "From: =?UTF-8?B?SsO2cmc=?= <me@example.com>\r\n"
           "To: ann@example.com,\r\n bob@example.org\r\n"
           "Subject: =?UTF-8?B?Q2Fmw6kgIEJjYzogaW5qZWN0ZWRAZXhhbXBsZS5jb20=?=\r\n"
           "X-Priority: 1\r\nImportance: high\r\n"
           "Disposition-Notification-To: <me@example.com>\r\n"
           "MIME-Version: 1.0\r\n"
           "Content-Type: text/plain; charset=UTF-8\r\n"
           "Content-Transfer-Encoding: quoted-printable\r\n"
           "\r\n"
           "Gr=C3=BC=C3=9Fe =3D 100% =E2=82=AC\r\n"
           "A line that is longer than seventy-six characters has to be folded by the e=\r\n"
           "ncoding =20\r\n"
           "end\r\n"
           "--=20\r\n"
           "Sig\r\n");

    MailMessage cyrillic;
    cyrillic.subject = "\xCF\xF0\xE8\xE2\xE5\xF2";
    cyrillic.to = "a@b.ru";
    cyrillic.body = "\xCF\xF0\xE8\xE2\xE5\xF2";
    std::string composed = mail_compose(cyrillic, me, PALM_CYRILLIC, "", 0, 0, "");
    expect_true("compose cyrillic subject", composed.find("Subject: =?UTF-8?B?0J/RgNC40LLQtdGC?=\r\n") != std::string::npos);
    expect_true("compose cyrillic body", composed.find("\r\n\r\n=D0=9F=D1=80=D0=B8=D0=B2=D0=B5=D1=82\r\n") != std::string::npos);

    // A long subject is split into several encoded words, never inside a character
    MailMessage longer;
    longer.subject = std::string(60, '\xE9');
    composed = mail_compose(longer, me, PALM_WESTERN, "", 0, 0, "");
    size_t subject_at = composed.find("Subject: ");
    std::string subject_lines = composed.substr(subject_at + 9, composed.find("\r\nMIME", subject_at) - subject_at - 9);
    expect("long subject round trip", mail_decode_header(mail_header_field("S: " + subject_lines + "\r\n", "S"), PALM_WESTERN), longer.subject);
    expect_true("long subject folded", subject_lines.find("?=\r\n =?UTF-8?B?") != std::string::npos);

    expect("base64", mail_base64(std::string("\0user\0pass", 10)), "AHVzZXIAcGFzcw==");

    printf(s_failures == 0 ? "mailtext: all passed\n" : "mailtext: %d FAILED\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
