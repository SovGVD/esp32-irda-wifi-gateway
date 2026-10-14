#pragma once

#include <Arduino.h>

// ==============================================================================
// Hardware Pin Definitions for ESP32-C3 & Vishay TFBS4711
// ==============================================================================
// TFBS4711 Pinout:
//   Pin 1: VCC2 (IRED Anode)   -> 3.3V or 5V (with decoupling cap & 10-22 ohm resistor)
//   Pin 2: TXD (Data Input)    -> ESP32 GPIO 6 (Active HIGH pulse)
//   Pin 3: RXD (Data Output)   -> ESP32 GPIO 7 (Active LOW pulse, internal weak pull-up)
//   Pin 4: SD  (Shutdown)      -> ESP32 GPIO 5 (Active HIGH, drive LOW to enable)
//   Pin 5: VCC1 (Supply VCC)   -> 3.3V (with 100nF ceramic cap close to module)
//   Pin 6: GND (Ground)        -> GND
#define PIN_TFBS_TX           6
#define PIN_TFBS_RX           7
#define PIN_TFBS_SD           5

// Status LED (On ESP32-C3 SuperMini, the onboard blue LED is on GPIO 8 and is Active LOW).
// Set to -1 if your board has no LED or if GPIO 8 is needed elsewhere.
#define PIN_STATUS_LED        8
#define LED_ON                LOW
#define LED_OFF               HIGH

// ==============================================================================
// Operating Mode Configuration
// ==============================================================================
// MODE_IRCOMM:
//   ESP32 implements the IrDA protocol stack (IrLAP discovery/link, IrLMP/IAS,
//   TinyTP, and IrCOMM 3-wire cooked serial emulation).
//   Linux sees a transparent serial connection (/dev/ttyACM0) that talks
//   directly to Palm OS HotSync (CMP/PADP/DLP protocols) without needing Linux IrDA drivers.
#define MODE_IRCOMM           0

// MODE_RAW_SIR:
//   ESP32 acts as a transparent SIR pulse encoder/decoder bridge.
//   Every byte written to USB CDC is pulsed out over IR; every IR pulse is decoded
//   to USB CDC. Use this if Palm runs a raw serial app (e.g., terminal) or if Linux
//   runs a legacy kernel/userspace IrDA stack.
#define MODE_RAW_SIR          1

#define CURRENT_OP_MODE       MODE_IRCOMM

// IrDA Protocol Stack Timing & Parameters
// Palm III & Palm IIIx IrDA Specifications:
// - Discovery and initial connection ALWAYS run at 9600 bps.
// - Palm OS HotSync ("IR to a PC/Mac") negotiates up to 57600 bps by default.
// - Hardware supports: 9600, 19200, 38400, 57600 (and 115200 if configured).
#define IRDA_DEFAULT_BAUD     9600     // IrDA discovery/connection rate (mandatory 9600)
#define IRDA_TARGET_BAUD      57600    // Palm III / IIIx standard HotSync baud (57600)
#define IRDA_DEVICE_NAME      "ESP32-IrCOMM"

// IrDA Minimum Turnaround Time (in microseconds) used before negotiation:
// Per IrDA standard, transceivers require time for optical recovery before TX,
// and 10 ms is the mandatory default during discovery / connection setup.
// Once connected, the value the Palm requests in its SNRM frame is used instead.
#define IRDA_MIN_TURNAROUND_US 10000

// Link supervision: drop back to discovery when the Palm stops polling.
// 3 s matches the disconnect time negotiated in the UA frame. The shorter
// timeout applies until the first frame arrives at the negotiated baud rate
// (the Palm repeats SNRM at 9600 bps if it missed our UA).
#define IRLAP_LINK_TIMEOUT_MS     3000
#define IRLAP_CONNECT_TIMEOUT_MS  1000

// Maximum LAP frame info payload
#define IRLAP_MAX_PAYLOAD     512

// FIFO Ring Buffer capacities (bytes)
#define USB_TO_IR_BUF_SIZE    2048

// USB CDC driver receive queue (bytes). The driver default of 256 silently drops
// data while the main loop is busy with an IrDA frame, and pilot-link sends
// blocks of about 1 KB when installing a file, so keep this well above that.
#define USB_CDC_RX_BUF_SIZE   8192
#define IR_TO_USB_BUF_SIZE    4096

// ==============================================================================
// Wi-Fi Internet Gateway (PPP over IrCOMM)
// ==============================================================================
// When the Palm opens a network connection (Prefs -> Network, Connection:
// "IR to a PC/Handheld"), the ESP32 answers the PPP session itself and routes
// the Palm onto Wi-Fi. HotSync sessions are still bridged to USB: the two are
// told apart by the first byte the Palm sends (PPP frames start with 0x7E).
// Set to 0 to leave Wi-Fi off; PPP is then passed through to USB like any
// other serial data (e.g. for pppd on the Linux host).
#define ENABLE_WIFI_GATEWAY   1

// Wi-Fi networks are added from the Palm: connect, then open
// http://PORTAL_DOMAIN/ (or http://PPP_GATEWAY_IP/) in a browser on the Palm.
// Several networks are remembered on the ESP32; it joins whichever saved one
// is in range, strongest first. WIFI_SSID / WIFI_PASSWORD below are only the
// default used before anything is saved, and can also be defined in
// include/wifi_secrets.h (ignored by git).
#define PORTAL_DOMAIN         "wifi.setup"  // Lower case
#define PORTAL_MAX_NETWORKS   10            // Scan results listed
#define PORTAL_MAX_SAVED      5             // Networks remembered
#define PORTAL_DNS_TIMEOUT_S  2             // Wait for each DNS server to answer

