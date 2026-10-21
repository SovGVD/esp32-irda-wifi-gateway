#include "irportal.h"
#include "irppp.h"
#include "irmail.h"
#include "irsyncdiag.h"
#include "irtasks.h"
#include "ircalendar.h"
#include "ircontacts.h"
#include "irdrive.h"
#include "irnotes.h"
#include "irexpenses.h"
#include "irhotsync.h"
#include "irdebug.h"
#include "irxiino.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include <lwip/sockets.h>
#include <lwip/inet.h>

// Wi-Fi networks saved from the portal (NVS), newest first
struct SavedNetwork {
    String ssid;
    String password;
    String status; // Outcome of the last connection attempt, for the status page
};
static SavedNetwork s_saved[PORTAL_MAX_SAVED];
static uint8_t s_saved_count = 0;

// Networks found by the last scan
struct ScannedNetwork {
    char ssid[33];
    int8_t rssi;
    uint8_t security; // wifi_auth_mode_t
};
static ScannedNetwork s_networks[PORTAL_MAX_NETWORKS];
static uint8_t s_network_count = 0;

// Wi-Fi connection manager
enum WifiPhase {
    PHASE_IDLE,       // About to scan
    PHASE_SCANNING,
    PHASE_CONNECTING, // Trying s_queue[s_queue_pos]
    PHASE_CONNECTED,
    PHASE_WAITING     // Nothing worked: pause before the next scan
};
static WifiPhase s_phase = PHASE_IDLE;
static uint32_t s_phase_since_ms = 0;
static bool s_scan_requested = true;        // Scan once at startup for the portal's list
static uint8_t s_queue[PORTAL_MAX_SAVED];   // Saved networks to try, in order
static uint8_t s_queue_len = 0;
static uint8_t s_queue_pos = 0;
static uint8_t s_attempt = 0;               // Try number for the current network
static String s_preferred;                  // Network to try first (just entered)
static bool s_reduced_power = false;         // Transmit power of the current / last try
static bool s_prefer_reduced = false;        // Reduced power is what worked last time
static volatile bool s_attempt_failed = false;
static volatile uint8_t s_fail_reason = 0;

// Recent name lookups by the Palm, for the diagnostics page
#define DNS_LOG_ENTRIES 8
struct DnsLogEntry {
    char name[44];
    char result[28];
};
static DnsLogEntry s_dns_log[DNS_LOG_ENTRIES];
static uint8_t s_dns_log_next = 0;
static portMUX_TYPE s_dns_log_lock = portMUX_INITIALIZER_UNLOCKED;

// The HTTP request being served (one at a time)
static int s_client = -1;          // Socket
static uint32_t s_client_ip = 0;   // Peer address, network byte order
static String s_target;            // Request target: path and query, as sent
static String s_params;            // Form fields: the query, plus the body of a POST
static String s_request_text;      // The whole request as received, for the diagnostics page
static String s_request_host;      // Site the browser asked for (address or Host header)
static String s_request_agent;     // Browser identification, passed on by the proxy
static String s_request_method;    // GET or POST
static String s_request_body;      // Form fields of a POST, passed on by the proxy
static XiinoRequest s_xiino_request;
static uint32_t s_dataserver_pages=0,s_direct_pages=0;

// Google sign-in from a PC: the sign-in pages answer on the Wi-Fi side while
// this is open (opened from the Palm's Mail page, for GOOGLE_SETUP_WINDOW_MS)
static bool s_google_window_open = false;
static uint32_t s_google_window_since = 0;
static bool s_google_from_tasks = false;
static bool s_google_from_calendar = false;
static bool s_google_from_contacts = false;
static bool s_google_from_drive = false;
static int s_google_from_notes = -1;
static bool s_google_from_expenses = false;

// Web proxy for all sites (see the proxy section); switched from the status page
static bool s_proxy_all_sites = PROXY_ALL_SITES_DEFAULT;
static bool s_wants_wml = false;

// ------------------------------------------------------------------------------
// Saved Networks
// ------------------------------------------------------------------------------

static void store_saved_networks() {
    Preferences prefs;
    prefs.begin("irda", false);
    prefs.putUChar("count", s_saved_count);
    for (uint8_t i = 0; i < s_saved_count; i++) {
        prefs.putString((String("ssid") + String(i)).c_str(), s_saved[i].ssid);
        prefs.putString((String("pass") + String(i)).c_str(), s_saved[i].password);
    }
    prefs.end();
}

static void load_saved_networks() {
    Preferences prefs;
    prefs.begin("irda", false);
    s_saved_count = 0;

    if (prefs.isKey("count")) {
        uint8_t count = prefs.getUChar("count", 0);
        for (uint8_t i = 0; i < count && s_saved_count < PORTAL_MAX_SAVED; i++) {
            String ssid = prefs.getString((String("ssid") + String(i)).c_str(), "");
            if (ssid.length() > 0) {
                s_saved[s_saved_count].ssid = ssid;
                s_saved[s_saved_count].password = prefs.getString((String("pass") + String(i)).c_str(), "");
                s_saved[s_saved_count].status = "";
                s_saved_count++;
            }
        }
    } else {
        // First start: take over the single network kept by earlier firmware
        String ssid = prefs.getString("ssid", "");
        if (ssid.length() > 0) {
            s_saved[0].ssid = ssid;
            s_saved[0].password = prefs.getString("pass", "");
            s_saved[0].status = "";
            s_saved_count = 1;
        }
    }
    prefs.end();

    // The network compiled in through config.h is always a candidate too
    String default_ssid = WIFI_SSID;
    if (default_ssid.length() > 0 && s_saved_count < PORTAL_MAX_SAVED) {
        bool listed = false;
        for (uint8_t i = 0; i < s_saved_count; i++) {
            listed = listed || s_saved[i].ssid == default_ssid;
        }
        if (!listed) {
            s_saved[s_saved_count].ssid = default_ssid;
            s_saved[s_saved_count].password = WIFI_PASSWORD;
            s_saved[s_saved_count].status = "";
            s_saved_count++;
        }
    }
}

static void load_settings() {
    Preferences prefs;
    prefs.begin("irda", true);
    s_proxy_all_sites = prefs.getUChar("proxy", PROXY_ALL_SITES_DEFAULT) != 0;
    prefs.end();
}

static void store_settings() {
    Preferences prefs;
    prefs.begin("irda", false);
    prefs.putUChar("proxy", s_proxy_all_sites ? 1 : 0);
    prefs.end();
}

static int find_saved_network(const String &ssid) {
    for (uint8_t i = 0; i < s_saved_count; i++) {
        if (s_saved[i].ssid == ssid) {
            return i;
        }
    }
    return -1;
}

static void remove_saved_network(int index) {
    for (uint8_t i = index; i + 1 < s_saved_count; i++) {
        s_saved[i] = s_saved[i + 1];
    }
    s_saved_count--;
}

// Newest first; when the list is full the oldest entry makes room
static void add_saved_network(const String &ssid, const String &password) {
    int existing = find_saved_network(ssid);
    if (existing >= 0) {
        remove_saved_network(existing);
    } else if (s_saved_count == PORTAL_MAX_SAVED) {
        s_saved_count--;
    }
    for (uint8_t i = s_saved_count; i > 0; i--) {
        s_saved[i] = s_saved[i - 1];
    }
    s_saved[0].ssid = ssid;
    s_saved[0].password = password;
    s_saved[0].status = "";
    s_saved_count++;
    store_saved_networks();
}

// ------------------------------------------------------------------------------
// Wi-Fi Connection Manager
// ------------------------------------------------------------------------------
// Scans, then tries the saved networks that are in range, strongest first, and
// keeps the outcome of each attempt for the status page. Runs in the portal task.

// Called by the Wi-Fi driver task when a connection attempt or a link fails
static void on_wifi_event(arduino_event_t *event) {
    uint8_t reason = event->event_info.wifi_sta_disconnected.reason;
    if (reason != WIFI_REASON_ASSOC_LEAVE) { // Not our own WiFi.disconnect()
        s_fail_reason = reason;
        s_attempt_failed = true;
    }
}

static String failure_text(bool failed, uint8_t reason) {
    if (!failed) {
        return "no answer";
    }
    String code = " (code " + String(reason) + ")";
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "not found" + code;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "refused, wrong password?" + code;
    default:
        return "failed" + code;
    }
}

static void enter_phase(WifiPhase phase) {
    s_phase = phase;
    s_phase_since_ms = millis();
}

static void begin_attempt() {
    const SavedNetwork &net = s_saved[s_queue[s_queue_pos]];

#ifdef WIFI_TX_POWER
    s_reduced_power = false;
    wifi_power_t power = WIFI_TX_POWER;
#else
    // The second try uses the other transmit power: many ESP32-C3 SuperMini
    // boards cannot complete the handshake at full power (antenna matching).
    // Whichever power connected last is tried first.
    s_reduced_power = (s_attempt > 1) ? !s_prefer_reduced : s_prefer_reduced;
    wifi_power_t power = s_reduced_power ? WIFI_POWER_8_5dBm : WIFI_POWER_19_5dBm;
#endif

    // Stop whatever the driver is still trying from the previous attempt
    WiFi.disconnect();
    delay(100);

    irdbg_printf("Wi-Fi: joining %s (try %u%s)", net.ssid.c_str(), s_attempt, s_reduced_power ? ", low power" : "");
    WiFi.begin(net.ssid.c_str(), net.password.length() > 0 ? net.password.c_str() : nullptr);
    WiFi.setTxPower(power);
    s_attempt_failed = false;
    s_saved[s_queue[s_queue_pos]].status = "connecting";
    enter_phase(PHASE_CONNECTING);
}

static void try_next_network() {
    if (s_queue_pos >= s_queue_len) {
        WiFi.disconnect();
        enter_phase(PHASE_WAITING); // Nothing (left) to try: look again later
        return;
    }
    s_attempt = 1;
    begin_attempt();
}

// An attempt that is being abandoned has no outcome to report
static void clear_unfinished_attempt() {
    for (uint8_t i = 0; i < s_saved_count; i++) {
        if (s_saved[i].status == "connecting") {
            s_saved[i].status = "";
        }
    }
}

// Drop whatever the manager is doing and start over with a fresh scan
static void restart_wifi_manager() {
    clear_unfinished_attempt();
    if (s_phase == PHASE_CONNECTING || s_phase == PHASE_CONNECTED) {
        WiFi.disconnect();
        delay(100);
    }
    s_scan_requested = true;
    if (s_phase != PHASE_SCANNING) {
        enter_phase(PHASE_IDLE);
    }
}

static void request_scan() {
    s_scan_requested = true;
    if (s_phase == PHASE_CONNECTING) {
        clear_unfinished_attempt();
        WiFi.disconnect(); // The radio cannot scan while it is joining a network
        delay(100);
        enter_phase(PHASE_IDLE);
    } else if (s_phase == PHASE_WAITING) {
        enter_phase(PHASE_IDLE);
    }
}

static void collect_scan_results(int16_t found) {
    // Results arrive strongest first; keep one entry per network name
    s_network_count = 0;
    for (int16_t i = 0; i < found && s_network_count < PORTAL_MAX_NETWORKS; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0 || ssid.length() >= sizeof(s_networks[0].ssid)) {
            continue;
        }
        bool duplicate = false;
        for (uint8_t k = 0; k < s_network_count; k++) {
            duplicate = duplicate || ssid == s_networks[k].ssid;
        }
        if (duplicate) {
            continue;
        }
        ScannedNetwork &net = s_networks[s_network_count++];
        strcpy(net.ssid, ssid.c_str());
        net.rssi = (int8_t)WiFi.RSSI(i);
        net.security = (uint8_t)WiFi.encryptionType(i);
    }
    WiFi.scanDelete();
}

// Order in which to try the saved networks after a scan
static void build_attempt_queue() {
    s_queue_len = 0;
    s_queue_pos = 0;
    bool queued[PORTAL_MAX_SAVED] = {};

    // 1. The network the user just entered, even if the scan missed it
    int preferred = find_saved_network(s_preferred);
    s_preferred = "";
    if (preferred >= 0) {
        s_queue[s_queue_len++] = preferred;
        queued[preferred] = true;
    }

    // 2. Saved networks seen by the scan, strongest first
    bool any_in_range = false;
    for (uint8_t i = 0; i < s_network_count; i++) {
        int saved = find_saved_network(s_networks[i].ssid);
        if (saved >= 0) {
            any_in_range = true;
            if (!queued[saved]) {
                s_queue[s_queue_len++] = saved;
                queued[saved] = true;
            }
        }
    }

    // 3. Nothing known in range: try them all anyway (hidden networks do not show up in a scan)
    for (uint8_t i = 0; i < s_saved_count; i++) {
        if (queued[i]) {
            continue;
        }
        if (any_in_range) {
            s_saved[i].status = "not in range";
        } else {
            s_queue[s_queue_len++] = i;
        }
    }
}

static void manage_wifi() {
    uint32_t now = millis();
    bool connected = WiFi.status() == WL_CONNECTED;

    switch (s_phase) {
    case PHASE_IDLE:
        if (s_saved_count == 0 && !s_scan_requested) {
            return; // Nothing to connect to and nobody is asking for a list
        }
        s_scan_requested = false;
        WiFi.scanDelete();
        enter_phase(WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING ? PHASE_SCANNING : PHASE_WAITING);
        break;

    case PHASE_SCANNING: {
        int16_t found = WiFi.scanComplete();
        if (found == WIFI_SCAN_RUNNING) {
            if (now - s_phase_since_ms > PORTAL_CONNECT_TIMEOUT_MS) {
                enter_phase(PHASE_WAITING);
            }
            return;
        }
        collect_scan_results(found);
        s_scan_requested = false;
        if (connected) {
            enter_phase(PHASE_CONNECTED); // "Scan again" while online
        } else {
            build_attempt_queue();
            try_next_network();
        }
        break;
    }

    case PHASE_CONNECTING:
        if (connected) {
            s_saved[s_queue[s_queue_pos]].status = "connected";
            s_prefer_reduced = s_reduced_power;
            enter_phase(PHASE_CONNECTED);
        } else if (s_attempt_failed || now - s_phase_since_ms > PORTAL_CONNECT_TIMEOUT_MS) {
            if (s_attempt < PORTAL_CONNECT_ATTEMPTS) {
                s_attempt++;
                begin_attempt();
            } else {
                s_saved[s_queue[s_queue_pos]].status = failure_text(s_attempt_failed, s_fail_reason);
                irdbg_printf("Wi-Fi: %s %s", s_saved[s_queue[s_queue_pos]].ssid.c_str(),
                             s_saved[s_queue[s_queue_pos]].status.c_str());
                s_queue_pos++;
                try_next_network();
            }
        }
        break;

    case PHASE_CONNECTED:
        if (!connected) {
            // Lost the network: look for it, or another saved one, right away
            for (uint8_t i = 0; i < s_saved_count; i++) {
                if (s_saved[i].status == "connected") {
                    s_saved[i].status = "connection lost";
                }
            }
            enter_phase(PHASE_IDLE);
        } else if (s_scan_requested) {
            s_scan_requested = false;
            WiFi.scanDelete();
            if (WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING) {
                enter_phase(PHASE_SCANNING);
            }
        }
        break;

    case PHASE_WAITING:
        if (s_scan_requested || now - s_phase_since_ms >= PORTAL_RETRY_INTERVAL_MS) {
            enter_phase(PHASE_IDLE);
        }
        break;
    }
}

