# Internet connection and browsing

[Documentation](README.md)

The same firmware also works as a small router: when the Palm opens a network connection over IR, the ESP32-C3 answers the PPP session itself and passes the Palm's traffic onto Wi-Fi (NAT). No Linux host is involved; the adapter only needs USB power. HotSync keeps working unchanged, because the firmware looks at the first byte the Palm sends on each connection (PPP starts with `0x7E`, HotSync with `0xBE`) and routes the session to Wi-Fi or to USB accordingly.

> [!NOTE]
> HotSync (backup and install), the PPP connection, the portal and joining a Wi-Fi network have been used with a real Palm IIIx running the Xiino browser. The user confirmed the OpenXiino about page displays, then reported Wikipedia's source limit, a long Receiving stage and Google's logo appearing as an unsupported image. The source-limit, streaming and image fixes have host-test and firmware-build coverage; those external pages still need a Palm retest.

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

For **Xiino**, use the DataServer settings below. For browsers fetching pages directly, the adapter has a **web proxy**, on by default:

- The adapter answers every site name with its own address, so the browser's requests come to it.
- It fetches the page over Wi-Fi, following redirects onto HTTPS, and passes the HTML to the Palm.
- On the way it removes scripts, style sheets, pictures and comments, converts UTF-8 text to ISO-8859-1, turns `https://` links into `http://` links (so the browser can follow them), and cuts pages off after 48 KB.
- The site's certificate is not checked. The Palm could not check it either; do not use this for anything that needs to be secure.

The status page has a link to turn the proxy off. Do that for programs that are not web browsers (mail, telnet, IRC): with the proxy on they would be handed the adapter's address instead of their server's. With it off the Palm connects to servers itself through the adapter (NAT), and a browser reaches plain-`http://` sites only.

Sites made for old or small browsers work best, for example `http://frogfind.com/` (search and simplified pages) or `http://68k.news/`.

## Xiino

The adapter now implements the [OpenXiino](https://github.com/nicl83/OpenXiino) DataServer protocol in firmware. No additional computer or Python server is needed.

1. In Xiino's **Options → Prefs → DataServer**, keep/set **`pds.mobirus.com`**, or enter **`192.168.99.1:4040`**. `pds.ilinx.com` is also recognized. If the setting was previously cleared, restore it.
2. Open **`http://about/`**. The built-in OpenXiino page includes its embedded logo and works before Wi-Fi is connected. **`http://deviceinfo/`** displays the width, colour/grayscale depth and encoding Xiino requested.
3. Open **`http://wifi.setup/`** to configure Wi-Fi, then browse normally. Leave the IR ports aligned while pages and pictures transfer.

Xiino requests receive their binary DataServer prefix, supported HTML and converted pictures embedded in the proprietary `EBDIMAGE` format. Anchor links are made absolute using the final page address after redirects; HTTPS anchor links are rewritten as HTTP as upstream does. The adapter follows origin redirects onto HTTPS. The portal and error pages also receive DataServer framing.

DataServer mode works with the portal's general **web proxy switched off**: only the two old DataServer hostnames and the portal name need local DNS answers. Other applications can then resolve their real servers normally. The server listens on both port 80 and 4040 and retains the existing Palm-only access restriction.

This port retains upstream's colour mode 9 images even for grayscale requests and its existing HTML/compression quirks. It does not run modern JavaScript. Static WebP (lossy/lossless), PNG, JPEG, GIF (first frame) and BMP images are supported. PNG/GIF are decoded into small thumbnails, WebP uses decoder scaling, and JPEG/BMP use native-channel decoding. Animated WebP and SVG are unsupported. Small inline `data:image/...` images are accepted within the existing tag/source budgets. Resampling/quantization can differ from Pillow. Image conversion uses integer white compositing and error diffusion, an exact fast palette lookup, and area reduction for JPEG/BMP; already-resized PNG/GIF/WebP thumbnails bypass further resampling. This avoids costly software floating-point work on the ESP32-C3.

Modern pages are filtered as they arrive: scripts, stylesheet text, comments and unused modern attributes are discarded before retaining the source. The retained page buffer is allocated once before opening TLS, avoiding heap peaks from growing the source string during HTTPS reception. The converter parses that buffer directly and converts text/attributes as needed; no-charset/Latin-1 pages do not require another complete encoding copy, and raw-text tag searches do not copy the page. Unused retained-source capacity is returned after closing TLS, and embedded images are held separately to avoid repeated copies of the accumulated payload. The raw scan budget is 2 MiB; compacted source and total output each stay within 48,000 bytes. Converted HTML is sent in small blocks, with a flush before each optional image request, rather than waiting for all pictures and assembling another complete HTML page. Embedded image data follows the HTML, as in OpenXiino. The HTTP/1.0 response ends when the connection closes. Xiino may still wait for the complete response before drawing it; the slow IR transfer remains part of Receiving.

Origin pages have a 20-second download deadline. Each optional image download has a 5-second deadline; image downloads share an 8-second network-time budget and an eight-image count cap. Sending HTML over IR and converting images do not consume the download-time budget. This prevents repeated slow picture servers from holding up the page indefinitely without skipping healthy images merely because IR delivery is slow. Cancelled/blocked Palm connections stop delivery rather than continuing image downloads.

After a slow request, **Diagnostics** reports separate Xiino timings for the origin page download (including source filtering), image downloads, remaining conversion/processing, and socket writes. “First body” measures time until the first HTML block is accepted by the socket; “Complete” means conversion/socket submission finished, not that the Palm has received or displayed every byte. Check the PPP queue and dropped-byte count on the same page for link backlogs/retransmissions. The external-request counts show whether pages use the DataServer or the direct proxy. If a request triggers a software reset, an RTC breadcrumb reports its last stage plus heap/stack readings after reboot; this is not retained through power loss/brownout. These measurements distinguish server work from IR delivery; the 8-second optional-image gate is not an overall page-load deadline.

If useful content exceeds the source/output bounds, the page is marked **[Page shortened to fit the Palm]**. Image files are limited to 64,000 bytes, decoder allocations to a shared 192 KiB with a 24 KiB free-heap reserve, and source dimensions to 2048×2048. PNG/GIF thumbnail decoding avoids the old full-frame RGBA copies. PNG uses separate dictionary/state allocations and falls back to a one-byte palette thumbnail when an RGB thumbnail would breach the heap reserve (without error-diffusion dithering on that fallback); larger JPEG/BMP or complex WebP images can still exceed available memory. Placeholders distinguish memory limits, unavailable downloads and unrecognized/invalid formats. See [dependency and port details](../third_party/README.md).

The portal's **Diagnostics** page records the last Xiino request's duration, raw bytes, sent bytes, image attempts and network error, plus the last decoded image's dimensions, decoder peak/live allocation and decoder error. The first image-download failure is retained even if later images hit a time/count limit. Placeholders distinguish download timeouts, image-budget skips and memory limits. Built-in pages such as `about/` contain pre-encoded images, so they do not increment decoder/image-fetch readings. These readings help distinguish a slow server, image-memory failure and a stalled Palm transfer.

Unlike the older direct proxy, the DataServer's HTTPS fetches check origin certificates with the firmware CA bundle and use the existing network clock. The Palm-to-adapter connection is still plain HTTP over PPP.

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