// Connecting: time allowed per try, tries per network (the second one at
// reduced transmit power), and the pause before looking again when none worked
#define PORTAL_CONNECT_TIMEOUT_MS  12000
#define PORTAL_CONNECT_ATTEMPTS    2
#define PORTAL_RETRY_INTERVAL_MS   20000

// Web proxy. Palm browsers only speak plain HTTP, which today's sites redirect
// to HTTPS. With the proxy on, the adapter answers every site name with its own
// address, fetches the page over Wi-Fi (HTTP or HTTPS), strips scripts, style
// sheets and pictures, and passes the HTML on. It is switched on the portal's
// status page; turn it off for programs that are not web browsers (mail,
// telnet), which need to reach their servers directly.
#define PROXY_ALL_SITES_DEFAULT 1                // Setting until changed in the portal

// Xiino can instead send every request to a "DataServer" (its preferences).
// The original one is gone; the adapter answers under its name. Clearing the
// DataServer setting in Xiino is the better choice: pages then display cleanly.
#define XIINO_DATASERVER_HOST "pds.mobirus.com"  // Xiino's default; lower case
#define PROXY_MAX_PAGE_BYTES  48000              // Longer pages are cut off
#define PROXY_MAX_REDIRECTS   4
#define PROXY_TIMEOUT_MS      12000              // Connecting, and waiting for data

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID             ""
#define WIFI_PASSWORD         ""
#endif

// Many ESP32-C3 SuperMini boards only join Wi-Fi reliably at reduced transmit
// power (antenna matching), so every second try uses 8.5 dBm automatically.
// Uncomment to use one fixed power for all tries instead.
// #define WIFI_TX_POWER      WIFI_POWER_8_5dBm

// Private point-to-point network between the ESP32 and the Palm
#define PPP_GATEWAY_IP        "192.168.99.1"
#define PPP_PALM_IP           "192.168.99.2"
#define PPP_FALLBACK_DNS      "1.1.1.1"  // Used if Wi-Fi did not provide a DNS server
#define PPP_TO_IR_BUF_SIZE    8192       // PPP bytes queued for the (slow) IR link

// ==============================================================================
// Mail (HotSync with a mail account instead of a desktop)
// ==============================================================================
// The Palm's Mail application only exchanges messages through HotSync. When a
// HotSync starts and the USB host does not answer it, the adapter answers it
// itself: it sends the Outbox through SMTP and copies new messages from an
// IMAP inbox to the Palm. The account is entered in the portal (needs the
// Wi-Fi gateway above); with Gmail, sign in with Google or use an "app password".
#define MAIL_PC_ANSWER_TIMEOUT_MS  1500   // A desktop that is listening answers within milliseconds
#define MAIL_ACCEPT_TIMEOUT_MS     10000  // Wait for the Palm's wake-up packet once a session is ours

// Servers until others are entered in the portal ("host" or "host:port").
// Both connections are TLS from the first byte (no STARTTLS).
#define MAIL_DEFAULT_IMAP_SERVER   "imap.gmail.com"
#define MAIL_DEFAULT_SMTP_SERVER   "smtp.gmail.com"
#define MAIL_IMAP_PORT             993
#define MAIL_SMTP_PORT             465
#define MAIL_CONNECT_TIMEOUT_MS    12000

// Per sync: only this many of the newest inbox messages are considered (older
// unsynced ones are passed over for good), and this many Outbox messages sent
#define MAIL_MAX_PER_SYNC          15
#define MAIL_MAX_SEND_PER_SYNC     10
// A message that only exists as HTML: bytes read to find its text
#define MAIL_HTML_FETCH_MAX        96000

// Google sign-in (OAuth) instead of an app password, for Gmail. The owner
// signs in once from a PC on the same Wi-Fi: "Sign in with Google" on the
// Mail page opens the adapter's sign-in page to the Wi-Fi side for this long.
// Request all eight configured Google services together. Existing tokens
// keep their original grants until the owner signs in again.
#include "googlescopes.h"
#define GOOGLE_SCOPES              GOOGLE_DEFAULT_SCOPES
#define GOOGLE_SETUP_WINDOW_MS     (15UL * 60 * 1000)

// Time server; message dates are converted to the time shown by the Palm's clock
#define MAIL_NTP_SERVER            "pool.ntp.org"

// ==============================================================================
// Google Tasks (Palm To Do List, alongside standalone Mail)
// ==============================================================================
#define TASKS_MAX_PER_SYNC         100
#define TASKS_HTTP_TIMEOUT_MS      12000
#define TASKS_HTTP_RESPONSE_MAX    24000

// ==============================================================================
// Debug Logging
// ==============================================================================
// Set to 1 to print a hex trace of every IrLAP frame sent / received on the
// USB CDC port (view with `pio device monitor`, then press HotSync on the Palm).
// CAUTION: The trace replaces the serial data stream on the CDC port, so
// pilot-link / jpilot cannot be used while it is enabled.
// Leave 0 during normal operation.
#define ENABLE_CDC_DEBUG      0
#define DEBUG_LOG_BUF_SIZE    8192