// ------------------------------------------------------------------------------
// Page Rendering (plain HTML for web browsers, WML for WAP browsers)
// ------------------------------------------------------------------------------

// Finds "name=" at the start of a form field (any letter case); returns the
// offset of its value in s_params, or -1
static int find_param(const char *name) {
    String key = String(name) + "=";
    int at = 0;
    while (at < (int)s_params.length()) {
        String candidate = s_params.substring(at, at + key.length());
        candidate.toLowerCase();
        if (candidate == key) {
            return at + key.length();
        }
        int next = s_params.indexOf('&', at);
        if (next < 0) {
            break;
        }
        at = next + 1;
    }
    return -1;
}

static String url_decode(const String &raw) {
    String value;
    for (size_t i = 0; i < raw.length(); i++) {
        char c = raw[i];
        if (c == '+') {
            value += ' ';
        } else if (c == '%' && i + 2 < raw.length() && isxdigit(raw[i + 1]) && isxdigit(raw[i + 2])) {
            char hex[3] = {raw[i + 1], raw[i + 2], '\0'};
            value += (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            value += c;
        }
    }
    return value;
}

// Value of a form field ("i=2&p=secret"), URL-decoded; empty if absent
static String request_arg(const char *name) {
    int start = find_param(name);
    if (start < 0) {
        return String();
    }
    int end = s_params.indexOf('&', start);
    return url_decode((end >= 0) ? s_params.substring(start, end) : s_params.substring(start));
}

// Value of the only field of a form. Browsers differ in how they send it, so it
// is also accepted under another name, or with no name at all ("?secret").
static String request_sole_field(const char *name) {
    if (find_param(name) >= 0) {
        return request_arg(name);
    }
    int end = s_params.indexOf('&');
    String first = (end >= 0) ? s_params.substring(0, end) : s_params;
    int equals = first.indexOf('=');
    return url_decode((equals >= 0) ? first.substring(equals + 1) : first);
}

// Returns false once the browser has gone away
static bool send_bytes(const char *data, size_t left) {
    while (left > 0) {
        int sent = send(s_client, data, left, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        left -= sent;
    }
    return true;
}

static bool send_all(const String &text) {
    return send_bytes(text.c_str(), text.length());
}

static void respond_bytes(const char *status, const char *content_type, const char *body, size_t length) {
    // Plain HTTP/1.0: one reply, exact length, then the connection is closed
    String headers = String("HTTP/1.0 ") + status + "\r\nContent-Type: " + content_type +
                     "\r\nContent-Length: " + String((unsigned)(length + (s_xiino_request.dataserver ? 16 : 0))) +
                     "\r\nPragma: no-cache\r\nConnection: close\r\n\r\n";
    if (!send_all(headers)) return;
    if (s_xiino_request.dataserver && !send_bytes(xiino_padding().data(), xiino_padding().size())) return;
    send_bytes(body, length);
}

static void respond(const char *status, const char *content_type, const String &body) {
    respond_bytes(status, content_type, body.c_str(), body.length());
}

class Page {
public:
    Page(const char *title) : wml_(s_wants_wml) {
        if (wml_) {
            out_ = "<?xml version=\"1.0\"?>\r\n"
                   "<!DOCTYPE wml PUBLIC \"-//WAPFORUM//DTD WML 1.1//EN\" "
                   "\"http://www.wapforum.org/DTD/wml_1.1.xml\">\r\n"
                   "<wml><card id=\"c\" title=\"";
            out_ += title;
            out_ += "\">\r\n";
        } else {
            // Upper-case HTML 3.2 with one element per line, for 1990s parsers.
            // Keep the full HTML / HEAD / BODY structure: without it Xiino
            // scrambles most of the page.
            out_ = "<HTML>\r\n<HEAD>\r\n<TITLE>";
            out_ += title;
            out_ += "</TITLE>\r\n</HEAD>\r\n<BODY>\r\n<H3>";
            out_ += title;
            out_ += "</H3>\r\n";
        }
    }

    // Text coming from outside (network names) must not break the markup
    static String escape(const String &text, bool wml) {
        String escaped;
        for (size_t i = 0; i < text.length(); i++) {
            char c = text[i];
            if (c == '&') escaped += "&amp;";
            else if (c == '<') escaped += "&lt;";
            else if (c == '>') escaped += "&gt;";
            else if (c == '"') escaped += "&quot;";
            else if (c == '$' && wml) escaped += "$$"; // '$' starts a WML variable
            else escaped += c;
        }
        return escaped;
    }

    void line(const String &text) {
        out_ += wml_ ? "<p>" : "<P>";
        out_ += escape(text, wml_);
        out_ += wml_ ? "</p>\r\n" : "</P>\r\n";
    }

    void heading(const String &text) {
        out_ += wml_ ? "<p><b>" : "<H4>";
        out_ += escape(text,wml_);
        out_ += wml_ ? "</b></p>\r\n" : "</H4>\r\n";
    }

    void link(const String &href, const String &label) {
        out_ += wml_ ? "<p><a href=\"" : "<P><A HREF=\"";
        out_ += href;
        out_ += "\">";
        out_ += escape(label, wml_);
        out_ += wml_ ? "</a></p>\r\n" : "</A></P>\r\n";
    }

    // Form with a password field, optionally preceded by a network name field.
    // Anything else the server needs is part of the action address, so nothing
    // depends on hidden fields.
    void password_form(const String &action, bool ask_name, const char *button) {
        if (wml_) {
            if (ask_name) {
                out_ += "<p>Network name: <input name=\"s\"/></p>\r\n";
            }
            out_ += "<p>Password: <input name=\"p\"/></p>\r\n<p><anchor>";
            out_ += button;
            out_ += "<go href=\"" + action + "\" method=\"get\">";
            if (ask_name) {
                out_ += "<postfield name=\"s\" value=\"$(s)\"/>";
            }
            out_ += "<postfield name=\"p\" value=\"$(p)\"/></go></anchor></p>\r\n";
        } else {
            out_ += "<FORM ACTION=\"" + action + "\" METHOD=\"GET\">\r\n";
            if (ask_name) {
                out_ += "<P>Network name: <INPUT TYPE=\"TEXT\" NAME=\"s\"></P>\r\n";
            }
            out_ += "<P>Password: <INPUT TYPE=\"TEXT\" NAME=\"p\"></P>\r\n<P><INPUT TYPE=\"SUBMIT\" VALUE=\"";
            out_ += button;
            out_ += "\"></P>\r\n</FORM>\r\n";
        }
    }

    struct Field {
        const char *name;  // Sent back as name=value
        const char *label;
        String value;      // Shown in the field, to be edited
    };

    // Form with several text fields. `post` keeps the fields out of the address
    // (HTML only; for the PC pages)
    void form(const String &action, const Field *fields, size_t count, const char *button, bool post = false) {
        if (wml_) {
            for (size_t i = 0; i < count; i++) {
                out_ += String("<p>") + fields[i].label + ": <input name=\"" + fields[i].name + "\" value=\"" +
                        escape(fields[i].value, true) + "\"/></p>\r\n";
            }
            out_ += String("<p><anchor>") + button + "<go href=\"" + action + "\" method=\"" + (post ? "post" : "get") + "\">";
            for (size_t i = 0; i < count; i++) {
                out_ += String("<postfield name=\"") + fields[i].name + "\" value=\"$(" + fields[i].name + ")\"/>";
            }
            out_ += "</go></anchor></p>\r\n";
        } else {
            out_ += "<FORM ACTION=\"" + action + "\" METHOD=\"" + (post ? "POST" : "GET") + "\">\r\n";
            for (size_t i = 0; i < count; i++) {
                out_ += String("<P>") + fields[i].label + ": <INPUT TYPE=\"TEXT\" NAME=\"" + fields[i].name + "\" VALUE=\"" +
                        escape(fields[i].value, false) + "\"></P>\r\n";
            }
            out_ += String("<P><INPUT TYPE=\"SUBMIT\" VALUE=\"") + button + "\"></P>\r\n</FORM>\r\n";
        }
    }

    // A link that opens in a new window (HTML only; for the PC pages)
    void link_new_window(const String &href, const String &label) {
        out_ += "<P><A HREF=\"" + escape(href, false) + "\" TARGET=\"_blank\">" + escape(label, false) + "</A></P>\r\n";
    }

    void send() {
        out_ += wml_ ? "</card></wml>\r\n" : "</BODY>\r\n</HTML>\r\n";
        respond("200 OK", wml_ ? "text/vnd.wap.wml" : "text/html", out_);
    }

private:
    bool wml_;
    String out_;
};

// ------------------------------------------------------------------------------
// Portal Pages
// ------------------------------------------------------------------------------

// A request for another site while there is no Wi-Fi to fetch it over
static void handle_other_site() {
    Page page("Not Connected");
    page.line("'" + s_request_host + "' cannot be opened: Wi-Fi is not connected.");
    page.link("http://" PORTAL_DOMAIN "/", "Wi-Fi status");
    page.link("http://" PORTAL_DOMAIN "/nets", "Add Wi-Fi network");
    page.send();
}

static void handle_status() {
    Page page("IrDA Wi-Fi Gateway");

    // What the connection manager is doing right now
    if (s_phase == PHASE_CONNECTED) {
        page.line("Wi-Fi: connected to " + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)");
        page.line("Address: " + WiFi.localIP().toString());
        page.line("Internet access for the Palm is on.");
    } else if (s_saved_count == 0) {
        page.line("Wi-Fi: no network saved yet.");
    } else if (s_phase == PHASE_CONNECTING) {
        page.line("Wi-Fi: connecting to " + s_saved[s_queue[s_queue_pos]].ssid + " (try " + String(s_attempt) +
                  " of " + String(PORTAL_CONNECT_ATTEMPTS) + ")...");
    } else if (s_phase == PHASE_WAITING) {
        uint32_t waited = millis() - s_phase_since_ms;
        uint32_t left = (waited < PORTAL_RETRY_INTERVAL_MS) ? (PORTAL_RETRY_INTERVAL_MS - waited) / 1000 : 0;
        page.line("Wi-Fi: not connected. Trying again in " + String((unsigned)left) + " s.");
    } else {
        page.line("Wi-Fi: looking for saved networks...");
    }
    if (s_reduced_power && s_phase == PHASE_CONNECTED) {
        page.line("(Using reduced transmit power.)");
    }

    // Saved networks with the outcome of the last attempt; each leads to its details
    for (uint8_t i = 0; i < s_saved_count; i++) {
        String text = "Saved: " + s_saved[i].ssid;
        if (s_saved[i].status.length() > 0) {
            text += " - " + s_saved[i].status;
        }
        page.link("/net/" + String(i), text);
    }

    if (s_proxy_all_sites) {
        page.line("Web proxy: on. Direct browser pages omit pictures. Xiino DataServer includes converted pictures.");
        page.link("/proxy/0", "Turn web proxy off");
    } else {
        page.line("Web proxy: off. The Palm connects to sites itself: plain http only, but other programs (mail, telnet) work.");
        page.link("/proxy/1", "Turn web proxy on");
    }
    page.line("Xiino DataServer: " XIINO_DATASERVER_HOST " (OpenXiino).");
    page.link("http://about/", "About OpenXiino (Xiino DataServer mode)");

    const IrMailSettings &mail = irmail_settings();
    page.link("/mail", mail.address.length() == 0 ? String("Mail: not set up")
                                                  : String(irmail_enabled() ? "Mail: on, " : "Mail: off, ") + mail.address);

    page.link("/calendar", ircalendar_ready() ? "Google Calendar: on" : "Google Calendar: off");
    page.link("/contacts", ircontacts_ready() ? "Address Book: on" : "Address Book: off");
    page.link("/tasks", irtasks_ready() ? "Google Tasks: on" : "Google Tasks: off");
    page.link("/memos",irnotes_ready(false) ? "Memo Pad: on" : "Memo Pad: off");
    page.link("/notes",irnotes_ready(true) ? "Note Pad: on" : "Note Pad: off");
    page.link("/expenses",irexpenses_ready() ? "Expenses: on" : "Expenses: off");
    page.link("/drive", irdrive_ready() ? "Drive backup: on" : "Drive backup: off");
    page.link("/hotsync","HotSync settings (user name)");

    page.link("/", "Refresh");
    page.link("/nets", "Add Wi-Fi network");
    page.link("/test", "Diagnostics");
    page.line("Palm: " PPP_PALM_IP " over IrDA, gateway " PPP_GATEWAY_IP);
    page.send();
}

// Details of a saved network. The stored password is shown in full, so that a
// slip while writing it can be spotted; the portal is reachable from the Palm only.
static void handle_saved_network(int index) {
    Page page("Saved Network");
    const SavedNetwork &net = s_saved[index];

    page.line("Network: " + net.ssid);
    page.line("Last attempt: " + (net.status.length() > 0 ? net.status : String("none yet")));
    if (net.password.length() > 0) {
        page.line("Stored password: " + net.password);
        page.line("(" + String((unsigned)net.password.length()) + " characters, upper and lower case matter)");
    } else {
        page.line("No password stored (open network).");
    }
    page.line("To change it, enter the password again:");
    page.password_form("/pass/" + String(index), false, "Save");
    page.link("/forget/" + String(index), "Forget this network");
    page.link("/", "Back");
    page.send();
}

static void show_saved_confirmation(Page &page, const String &ssid, const String &password) {
    // Graffiti slips are easy: show how much was understood
    page.line("Saved " + ssid + " (password: " + String((unsigned)password.length()) + " characters).");
    if (password.length() == 0) {
        // Either an open network, or the browser sent the form in a way not understood
        page.line("No password was received. The browser asked for: " + s_target +
                  (s_params.length() > 0 ? " with " + s_params : String("")));
    }
    page.line("Connecting takes up to half a minute.");
    page.link("/", "Check status");
}

// index: position in the saved list
static void handle_change_password(int index) {
    Page page("Saved Network");

    String ssid = s_saved[index].ssid;
    String password = request_sole_field("p");
    add_saved_network(ssid, password);
    s_preferred = ssid;
    restart_wifi_manager();
    show_saved_confirmation(page, ssid, password);
    page.send();
}

static const char *security_name(uint8_t security) {
    switch (security) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "enterprise login, not supported";
    default:                        return "other security";
    }
}

static void handle_networks(bool rescan) {
    Page page("Wi-Fi Networks");

    if (rescan || (s_network_count == 0 && s_phase != PHASE_SCANNING && !s_scan_requested)) {
        request_scan();
    }
    if (s_phase == PHASE_SCANNING || s_scan_requested) {
        page.line("Scanning, this takes a few seconds...");
        page.link("/nets", "Show results");
    } else {
        for (uint8_t i = 0; i < s_network_count; i++) {
            const ScannedNetwork &net = s_networks[i];
            String label = String(net.ssid) + " (" + String(net.rssi) + " dBm, " + security_name(net.security) + ")";
            page.link("/join/" + String(i), label);
        }
        if (s_network_count == 0) {
            page.line("No networks found.");
        }
        page.link("/scan", "Scan again");
    }
    page.link("/join", "Other network (type the name)");
    page.link("/", "Back");
    page.send();
}

static String diagnostic_bytes(size_t bytes) {
    return String((unsigned long)bytes)+" B ("+String(bytes/1024.0,1)+" KiB)";
}

static const char *reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software reset";
    case ESP_RST_PANIC: return "crash/panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "power brownout";
    case ESP_RST_SDIO: return "SDIO reset";
    default: return "unknown";
    }
}

