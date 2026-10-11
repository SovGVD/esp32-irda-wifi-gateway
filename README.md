# ESP32-C3 IrDA Adapter for the Palm IIIx: HotSync over USB, Internet and Mail over Wi-Fi

This project turns an **ESP32-C3** paired with a **Vishay TFBS4711** infrared transceiver into an **IrDA adapter** for vintage IrDA devices like the **Palm IIIx**. One firmware does three jobs:

- **HotSync with a modern Linux computer.** The adapter shows up as a USB serial port, and `pilot-link` / `jpilot` talk to the Palm through it as if it sat in a serial cradle.
- **Internet access for the Palm.** The adapter answers the Palm's network connection itself and routes it onto Wi-Fi. The Wi-Fi network is chosen from a small web portal opened on the Palm.
- **E-mail in the Palm's Mail application.** When a HotSync finds no computer, the adapter performs it itself and exchanges the Mail application's messages with a mail account (IMAP / SMTP, for example Gmail) over Wi-Fi.

```
                                                     +---------------+      USB CDC ACM      +------------------+
                                                     |               | <===================> |   Linux Host     |
+----------------+        IrDA SIR (IR Light)        | ESP32-C3 with |    (/dev/ttyACM0)     | (pilot-xfer,     |
|   Palm IIIx    | <===============================> |   TFBS4711    |                       |     jpilot)      |
| (HotSync or    |      9600 - 57600 baud            |  Transceiver  |         Wi-Fi         +------------------+
|  Network / PPP)|   (IrLAP / IrLMP / IrCOMM)        |               | <===================>     Internet
+----------------+                                   +---------------+
```

---

## Why This Project is Needed

Modern Linux kernels (starting with kernel 4.17 in 2018) completely removed the kernel IrDA subsystem (`irda.ko`, `ircomm`, `irattach`). Legacy USB-IrDA dongles (like KingSun or ACTiSYS) no longer work out of the box because there is no driver to handle the IrLAP/IrLMP/IrCOMM layers.

**This project solves that entirely in firmware:**
1. The **ESP32-C3** handles the IrDA protocol stack on-chip:
   - **IrPHY**: 3/16th bit-time SIR optical pulse encoding/decoding via hardware UART.
   - **IrLAP**: Device discovery (`XID`), link connection (`SNRM`), parameter negotiation, acknowledgment and retransmission.
   - **IrLMP / IAS**: Information Access Service responding to queries for `"IrDA:IrCOMM"`.
   - **TinyTP & IrCOMM**: Emulates a serial port over IR, with credit-based flow control.
2. For HotSync, the ESP32-C3 enumerates as a native USB CDC ACM serial device (`/dev/ttyACM0` or `/dev/palm-irda`) on Linux. To `pilot-link` (`pilot-xfer`) or `jpilot`, the Palm looks like a standard wired serial Palm.
3. For networking, the ESP32-C3 terminates the Palm's PPP session and forwards its traffic to Wi-Fi with NAT, so no computer is needed at all.
4. For mail, the ESP32-C3 speaks the desktop's side of HotSync itself (SLP, PADP, CMP and DLP, the protocols `pilot-link` implements) and acts as the Mail conduit, with an IMAP and an SMTP client on the Wi-Fi side.

---

## What You Need

**Parts**
- ESP32-C3 SuperMini board (other ESP32-C3 boards work with different pin numbers in `include/config.h`).
- Vishay TFBS4711 IrDA transceiver.
- 100 nF ceramic capacitor (recommended, close to the transceiver).
- For the stronger emitter supply (Option B below): one 15 Ω resistor and one 4.7 µF - 10 µF capacitor.
- USB-C cable.

