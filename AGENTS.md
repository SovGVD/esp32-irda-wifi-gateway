# Notes for AI coding agents

Firmware for an ESP32-C3 with a Vishay TFBS4711 infrared transceiver that acts as an IrDA adapter for a Palm IIIx. One firmware does three jobs: HotSync to a Linux host over USB CDC, Internet for the Palm (PPP routed onto Wi-Fi with NAT, plus a portal and web proxy), and a built-in Mail conduit that syncs the Palm's Mail application with an IMAP / SMTP account (including Gmail with Google OAuth / XOAUTH2).

`README.md` is the user manual (wiring, setup, every user-visible behaviour, configuration table). Read it before changing behaviour, and update it in the same change when behaviour or settings change.

## Source layout

```
include/config.h      All compile-time settings (pins, baud, buffer sizes, timeouts, feature flags)
src/main.cpp          setup() / loop(): IrLAP state handling, LED, routing of each IrCOMM session
src/irphy.cpp         IrPHY: SIR pulses through the hardware UART, transceiver shutdown pin
src/irlap.cpp         IrLAP: framing, FCS, XID discovery, SNRM, parameter negotiation, ack / retransmit
src/irlmp.cpp         IrLMP and IAS (answers queries for "IrDA:IrCOMM")
include/irttp.h       TinyTP header and credit constants (logic lives in ircomm.cpp)
src/ircomm.cpp        IrCOMM over TinyTP with credit flow control
src/irppp.cpp         PPP server on lwIP towards the Palm, NAT onto Wi-Fi
src/irportal.cpp      Wi-Fi manager (saved networks, retries), HTTP portal (HTML / WML), DNS server,
                      web proxy, Mail settings pages, Google sign-in pages for a PC
src/irmail.cpp        Mail conduit glue on the ESP32: NVS settings, TLS connections (esp_tls), FreeRTOS task
src/hotsync.cpp       Desktop side of HotSync: SLP, PADP, CMP, DLP          (host-testable)
src/mailsync.cpp      The conduit logic: Outbox -> SMTP, IMAP -> Inbox, read / deleted both ways  (host-testable)
src/mailnet.cpp       IMAP and SMTP clients over an abstract MailConnection (host-testable)
src/mailoauth.cpp     Google OAuth 2.0 (PKCE, token refresh, XOAUTH2 strings)  (host-testable)
src/mailtext.cpp      Palm Mail record format, charsets, MIME, HTML to text, headers, dates (host-testable)
src/irdebug.cpp       Protocol trace over USB, enabled with ENABLE_CDC_DEBUG
test/host/            Host-side tests (see below)
udev/                 udev rule for the Linux host (/dev/palm-irda, ModemManager off)
case/                 FreeCAD case design (make_case.py) and its exported STL / STEP / 3MF
```

## How a connection is routed

`main.cpp` runs the IrLAP / IrLMP / IrCOMM stack in `loop()`. When an IrCOMM session opens, the first byte the Palm sends decides where it goes (`route_palm_data()`):

- `0x7E` (PPP flag): the session goes to `irppp` (Wi-Fi gateway), if `ENABLE_WIFI_GATEWAY` is on.
- Anything else, including HotSync (`0xBE`): bridged to USB CDC. If it is a HotSync and the USB host has not answered within `MAIL_PC_ANSWER_TIMEOUT_MS`, the session is handed to the mail conduit (`hand_over_to_mail()`), which replays the opening bytes it saw.

The mail conduit and the portal's HTTP and DNS servers run in their own FreeRTOS tasks; the IrDA stack must never block in `loop()`, because IrLAP turnaround times are tight. Debug output is buffered and drained from `loop()` for the same reason.

## Two kinds of code

1. **ESP32-only** (`main`, `ir*.cpp`): uses Arduino (`String`, `Preferences`, `WiFi`), ESP-IDF (`esp_tls`, lwIP) and FreeRTOS. It can only be checked by building the firmware.
2. **Portable** (`hotsync`, `mailsync`, `mailnet`, `mailoauth`, `mailtext` and their headers): plain C++17 with `std::string` / `std::vector`, no Arduino or ESP-IDF includes. I/O goes through injected interfaces (`HotSync_Io`, `MailConnection`) so the host tests can drive it. Keep it that way: do not include `Arduino.h` or `config.h` (which includes `Arduino.h`) from these files, and put new protocol or text logic here rather than in the `ir*.cpp` glue when it can be tested on a host.

## Building

```bash
pio run              # build the firmware
pio run -t upload    # flash (hold BOOT while plugging in if the upload cannot reach the board)
pio device monitor   # serial monitor (useful with ENABLE_CDC_DEBUG 1)
```