static void show_esp_status(Page &page,const multi_heap_info_t &heap,uint64_t uptime_seconds) {
    page.heading("ESP status");
    page.line(String("Chip: ")+ESP.getChipModel()+", revision "+String(ESP.getChipRevision())+
              ", cores: "+String(ESP.getChipCores()));
    page.line("CPU clock: "+String(getCpuFrequencyMhz())+" MHz; tasks: "+String((unsigned)uxTaskGetNumberOfTasks()));
    char uptime[64];
    snprintf(uptime,sizeof(uptime),"%lu days %02u:%02u:%02u",(unsigned long)(uptime_seconds/86400),
             (unsigned)(uptime_seconds/3600%24),(unsigned)(uptime_seconds/60%60),(unsigned)(uptime_seconds%60));
    page.line(String("Uptime: ")+uptime);
    page.line(String("Last reset: ")+reset_reason_name(esp_reset_reason()));

    const size_t total=heap.total_allocated_bytes+heap.total_free_bytes;
    page.line("Internal heap used: "+diagnostic_bytes(heap.total_allocated_bytes)+
              " ("+String(total ? heap.total_allocated_bytes*100.0/total : 0.0,1)+"%)");
    page.line("Heap total: "+diagnostic_bytes(total));
    page.line("Heap free: "+diagnostic_bytes(heap.total_free_bytes));
    page.line("Lowest free heap since boot: "+diagnostic_bytes(heap.minimum_free_bytes));
    page.line("Largest allocatable heap block: "+diagnostic_bytes(heap.largest_free_block));
    page.line("External requests since boot: Xiino DataServer "+String((unsigned long)s_dataserver_pages)+
              ", direct proxy "+String((unsigned long)s_direct_pages));
    page.line(String(irxiino_diagnostics().c_str()));
    std::string interrupted=irxiino_previous_diagnostics();
    if (!interrupted.empty()) page.line(String(interrupted.c_str()));
    page.line(String(xiino_image_diagnostics().c_str()));
    if (ESP.getPsramSize()) {
        page.line("PSRAM total: "+diagnostic_bytes(ESP.getPsramSize())+"; free: "+diagnostic_bytes(ESP.getFreePsram()));
    } else page.line("PSRAM: not available");

    page.line("Flash capacity: "+diagnostic_bytes(ESP.getFlashChipSize()));
    const esp_partition_t *app=esp_ota_get_running_partition();
    // Arduino verifies the flash image to obtain its length. Cache this once
    // per boot rather than repeating that work on every diagnostics refresh.
    static const uint32_t image_size=ESP.getSketchSize();
    if (app && image_size && image_size<=app->size) {
        page.line("Firmware image: "+diagnostic_bytes(image_size)+" ("+String(image_size*100.0/app->size,1)+"% of app partition)");
        page.line("Unused app partition space: "+diagnostic_bytes(app->size-image_size));
    } else page.line("Firmware image size: unavailable");
    page.line(String("ESP-IDF version: ")+ESP.getSdkVersion());

    nvs_stats_t storage={};
    esp_err_t result=nvs_get_stats(nullptr,&storage);
    if (result==ESP_OK) {
        page.line("Settings storage (NVS): "+String((unsigned)storage.used_entries)+" used, "+
                  String((unsigned)storage.free_entries)+" free, "+String((unsigned)storage.total_entries)+" total entries");
        page.line("NVS namespaces: "+String((unsigned)storage.namespace_count));
    } else page.line(String("Settings storage (NVS): unavailable (")+esp_err_to_name(result)+")");

    page.heading("Wi-Fi and Palm link");
    if (WiFi.status()==WL_CONNECTED) {
        page.line("Wi-Fi: "+WiFi.SSID()+"; signal: "+String(WiFi.RSSI())+" dBm; channel: "+String(WiFi.channel()));
        page.line("Adapter IP: "+WiFi.localIP().toString()+"; gateway: "+WiFi.gatewayIP().toString());
    } else page.line("Wi-Fi: not connected");
    uint32_t palm_ip=irppp_palm_ip();
    page.line(palm_ip ? "Palm PPP: connected, "+IPAddress(palm_ip).toString() : String("Palm PPP: disconnected"));
    page.line("PPP output queued: "+String((unsigned)irppp_pending_output())+" B");
    page.line("PPP bytes dropped since boot: "+String((unsigned long)irppp_dropped_bytes())+" B");
}

static void show_sync_diagnostics(Page &page,bool previous) {
    SyncDiagnosticState state;
    if (!irsyncdiag_snapshot(state,previous)) return;
    page.heading(previous ? "Previous HotSync diagnostics (retained across reboot)" : "HotSync diagnostics since boot");
    page.line(String("State: ")+(state.active ? (previous ? "interrupted" : "in progress") : state.failed ? "failed" : "idle/completed"));
    if (state.service[0]) page.line(String("Last service: ")+state.service);
    if (state.outcome[0]) page.line(String("Result: ")+state.outcome);
    if (state.allocation_size) {
        page.line("Failed allocation: "+diagnostic_bytes(state.allocation_size)+"; function: "+state.allocation_function+
                  "; capabilities: "+String(state.allocation_caps,HEX));
    }
    for (unsigned n=0;n<state.count;n++) {
        const auto &e=state.events[(state.next+8-state.count+n)%8];
        page.line(String(e.uptime_ms)+" ms: "+e.stage+"; free heap "+String(e.free_heap)+
                  " B; largest block "+String(e.largest_block)+" B; task stack headroom "+String(e.stack_free)+" B");
    }
}

