# Troubleshooting

[Documentation](README.md)

- **No optical response / Discovery fails:**
  - Verify that TFBS4711 Pin 4 (SD) is at 0V (GND/LOW). When SD is HIGH (3.3V), the transceiver is in shutdown.
  - Verify TXD (Pin 2) is connected to GPIO 6 and RXD (Pin 3) is connected to GPIO 7.
  - Check distance and optical alignment: IrDA is directional (typically $\pm 15^\circ$ cone). Keep the Palm and transceiver within 10 to 40 cm.
- **Connection starts then drops:**
  - Avoid bright ambient infrared interference (direct sunlight, incandescent desk lamps, or halogen bulbs directly over the optical transceiver).
  - Wire the emitter as in [Option B](hardware.md#option-b-separate-emitter-supply-recommended-with-wi-fi): the $4.7\ \mu\text{F} - 10\ \mu\text{F}$ capacitor on Pin 1 (IRED Anode) prevents voltage dips.
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
  - "Host does not exist" for every address in Xiino: see [Xiino](internet.md#xiino).
  - "Cannot Open Page" from the adapter names the reason (could not connect, no answer, too many redirects).
  - If the portal does not appear either, the browser may need a WAP gateway; try one that fetches `http://` URLs directly.

- **A HotSync meant for the computer turned into a mail sync:**
  - The desktop program was not waiting yet. Start `pilot-xfer` (or the sync in `jpilot`) first, then tap HotSync; or turn mail sync off on the portal's Mail page.
- **The mail sync reports a problem (HotSync log on the Palm, or the portal's Mail page):**
  - `IMAP login: ...` / `SMTP login: ...`: the server refused the password. With Gmail it must be an app password, or use "Sign in with Google"; "Check the login now" on the Mail page tests it without a HotSync.
  - `Google sign-in no longer valid, sign in again from a PC`: Google ended the sign-in (see [Google sign-in notes](google/README.md#shared-google-sign-in)). `Google sign-in: invalid_client`: the client ID or secret is wrong, or the client was deleted.
  - `cannot connect to ...`: wrong server name, a server without TLS on that port, or a certificate that cannot be verified.
  - `Wi-Fi is not connected`: check the status page of the portal.
- **The Palm reports a HotSync connection error with mail set up:**
  - Set `ENABLE_CDC_DEBUG` to `1` and watch the trace: lines starting with `Mail:` show each step of the sync, and the free memory at its start.
- **Calendar or Contacts access reports `HTTP -1` while Tasks works:**
  - Update the firmware: older builds used ESP-IDF's 512-byte HTTPS request buffer, which could reject Google's long pagination URLs. The shared transport now sizes the buffer for the URL and headers, up to 16 KB.
  - Run the access check again. Transport failures now show the ESP-IDF error name (and socket error when available), initialization/header memory failures, or the response byte limit. These details are also used in HotSync logs. HTTP 403 continues to identify permission/API-access problems.
- **Contacts reports `ESP_ERR_HTTP_MAX_REDIRECT`:**
  - Update the firmware. ESP-IDF 4.x's automatic request function could label every non-2xx response as a redirect error when redirects were disabled. The adapter now streams the response directly, retaining its real status and complete body while keeping redirects disabled.
  - Check Contacts again. If the page says People API is disabled, enable **People API** in the Google Cloud project owning the OAuth client and retry; this does not require new OAuth consent. Permission and quota failures show Google's reason instead of a redirect message.
- **People API is enabled, but `PeopleService.GetPerson` has a 100% error rate:**
  - The account identity lookup (`people/me?personFields=metadata`) needs **basic profile** permission (`https://www.googleapis.com/auth/userinfo.profile`) in addition to Contacts permission. Older firmware omitted this grant. Update the firmware, add the profile scope under Google Auth Platform → Data Access, sign in again from Address Book, and grant all requested permissions. Run **Check Contacts access** again. The updated portal identifies the missing grant before sending a failing API request, and keeps Contacts sync off until both permissions are present.
- **Contacts access says OK, but HotSync reports `An Address Book record is too large or invalid`:**
  - The access check only reads Google; this error is from decoding a Palm `AddressDB` record. Older firmware incorrectly required unused header bytes and bits to be zero and rejected zero padding after the contact fields. Update the firmware and run HotSync again; the existing Google sign-in can be reused.
  - Genuine record errors now identify the zero-based record index, record ID, received byte count and reason, such as an unterminated field, unsupported phone label, field length over its limit or truncated HotSync response. Check **Address Book → Last sync** or the Palm's HotSync log. Sync stops before transferring contacts when the Palm snapshot cannot be decoded completely.

## Related guides

- [ESP and network diagnostics](internet.md#notes)
- [Hardware and wiring](hardware.md)
- [Google sign-in](google/README.md)