- `framework = arduino, espidf`: ESP-IDF is compiled from source with `sdkconfig.defaults` because the prebuilt Arduino SDK lacks PPP, IP forwarding and NAT. The first build takes several minutes. `sdkconfig.esp32*` files are generated; do not commit them. Change `sdkconfig.defaults` instead.
- The root `CMakeLists.txt` and `src/CMakeLists.txt` exist for the ESP-IDF build PlatformIO runs; new `.cpp` files in `src/` are picked up by the glob.
- `include/wifi_secrets.h` is optional and git-ignored (default `WIFI_SSID` / `WIFI_PASSWORD`). Never commit credentials.
- Flash layout is in `partitions.csv` (one 3 MB factory app, no OTA). Watch the image size when adding large features.

## Host-side tests

```bash
test/host/run.sh     # needs g++ and python3; about a minute
```

It compiles each portable module with `-Wall -Wextra -fsanitize=address,undefined` and runs:

- `mailtext_test`, `mailnet_test`, `mailoauth_test`: unit tests; the mail and Google servers are `ScriptedServer` (`scripted_server.h`), which checks the client's exact bytes and replies in small pieces to exercise parsing across packet boundaries.
- `hotsync_test` against `fake_palm.py` (a simulated Palm on a pseudo terminal). The DLP requests must match `test/host/data/pilot-link-requests.txt` byte for byte (what real `pilot-link` sent), then the sync is repeated with every packet dropped or ignored once.
- `mailsync_test`: whole conduit runs (`normal`, `bad-imap`, `offline`, `oauth`, `changes`). The Palm's database afterwards must equal `test/host/data/mailsync-<scenario>.json`.

When a change intentionally alters what the conduit writes or requests, update the expected data files and say why in the commit. To add a source file to a test, add it to the matching `g++` line in `run.sh`. A new `.cpp` added to `src/` is built into the firmware automatically, but `run.sh` lists its files by hand.

## Conventions

- Match the surrounding code: 4-space indent, `snake_case` functions prefixed with their module (`irppp_start`, `hotsync_accept`), `s_` for file-static state, `UPPER_CASE` macros. Settings belong in `include/config.h` with a comment, and user-visible ones also go in the README configuration table.
- Comments and README text are plain, full English sentences that say what something does and why, written for the owner of the device. Avoid jargon in user-facing text (portal pages, HotSync log lines, README).
- Memory is small (ESP32-C3, ~400 KB RAM, no PSRAM). Prefer fixed buffers and the limits in `config.h` / module headers; mind task stack sizes (TLS needs about 16 KB).
- Persistent settings use `Preferences` (NVS): namespace `irda` for Wi-Fi and portal settings, `irmail` for mail. Keep existing keys readable when changing what is stored.
- USB CDC carries the raw HotSync byte stream. Nothing may print to `Serial` in normal builds; use `irdbg_printf()` (compiled out unless `ENABLE_CDC_DEBUG`).
- The portal is reachable from the Palm (PPP side) only, except the Google sign-in pages during `GOOGLE_SETUP_WINDOW_MS`. Do not expose other pages to the Wi-Fi side.
- Commit messages: a short imperative subject, then a body explaining the user-visible effect and the reason.

## Gotchas

- **No CI.** Nothing checks a pull request automatically. Run `test/host/run.sh` yourself and, where possible, `pio run`.
- **PlatformIO downloads may be blocked in sandboxes.** `pip install platformio` works, but `pio run` then needs to download the espressif32 platform and toolchains, which fails behind restrictive network proxies (`HTTPClientError`). If you cannot build, say so plainly, and be extra careful with ESP32-only code: check APIs against the ESP-IDF / Arduino-ESP32 versions the platform uses.
- **Hardware testing is manual.** Many features (mail sync, web proxy) have only been checked in host tests, as noted in the README. Do not claim something works on a real Palm unless the user has confirmed it.
- `ENABLE_CDC_DEBUG 1` replaces the HotSync serial stream with the trace; `pilot-xfer` will not work in that build. Ship with `0`.
- IrLAP timing: code on the IR path (`main.cpp`, `irlap`, `irlmp`, `ircomm`, `irphy`) must not block or allocate heavily; long work goes in the mail or portal tasks.
- The Palm's text is one byte per character (Windows-1252, or 1251 with the Cyrillic option); Mail bodies are limited to 8000 characters (`MAILSYNC_BODY_MAX`).
- Gmail OAuth uses the owner's own Desktop OAuth client and the loopback redirect `http://127.0.0.1` (the user pastes the redirected address back). The scope must stay `https://mail.google.com/`: IMAP and SMTP accept no narrower scope.