static void handle_diagnostics() {
    // Take the internal byte-addressable heap snapshot before allocating the
    // page, so rendering its status text does not inflate the displayed use.
    multi_heap_info_t heap={};
    heap_caps_get_info(&heap,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    const uint64_t uptime_seconds=(uint64_t)(esp_timer_get_time()/1000000);
    Page page("Diagnostics");
    page.link("/test","Refresh readings");
    show_esp_status(page,heap,uptime_seconds);
    show_sync_diagnostics(page,false);
    show_sync_diagnostics(page,true);

    // Which names the Palm asked for and what came of it, newest first
    page.heading("Name lookups");
    page.line("DNS server of the Wi-Fi network: " +
              (WiFi.status() == WL_CONNECTED ? WiFi.dnsIP().toString() : String("(not connected)")));
    page.line("Recent name lookups:");
    for (uint8_t i = 1; i <= DNS_LOG_ENTRIES; i++) {
        DnsLogEntry entry;
        portENTER_CRITICAL(&s_dns_log_lock);
        entry = s_dns_log[(s_dns_log_next + DNS_LOG_ENTRIES - i) % DNS_LOG_ENTRIES];
        portEXIT_CRITICAL(&s_dns_log_lock);
        if (entry.name[0] != '\0') {
            page.line(String(entry.name) + " - " + entry.result);
        }
    }

    page.link("/echo?a=1&b=two", "Show what the browser sends");
    page.link("/", "Back");
    page.send();
}

// Shows the request exactly as the browser sent it, and offers a form to try:
// how links with "?" and form fields arrive differs between old browsers
static void handle_echo() {
    Page page("Browser Request");

    page.line("Fields understood: " + (s_params.length() > 0 ? s_params : String("(none)")));
    page.line("Received:");
    int from = 0;
    for (int shown = 0; from < (int)s_request_text.length() && shown < 16; shown++) {
        int to = s_request_text.indexOf('\n', from);
        String text = (to >= 0) ? s_request_text.substring(from, to) : s_request_text.substring(from);
        text.trim();
        if (text.length() > 0) {
            page.line(text);
        }
        if (to < 0) {
            break;
        }
        from = to + 1;
    }
    page.line("Form test: type anything and send it.");
    page.password_form("/echo", true, "Send");
    page.link("/test", "Back");
    page.send();
}

// index: position in the scanned list, or -1 to type the network name
static void handle_join(int index) {
    Page page("Join Network");

    if (index >= 0) {
        page.line(String("Network: ") + s_networks[index].ssid + " (" + security_name(s_networks[index].security) + ")");
        if (s_networks[index].security == WIFI_AUTH_OPEN) {
            page.line("Open network: leave the password empty.");
        }
    }
    page.password_form((index < 0) ? String("/save") : "/save/" + String(index), index < 0, "Join");
    page.link("/nets", "Back");
    page.send();
}

// index: position in the scanned list, or -1 when the name was typed in
static void handle_save(int index) {
    Page page("Join Network");

    String ssid = (index >= 0) ? String(s_networks[index].ssid) : request_arg("s");
    String password = (index >= 0) ? request_sole_field("p") : request_arg("p");

    if (ssid.length() == 0 || ssid.length() > 32) {
        page.line("No network name given.");
        page.link("/nets", "Back");
    } else {
        add_saved_network(ssid, password);
        s_preferred = ssid;
        restart_wifi_manager();
        show_saved_confirmation(page, ssid, password);
    }
    page.send();
}

// index: position in the saved list
static void handle_forget(int index) {
    Page page("IrDA Wi-Fi Gateway");

    page.line("Forgot " + s_saved[index].ssid + ".");
    remove_saved_network(index);
    store_saved_networks();
    restart_wifi_manager();
    page.link("/", "Back");
    page.send();
}

// ------------------------------------------------------------------------------
// Mail Pages
// ------------------------------------------------------------------------------
// Settings of the mail conduit (irmail). A HotSync cannot run while these pages
// are open: the browser and HotSync need the same infrared link.

// Google Tasks settings use the same small HTML/WML forms as Mail.
static void handle_tasks(bool save = false) {
    Page page("Google Tasks");
    IrTasksSettings settings = irtasks_settings();
    if (save) {
        String list = request_arg("l"); list.trim();
        settings.list_id = list.length() ? list : String("@default");
        page.line(irtasks_store_settings(settings) ? "Saved." : "Could not save. The list cannot change after tasks have been synced.");
        settings = irtasks_settings();
    }
    const IrMailSettings &google = irmail_settings();
    if (!google.google()) page.line("Sign in with Google first.");
    else if (!google.google_tasks()) page.line("This Google sign-in only allows mail. Sign in again to allow Tasks access.");
    else page.line("Using the Google sign-in for " + google.address + ".");
    page.link("/tasksgoogle", google.google() ? "Sign in with Google again (from a PC)" : "Sign in with Google (from a PC)");
    page.line(irtasks_ready() ? "Tasks sync: on. Disconnect Network, then tap HotSync with no desktop waiting." : "Tasks sync: off.");
    page.line("Syncs titles, notes, due dates, completion and deletions with the Palm To Do List. Private and archived tasks stay on the Palm.");
    const Page::Field fields[] = {{"l", "Task list ID", settings.list_id}};
    page.form("/taskssave", fields, 1, "Save", true);
    page.link(settings.enabled ? "/taskson/0" : "/taskson/1", settings.enabled ? "Turn Tasks off" : "Turn Tasks on");
    page.link("/taskstest", "Check Google access");
    page.link(settings.cyrillic ? "/taskstext/0" : "/taskstext/1", settings.cyrillic ? "Text: Cyrillic. Switch to Western" : "Text: Western. Switch to Cyrillic");
    String result = irtasks_last_result();
    if (result.length()) page.line("Last sync: " + result);
    page.link("/", "Back"); page.send();
}

static void handle_calendar(bool save=false) {
    Page page("Google Calendar");
    IrCalendarSettings settings=ircalendar_settings();
    if (save) {
        String id=request_arg("c"),past=request_arg("p"),future=request_arg("f");
        id.trim(); past.trim(); future.trim();
        bool valid=past.length() && future.length() && past.length()<=4 && future.length()<=4;
        for (unsigned i=0;i<past.length();i++) valid=valid && isDigit(past[i]);
        for (unsigned i=0;i<future.length();i++) valid=valid && isDigit(future[i]);
        settings.calendar_id=id.length() ? id : String("primary");
        settings.past_days=past.toInt(); settings.future_days=future.toInt();
        page.line(valid && ircalendar_store_settings(settings) ? "Saved." :
            "Could not save. Use 0-3650 past days and 1-3650 future days. A linked calendar cannot be changed.");
        settings=ircalendar_settings();
    }
    const IrMailSettings &google=irmail_settings();
    if (!google.google()) page.line("Sign in with Google first.");
    else if (!google.google_scope(GOOGLE_CALENDAR_SCOPE)) page.line("Sign in again to allow Calendar access.");
    else page.line("Using the Google sign-in for "+google.address+".");
    page.line(ircalendar_ready() ? "Calendar sync: on. Disconnect Network, then tap HotSync with no desktop waiting." : "Calendar sync: off.");
    page.line("Syncs Date Book appointments, notes, alarms and supported repeating events. Private and archived appointments stay on the Palm.");
    page.line("Times use the Google calendar's time zone. Use primary or the calendar ID from Google Calendar settings.");
    const Page::Field fields[]={{"c","Calendar ID",settings.calendar_id},{"p","Days before today",String(settings.past_days)},
                               {"f","Days after today",String(settings.future_days)}};
    page.form("/calendarsave",fields,3,"Save",true);
    page.link(settings.enabled ? "/calendaron/0" : "/calendaron/1",settings.enabled ? "Turn Calendar off" : "Turn Calendar on");
    page.link("/calendarcheck","Check Calendar access");
    page.link("/calendargoogle","Sign in with Google (from a PC)");
    page.link(settings.cyrillic ? "/calendartext/0" : "/calendartext/1",
        settings.cyrillic ? "Text: Cyrillic. Switch to Western" : "Text: Western. Switch to Cyrillic");
    String last=ircalendar_last_result();
    if (last.length()) page.line("Last sync: "+last);
    page.link("/","Back"); page.send();
}

static void handle_hotsync(bool save=false) {
    Page page("HotSync Settings"); auto settings=irhotsync_settings();
    if (save) {
        settings.username=request_arg("u"); settings.username.trim(); String problem;
        page.line(irhotsync_store_settings(settings,problem) ? "Saved. The name will be applied at the next standalone HotSync if the Palm has no name." : problem);
        settings=irhotsync_settings();
    }
    page.line("One HotSync user name for Memo Pad, Note Pad, Expenses and Drive backup. Enter the name here, disconnect Network, then tap HotSync with no desktop waiting.");
    page.line("An existing Palm name is used as-is. The Palm's numeric user identity and existing sync links are preserved. Use the original name when recovering existing backups.");
    const Page::Field fields[]={{"u","HotSync user name (for an unnamed Palm)",settings.username}};
    page.form("/hotsyncsave",fields,1,"Save",true);
    page.line("Up to 40 Palm characters. Leave empty to keep automatic setup off.");
    page.link(settings.cyrillic ? "/hotsynctext/0" : "/hotsynctext/1",settings.cyrillic ? "Name text: Cyrillic. Switch to Western" : "Name text: Western. Switch to Cyrillic");
    String last=irhotsync_last_result(); if (last.length()) page.line("Last HotSync: "+last);
    page.link("/","Back"); page.send();
}

static void handle_notes(bool drawing) {
    Page page(drawing ? "Note Pad / Google Drive" : "Memo Pad / Google Docs");
    const auto &settings=irnotes_settings(drawing);
    String base=drawing ? "/notes" : "/memos";
    page.line(irnotes_ready(drawing) ? "Sync is on. Disconnect Network, then tap HotSync with no desktop waiting." : "Sync is off. Allow the required Google permissions, then enable it.");
    page.line("Folder: "+irnotes_folder(drawing));
    page.link("/hotsync","HotSync user settings");
    page.line(drawing ? "Two-way sync of standard Palm OS 4 Note Pad drawings and PNG files. Titles are file names. Scrolling canvases are supported. PNG imports up to 160 pixels wide are converted to black and white; Palm alarms and categories stay on Palm." :
        "Two-way sync of classic Memo Pad text and Google Docs. One document per memo; the full memo is its body. Private and archived records are excluded.");
    page.line("Deleting a linked note on either side deletes its counterpart. Google files are moved to Trash. Moving a file out of this folder pauses its sync.");
    page.link(base+"on/"+(settings.enabled ? "0" : "1"),settings.enabled ? "Turn sync off" : "Turn sync on");
    page.link(base+"check","Check Google access");
    page.link(base+"google","Sign in with Google (from a PC)");
    page.link(base+"text/"+(settings.cyrillic ? "0" : "1"),settings.cyrillic ? "Text: Cyrillic. Switch to Western" : "Text: Western. Switch to Cyrillic");
    const char *policies[]={"Stop for review","Prefer Palm edits","Prefer Google edits"};
    page.line(String("Conflicts: ")+policies[settings.conflicts<=2 ? settings.conflicts : 0]);
    for (unsigned i=0;i<3;i++) if (i!=settings.conflicts) page.link(base+"conflict/"+String(i),policies[i]);
    if (!drawing) {
        page.line("If a Google document creation is unconfirmed, check for it in Drive and retry HotSync first. An explicit retry before Google finishes propagating can create a duplicate.");
        page.link(base+"retry","Retry unconfirmed creations");
    }
    String last=irnotes_last_result(drawing); if (last.length()) page.line("Last sync: "+last);
    page.link("/","Back"); page.send();
}

static void handle_expenses() {
    Page page("Expenses / Google Sheets"); const auto &s=irexpenses_settings();
    page.line(irexpenses_ready() ? "Sync is on. Disconnect Network, then tap HotSync with no desktop waiting." : "Sync is off. Allow Drive and Sheets access, then enable it.");
    page.line("Two-way sync of classic Palm Expense records, including date, type, payment, currency, amount, vendor, city, attendees, note and category.");
    page.line("First HotSync creates Palm/<HotSync user name>/Expenses in Google Drive. Open the spreadsheet from a PC to edit or add expenses.");
    String url=irexpenses_sheet_url(); if (url.length()) page.link_new_window(url,"Open Expenses spreadsheet (from a PC)");
    page.link("/hotsync","HotSync user settings");
    page.line("Leave Record ID empty for new rows; keep linked IDs and headers unchanged. Use YYYY-MM-DD dates and plain-text amounts. Use currencies such as NZD ($), USD ($) or EUR; existing Palm currency codes are also accepted. Category is its 0-15 slot.");
    page.line("Set Deleted to TRUE to remove an expense from Palm; deleting on Palm marks its linked row TRUE. Keep other expense cells as plain text. Private and archived Palm records are excluded.");
    page.line("Finish editing the spreadsheet before HotSync. Stop for review is the default when the same field changes on both sides.");
    page.link(s.enabled ? "/expenseson/0" : "/expenseson/1",s.enabled ? "Turn Expenses off" : "Turn Expenses on");
    page.link("/expensescheck","Check Google access"); page.link("/expensesgoogle","Sign in with Google (from a PC)");
    page.link(s.cyrillic ? "/expensestext/0" : "/expensestext/1",s.cyrillic ? "Text: Cyrillic. Switch to Western" : "Text: Western. Switch to Cyrillic");
    const char *policies[]={"Stop for review","Prefer Palm edits","Prefer Google edits"};
    page.line(String("Conflicts: ")+policies[s.conflicts<=2 ? s.conflicts : 0]);
    for (unsigned i=0;i<3;i++) if (i!=s.conflicts) page.link("/expensesconflict/"+String(i),policies[i]);
    page.line("If a creation is unconfirmed, check Drive and the spreadsheet, then retry HotSync first. Explicit retry can duplicate an operation Google has not yet made visible.");
    page.link("/expensesretry","Retry unconfirmed creations");
    String last=irexpenses_last_result(); if (last.length()) page.line("Last sync: "+last);
    page.link("/","Back"); page.send();
}

static void handle_drive() {
    Page page("Google Drive Backup");
    auto settings=irdrive_settings();
    const auto &google=irmail_settings();
    if (!google.google_scope(GOOGLE_DRIVE_SCOPE)) page.line("Sign in with Google and allow Drive access first.");
    page.line("Folder: "+irdrive_folder());
    page.line(irdrive_ready() ? "Drive backup: on. Disconnect Network, then tap HotSync with no desktop waiting." : "Drive backup: off.");
    page.line("Backs up all RAM databases and applications as PDB/PRC files, including private records and preferences. ROM and expansion cards are excluded.");
    page.line("Newest wins: a newer Drive file installs/replaces its Palm database; newer Palm data updates Drive. Equal times keep the Palm copy. Replaced Palm data is saved in Recovery first.");
    page.link(settings.enabled ? "/driveon/0" : "/driveon/1",settings.enabled ? "Turn Drive backup off" : "Turn Drive backup on");
    page.link("/drivecheck","Check Drive access");
    page.link("/drivegoogle","Sign in with Google (from a PC)");
    page.line(settings.restore_saved ? "Saved backups will restore missing databases at the next HotSync." : "To recover missing databases from existing backups, request a one-time restore.");
    page.link("/driverestore","Restore saved backups once");
    String interrupted=irdrive_interrupted_name();
    if (interrupted.length()) {
        page.line("Interrupted installation: "+interrupted+". Retry removes this incomplete Palm database and reinstalls it from Drive.");
        page.link("/driveretry","Retry interrupted install");
    }
    page.line("The Palm HotSync user name selects the folder. Set a name for an unnamed Palm in the shared HotSync settings.");
    page.link("/hotsync","HotSync user settings");
    page.link(settings.cyrillic ? "/drivetext/0" : "/drivetext/1",settings.cyrillic ? "Text: Cyrillic. Switch to Western" : "Text: Western. Switch to Cyrillic");
    String last=irdrive_last_result(); if (last.length()) page.line("Last sync: "+last);
    page.link("/","Back"); page.send();
}

static void handle_contacts() {
    Page page("Address Book / Google Contacts");
    auto settings=ircontacts_settings(); const auto &google=irmail_settings();
    if (!google.google()) page.line("Sign in with Google first.");
    else if (!google.google_scope(GOOGLE_CONTACTS_SCOPE)) page.line("Sign in again to allow Contacts access.");
    else if (!google.google_scope(GOOGLE_PROFILE_SCOPE)) page.line("Sign in again and allow basic profile access to identify the Contacts account.");
    else page.line("Using the Google sign-in for "+google.address+".");
    page.line(ircontacts_ready() ? "Contacts sync: on. Disconnect Network, then tap HotSync with no desktop waiting." : "Contacts sync: off.");
    page.line("Syncs names, company/title, phones, email, address, notes and four Palm custom fields. Private and archived contacts stay on the Palm.");
    page.line("Up to five phone/email values fit on the Palm; other Google values are preserved. Custom fields use Palm Custom 1 through 4 on Google.");
    page.link(settings.enabled ? "/contactson/0" : "/contactson/1",settings.enabled ? "Turn Contacts off" : "Turn Contacts on");
    page.link("/contactscheck","Check Contacts access");
    page.link("/contactsgoogle","Sign in with Google (from a PC)");
    page.link(settings.cyrillic ? "/contactstext/0" : "/contactstext/1",
        settings.cyrillic ? "Text: Cyrillic. Switch to Western" : "Text: Western. Switch to Cyrillic");
    if (ircontacts_has_pending()) {
        page.line("An unconfirmed creation is waiting. Google may take several minutes to show a new contact. Sync again first; retry only if it was never created. Retrying can duplicate it.");
        page.link("/contactsretry","Retry unconfirmed creations");
    }
    String last=ircontacts_last_result(); if (last.length()) page.line("Last sync: "+last);
    page.link("/","Back"); page.send();
}

static void handle_mail() {
    Page page("Mail");
    const IrMailSettings &mail = irmail_settings();

    if (mail.address.length() == 0) {
        page.line("No mail account entered yet.");
    } else {
        page.line("Account: " + mail.address);
        if (mail.google()) {
            page.line("Login: signed in with Google");
        } else {
            page.line(mail.password.length() > 0 ? "Password: " + String((unsigned)mail.password.length()) + " characters stored"
                                                : String("Password: none stored"));
        }
    }
    page.link("/mailgoogle", mail.google() ? "Sign in with Google again (from a PC)" : "Gmail: sign in with Google (from a PC)");
    if (mail.google()) {
        page.link("/mailgoogleoff", "Sign out of Google");
    }
    page.link("/mailacct", mail.address.length() == 0 ? "Enter the account (with a password)" : "Change the account");

    if (irmail_enabled()) {
        page.line("Mail sync: on. Tap HotSync on the Palm while no desktop program is waiting for it: "
                  "the Outbox is sent and new messages arrive in the Inbox.");
        page.link("/mailon/0", "Turn mail sync off");
    } else if (mail.address.length() > 0 && (mail.password.length() > 0 || mail.google())) {
        page.line("Mail sync: off.");
        page.link("/mailon/1", "Turn mail sync on");
    }
    String last = irmail_last_result();
    if (last.length() > 0) {
        page.line("Last sync: " + last);
    }

    page.link("/mailtest", "Check the login now");
    page.link("/mailsrv", "Servers: " + mail.imap_host + ", " + mail.smtp_host);
    page.link(mail.cyrillic ? "/mailtext/0" : "/mailtext/1",
              mail.cyrillic ? "Text: Cyrillic (Windows-1251). Switch to Western" : "Text: Western. Switch to Cyrillic");
    page.link("/mailagain", "Fetch the newest messages again");
    page.link("/", "Back");
    page.send();
}

static void handle_mail_account() {
    Page page("Mail Account");
    const IrMailSettings &mail = irmail_settings();

    page.line("With Gmail, signing in with Google (from a PC) is easier. Otherwise the password is an app password: "
              "Google account, Security, 2-Step Verification, App passwords.");
    if (mail.password.length() > 0) {
        page.line("Leave the password empty to keep the stored one.");
    }
    const Page::Field fields[] = {{"a", "E-mail address", mail.address}, {"p", "Password", ""}, {"n", "Your name", mail.name}};
    page.form("/mailsave", fields, 3, "Save");
    page.link("/mail", "Back");
    page.send();
}

static void handle_mail_account_save() {
    Page page("Mail Account");
    IrMailSettings mail = irmail_settings();

    String address = request_arg("a");
    String password = request_arg("p");
    address.trim();
    if (address.indexOf('@') < 1 || address.indexOf(' ') >= 0) {
        page.line("'" + address + "' is not an e-mail address. The browser asked for: " + s_target);
        page.link("/mailacct", "Back");
        page.send();
        return;
    }
    mail.address = address;
    if (password.length() > 0) {
        mail.password = password;
    }
    mail.name = request_arg("n");
    mail.name.trim();
    irmail_store_settings(mail);
    mail = irmail_settings(); // Another address ends a Google sign-in
    mail.enabled = mail.password.length() > 0 || mail.google();
    irmail_store_settings(mail);

    // Graffiti slips are easy: show how much was understood
    page.line("Saved " + mail.address + " (password: " + String((unsigned)mail.password.length()) + " characters).");
    page.line(mail.enabled ? "Mail sync is on." : "Mail sync stays off until a password is entered.");
    page.link("/mailtest", "Check the login now");
    page.link("/mail", "Back");
    page.send();
}

static void handle_mail_servers(bool save) {
    Page page("Mail Servers");
    IrMailSettings mail = irmail_settings();

    if (save) {
        mail.imap_host = request_arg("i");
        mail.smtp_host = request_arg("s");
        mail.login = request_arg("l");
        mail.imap_host.trim();
        mail.smtp_host.trim();
        mail.login.trim();
        if (mail.imap_host.length() == 0) {
            mail.imap_host = MAIL_DEFAULT_IMAP_SERVER;
        }
        if (mail.smtp_host.length() == 0) {
            mail.smtp_host = MAIL_DEFAULT_SMTP_SERVER;
        }
        irmail_store_settings(mail);
        page.line("Saved.");
    }
    page.line("Both servers are contacted with TLS, on ports " + String(MAIL_IMAP_PORT) + " and " + String(MAIL_SMTP_PORT) +
              " unless another is given as server:port.");
    const Page::Field fields[] = {{"i", "Receiving (IMAP)", mail.imap_host},
                                  {"s", "Sending (SMTP)", mail.smtp_host},
                                  {"l", "Login name, if not the address", mail.login}};
    page.form("/mailsrvsave", fields, 3, "Save");
    page.link("/mail", "Back");
    page.send();
}

// Logs in to both servers; the browser waits for the result
static void handle_mail_test() {
    Page page("Mail Login");
    const IrMailSettings &mail = irmail_settings();

    if (mail.address.length() == 0 || (mail.password.length() == 0 && !mail.google())) {
        page.line("Enter the account first.");
    } else {
        String receiving, sending;
        irmail_check_login(receiving, sending);
        page.line("Receiving (" + mail.imap_host + "): " + receiving);
        page.line("Sending (" + mail.smtp_host + "): " + sending);
    }
    page.link("/mail", "Back");
    page.send();
}

// The Palm cannot show Google's sign-in page: open the sign-in pages to a PC on
// the same Wi-Fi for a while, and say where to find them
static void handle_mail_google(bool tasks = false, bool calendar = false, bool contacts = false, bool drive = false, int notes = -1, bool expenses = false) {
    s_google_from_tasks = tasks;
    s_google_from_calendar = calendar;
    s_google_from_contacts = contacts;
    s_google_from_drive = drive;
    s_google_from_notes = notes;
    s_google_from_expenses = expenses;
    Page page("Sign in with Google");

    if (WiFi.status() != WL_CONNECTED) {
        page.line("Wi-Fi is not connected: the PC must reach the adapter over Wi-Fi.");
    } else {
        s_google_window_open = true;
        s_google_window_since = millis();
        page.line("On a PC on the same Wi-Fi (" + WiFi.SSID() + "), open within " +
                  String((unsigned)(GOOGLE_SETUP_WINDOW_MS / 60000)) + " minutes:");
        page.line("http://" + WiFi.localIP().toString() + "/google");
        page.line("You need your own Google OAuth client (client ID and secret): see docs/google/README.md in the project.");
    }
    page.link(expenses ? "/expenses" : notes>=0 ? (notes ? "/notes" : "/memos") : drive ? "/drive" : contacts ? "/contacts" : calendar ? "/calendar" : tasks ? "/tasks" : "/mail", "Back");
    page.send();
}

static void handle_mail_google_off() {
    Page page("Mail");
    irmail_google_sign_out();
    page.line("Signed out. The adapter no longer uses the Google sign-in.");
    page.line("To remove its access completely: Google account, Security, Your connections to third-party apps.");
    page.link("/mail", "Back");
    page.send();
}

// ------------------------------------------------------------------------------
// Google Sign-In Pages (for a PC on the Wi-Fi side, while opened from the Palm)
// ------------------------------------------------------------------------------

static void pc_google_form(Page &page, const String &address, const String &client_id) {
    const Page::Field fields[] = {{"a", "Google account address", address},
                                  {"i", "Client ID", client_id},
                                  {"s", "Client secret", ""}};
    page.form("/googlestart", fields, 3, "Continue", true);
}

static void handle_pc_google() {
    Page page("Sign in with Google");
    const IrMailSettings &mail = irmail_settings();

    if (mail.google()) {
        page.line("Signed in as " + mail.address + ". Signing in again replaces that.");
    }
    page.line("1. In the Google Cloud console, create an OAuth client of type \"Desktop app\" "
              "(docs/google/README.md in the project has the steps), and enter it here:");
    page.line("This sign-in requests Mail, Tasks, Contacts, Calendar, Drive, Docs, Sheets and Slides access together.");
    page.line("Basic profile access identifies the Google account for Contacts sync.");
    page.line("Drive backs up Palm databases and Note Pad drawings; Docs syncs Memo Pad text; Sheets syncs Expenses. Slides permission is prepared for future features.");
    page.line("Leave the client secret blank to keep the stored secret for this client ID.");
    pc_google_form(page, mail.address, mail.google_client_id);
    page.send();
}

static void handle_pc_google_start() {
    Page page("Sign in with Google");

    String address = request_arg("a");
    String client_id = request_arg("i");
    String client_secret = request_arg("s");
    address.trim();
    client_id.trim();
    client_secret.trim();
    const IrMailSettings &stored = irmail_settings();
    if (client_secret.length() == 0 && client_id == stored.google_client_id) client_secret = stored.google_client_secret;
    if (address.indexOf('@') < 1 || address.indexOf(' ') >= 0 || client_id.length() == 0 || client_secret.length() == 0) {
        page.line("Enter the Google account address, the client ID and the client secret.");
        pc_google_form(page, address, client_id);
        page.send();
        return;
    }

    String consent = irmail_google_begin(address, client_id, client_secret);
    page.line("2. Open Google's page, sign in as " + address + " and allow access. Google warns that the app is not "
              "verified: it is your own client, choose Continue.");
    page.link_new_window(consent, "Open Google's sign-in page");
    page.line("3. Google then sends the browser to http://127.0.0.1/..., which does not load. "
              "Copy the whole address from the address bar and paste it here:");
    const Page::Field fields[] = {{"u", "Address", ""}};
    page.form("/googledone", fields, 1, "Finish", true);
    page.send();
}

static void handle_pc_google_done() {
    Page page("Sign in with Google");

    bool mail_was_enabled = irmail_settings().enabled;
    String message;
    if (!irmail_google_finish(request_arg("u"), message)) {
        page.line(message);
        page.link("/google", "Start again");
        page.send();
        return;
    }
    if (s_google_from_expenses) {
        IrMailSettings mail=irmail_settings(); mail.enabled=mail_was_enabled; irmail_store_settings(mail);
        auto expenses=irexpenses_settings(); expenses.enabled=mail.google_scope(GOOGLE_DRIVE_SCOPE) && mail.google_scope(GOOGLE_SHEETS_SCOPE);
        if (!expenses.enabled) page.line("Google did not grant Drive and Sheets access. Sign in again and allow both.");
        else page.line(irexpenses_store_settings(expenses) ? "Expenses sync is on." : "Could not enable Expenses sync.");
        page.line(irexpenses_check_login());
    } else if (s_google_from_notes>=0) {
        IrMailSettings mail=irmail_settings(); mail.enabled=mail_was_enabled; irmail_store_settings(mail);
        bool drawing=s_google_from_notes==1;
        auto notes=irnotes_settings(drawing); notes.enabled=mail.google_scope(GOOGLE_DRIVE_SCOPE) && (drawing || mail.google_scope(GOOGLE_DOCS_SCOPE));
        if (!notes.enabled) page.line("Google did not grant the required Drive/Docs permissions. Sign in again and allow them.");
        else page.line(irnotes_store_settings(drawing,notes) ? "Notes sync is on." : "Could not enable notes sync.");
        page.line(irnotes_check_login(drawing));
    } else if (s_google_from_drive) {
        IrMailSettings mail=irmail_settings(); mail.enabled=mail_was_enabled; irmail_store_settings(mail);
        auto drive=irdrive_settings(); drive.enabled=mail.google_scope(GOOGLE_DRIVE_SCOPE);
        if (!drive.enabled) page.line("Google did not grant Drive access. Sign in again and allow Drive.");
        else page.line(irdrive_store_settings(drive) ? "Drive backup is on." : "Could not enable Drive backup. Enable it from the Palm's Drive page.");
        page.line(irdrive_check_login());
    } else if (s_google_from_contacts) {
        IrMailSettings mail=irmail_settings(); mail.enabled=mail_was_enabled; irmail_store_settings(mail);
        auto contacts=ircontacts_settings(); contacts.enabled=mail.google_contacts();
        if (!contacts.enabled) page.line("Google did not grant both Contacts and basic profile access. Sign in again and allow both.");
        else page.line(ircontacts_store_settings(contacts) ? "Contacts sync is on." : "Could not enable Contacts. Enable it from the Palm's Contacts page.");
        page.line(ircontacts_check_login());
    } else if (s_google_from_calendar) {
        IrMailSettings mail=irmail_settings();
        mail.enabled=mail_was_enabled;
        irmail_store_settings(mail);
        IrCalendarSettings calendar=ircalendar_settings();
        calendar.enabled=mail.google_scope(GOOGLE_CALENDAR_SCOPE);
        if (!calendar.enabled) page.line("Google did not grant Calendar access. Sign in again and allow Calendar.");
        else page.line(ircalendar_store_settings(calendar) ? "Calendar sync is on." : "Could not enable Calendar. Enable it from the Palm's Calendar page.");
        page.line(ircalendar_check_login());
    } else if (s_google_from_tasks) {
        IrMailSettings mail = irmail_settings();
        mail.enabled = mail_was_enabled;
        irmail_store_settings(mail);
        IrTasksSettings tasks = irtasks_settings();
        tasks.enabled = mail.google_tasks();
        if (!tasks.enabled) page.line("Google did not grant Tasks access. Sign in again and allow Tasks.");
        else page.line(irtasks_store_settings(tasks) ? "Google Tasks sync is on." : "Could not enable Tasks. Enable it from the Palm's Tasks page.");
        page.line(irtasks_check_login());
    } else {
        page.line(message);
        String receiving, sending;
        irmail_check_login(receiving, sending);
        page.line("Receiving: " + receiving);
        page.line("Sending: " + sending);
    }
    const IrMailSettings &google = irmail_settings();
    const char *names[] = {"Mail", "Tasks", "Contacts", "Calendar", "Drive", "Docs", "Sheets", "Slides", "Basic profile"};
    const char *scopes[] = {GOOGLE_MAIL_SCOPE, GOOGLE_TASKS_SCOPE, GOOGLE_CONTACTS_SCOPE, GOOGLE_CALENDAR_SCOPE,
                            GOOGLE_DRIVE_SCOPE, GOOGLE_DOCS_SCOPE, GOOGLE_SHEETS_SCOPE, GOOGLE_SLIDES_SCOPE, GOOGLE_PROFILE_SCOPE};
    static_assert(sizeof(names) / sizeof(names[0]) == sizeof(scopes) / sizeof(scopes[0]), "Google permission names and scopes must match");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        page.line(String(names[i]) + " access: " + (google.google_scope(scopes[i]) ? "granted" : "not granted"));
    }
    page.line("You can close this page. Tap HotSync on the Palm to run the enabled conduits.");
    s_google_window_open = false; // Done: the Wi-Fi side is shut out again
    page.send();
}

