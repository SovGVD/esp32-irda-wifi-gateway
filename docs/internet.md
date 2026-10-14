# Internet connection and browsing

[Documentation](README.md)

The same firmware also works as a small router: when the Palm opens a network connection over IR, the ESP32-C3 answers the PPP session itself and passes the Palm's traffic onto Wi-Fi (NAT). No Linux host is involved; the adapter only needs USB power. HotSync keeps working unchanged, because the firmware looks at the first byte the Palm sends on each connection (PPP starts with `0x7E`, HotSync with `0xBE`) and routes the session to Wi-Fi or to USB accordingly.

> [!NOTE]
> HotSync (backup and install), the PPP connection, the portal and joining a Wi-Fi network have been used with a real Palm IIIx running the Xiino browser. The web proxy is newer and has so far only been checked in host-side tests, not on hardware.

## 1. Configure the Palm

In **Prefs → Network**:

- **Connection:** "IR to a PC/Handheld".
- **User Name:** anything; **Phone:** leave empty; no login script.
- **Details:** Connection type PPP, IP Address automatic, Query DNS on.

Tap **Connect** with the IR ports aligned. The Palm gets the address `192.168.99.2`, the adapter is `192.168.99.1`.

## 2. Choose the Wi-Fi network from the Palm

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

## 3. Browse

Once the status page shows "connected", the Palm is online. Keep the IR ports aligned while the connection is in use, and tap **Disconnect** in Prefs → Network when done.

Palm-era browsers only speak plain `http://`, and almost every site today answers that with "use https". The adapter therefore has a **web proxy**, on by default:

- The adapter answers every site name with its own address, so the browser's requests come to it.
- It fetches the page over Wi-Fi, following redirects onto HTTPS, and passes the HTML to the Palm.
- On the way it removes scripts, style sheets, pictures and comments, converts UTF-8 text to ISO-8859-1, turns `https://` links into `http://` links (so the browser can follow them), and cuts pages off after 48 KB.
- The site's certificate is not checked. The Palm could not check it either; do not use this for anything that needs to be secure.

The status page has a link to turn the proxy off. Do that for programs that are not web browsers (mail, telnet, IRC): with the proxy on they would be handed the adapter's address instead of their server's. With it off the Palm connects to servers itself through the adapter (NAT), and a browser reaches plain-`http://` sites only.

Sites made for old or small browsers work best, for example `http://frogfind.com/` (search and simplified pages) or `http://68k.news/`.

## Xiino

Xiino by default sends every request to a "DataServer" (`pds.mobirus.com`) that no longer exists, which shows as "Host does not exist" for every address. **Clear the DataServer setting in Xiino's preferences**: Xiino then fetches pages directly and displays them cleanly. If it is left set, the adapter answers in the DataServer's place and browsing still works, but Xiino shows a line of stray characters at the top of every page, because it expects the DataServer's own page format.

## Notes

- "Diagnostics" on the status page shows ESP status: internal heap used/total/free, minimum free heap since boot, largest allocatable block, PSRAM availability, uptime/reset reason, chip/CPU/task details, flash and firmware image size, NVS entry usage, Wi-Fi signal/channel/IP and PPP queue/drop counters. Heap use measures the internal byte-addressable allocator, rather than all physical RAM; NVS counts are entries rather than byte capacities. Choose **Refresh readings** for a new snapshot. It also lists the last names the Palm looked up and what came of each, and can show a browser's request exactly as it arrived. The page uses the existing HTML/WML rendering for Palm browsers.
- A name that does not exist leads to a page from the adapter explaining so, rather than to a browser error.
- Throughput is limited by the IR link (57600 bps, about 5 KB/s).
- If a network only ever connects on the second (reduced power) try, uncomment `WIFI_TX_POWER` in `include/config.h` to use that power from the start.
- Wi-Fi transmit bursts and the IR emitter both load the board's small 3.3 V regulator. If the board resets while transferring, wire the emitter as in [Option B](hardware.md#option-b-separate-emitter-supply-recommended-with-wi-fi) and lower `WIFI_TX_POWER`.
- Set `ENABLE_WIFI_GATEWAY` to `0` in `include/config.h` to switch Wi-Fi off; PPP is then passed to USB like any serial data (for `pppd` on the Linux host).

## Related guides

- [Hardware and wiring](hardware.md)
- [Configuration](configuration.md)
- [Troubleshooting](troubleshooting.md)