**Software**
- [PlatformIO Core (CLI)](https://platformio.org/install/cli) or VS Code with the PlatformIO IDE extension, to build and flash.
- For HotSync: a Linux computer with `pilot-link` (`sudo apt-get install pilot-link`) or `jpilot`.

**On the Palm**
- The connection type **"IR to a PC/Handheld"** must be available (Palm OS 3.3 and later include it).
- For Internet access: a browser or other network application that connects directly over TCP/IP.

---

## Hardware Wiring

### ESP32-C3 SuperMini Pinout & Connections

The **ESP32-C3 SuperMini** board layout makes connecting the TFBS4711 very simple, as GPIO 5, 6, and 7 are adjacent on the right header:

```
                  ESP32-C3 SuperMini Pinout
                       +-----------+
                       |  [USB-C]  |
             5V   --- [|           |] ---  GPIO 5  -------> TFBS4711 Pin 4 (SD)
            GND   --- [|           |] ---  GPIO 6  -------> TFBS4711 Pin 2 (TXD)
            3V3   --- [|           |] ---  GPIO 7  <------- TFBS4711 Pin 3 (RXD)
         GPIO 0   --- [|           |] ---  GPIO 8  (Onboard Blue LED, Active Low)
         GPIO 1   --- [|           |] ---  GPIO 9  (Boot Button)
         GPIO 2   --- [|           |] ---  GPIO 10
         GPIO 3   --- [|           |] ---  GPIO 20
         GPIO 4   --- [|           |] ---  GPIO 21
                       +-----------+
```

### Vishay TFBS4711 Pinout

| Pin | Name | Description | Connection |
|:---:|:----:|:------------|:-----------|
| **1** | **VCC2 / IRED Anode** | IR Emitter Power | **3V3** directly (Option A), or **5V** through a 15 Ω resistor (Option B). **Do NOT connect to 5V without a resistor!** |
| **2** | **TXD** | Transmitter Data Input | **GPIO 6** (Active HIGH optical pulse input) |
| **3** | **RXD** | Receiver Data Output | **GPIO 7** (Uses ESP32 internal pull-up, no external resistor required) |
| **4** | **SD** | Shutdown (Active HIGH) | **GPIO 5** (Firmware drives LOW to activate transceiver) |
| **5** | **VCC1** | Logic Supply (2.4V - 3.6V) | **3V3** |
| **6** | **GND** | Ground | **GND** |

### Option A: Minimal Wiring (no external parts)

Six wires and nothing else: both supply pins of the transceiver go to 3V3.

```
                 +-----------------------------+
                 |     ESP32-C3 SuperMini      |
                 |                             |
                 |      GPIO 5 (Right Pin 1)   |--------> SD   (Pin 4)
                 |      GPIO 6 (Right Pin 2)   |--------> TXD  (Pin 2)
                 |      GPIO 7 (Right Pin 3)   |<-------- RXD  (Pin 3)
                 |                             |
                 |         3V3 (Left Pin 3)    |---+----> VCC1 (Pin 5)
                 |                             |   +----> VCC2 / IRED Anode (Pin 1)
                 |                             |
                 |         GND (Left Pin 2)    |--------> GND  (Pin 6)
                 +-----------------------------+
```

### Option B: Separate Emitter Supply (recommended with Wi-Fi)

The IR emitter draws short, high current pulses. Feeding it from the 5V pin through a resistor, with a capacitor next to the transceiver, keeps those pulses off the board's small 3.3 V regulator, which also has to supply the Wi-Fi radio.

```
                 +-----------------------------+
                 |     ESP32-C3 SuperMini      |
                 |                             |
                 |      GPIO 5 (Right Pin 1)   |--------> SD  (Pin 4)
                 |      GPIO 6 (Right Pin 2)   |--------> TXD (Pin 2)
                 |      GPIO 7 (Right Pin 3)   |<-------- RXD (Pin 3)
                 |                             |
                 |         3V3 (Left Pin 3)    |---+----> VCC1 (Pin 5)
                 |                             |   |
                 |                             |  --- 100nF Ceramic
                 |                             |  ---
                 |                             |   |
                 |         GND (Left Pin 2)    |---+----> GND  (Pin 6)
                 +-----------------------------+   |
                                                   |
                     5V (Left Pin 1)               |
                          |                        |
                        [ 15 Ohm ]                 |
                          |                        |
                          +------------------------+----> VCC2 / IRED Anode (Pin 1)
                          |                        |
                        ===== 4.7uF - 10uF         |
                          |                        |
                         GND                      GND
```

---

## Building and Flashing

### 1. Build the Firmware
```bash
pio run
```
The first build downloads ESP-IDF and compiles it from source (see `sdkconfig.defaults`), which takes a few minutes; the Wi-Fi gateway needs PPP and NAT support that the prebuilt Arduino SDK does not include. Later builds are fast.

### 2. Flash to ESP32-C3
Connect the ESP32-C3 to your computer via USB:
```bash
pio run -t upload
```
If the upload cannot reach the board, hold the **BOOT** button while plugging in the USB cable and try again.

After flashing, the blue LED blinks slowly: the adapter is waiting for the Palm.

---

## Linux Host Setup (for HotSync)

### 1. Configure udev Rules
By default, Linux's `ModemManager` attempts to probe newly attached `/dev/ttyACM*` serial devices with AT commands. This will corrupt the Palm HotSync handshake.

Install the provided udev rule to disable ModemManager probing and create a convenient `/dev/palm-irda` symlink:

```bash
sudo cp udev/99-esp32-irda.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Ensure your user is part of the `dialout` group:
```bash
sudo usermod -aG dialout $USER
```
*(Log out and back in if you were not already in the dialout group).*

### 2. Verify Connection
Plug in the ESP32-C3 and check `dmesg`:
```bash
dmesg | tail -n 20
```
You should see:
```text
usb 1-1: new full-speed USB device number ... using xhci_hcd
cdc_acm 1-1:1.0: ttyACM0: USB ACM device
```
And `/dev/palm-irda` should point to the device:
```bash
ls -l /dev/palm-irda
```
Without the udev rule, use `/dev/ttyACM0` in the commands below.

---

## Performing a HotSync with Palm IIIx

### 1. Configure the Palm IIIx
1. On your Palm IIIx, open the **HotSync** app from the launcher.
2. Tap the connection dropdown below the HotSync icon (default is usually "Cradle / Cable").
3. Select **"IR to a PC/Handheld"**.

### 2. Start pilot-link on Linux
Start the command first; it waits for the Palm:
```bash
pilot-xfer -p /dev/palm-irda -b ./palm_backup
```
`pilot-xfer` will display:
```text
Listening for incoming connection on /dev/palm-irda...
```

### 3. Initiate HotSync
1. Align the Palm IIIx infrared port with the TFBS4711 transceiver (distance between 10 cm and 30 cm).
2. Tap the **HotSync icon** on the Palm screen.
3. The Palm and ESP32-C3 will discover each other (`XID`), connect (`SNRM` / `UA`), negotiate IrCOMM, and begin transferring databases.
4. The onboard LED on the ESP32-C3 will turn solid ON and flicker during active data transfer.

### Other Useful Commands
Each one is started the same way: run the command, then tap HotSync on the Palm.

| Task | Command |
|:-----|:--------|
| Back up everything | `pilot-xfer -p /dev/palm-irda -b ./palm_backup` |
| Install an application or database | `pilot-xfer -p /dev/palm-irda -i game.prc` |
| List what is on the Palm | `pilot-xfer -p /dev/palm-irda -l` |
| Restore a backup | `pilot-xfer -p /dev/palm-irda -r ./palm_backup` |

---

## Internet for the Palm over Wi-Fi

The same firmware also works as a small router: when the Palm opens a network connection over IR, the ESP32-C3 answers the PPP session itself and passes the Palm's traffic onto Wi-Fi (NAT). No Linux host is involved; the adapter only needs USB power. HotSync keeps working unchanged, because the firmware looks at the first byte the Palm sends on each connection (PPP starts with `0x7E`, HotSync with `0xBE`) and routes the session to Wi-Fi or to USB accordingly.

> [!NOTE]
> HotSync (backup and install), the PPP connection, the portal and joining a Wi-Fi network have been used with a real Palm IIIx running the Xiino browser. The web proxy is newer and has so far only been checked in host-side tests, not on hardware.

### 1. Configure the Palm
In **Prefs → Network**:
- **Connection:** "IR to a PC/Handheld".
- **User Name:** anything; **Phone:** leave empty; no login script.
- **Details:** Connection type PPP, IP Address automatic, Query DNS on.

Tap **Connect** with the IR ports aligned. The Palm gets the address `192.168.99.2`, the adapter is `192.168.99.1`.

### 2. Choose the Wi-Fi network from the Palm
Open **`http://wifi.setup/`** (or `http://192.168.99.1/`) in a browser on the Palm. The portal shows the Wi-Fi status and lets you scan for networks, pick one and enter its password. The pages are a few hundred bytes of plain HTML, or WML for browsers that ask for it.

Up to five networks are remembered on the ESP32. Whenever it is not connected it scans and joins whichever saved network is in range, strongest first, so the adapter can move between home, office and a phone hotspot without being set up again.

Only 2.4 GHz networks can be used (the ESP32-C3 has no 5 GHz radio), so a 5 GHz-only network never appears in the list. The list shows each network's security: open, WPA, WPA2 and WPA3 personal networks work; enterprise logins do not.

Each saved network on the status page is a link to its details: the outcome of the last attempt, the stored password in plain text (to spot a Graffiti slip; the portal is reachable from the Palm only), a field to enter the password again, and "Forget this network".

The status page shows what the adapter is doing ("looking for saved networks", "connecting to X (try 1 of 2)", "not connected, trying again in 15 s") and, for each saved network, how the last attempt ended:

| Shown next to a saved network | Meaning |
|:------------------------------|:--------|
| connected | In use now |
| refused, wrong password? (code N) | The access point did not complete the handshake: check the stored password on the network's details page. Codes 15 and 204 are handshake timeouts, 202 is an authentication failure |
| not found (code 201) / not in range | The network was not seen |
| no answer | The access point did not respond in time; usually a weak signal |
| failed (code N) | Any other reason code reported by the Wi-Fi driver |

Each network gets two tries, the second at reduced transmit power, because many ESP32-C3 SuperMini boards cannot complete the handshake at full power.

- The portal is only reachable from the Palm, never from the Wi-Fi side.
- The portal works before any Wi-Fi network is configured. While Wi-Fi is not connected, every address typed into the browser leads to the portal.
- WAP browsers that need a WAP gateway (WSP on UDP port 9200) are not supported; the browser must be able to fetch `http://` URLs directly.
- A default network can also be compiled in: define `WIFI_SSID` and `WIFI_PASSWORD` in `include/wifi_secrets.h` (ignored by git).

### 3. Browse
Once the status page shows "connected", the Palm is online. Keep the IR ports aligned while the connection is in use, and tap **Disconnect** in Prefs → Network when done.

Palm-era browsers only speak plain `http://`, and almost every site today answers that with "use https". The adapter therefore has a **web proxy**, on by default:

- The adapter answers every site name with its own address, so the browser's requests come to it.
- It fetches the page over Wi-Fi, following redirects onto HTTPS, and passes the HTML to the Palm.
- On the way it removes scripts, style sheets, pictures and comments, converts UTF-8 text to ISO-8859-1, turns `https://` links into `http://` links (so the browser can follow them), and cuts pages off after 48 KB.
- The site's certificate is not checked. The Palm could not check it either; do not use this for anything that needs to be secure.

The status page has a link to turn the proxy off. Do that for programs that are not web browsers (mail, telnet, IRC): with the proxy on they would be handed the adapter's address instead of their server's. With it off the Palm connects to servers itself through the adapter (NAT), and a browser reaches plain-`http://` sites only.

Sites made for old or small browsers work best, for example `http://frogfind.com/` (search and simplified pages) or `http://68k.news/`.

### Xiino
Xiino by default sends every request to a "DataServer" (`pds.mobirus.com`) that no longer exists, which shows as "Host does not exist" for every address. **Clear the DataServer setting in Xiino's preferences**: Xiino then fetches pages directly and displays them cleanly. If it is left set, the adapter answers in the DataServer's place and browsing still works, but Xiino shows a line of stray characters at the top of every page, because it expects the DataServer's own page format.

### Notes
- "Diagnostics" on the status page lists the last names the Palm looked up and what came of each, and can show a browser's request exactly as it arrived. Useful when a browser behaves unexpectedly.
- A name that does not exist leads to a page from the adapter explaining so, rather than to a browser error.
- Throughput is limited by the IR link (57600 bps, about 5 KB/s).
- If a network only ever connects on the second (reduced power) try, uncomment `WIFI_TX_POWER` in `include/config.h` to use that power from the start.
- Wi-Fi transmit bursts and the IR emitter both load the board's small 3.3 V regulator. If the board resets while transferring, wire the emitter as in Option B and lower `WIFI_TX_POWER`.
- Set `ENABLE_WIFI_GATEWAY` to `0` in `include/config.h` to switch Wi-Fi off; PPP is then passed to USB like any serial data (for `pppd` on the Linux host).

---

## Mail on the Palm

The Palm's built-in **Mail** application has no network code: it only exchanges messages during a HotSync, through a "conduit" on the desktop. The adapter can be that desktop. When a HotSync starts and no program on the USB host answers it, the adapter takes the HotSync over and synchronises Mail with a mail account over Wi-Fi:

- Messages in the **Outbox** are sent (SMTP) and then removed from the Palm.
- Messages that arrived in the account's inbox since the last sync are copied to the Palm's **Inbox** (IMAP).

No computer is involved; the adapter only needs USB power and a saved Wi-Fi network.

> [!NOTE]
> The mail function has so far only been checked in host-side tests: against a simulated Palm, against `pilot-link`, and against scripted mail servers (see "Host-Side Tests"). It has not yet been used with a real Palm or a real mail server.

### 1. Enter the account
Connect the Palm to the network as described above, open **`http://wifi.setup/`** and follow **Mail**:

- **Gmail:** sign in with Google from a PC, once (see "Sign in with Google" below). This needs no app password.
- **Enter the account** (any provider, or Gmail with an app password): e-mail address, password, and the name recipients should see.
- With **Gmail** a password must be an *app password*: Google account → Security → 2-Step Verification → App passwords. Google refuses the normal account password here. It is 16 lower-case letters. Accounts without 2-Step Verification, with Advanced Protection, or managed by an organisation that blocks them cannot create one: use "Sign in with Google" instead.
- For **other providers**, open "Servers" and enter the IMAP and SMTP server names. Both are contacted with TLS from the first byte, on ports 993 and 465 (another port can be given as `server:port`; STARTTLS on ports 143 / 587 is not supported).
- **Check the login now** tries both servers and shows what they answer.

Then tap **Disconnect** in Prefs → Network: HotSync needs the infrared link for itself.

### Sign in with Google (Gmail)
The adapter logs in to Gmail with Google's own sign-in (OAuth, SASL XOAUTH2) instead of a password. Google's sign-in page needs a modern browser, so this is done once from a PC on the same Wi-Fi network. Each owner uses their own Google "OAuth client": an app used only by its owner needs no review by Google.

**A. Create the OAuth client** (once, at [console.cloud.google.com](https://console.cloud.google.com/), signed in with any Google account):
1. Create a project (top bar → project list → New project), for example "Palm mail".
2. APIs & Services → Library → **Gmail API** → Enable.
3. Google Auth Platform (or "OAuth consent screen") → Get started: an app name, your e-mail address as support and contact address, Audience **External**.
4. Audience → **Publish app**, so that its status is **In production**. Leave it unverified. In "Testing" status Google ends the sign-in after 7 days.
5. Clients → Create client → Application type **Desktop app** → Create. Keep the **Client ID** and **Client secret** it shows.

**B. Sign in:**
1. On the Palm, on the portal's Mail page, tap **Gmail: sign in with Google (from a PC)**. The page shows an address such as `http://192.168.1.50/google`. For the next 15 minutes the adapter answers that page on the Wi-Fi side; at all other times only the Palm reaches the portal.
2. Open that address on the PC. Enter the Gmail address, the client ID and the client secret, and press **Continue**.
3. Press **Open Google's sign-in page**, choose the Gmail account and allow access. Google warns that it "hasn't verified this app": it is your own client, so choose Continue (under "Advanced" in some versions).
4. Google then sends the browser to `http://127.0.0.1/?state=...&code=...`, a page that does not load. That is expected: copy the whole address from the address bar, paste it into the adapter's page and press **Finish**.
5. The page then logs in to both servers and shows the outcome. Mail sync is on; tap HotSync on the Palm.

**Notes:**
- The adapter keeps the "refresh token" Google gives it, and gets a fresh access token from Google at the start of each sync. The token, client ID and secret are stored unencrypted in the ESP32's flash, like the Wi-Fi passwords. The token gives full access to the mailbox (IMAP and SMTP accept no narrower permission).
- Google ends a sign-in when the account's password is changed, when access is removed (Google account → Security → Your connections to third-party apps), when it has not been used for six months, or after 7 days if the app is still in "Testing". The HotSync log then says `Google sign-in no longer valid, sign in again from a PC`: repeat step B.
- **Sign out of Google** on the Mail page makes the adapter forget the token; the account's own security page removes the access on Google's side.
- Changing the e-mail address on the Palm ends the Google sign-in (it belonged to the other account).

### 2. Synchronise
In the **HotSync** application choose "IR to a PC/Handheld" and tap the HotSync icon, exactly as for a desktop sync.

- The adapter passes the HotSync to USB first. A desktop program that is waiting (`pilot-xfer`, `jpilot`) answers within milliseconds and gets the session, as before.
- If nothing answers within 1.5 seconds, the adapter does the mail sync itself.

The outcome is written to the **HotSync log** on the Palm (HotSync → Log), for example `Mail: 1 sent, 3 received`, or the reason something failed. The portal's Mail page shows the same line.

### What is synchronised
- The choice under **Options → HotSync Options** in the Mail application is respected: All, Send only, Filter, Unread, and the Truncate length.
- Only the 15 newest messages of the inbox are looked at in one sync. If more than that arrived since the last sync, the older ones are passed over for good. The first sync fetches the 15 newest.
- Messages arrive as text. Of a message with several parts the plain text is taken; a message that only exists as HTML is reduced to its text; attachments are left out.
- Text is converted to the Palm's character set (Windows-1252). Accented letters it lacks become plain letters, other scripts become `?`. With a Cyrillic localisation on the Palm, switch "Text" on the Mail page to Cyrillic (Windows-1251).
- **Nothing is changed on the server.** The mailbox is opened read-only: reading or deleting a message on the Palm does not mark or delete it in the account. A message already read elsewhere arrives marked as read.
- A sent message leaves the Outbox once the server has accepted it. Gmail keeps its own copy under "Sent"; other providers may not.
- Messages deleted on the Palm are purged from its memory during the sync.
- The signature set in Mail's preferences is appended to messages that ask for it.
- Dates are shown in the Palm's time. The adapter gets the time from the network and takes the time zone from the Palm's clock, so set that clock correctly.

### Notes
- The password (or the Google sign-in token) is stored unencrypted in the ESP32's flash, like the Wi-Fi passwords. Both can be revoked at any time; a mail account used only for the Palm limits the exposure further.
- The mail servers' certificates are checked (unlike the web proxy's).
- Keep the IR ports aligned until the Palm reports that the HotSync is complete.
- "Fetch the newest messages again" on the Mail page makes the next sync start over, for example after emptying the Palm's Inbox.
- To sync with a computer while mail is set up, start `pilot-xfer` / `jpilot` **before** tapping HotSync, or turn mail sync off on the Mail page.

---

## Status LED

| LED | Meaning |
|:----|:--------|
| Slow blink (once per second) | Idle, waiting for the Palm |
| Solid on | IR link to the Palm is established |
| Flicker | Frames or data are being transferred |

---

## Configuration

All settings are in `include/config.h`.

| Setting | Default | Purpose |
|:--------|:--------|:--------|
| `PIN_TFBS_TX` / `PIN_TFBS_RX` / `PIN_TFBS_SD` | 6 / 7 / 5 | GPIOs wired to the transceiver |
| `PIN_STATUS_LED` | 8 | Status LED (`-1` for none); `LED_ON` / `LED_OFF` set its polarity |
| `IRDA_TARGET_BAUD` | 57600 | Highest IR speed to negotiate with the Palm |
| `IRDA_DEVICE_NAME` | `ESP32-IrCOMM` | Name the Palm sees during discovery |
| `ENABLE_WIFI_GATEWAY` | 1 | Wi-Fi Internet gateway and portal on / off |
| `PORTAL_DOMAIN` | `wifi.setup` | Name of the settings portal |
| `WIFI_SSID` / `WIFI_PASSWORD` | empty | Default Wi-Fi network until one is chosen in the portal |
| `WIFI_TX_POWER` | not set | Reduced Wi-Fi transmit power for boards that need it |
| `PROXY_ALL_SITES_DEFAULT` | 1 | Web proxy on or off until changed in the portal |
| `PROXY_MAX_PAGE_BYTES` | 48000 | Longer pages are cut off |
| `XIINO_DATASERVER_HOST` | `pds.mobirus.com` | Name under which the adapter stands in for Xiino's DataServer |
| `PPP_GATEWAY_IP` / `PPP_PALM_IP` | `192.168.99.1` / `.2` | Addresses on the link between adapter and Palm |
| `MAIL_PC_ANSWER_TIMEOUT_MS` | 1500 | How long a HotSync waits for a desktop on USB before it becomes a mail sync |
| `MAIL_DEFAULT_IMAP_SERVER` / `MAIL_DEFAULT_SMTP_SERVER` | Gmail's | Mail servers until others are entered in the portal |
| `MAIL_MAX_PER_SYNC` / `MAIL_MAX_SEND_PER_SYNC` | 15 / 10 | Newest inbox messages looked at, and Outbox messages sent, per sync |
| `MAIL_HTML_FETCH_MAX` | 96000 | Bytes read of a message that only exists as HTML |
| `GOOGLE_SCOPES` | `https://mail.google.com/` | Access asked for when signing in with Google |
| `GOOGLE_SETUP_WINDOW_MS` | 15 minutes | How long the sign-in page answers on the Wi-Fi side once opened from the Palm |
| `MAIL_NTP_SERVER` | `pool.ntp.org` | Time server (for message dates) |
| `ENABLE_CDC_DEBUG` | 0 | Print a protocol trace on USB instead of serial data |
| `CURRENT_OP_MODE` | `MODE_IRCOMM` | `MODE_RAW_SIR` turns the adapter into a plain IR pulse bridge without the IrDA stack |

---

## Troubleshooting

- **No optical response / Discovery fails:**
  - Verify that TFBS4711 Pin 4 (SD) is at 0V (GND/LOW). When SD is HIGH (3.3V), the transceiver is in shutdown.
  - Verify TXD (Pin 2) is connected to GPIO 6 and RXD (Pin 3) is connected to GPIO 7.
  - Check distance and optical alignment: IrDA is directional (typically $\pm 15^\circ$ cone). Keep the Palm and transceiver within 10 to 40 cm.
- **Connection starts then drops:**
  - Avoid bright ambient infrared interference (direct sunlight, incandescent desk lamps, or halogen bulbs directly over the optical transceiver).
  - Wire the emitter as in Option B: the $4.7\ \mu\text{F} - 10\ \mu\text{F}$ capacitor on Pin 1 (IRED Anode) prevents voltage dips.
- **Seeing what the Palm and the adapter say to each other:**
  - Set `ENABLE_CDC_DEBUG` to `1` in `include/config.h`, flash, run `pio device monitor` and tap HotSync (or Connect) on the Palm. Do not start `pilot-xfer`; the trace takes over the serial port.
  - Every IrLAP frame is printed in hex: `RX` from the Palm, `TX` from the adapter, `RX bad FCS` for corrupted frames. Wi-Fi and PPP state changes are printed as well.
  - Only `RX ff 3f ...` lines (discovery) repeating, with a `TX fe bf ...` after each first one but never an `RX ff 93 ...` (SNRM): the Palm does not hear the adapter, so check the TXD wiring and the IRED supply.
  - `LAP connected` followed by `IrCOMM connected`: the link is up, set the flag back to `0` and run `pilot-xfer`.
- **Pilot-link reports "Connection refused" or garbage bytes:**
  - Verify ModemManager is not interfering (`sudo systemctl stop ModemManager` to test).
  - Ensure `ENABLE_CDC_DEBUG` is set to `0` in `include/config.h` so debug strings don't mix into the binary data stream.
- **The Palm connects to the network but no page loads:**
  - Open `http://192.168.99.1/` first. If the portal appears, the IR and PPP side is fine; check the Wi-Fi line on the status page, then "Diagnostics" for the name lookups.
  - "Host does not exist" for every address in Xiino: see the Xiino section above.
  - "Cannot Open Page" from the adapter names the reason (could not connect, no answer, too many redirects).
  - If the portal does not appear either, the browser may need a WAP gateway; try one that fetches `http://` URLs directly.

- **A HotSync meant for the computer turned into a mail sync:**
  - The desktop program was not waiting yet. Start `pilot-xfer` (or the sync in `jpilot`) first, then tap HotSync; or turn mail sync off on the portal's Mail page.
- **The mail sync reports a problem (HotSync log on the Palm, or the portal's Mail page):**
  - `IMAP login: ...` / `SMTP login: ...`: the server refused the password. With Gmail it must be an app password, or use "Sign in with Google"; "Check the login now" on the Mail page tests it without a HotSync.
  - `Google sign-in no longer valid, sign in again from a PC`: Google ended the sign-in (see "Sign in with Google", Notes). `Google sign-in: invalid_client`: the client ID or secret is wrong, or the client was deleted.
  - `cannot connect to ...`: wrong server name, a server without TLS on that port, or a certificate that cannot be verified.
  - `Wi-Fi is not connected`: check the status page of the portal.
- **The Palm reports a HotSync connection error with mail set up:**
  - Set `ENABLE_CDC_DEBUG` to `1` and watch the trace: lines starting with `Mail:` show each step of the sync, and the free memory at its start.

---

## Host-Side Tests

The parts that do not need the ESP32 can be tried on a computer (needs `g++` and `python3`):

```bash
test/host/run.sh
```

- **HotSync protocols:** the firmware's code talks to a simulated Palm (`test/host/fake_palm.py`) over a pseudo terminal. The requests it sends must be byte for byte those that `pilot-link` sent to the same simulated Palm for the same steps (recorded in `test/host/data/`). The sync is repeated with every single packet lost once, in either direction.
- **Mail text:** records of the Mail database, character sets, MIME encodings, HTML to text, headers, dates.
- **IMAP and SMTP clients:** against scripted servers that answer as Gmail does, with the answers arriving whole and one byte at a time; with passwords and with Google sign-in (XOAUTH2).
- **Google sign-in:** the consent address, the PKCE challenge (RFC 7636 example), taking the code from a pasted address, and the token server's answers (tokens, revoked token, wrong client).
- **Whole mail syncs:** the conduit between the simulated Palm and the scripted servers, with a password and with Google sign-in; what the Palm holds afterwards is compared with the expected contents.

---

## Project Structure

```
irda-esp32/
├── platformio.ini         # PlatformIO environment (Arduino on top of ESP-IDF, native USB CDC)
├── sdkconfig.defaults     # ESP-IDF options: PPP, IP forwarding, NAT, quiet USB port
├── partitions.csv         # Flash layout (3 MB application, settings storage)
├── CMakeLists.txt         # ESP-IDF project file used by the PlatformIO build
├── include/
│   ├── config.h           # Pin assignments, baud rates, Wi-Fi gateway and debug settings
│   ├── ringbuffer.h       # Circular FIFO for serial stream bridging
│   ├── irphy.h            # Physical layer (ESP32-C3 UART1 IrDA driver)
│   ├── irlap.h            # IrLAP link protocol (framing, CRC16 FCS, discovery)
│   ├── irlmp.h            # IrLMP multiplexer and IAS responder
│   ├── irttp.h            # Tiny Transport Protocol
│   ├── ircomm.h           # IrCOMM serial stream emulation
│   ├── irppp.h            # PPP session towards the Palm, routed onto Wi-Fi
│   ├── irportal.h         # Wi-Fi settings portal
│   ├── irmail.h           # Mail: HotSync sessions served by the adapter itself
│   ├── hotsync.h          # HotSync from the desktop's side (SLP, PADP, CMP, DLP)
│   ├── mailsync.h         # The mail conduit (Outbox to SMTP, IMAP inbox to Inbox)
│   ├── mailnet.h          # IMAP and SMTP clients
│   ├── mailoauth.h        # Google sign-in (OAuth tokens, XOAUTH2)
│   ├── mailtext.h         # Mail records, character sets, MIME, headers, dates
│   └── irdebug.h          # Protocol trace
├── src/
│   ├── irphy.cpp          # Hardware UART IrDA direction switching and turn-around
│   ├── irlap.cpp          # Framing, CRC calculation, acknowledgment and retransmission
│   ├── irlmp.cpp          # Information Access Service queries (IrDA:IrCOMM)
│   ├── ircomm.cpp         # Serial payload wrapping/unwrapping, TinyTP flow control
│   ├── irppp.cpp          # PPP over IrCOMM, NAT onto the Wi-Fi interface
│   ├── irportal.cpp       # Wi-Fi manager, web portal (HTML / WML), web proxy and DNS for the Palm
│   ├── irmail.cpp         # Mail settings, conduit task, TLS connection to the mail servers
│   ├── hotsync.cpp        # HotSync protocols, as pilot-link speaks them
│   ├── mailsync.cpp       # Steps of a mail sync
│   ├── mailnet.cpp        # IMAP and SMTP
│   ├── mailoauth.cpp      # Google's token server, consent address, PKCE
│   ├── mailtext.cpp       # Conversion between Mail records and Internet mail
│   ├── irdebug.cpp        # Protocol trace buffer
│   └── main.cpp           # IrLAP state machine; routes each session to USB, Wi-Fi or mail
├── test/host/             # Tests that run on a computer (see "Host-Side Tests")
└── udev/
    └── 99-esp32-irda.rules# Linux udev rules (disables ModemManager probing)
```