// A request from the Wi-Fi side: answered only for the sign-in pages, while
// they are open. Returns false if it is not one of those.
static bool serve_pc_request() {
    if (!s_google_window_open || millis() - s_google_window_since > GOOGLE_SETUP_WINDOW_MS) {
        s_google_window_open = false;
        return false;
    }
    String path = s_target;
    int query = path.indexOf('?');
    if (query >= 0) {
        path = path.substring(0, query);
    }
    if (path == "/google") handle_pc_google();
    else if (path == "/googlestart") handle_pc_google_start();
    else if (path == "/googledone") handle_pc_google_done();
    else return false;
    return true;
}

// ------------------------------------------------------------------------------
// Web Proxy
// ------------------------------------------------------------------------------
// Palm-era browsers only speak plain HTTP, and today's sites answer that with
// "use HTTPS". With the proxy on, the adapter's DNS gives its own address for
// every site name, so the browser's requests arrive here; the adapter fetches
// the page over Wi-Fi (following redirects onto HTTPS), strips what the Palm
// cannot use, and passes the HTML on.
//
// Xiino's DataServer requests are handled separately by the OpenXiino port.

struct ProxyUrl {
    bool https;
    String host;
    uint16_t port;
    String path; // Path and query, starting with "/"
};

static bool parse_proxy_url(const String &url, ProxyUrl &out) {
    String lower = url;
    lower.toLowerCase();
    int start;
    if (lower.startsWith("http://")) {
        out.https = false;
        start = 7;
    } else if (lower.startsWith("https://")) {
        out.https = true;
        start = 8;
    } else {
        return false;
    }

    int end = start;
    while (end < (int)url.length() && url[end] != '/' && url[end] != '?' && url[end] != '#') {
        end++;
    }
    String authority = url.substring(start, end);
    int user = authority.lastIndexOf('@');
    if (user >= 0) {
        authority = authority.substring(user + 1);
    }
    out.port = out.https ? 443 : 80;
    int colon = authority.indexOf(':');
    if (colon >= 0) {
        out.port = (uint16_t)authority.substring(colon + 1).toInt();
        authority = authority.substring(0, colon);
    }
    out.host = authority;

    String rest = url.substring(end);
    int fragment = rest.indexOf('#');
    if (fragment >= 0) {
        rest = rest.substring(0, fragment);
    }
    out.path = rest.startsWith("/") ? rest : "/" + rest;
    return out.host.length() > 0 && out.port != 0;
}

