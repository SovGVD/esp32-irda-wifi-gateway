# Notes for AI coding agents

Firmware for an ESP32-C3 with a Vishay TFBS4711 infrared transceiver that acts as an IrDA adapter for a Palm (IIIx and Palm OS 4 devices). One firmware does three jobs:

- **Desktop HotSync**: the Palm's IrCOMM stream is bridged to USB CDC, so `pilot-link` / `jpilot` on Linux see a serial Palm.
- **Internet for the Palm**: PPP terminated on the ESP32 and routed onto Wi-Fi with NAT, plus a settings portal, DNS, a web proxy and an OpenXiino DataServer port for the Xiino browser.
- **Standalone HotSync**: when no desktop answers, the adapter speaks the desktop side of HotSync itself and runs conduits over Wi-Fi: Mail (IMAP / SMTP, Gmail via XOAUTH2), and Google Tasks, Calendar, Contacts, Docs (Memo Pad), Drive PNG (Note Pad), Sheets (Expense) and Drive backup / restore, all sharing one Google sign-in.

## Where the documentation is

- `README.md`: project overview and features.
- `docs/`: user guides (hardware, building, HotSync, Internet, Mail, `docs/google/*` per service), `docs/configuration.md` (every `config.h` setting) and `docs/troubleshooting.md`.
- `docs/development.md`: architecture, the full per-file source map, what each host test covers, and the **validation status** (which features the owner has confirmed on a real Palm, and which only have host-test coverage). Read it before changing a conduit.
- `third_party/README.md`: provenance and local changes of the vendored code.

When behaviour or a setting changes, update the matching guide in `docs/` in the same change. When a change affects what has been confirmed on hardware, update the validation status in `docs/development.md` and say plainly what still needs a device retest.

## How the code is organised

All sources are flat in `src/` and `include/`; the ESP-IDF component in `src/CMakeLists.txt` globs every `src/*.cpp` plus the libwebp and pngle sources from `third_party/`.

**IrDA stack and routing (ESP32 only).** `irphy` (UART SIR), `irlap`, `irlmp` (IAS), `ircomm` (with TinyTP from `irttp.h`), driven from `loop()` in `main.cpp`. The first byte of each IrCOMM session decides where it goes in `route_palm_data()`: `0x7E` goes to PPP (`irppp`); anything else is bridged to USB, and a HotSync (`0xBE`) that the USB host leaves unanswered for `MAIL_PC_ANSWER_TIMEOUT_MS` is handed to the standalone sync in `irmail.cpp`, which replays the opening bytes.

**Standalone sync.** `irmail.cpp` owns the FreeRTOS task that accepts the HotSync (`hotsync.cpp`: SLP, PADP, CMP, DLP) and then runs each enabled conduit in turn: Drive restore, Mail, Tasks, Calendar, Contacts, Memo / Note, Expenses, Drive backup. `syncdiagnostics` / `irsyncdiag` record stage breadcrumbs and heap figures in RTC memory so a crash can be diagnosed after reset.

**Each Google conduit has the same four layers:**

| Layer | Example (Calendar) | Depends on ESP32? |
|:------|:-------------------|:-----------------|
| `ir<service>.cpp`: settings in NVS (`Preferences`), portal hooks, wiring to the shared HTTPS transport | `ircalendar.cpp` | yes |
| `<service>sync.cpp`: the two-way sync, mappings, conflicts, journals | `calendarsync.cpp` | no |
| `<service>text.cpp`: the Palm database record codec | `calendartext.cpp` | no |
| `google<service>.cpp`: REST client over the `GoogleHttp` interface | `googlecalendar.cpp` | no |

Mail follows the same split with `irmail` / `mailsync` / `mailtext` / `mailnet` (over the abstract `MailConnection`). Drive backup uses `irdrive` / `drivebackup` / `palmbackup` / `googledrive`; Memo and Note Pad share `irnotes` / `notesync` / `notetext` / `noteimage` / `googlenotes`.

**Google sign-in** is shared: `mailoauth.cpp` (consent URL, PKCE, token refresh, XOAUTH2), scopes in `include/googlescopes.h` (`GOOGLE_SCOPES` in `config.h`), and the refresh token stored with the mail settings in `irmail.cpp`. The sign-in pages for a PC are in `irportal.cpp`. `irgooglehttp.cpp` is the one ESP32 HTTPS adapter behind `GoogleHttp` (bounded responses, HotSync keep-alives while waiting).

**Portal and web.** `irportal.cpp` holds the Wi-Fi manager, the HTML / WML portal, DNS, the direct web proxy and the settings pages of every conduit. Xiino support is `xiino.cpp` (HTML conversion), `xiinoimage.cpp` (image decoding to the Xiino palette), `xiinosource.cpp` and `irxiino.cpp` (ESP32 transport), with a bounded allocator in `xiinoalloc.h`.

**Portable code** (everything without an `ir` prefix, apart from `main.cpp`) is plain C++17 with `std::string` / `std::vector` and no Arduino or ESP-IDF includes, so the host tests can build it. Do not include `Arduino.h` or `config.h` (which includes `Arduino.h`) there; pass limits in from the `ir*` adapter instead. Put new logic in the portable layer whenever it can be tested on a computer.

## Building

```bash
pio run              # build the firmware (first build compiles ESP-IDF from source, several minutes)
pio run -t upload    # flash
pio device monitor   # serial monitor (useful with ENABLE_CDC_DEBUG 1)
```