// Address a redirect points to, which may be relative to the page it came from
static String resolve_redirect(const String &location, const ProxyUrl &from) {
    String lower = location;
    lower.toLowerCase();
    if (lower.startsWith("http://") || lower.startsWith("https://")) {
        return location;
    }
    String scheme = from.https ? "https:" : "http:";
    if (location.startsWith("//")) {
        return scheme + location;
    }
    String site = scheme + "//" + from.host;
    if (from.port != (from.https ? 443 : 80)) {
        site += ":" + String(from.port);
    }
    if (location.startsWith("/")) {
        return site + location;
    }
    String folder = from.path;
    int query = folder.indexOf('?');
    if (query >= 0) {
        folder = folder.substring(0, query);
    }
    return site + folder.substring(0, folder.lastIndexOf('/') + 1) + location;
}

// Pictures, style sheets and scripts are not fetched at all: Xiino expects them
// converted by the DataServer, and they would only cost time on the IR link
static bool is_unusable_resource(const String &path) {
    String name = path;
    int query = name.indexOf('?');
    if (query >= 0) {
        name = name.substring(0, query);
    }
    name.toLowerCase();
    static const char *const extensions[] = {".gif", ".jpg", ".jpeg", ".png", ".bmp", ".ico", ".webp", ".svg",
                                             ".css", ".js", ".woff", ".woff2", ".ttf", ".mp3", ".mp4"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        if (name.endsWith(extensions[i])) {
            return true;
        }
    }
    return false;
}

// Removes what a 1990s browser on a slow link cannot use (scripts, style sheets,
// inline drawings, comments, pictures), turns UTF-8 text into ISO-8859-1, and
// rewrites https:// links as http:// so the browser can follow them (the proxy
// finds its way back to HTTPS). Works on a stream: feed() can be called with
// pieces of any size; finish() releases what is still held back.
class HtmlFilter {
public:
    HtmlFilter(bool html, bool utf8) : html_(html), utf8_(utf8) {}

    void finish(String &out) {
        out += link_held_;
        link_held_ = "";
    }

    void feed(const uint8_t *data, size_t len, String &out) {
        for (size_t i = 0; i < len; i++) {
            if (html_) {
                markup((char)data[i], out);
            } else {
                text((char)data[i], out);
            }
        }
    }

private:
    enum State { PASS, TAG_NAME, SKIP_ELEMENT, SKIP_COMMENT, SKIP_TAG };

    void markup(char c, String &out) {
        switch (state_) {
        case PASS:
            if (c == '<') {
                state_ = TAG_NAME;
                held_ = "<";
            } else {
                text(c, out);
            }
            break;

        case TAG_NAME: {
            // Collect "<name" until it is clear which element this is
            bool part_of_name = isalnum((unsigned char)c) || c == '!' || c == '-';
            if (part_of_name && held_.length() < 10) {
                held_ += c;
                if (held_ == "<!--") {
                    state_ = SKIP_COMMENT;
                    matched_ = 0;
                }
                break;
            }
            String name = held_.substring(1);
            name.toLowerCase();
            if (name == "script" || name == "style" || name == "svg") {
                closing_ = "</" + name;
                matched_ = 0;
                state_ = SKIP_ELEMENT;
                markup(c, out);
            } else if (name == "img" || name == "link" || name == "meta") {
                state_ = SKIP_TAG;
                markup(c, out);
            } else {
                for (size_t k = 0; k < held_.length(); k++) {
                    emit(held_[k], out);
                }
                state_ = PASS;
                markup(c, out);
            }
            break;
        }

        case SKIP_ELEMENT: // Until "</script" (any case), then to the end of that tag
            if (tolower((unsigned char)c) == closing_[matched_]) {
                if (++matched_ == closing_.length()) {
                    state_ = SKIP_TAG;
                }
            } else {
                matched_ = (c == '<') ? 1 : 0;
            }
            break;

        case SKIP_COMMENT: // Until "-->"
            if (c == '-') {
                matched_ = (matched_ < 2) ? matched_ + 1 : 2;
            } else if (c == '>' && matched_ == 2) {
                state_ = PASS;
            } else {
                matched_ = 0;
            }
            break;

        case SKIP_TAG:
            if (c == '>') {
                state_ = PASS;
            }
            break;
        }
    }

    // Character data: UTF-8 sequences become the nearest ISO-8859-1 / ASCII text
    void text(char c, String &out) {
        uint8_t b = (uint8_t)c;
        if (!utf8_ || (b < 0x80 && pending_ == 0)) {
            emit(c, out);
            return;
        }
        if (pending_ == 0) {
            if (b >= 0xF0) {
                pending_ = 3;
            } else if (b >= 0xE0) {
                pending_ = 2;
            } else if (b >= 0xC0) {
                pending_ = 1;
            } else {
                return; // Stray continuation byte
            }
            code_ = b & (0x3F >> pending_);
            return;
        }
        if ((b & 0xC0) != 0x80) {
            pending_ = 0; // Broken sequence: start over with this byte
            text(c, out);
            return;
        }
        code_ = (code_ << 6) | (b & 0x3F);
        if (--pending_ > 0) {
            return;
        }
        if (code_ < 0x100) {
            emit((char)code_, out);
        } else if (code_ == 0x2018 || code_ == 0x2019) {
            emit('\'', out);
        } else if (code_ == 0x201C || code_ == 0x201D) {
            emit('"', out);
        } else if (code_ == 0x2013 || code_ == 0x2014) {
            emit('-', out);
        } else if (code_ == 0x2026) {
            emit('.', out);
            emit('.', out);
            emit('.', out);
        } else if (code_ == 0x2022) {
            emit('*', out);
        } else {
            emit('?', out);
        }
    }

    // Last step: "https://" leaves as "http://" (HTML pages only)
    void emit(char c, String &out) {
        static const char pattern[] = "https://";
        if (!html_) {
            out += c;
            return;
        }
        if (tolower((unsigned char)c) == pattern[link_held_.length()]) {
            link_held_ += c;
            if (link_held_.length() == sizeof(pattern) - 1) {
                out += link_held_.substring(0, 4) + "://";
                link_held_ = "";
            }
            return;
        }
        out += link_held_;
        link_held_ = "";
        if (tolower((unsigned char)c) == pattern[0]) {
            link_held_ += c;
        } else {
            out += c;
        }
    }

    bool html_;
    bool utf8_;
    State state_ = PASS;
    String held_;
    String link_held_;
    String closing_;
    size_t matched_ = 0;
    uint8_t pending_ = 0;
    uint32_t code_ = 0;
};

// Buffered reading from the web server with a time limit
struct OriginReader {
    WiFiClient &origin;
    uint8_t buf[512];
    int len = 0;
    int pos = 0;

    explicit OriginReader(WiFiClient &client) : origin(client) {}

    // Next piece of data into buf; 0 at the end, -1 when the server stops responding
    int fill() {
        uint32_t start = millis();
        for (;;) {
            int n = (origin.available() > 0) ? origin.read(buf, sizeof(buf)) : 0;
            if (n > 0) {
                len = n;
                pos = 0;
                return n;
            }
            if (!origin.connected() && origin.available() <= 0) {
                return 0;
            }
            if (millis() - start > PROXY_TIMEOUT_MS) {
                return -1;
            }
            delay(10);
        }
    }

    // One header line without its line ending; false at the end of the data
    bool line(String &text) {
        text = "";
        for (;;) {
            if (pos >= len && fill() <= 0) {
                return text.length() > 0;
            }
            char c = (char)buf[pos++];
            if (c == '\n') {
                return true;
            }
            if (c != '\r' && text.length() < 512) {
                text += c;
            }
        }
    }
};

static void proxy_error(const String &url, const String &reason) {
    irdbg_printf("Proxy: %s: %s", url.c_str(), reason.c_str());
    Page page("Cannot Open Page");
    page.line(url);
    page.line("Reason: " + reason);
    page.link("http://" PORTAL_DOMAIN "/", "Wi-Fi status");
    page.send();
}

// Fetches a page for the browser and relays it
static void proxy_fetch(const String &first_url) {
    String url = first_url;

    for (int hop = 0; hop <= PROXY_MAX_REDIRECTS; hop++) {
        ProxyUrl target;
        if (!parse_proxy_url(url, target)) {
            proxy_error(url, "address not understood");
            return;
        }
        if (is_unusable_resource(target.path)) {
            respond("404 Not Found", "text/plain", "Not relayed\r\n");
            return;
        }

        // HTTPS is ended here, without checking the site's certificate: the Palm
        // could not check it either, and nothing better is possible for it
        WiFiClient plain;
        WiFiClientSecure secure;
        secure.setInsecure();
        irdbg_printf("Proxy: fetching %s", url.c_str());
        int connected = target.https ? secure.connect(target.host.c_str(), target.port, PROXY_TIMEOUT_MS)
                                     : plain.connect(target.host.c_str(), target.port, PROXY_TIMEOUT_MS);
        if (!connected && !target.https && target.port == 80) {
            // Some sites no longer listen for plain HTTP at all
            target.https = true;
            target.port = 443;
            connected = secure.connect(target.host.c_str(), target.port, PROXY_TIMEOUT_MS);
        }
        if (!connected) {
            proxy_error(url, "could not connect to " + target.host);
            return;
        }
        WiFiClient &origin = target.https ? secure : plain;

        // A form is passed on as sent; after a redirect the page is simply fetched
        bool post = (hop == 0) && s_request_method == "POST";
        String request = String(post ? "POST " : "GET ") + target.path + " HTTP/1.0\r\nHost: " + target.host +
                         "\r\nUser-Agent: " + (s_request_agent.length() > 0 ? s_request_agent : String("Mozilla/3.0")) +
                         "\r\nAccept: text/html, text/plain\r\nAccept-Encoding: identity\r\n";
        if (post) {
            request += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                       String((unsigned)s_request_body.length()) + "\r\n";
        }
        request += "Connection: close\r\n\r\n";
        if (post) {
            request += s_request_body;
        }
        origin.write((const uint8_t *)request.c_str(), request.length());

        // Status line and headers
        OriginReader reader(origin);
        String header;
        if (!reader.line(header)) {
            origin.stop();
            proxy_error(url, "no answer from " + target.host);
            return;
        }
        int space = header.indexOf(' ');
        int code = (space >= 0) ? header.substring(space + 1, space + 4).toInt() : 0;
        String location;
        String content_type;
        while (reader.line(header) && header.length() > 0) {
            String lower = header;
            lower.toLowerCase();
            if (lower.startsWith("location:")) {
                location = header.substring(9);
                location.trim();
            } else if (lower.startsWith("content-type:")) {
                content_type = lower.substring(13);
                content_type.trim();
            }
        }

        if (code >= 301 && code <= 308 && code != 304 && location.length() > 0) {
            origin.stop();
            url = resolve_redirect(location, target);
            continue;
        }

        bool html = content_type.length() == 0 || content_type.indexOf("html") >= 0;
        if (!html && !content_type.startsWith("text/")) {
            origin.stop();
            respond("404 Not Found", "text/plain", "Not relayed: " + content_type + "\r\n");
            return;
        }

        // Relay the page as it arrives; the IR link sets the pace
        bool palm_listening = send_all(String("HTTP/1.0 200 OK\r\nContent-Type: ") + (html ? "text/html" : "text/plain") +
                                       "\r\nConnection: close\r\n\r\n");
        HtmlFilter filter(html, content_type.indexOf("utf-8") >= 0);
        size_t relayed = 0;
        String out;
        if (reader.pos < reader.len) {
            filter.feed(reader.buf + reader.pos, reader.len - reader.pos, out); // Body bytes read along with the headers
        }
        while (palm_listening) {
            if (relayed + out.length() > PROXY_MAX_PAGE_BYTES) {
                out = out.substring(0, PROXY_MAX_PAGE_BYTES - relayed) + (html ? "\r\n<P>[Page cut off here: too long]</P>\r\n" : "\r\n[cut off]\r\n");
                send_all(out);
                break;
            }
            relayed += out.length();
            palm_listening = send_all(out);
            out = "";
            int n = reader.fill();
            if (n <= 0) {
                filter.finish(out);
                send_all(out);
                break;
            }
            filter.feed(reader.buf, n, out);
        }
        origin.stop();
        irdbg_printf("Proxy: relayed %u bytes", (unsigned)relayed);
        return;
    }
    proxy_error(url, "too many redirects");
}

// ------------------------------------------------------------------------------
// HTTP Server (deliberately simple and forgiving, for 1990s browsers)
// ------------------------------------------------------------------------------

#define HTTP_REQUEST_MAX     4096  // Bytes of request kept (request line, headers, form fields)
#define HTTP_READ_TIMEOUT_S  4     // Wait for the request to arrive
#define HTTP_CLOSE_TIMEOUT_S 2     // Wait for the client to close after our reply

static void route_request() {
    // A browser set to use a proxy sends the full URL ("http://wifi.setup/nets"):
    // the site is taken from it, and only the path is used for the page.
    String path = s_target;
    int scheme = path.indexOf("://");
    if (scheme >= 0) {
        int slash = path.indexOf('/', scheme + 3);
        s_request_host = (slash >= 0) ? path.substring(scheme + 3, slash) : path.substring(scheme + 3);
        path = (slash >= 0) ? path.substring(slash) : String("/");
    }

    // Requests for any other site: Xiino sends them all here (we are its
    // DataServer), and so does a browser whose name lookup failed. Fetch the
    // page on the browser's behalf.
    String site = s_request_host;
    int port = site.indexOf(':');
    if (port >= 0) {
        site = site.substring(0, port);
    }
    site.toLowerCase();
    if (site.length()>0 && site!=PORTAL_DOMAIN && site!=PPP_GATEWAY_IP) {
        if (s_xiino_request.dataserver) ++s_dataserver_pages; else ++s_direct_pages;
    }
    if (s_xiino_request.dataserver && site != PORTAL_DOMAIN && site != PPP_GATEWAY_IP) {
        if (s_request_method != "GET") {
            respond("501 Not Implemented", "text/html", "<HTML><BODY>Unsupported method</BODY></HTML>");
        } else if (WiFi.status() == WL_CONNECTED || !xiino_builtin(s_xiino_request, "").empty()) {
            std::string problem;
            bool started=false;
            bool ok=irxiino_stream(s_xiino_request,s_request_text.c_str(),[&]() {
                started=true;
                return send_all("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n") &&
                    send_bytes(xiino_padding().data(),xiino_padding().size());
            },send_bytes,problem);
            if (!ok && !started) proxy_error(s_target,String(problem.c_str()));
        } else {
            s_request_host = site;
            handle_other_site();
        }
        return;
    }
    if (site.length() > 0 && site != PORTAL_DOMAIN && site != PPP_GATEWAY_IP && site != XIINO_DATASERVER_HOST) {
        if (WiFi.status() == WL_CONNECTED) {
            proxy_fetch(scheme >= 0 ? s_target : "http://" + s_request_host + s_target);
        } else {
            s_request_host = site;
            handle_other_site();
        }
        return;
    }
    int query = path.indexOf('?');
    if (query >= 0) {
        path = path.substring(0, query);
    }

    // "/join/3", "/save/3" and "/forget/1" carry a list position in the address
    int index = -1;
    bool indexed = false;
    int digits = path.lastIndexOf('/') + 1;
    if (digits > 1 && digits < (int)path.length()) {
        indexed = true;
        index = path.substring(digits).toInt();
        for (size_t i = digits; i < path.length(); i++) {
            if (!isdigit(path[i])) {
                index = -1;
            }
        }
        path = path.substring(0, digits - 1);
    }
    bool scanned = indexed && index >= 0 && index < (int)s_network_count;
    bool saved = indexed && index >= 0 && index < (int)s_saved_count;

    if (path == "/nets") handle_networks(false);
    else if (path == "/scan") handle_networks(true);
    else if (path == "/join" && !indexed) handle_join(-1);
    else if (path == "/save" && !indexed) handle_save(-1);
    else if (path == "/join" && scanned) handle_join(index);
    else if (path == "/save" && scanned) handle_save(index);
    else if (path == "/join" || path == "/save") handle_networks(false); // The list changed: choose again
    else if (path == "/test" && !indexed) handle_diagnostics();
    else if (path == "/echo") handle_echo();
    else if (path == "/proxy" && indexed && index >= 0 && index <= 1) {
        s_proxy_all_sites = (index == 1);
        store_settings();
        handle_status();
    }
    else if (path=="/hotsync" && !indexed) handle_hotsync();
    else if ((path=="/hotsyncsave" || path=="/drivesave") && !indexed) handle_hotsync(true);
    else if (path=="/hotsynctext" && indexed && index>=0 && index<=1) {
        auto settings=irhotsync_settings(); settings.cyrillic=index==1; String problem;
        if (!irhotsync_store_settings(settings,problem)) { Page page("HotSync Settings"); page.line(problem); page.link("/hotsync","Back"); page.send(); }
        else handle_hotsync();
    }
    else if (path=="/expenses" && !indexed) handle_expenses();
    else if (path=="/expensescheck" && !indexed) {
        Page page("Expenses Access"); page.line(irexpenses_check_login()); page.link("/expenses","Back"); page.send();
    }
    else if (path=="/expensesgoogle" && !indexed) handle_mail_google(false,false,false,false,-1,true);
    else if (path=="/expensesretry" && !indexed) {
        Page page("Expense Retry"); page.line(irexpenses_retry_creations() ? "Currently unconfirmed creations will be checked and retried at the next HotSync." : "No unconfirmed creations are pending, or state could not be saved."); page.link("/expenses","Back"); page.send();
    }
    else if ((path=="/expenseson" || path=="/expensestext" || path=="/expensesconflict") && indexed && index>=0 && index<=2) {
        auto settings=irexpenses_settings(); bool valid=true;
        if (path=="/expensesconflict") settings.conflicts=index;
        else if (index>1) valid=false;
        else if (path=="/expenseson") settings.enabled=index==1;
        else settings.cyrillic=index==1;
        if (!valid || !irexpenses_store_settings(settings)) { Page page("Expense Settings"); page.line("Could not save Expense settings."); page.link("/expenses","Back"); page.send(); }
        else handle_expenses();
    }
    else if ((path=="/memos" || path=="/notes") && !indexed) handle_notes(path=="/notes");
    else if ((path=="/memoscheck" || path=="/notescheck") && !indexed) {
        bool drawing=path=="/notescheck"; Page page("Notes Access"); page.line(irnotes_check_login(drawing)); page.link(drawing ? "/notes" : "/memos","Back"); page.send();
    }
    else if ((path=="/memosgoogle" || path=="/notesgoogle") && !indexed) handle_mail_google(false,false,false,false,path=="/notesgoogle" ? 1 : 0);
    else if (path=="/memosretry" && !indexed) {
        Page page("Memo Retry"); page.line(irnotes_retry_creations(false) ? "Currently unconfirmed creations will be checked and retried at the next HotSync." : "No unconfirmed creations are pending, or sync state could not be saved."); page.link("/memos","Back"); page.send();
    }
    else if ((path=="/memoson" || path=="/noteson" || path=="/memostext" || path=="/notestext" || path=="/memosconflict" || path=="/notesconflict") && indexed && index>=0 && index<=2) {
        bool drawing=path.startsWith("/notes"); auto settings=irnotes_settings(drawing);
        bool valid=true;
        if (path.endsWith("conflict")) settings.conflicts=index;
        else if (index>1) valid=false;
        else if (path.endsWith("on")) settings.enabled=index==1;
        else settings.cyrillic=index==1;
        if (!valid || !irnotes_store_settings(drawing,settings)) { Page page("Notes Settings"); page.line("Could not save notes settings."); page.link(drawing ? "/notes" : "/memos","Back"); page.send(); }
        else handle_notes(drawing);
    }
    else if (path == "/drive" && !indexed) handle_drive();
    else if (path == "/drivecheck" && !indexed) {
        Page page("Drive Access"); page.line(irdrive_check_login()); page.link("/drive","Back"); page.send();
    }
    else if (path == "/drivegoogle" && !indexed) handle_mail_google(false,false,false,true);
    else if (path == "/driverestore" && !indexed) {
        Page page("Drive Restore"); page.line(irdrive_restore_once() ? "Missing databases from saved backups will restore at the next HotSync." : "Could not save the restore request.");
        page.link("/drive","Back"); page.send();
    }
    else if (path == "/driveretry" && !indexed) {
        Page page("Drive Retry"); page.line(irdrive_retry_interrupted() ? "The interrupted install will retry at the next HotSync." : "No interrupted install is pending, or the request could not be saved.");
        page.link("/drive","Back"); page.send();
    }
    else if ((path == "/driveon" || path == "/drivetext") && indexed && index>=0 && index<=1) {
        auto settings=irdrive_settings();
        if (path=="/driveon") settings.enabled=index==1; else settings.cyrillic=index==1;
        if (!irdrive_store_settings(settings)) { Page page("Drive Settings"); page.line("Could not save Drive settings."); page.link("/drive","Back"); page.send(); }
        else handle_drive();
    }
    else if (path == "/contacts" && !indexed) handle_contacts();
    else if (path == "/contactscheck" && !indexed) {
        Page page("Contacts Access"); page.line(ircontacts_check_login()); page.link("/contacts","Back"); page.send();
    }
    else if (path == "/contactsgoogle" && !indexed) handle_mail_google(false,false,true);
    else if (path == "/contactsretry" && !indexed) {
        Page page("Contacts Retry"); page.line(ircontacts_retry_pending() ? "Pending creations will be checked and retried at the next sync." : "Could not save Contacts state.");
        page.link("/contacts","Back"); page.send();
    }
    else if ((path == "/contactson" || path == "/contactstext") && indexed && index>=0 && index<=1) {
        auto settings=ircontacts_settings();
        if (path=="/contactson") settings.enabled=index==1; else settings.cyrillic=index==1;
        ircontacts_store_settings(settings); handle_contacts();
    }
    else if (path == "/calendar" && !indexed) handle_calendar();
    else if (path == "/calendarsave" && !indexed) handle_calendar(true);
    else if (path == "/calendarcheck" && !indexed) {
        Page page("Calendar Access"); page.line(ircalendar_check_login()); page.link("/calendar","Back"); page.send();
    }
    else if (path == "/calendargoogle") handle_mail_google(false,true);
    else if ((path == "/calendaron" || path == "/calendartext") && indexed && index>=0 && index<=1) {
        IrCalendarSettings settings=ircalendar_settings();
        if (path=="/calendaron") settings.enabled=index==1; else settings.cyrillic=index==1;
        ircalendar_store_settings(settings); handle_calendar();
    }
    else if (path == "/tasks" && !indexed) handle_tasks();
    else if (path == "/taskssave" && !indexed) handle_tasks(true);
    else if (path == "/taskstest" && !indexed) {
        Page page("Google Tasks Access"); page.line(irtasks_check_login()); page.link("/tasks", "Back"); page.send();
    }
    else if ((path == "/taskson" || path == "/taskstext") && indexed && index >= 0 && index <= 1) {
        IrTasksSettings settings = irtasks_settings();
        if (path == "/taskson") settings.enabled = index == 1; else settings.cyrillic = index == 1;
        irtasks_store_settings(settings); handle_tasks();
    }
    else if (path == "/mail" && !indexed) handle_mail();
    else if (path == "/mailacct") handle_mail_account();
    else if (path == "/mailsave") handle_mail_account_save();
    else if (path == "/mailsrv") handle_mail_servers(false);
    else if (path == "/mailsrvsave") handle_mail_servers(true);
    else if (path == "/mailtest") handle_mail_test();
    else if (path == "/tasksgoogle") handle_mail_google(true);
    else if (path == "/mailgoogle") handle_mail_google();
    else if (path == "/mailgoogleoff") handle_mail_google_off();
    else if ((path == "/mailon" || path == "/mailtext") && indexed && index >= 0 && index <= 1) {
        IrMailSettings mail = irmail_settings();
        if (path == "/mailon") {
            mail.enabled = (index == 1);
        } else {
            mail.cyrillic = (index == 1);
        }
        irmail_store_settings(mail);
        handle_mail();
    }
    else if (path == "/mailagain") {
        irmail_forget_position();
        handle_mail();
    }
    else if (path == "/net" && saved) handle_saved_network(index);
    else if (path == "/pass" && saved) handle_change_password(index);
    else if (path == "/forget" && saved) handle_forget(index);
    else handle_status();
}

// Value of a request header (name in lower case, with colon), from the lower-cased request text
static String header_value(const String &request_lower, const char *name) {
    int at = request_lower.indexOf(String("\n") + name);
    if (at < 0) {
        return String();
    }
    int start = at + 1 + strlen(name);
    int end = request_lower.indexOf('\n', start);
    return (end >= 0) ? request_lower.substring(start, end) : request_lower.substring(start);
}

static void serve_client() {
    s_xiino_request = XiinoRequest();
    // Read until the blank line that ends the headers. Old clients may use bare
    // line feeds, send no headers at all (HTTP/0.9), or close their side early.
    static char raw[HTTP_REQUEST_MAX + 1];
    size_t len = 0;
    raw[0] = '\0';
    while (len < HTTP_REQUEST_MAX) {
        int got = recv(s_client, raw + len, HTTP_REQUEST_MAX - len, 0);
        if (got <= 0) {
            break; // Closed by the client, or nothing more within the timeout
        }
        len += got;
        raw[len] = '\0';
        if (strstr(raw, "\r\n\r\n") != NULL || strstr(raw, "\n\n") != NULL) {
            break;
        }
        // "GET /path" with no version is a complete HTTP/0.9 request
        if (strchr(raw, '\n') != NULL && strstr(raw, " HTTP/") == NULL && strstr(raw, " http/") == NULL) {
            break;
        }
    }

    // A form sent with POST carries its fields after the headers: read them too
    String body;
    size_t headers_len = 0;
    char *blank = strstr(raw, "\r\n\r\n");
    if (blank != NULL) {
        headers_len = (blank - raw) + 4;
    } else if ((blank = strstr(raw, "\n\n")) != NULL) {
        headers_len = (blank - raw) + 2;
    }
    if (headers_len > 0) {
        String headers_lower = String(raw).substring(0, headers_len);
        headers_lower.toLowerCase();
        size_t wanted = headers_len + header_value(headers_lower, "content-length:").toInt();
        if (wanted > HTTP_REQUEST_MAX) {
            wanted = HTTP_REQUEST_MAX;
        }
        while (len < wanted) {
            int got = recv(s_client, raw + len, wanted - len, 0);
            if (got <= 0) {
                break;
            }
            len += got;
            raw[len] = '\0';
        }
        body = String(raw + headers_len);
        body.trim();
    }

    // Request line: METHOD target [version]
    String request(raw);
    s_request_text = request;
    int line_end = request.indexOf('\n');
    String line = (line_end >= 0) ? request.substring(0, line_end) : request;
    line.trim();
    bool task_credentials = line.indexOf("/googlestart") >= 0 || line.indexOf("/googledone") >= 0;
    irdbg_printf("HTTP from %s: %s", IPAddress(s_client_ip).toString().c_str(),
                 task_credentials ? "Google sign-in credentials (redacted)" : line.c_str());
    if (task_credentials) s_request_text = "Google sign-in credentials (redacted)";

    int first_space = line.indexOf(' ');
    if (first_space < 0) {
        respond("400 Bad Request", "text/plain", "Bad request\r\n");
        return;
    }
    s_request_method = line.substring(0, first_space);
    s_request_method.toUpperCase();
    s_request_body = body;
    int second_space = line.indexOf(' ', first_space + 1);
    s_target = (second_space >= 0) ? line.substring(first_space + 1, second_space) : line.substring(first_space + 1);

    // Preserve DataServer settings and response framing; local portal pages
    // still route normally after extracting their real address.
    s_xiino_request = xiino_request(s_target.c_str());
    if (s_xiino_request.dataserver) s_target = s_xiino_request.url.c_str();

    // Form fields: after the "?" of the address, and / or in the body
    int query = s_target.indexOf('?');
    s_params = (query >= 0) ? s_target.substring(query + 1) : String();
    if (body.length() > 0) {
        if (s_params.length() > 0) {
            s_params += "&";
        }
        s_params += body;
    }

    // WAP browsers ask for WML and do not accept HTML
    String request_lower = request;
    request_lower.toLowerCase();
    s_request_host = header_value(request_lower, "host:");
    s_request_host.trim();
    int agent = request_lower.indexOf("\nuser-agent:");
    s_request_agent = "";
    if (agent >= 0) {
        int agent_end = request.indexOf('\n', agent + 1);
        s_request_agent = (agent_end >= 0) ? request.substring(agent + 12, agent_end) : request.substring(agent + 12);
        s_request_agent.trim();
    }
    String accept = header_value(request_lower, "accept:");
    s_wants_wml = accept.indexOf("vnd.wap.wml") >= 0 && accept.indexOf("text/html") < 0;
#if ENABLE_CDC_DEBUG
    // Every header the browser sent, one trace line each
    int from = (line_end >= 0) ? line_end + 1 : (int)request.length();
    while (!task_credentials && from < (int)request.length()) {
        int to = request.indexOf('\n', from);
        String header = (to >= 0) ? request.substring(from, to) : request.substring(from);
        header.trim();
        if (header.length() > 0) {
            irdbg_printf("HTTP   %s", header.c_str());
        }
        if (to < 0) {
            break;
        }
        from = to + 1;
    }
#endif

    // Only the temporary Google sign-in pages are reachable from Wi-Fi.
    if (s_client_ip == 0 || s_client_ip != irppp_palm_ip()) {
        if (!serve_pc_request()) respond("403 Forbidden", "text/plain", "Forbidden\r\n");
    } else {
        route_request();
    }
    if (task_credentials) {
        s_request_body = "";
        s_params = "";
        s_target = "/google";
        memset(raw, 0, len);
    }
}