- `framework = arduino, espidf` with `sdkconfig.defaults`, because the prebuilt Arduino SDK lacks PPP, IP forwarding and NAT. Generated `sdkconfig.esp32*` files are git-ignored; change `sdkconfig.defaults` instead.
- `include/wifi_secrets.h` is optional and git-ignored. Never commit credentials, OAuth clients or tokens.
- `partitions.csv` has one 3 MB factory app and no OTA. Watch the image size.
- `src/xiino_assets.inc` and `src/xiino_palette.inc` are generated by `tools/generate_xiino_assets.py`; `test/host/data/xiino_codec_fixtures.h` by `tools/generate_xiino_codec_fixtures.py` (needs Pillow). Regenerate them rather than editing by hand.

## Host-side tests

```bash
test/host/run.sh     # everything; runs test/host/xiino.sh first. About 8 minutes.
sh test/host/xiino.sh   # only the Xiino converter and transport
```

Needs `g++`, `cc`, `python3` and zlib headers. Everything is built with `-fsanitize=address,undefined`. Besides that, `run.sh` needs two sources from the ESP-IDF package that `pio run` downloads:

- **cJSON**: found at `~/.platformio/packages/framework-espidf/components/json/cJSON`, or set `CJSON_DIR`.
- **miniz** from esptool's `flasher_stub`: found next to it, or set `MINIZ_DIR`.

If PlatformIO cannot download (see below), clone them instead; this is enough for the whole suite to pass:

```bash
git clone --depth 1 https://github.com/DaveGamble/cJSON.git /tmp/deps/cJSON
git clone --depth 1 -b v4.7.0 https://github.com/espressif/esptool.git /tmp/deps/esptool
CJSON_DIR=/tmp/deps/cJSON MINIZ_DIR=/tmp/deps/esptool/flasher_stub test/host/run.sh
```

How the tests work:

- Conduits run real HotSyncs against simulated Palms on a pseudo terminal (`fake_palm.py`, `fake_backup_palm.py`, `fake_notes_palm.py`, `fake_expenses_palm.py`) and in-memory Google APIs or scripted servers (`scripted_server.h`, `drive_server.h`).
- The base HotSync requests must equal what real `pilot-link` sent (`test/host/data/pilot-link-requests.txt`), and the sync is repeated with every packet lost once in each direction.
- Whole mail syncs compare the Palm's database afterwards with `test/host/data/mailsync-<scenario>.json`.
- The real ESP32 HTTPS adapters (`irgooglehttp.cpp`, `irxiino.cpp`) are tested against SDK shims in `test/host/esp_http_stubs/`.

A new source file is built into the firmware automatically, but `run.sh` and `xiino.sh` list the files of each test by hand: add it there too. When a change intentionally alters what a conduit writes or requests, update the expected data and say why in the commit.

## Conventions

- Match the file you are editing. Older modules use 4-space indent and spaced code; newer conduit code is denser. Functions are `snake_case` prefixed with their module (`irtasks_sync`, `hotsync_accept`), file-static state is `s_`, macros are `UPPER_CASE`.
- Settings go in `include/config.h` with a comment, and user-visible ones in `docs/configuration.md`.
- Persistent state uses `Preferences` (NVS), one namespace per area (`irda` for Wi-Fi and portal, `irmail`, `irtasks`, `irexpenses`, ...). Keep existing keys readable when changing what is stored; sync state that must survive an interrupted HotSync uses journals.
- Docs, comments, portal pages and HotSync log lines are plain, full English sentences written for the owner of the device.
- Commit messages: a short imperative subject, then a body explaining the user-visible effect and the reason.

## Gotchas

- **No CI.** Nothing checks a pull request automatically. Run `test/host/run.sh` yourself, and `pio run` where you can.
- **PlatformIO downloads may be blocked in sandboxes.** `pip install platformio` works, but the registry (`api.registry.platformio.org`) can be unreachable behind restrictive proxies, so `pio run` fails with `HTTPClientError` and the ESP32 build cannot be checked. Say so plainly when that happens, take extra care with ESP32-only code, and use the cloned cJSON / miniz above for the host tests.
- **Memory is the main constraint.** ESP32-C3, about 400 KB RAM, no PSRAM, and a fragmented heap during TLS (device reports show a largest free block near 38 KB). Avoid whole-body or whole-page copies, stream where possible, and respect the HTTPS response caps and the Xiino allocator. Several past device failures were allocation failures that host tests only caught once a heap model was added.
- **Hardware testing is manual.** Never claim something works on a real Palm unless the owner confirmed it; record it in the validation status in `docs/development.md`.
- **USB carries the raw HotSync stream.** Nothing may print to `Serial` in normal builds; use `irdbg_printf()`, compiled in only with `ENABLE_CDC_DEBUG 1` (which also breaks `pilot-xfer`; ship with `0`).
- **IrLAP timing.** Code on the IR path (`main.cpp`, `irlap`, `irlmp`, `ircomm`, `irphy`) must not block or allocate heavily; long work belongs in the sync or portal tasks.
- **Palm text** is one byte per character (Windows-1252, or 1251 with the Cyrillic option).
- **Google sign-in** uses the owner's own Desktop OAuth client and the loopback redirect `http://127.0.0.1` (the owner pastes the redirected address back). All scopes are requested in one consent (`GOOGLE_DEFAULT_SCOPES`); adding a scope means the owner must sign in again and add it in their Cloud project, so document that in `docs/google/README.md`.
- **The portal** is reachable from the Palm only, except the Google sign-in pages during `GOOGLE_SETUP_WINDOW_MS`. Do not expose other pages to the Wi-Fi side.
- `third_party/` is vendored with small, documented local changes. Record any further change in `third_party/README.md`.