static void http_task(void *arg) {
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(80);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (listener < 0 || bind(listener, (struct sockaddr *)&local, sizeof(local)) < 0 || listen(listener, 2) < 0) {
        if (listener >= 0) close(listener);
        vTaskDelete(NULL);
        return;
    }
    int xiino_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    local.sin_port = htons(XIINO_DATASERVER_PORT);
    if (xiino_listener < 0 || bind(xiino_listener, (struct sockaddr *)&local, sizeof(local)) < 0 || listen(xiino_listener, 2) < 0) {
        if (xiino_listener >= 0) close(xiino_listener);
        xiino_listener = -1;
        irdbg_printf("Xiino: could not listen on port %u", XIINO_DATASERVER_PORT);
    }

    for (;;) {
        manage_wifi();

        // Wait briefly for a connection, so the Wi-Fi manager keeps running
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        if (xiino_listener >= 0) FD_SET(xiino_listener, &readable);
        struct timeval wait = {0, 200 * 1000};
        if (select((xiino_listener > listener ? xiino_listener : listener) + 1, &readable, NULL, NULL, &wait) <= 0) {
            continue;
        }

        struct sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        int ready_listener = FD_ISSET(listener, &readable) ? listener : xiino_listener;
        s_client = accept(ready_listener, (struct sockaddr *)&peer, &peer_len);
        if (s_client < 0) {
            continue;
        }
        s_client_ip = peer.sin_addr.s_addr;

        struct timeval timeout = {HTTP_READ_TIMEOUT_S, 0};
        setsockopt(s_client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        struct timeval send_timeout = {PROXY_TIMEOUT_MS / 1000, (PROXY_TIMEOUT_MS % 1000) * 1000};
        setsockopt(s_client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
        serve_client();

        // Finish our side, then wait for the client to close. Closing with unread
        // data would reset the connection and the browser could lose the page.
        shutdown(s_client, SHUT_WR);
        timeout.tv_sec = HTTP_CLOSE_TIMEOUT_S;
        setsockopt(s_client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char discard[64];
        while (recv(s_client, discard, sizeof(discard), 0) > 0) {
        }
        close(s_client);
        s_client = -1;
    }
}

// ------------------------------------------------------------------------------
// DNS for the Palm: answers PORTAL_DOMAIN itself and relays everything else.
// While Wi-Fi is not connected every name leads to the portal, so whatever
// address is opened on the Palm ends up on the settings page.
// ------------------------------------------------------------------------------

#define DNS_HEADER_LEN      12
#define DNS_TYPE_A          1
#define DNS_FLAG_RESPONSE   0x80
#define DNS_RCODE_SERVFAIL  0x02
#define DNS_RCODE_NXDOMAIN  0x03
#define DNS_MAX_PENDING     4     // Lookups waiting for a DNS server at the same time

// Reads the question name as lower-case dotted text; returns the offset after it, or -1
static int read_question_name(const uint8_t *pkt, int len, char *name, size_t name_size) {
    int pos = DNS_HEADER_LEN;
    size_t n = 0;
    while (pos < len && pkt[pos] != 0) {
        uint8_t label_len = pkt[pos++];
        if (label_len > 63 || pos + label_len > len || n + label_len + 2 > name_size) {
            return -1;
        }
        if (n > 0) {
            name[n++] = '.';
        }
        for (uint8_t i = 0; i < label_len; i++) {
            name[n++] = (char)tolower(pkt[pos++]);
        }
    }
    name[n] = '\0';
    pos++; // Root label
    return (pos + 4 <= len) ? pos : -1; // QTYPE and QCLASS must follow
}

// Turns the query in pkt into an answer naming the gateway; returns its length
static int make_portal_answer(uint8_t *pkt, int question_end, uint32_t gateway_ip) {
    uint16_t qtype = (pkt[question_end] << 8) | pkt[question_end + 1];
    int n = question_end + 4;
    pkt[2] = 0x84 | (pkt[2] & 0x01);                    // Response, authoritative, keep RD
    pkt[3] = 0x80;                                      // Recursion available, no error
    pkt[6] = 0; pkt[7] = (qtype == DNS_TYPE_A) ? 1 : 0; // Answer count
    pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;            // No authority / additional records
    if (qtype == DNS_TYPE_A) {
        static const uint8_t answer[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04};
        memcpy(pkt + n, answer, sizeof(answer));        // Name pointer, A, IN, TTL 60 s, 4 bytes
        n += sizeof(answer);
        memcpy(pkt + n, &gateway_ip, 4);
        n += 4;
    }
    return n;
}

// Remembers the last few lookups for the diagnostics page
static void log_lookup(const char *name, const String &result) {
    portENTER_CRITICAL(&s_dns_log_lock);
    DnsLogEntry &entry = s_dns_log[s_dns_log_next];
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    strncpy(entry.result, result.c_str(), sizeof(entry.result) - 1);
    entry.result[sizeof(entry.result) - 1] = '\0';
    s_dns_log_next = (s_dns_log_next + 1) % DNS_LOG_ENTRIES;
    portEXIT_CRITICAL(&s_dns_log_lock);
    irdbg_printf("DNS %s -> %s", name, result.c_str());
}

// A lookup passed on to a DNS server on the Wi-Fi side and not answered yet
struct PendingLookup {
    bool active;
    bool tried_fallback;
    uint32_t sent_ms;
    struct sockaddr_in from;  // Where the Palm's query came from
    int len;
    int question_end;
    char name[44];
    uint8_t query[512 + 16];
};
static PendingLookup s_pending[DNS_MAX_PENDING];

static void send_to_dns_server(int upstream, PendingLookup &lookup, uint32_t server_ip) {
    struct sockaddr_in server = {};
    server.sin_family = AF_INET;
    server.sin_port = htons(53);
    server.sin_addr.s_addr = server_ip;
    sendto(upstream, lookup.query, lookup.len, 0, (struct sockaddr *)&server, sizeof(server));
    lookup.sent_ms = millis();
}

// Unknown name, or no DNS server reachable: lead the browser to the portal,
// which explains what happened, instead of leaving it with a bare error
static void answer_with_portal(int sock, PendingLookup &lookup, const char *reason) {
    log_lookup(lookup.name, reason);
    int n = make_portal_answer(lookup.query, lookup.question_end, inet_addr(PPP_GATEWAY_IP));
    sendto(sock, lookup.query, n, 0, (struct sockaddr *)&lookup.from, sizeof(lookup.from));
    lookup.active = false;
}

static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int upstream = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || upstream < 0 || bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        vTaskDelete(NULL);
        return;
    }

    static uint8_t pkt[512 + 16];

    for (;;) {
        // Wait for a query from the Palm or a reply from a DNS server. Lookups
        // never wait for each other: a silent server must not hold up the
        // portal's own name.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(sock, &readable);
        FD_SET(upstream, &readable);
        struct timeval wait = {0, 250 * 1000};
        int ready = select((sock > upstream ? sock : upstream) + 1, &readable, NULL, NULL, &wait);

        uint32_t wifi_dns = (WiFi.status() == WL_CONNECTED) ? (uint32_t)WiFi.dnsIP() : 0;
        uint32_t fallback_dns = inet_addr(PPP_FALLBACK_DNS);
        bool wifi_up = WiFi.status() == WL_CONNECTED;

        // 1. Query from the Palm
        if (ready > 0 && FD_ISSET(sock, &readable)) {
            struct sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            int len = recvfrom(sock, pkt, 512, 0, (struct sockaddr *)&from, &from_len);
            uint32_t palm_ip = irppp_palm_ip();
            if (len >= DNS_HEADER_LEN && palm_ip != 0 && from.sin_addr.s_addr == palm_ip && !(pkt[2] & DNS_FLAG_RESPONSE)) {
                char name[80];
                int question_end = read_question_name(pkt, len, name, sizeof(name));

                if (question_end < 0) {
                    // Not a question we can read: refuse rather than guess
                    pkt[2] |= DNS_FLAG_RESPONSE;
                    pkt[3] = 0x80 | DNS_RCODE_SERVFAIL;
                    sendto(sock, pkt, len, 0, (struct sockaddr *)&from, from_len);
                } else if (strcmp(name, PORTAL_DOMAIN) == 0 || strcmp(name, XIINO_DATASERVER_HOST) == 0 ||
                           strcmp(name, XIINO_DATASERVER_LEGACY_HOST) == 0 || !wifi_up ||
                           s_proxy_all_sites) {
                    // Our own names, no Wi-Fi yet, or the web proxy is on (the browser
                    // must come to us for every site): answer with the gateway address
                    bool own = strcmp(name, PORTAL_DOMAIN) == 0 || strcmp(name, XIINO_DATASERVER_HOST) == 0 ||
                               strcmp(name, XIINO_DATASERVER_LEGACY_HOST) == 0;
                    log_lookup(name, own ? "portal" : wifi_up ? "web proxy" : "portal (no Wi-Fi)");
                    int n = make_portal_answer(pkt, question_end, inet_addr(PPP_GATEWAY_IP));
                    sendto(sock, pkt, n, 0, (struct sockaddr *)&from, from_len);
                } else {
                    // Everything else goes to the DNS server of the Wi-Fi network.
                    // Take a free slot, or the one waiting longest.
                    PendingLookup *slot = &s_pending[0];
                    for (int i = 0; i < DNS_MAX_PENDING; i++) {
                        if (!s_pending[i].active) {
                            slot = &s_pending[i];
                            break;
                        }
                        if ((int32_t)(s_pending[i].sent_ms - slot->sent_ms) < 0) {
                            slot = &s_pending[i];
                        }
                    }
                    slot->active = true;
                    slot->tried_fallback = (wifi_dns == 0);
                    slot->from = from;
                    slot->len = len;
                    slot->question_end = question_end;
                    strncpy(slot->name, name, sizeof(slot->name) - 1);
                    slot->name[sizeof(slot->name) - 1] = '\0';
                    memcpy(slot->query, pkt, len);
                    send_to_dns_server(upstream, *slot, wifi_dns != 0 ? wifi_dns : fallback_dns);
                }
            }
        }

        // 2. Reply from a DNS server: match it to its lookup by transaction ID
        if (ready > 0 && FD_ISSET(upstream, &readable)) {
            int reply_len = recvfrom(upstream, pkt, 512, 0, NULL, NULL);
            for (int i = 0; i < DNS_MAX_PENDING && reply_len >= DNS_HEADER_LEN; i++) {
                PendingLookup &lookup = s_pending[i];
                if (!lookup.active || lookup.query[0] != pkt[0] || lookup.query[1] != pkt[1]) {
                    continue;
                }
                uint8_t rcode = pkt[3] & 0x0F;
                uint16_t answers = (pkt[6] << 8) | pkt[7];
                if (rcode == DNS_RCODE_NXDOMAIN) {
                    answer_with_portal(sock, lookup, "no such name");
                } else {
                    log_lookup(lookup.name, rcode != 0 ? "error code " + String(rcode)
                                                       : answers > 0 ? String("found") : String("no address"));
                    sendto(sock, pkt, reply_len, 0, (struct sockaddr *)&lookup.from, sizeof(lookup.from));
                    lookup.active = false;
                }
                break;
            }
        }

        // 3. Servers that stay silent: try the public one, then give up
        for (int i = 0; i < DNS_MAX_PENDING; i++) {
            PendingLookup &lookup = s_pending[i];
            if (!lookup.active || millis() - lookup.sent_ms < PORTAL_DNS_TIMEOUT_S * 1000UL) {
                continue;
            }
            if (!lookup.tried_fallback && fallback_dns != wifi_dns) {
                lookup.tried_fallback = true;
                send_to_dns_server(upstream, lookup, fallback_dns);
            } else {
                answer_with_portal(sock, lookup, "DNS server did not answer");
            }
        }
    }
}

// ------------------------------------------------------------------------------
// Startup
// ------------------------------------------------------------------------------

void irportal_init() {
    irxiino_init();
    // The connection manager in the portal task decides what to join and when
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.onEvent(on_wifi_event, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    load_saved_networks();
    load_settings();

    // Both servers block on the network, so they run outside the IrDA loop
    xTaskCreate(http_task, "portal_http", 16384, NULL, 1, NULL); // Room for a TLS handshake
    xTaskCreate(dns_task, "portal_dns", 4096, NULL, 1, NULL);
}
