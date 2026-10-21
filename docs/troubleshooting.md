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
  - Open **wifi.setup → Diagnostics** after reconnecting. Check **Last reset**, **HotSync diagnostics since boot** and **Previous HotSync diagnostics**. The trace includes the last service/stage, free heap, largest allocatable block, task stack headroom and failed allocation size/function. It works with USB debug logging disabled.
  - Breadcrumbs survive warm resets, including many panic/watchdog resets. On the next boot the adapter saves a valid interrupted/failed record in settings storage, keeping it through later boots when storage is available. A power loss or brownout before that boot can lose the latest record. This is a short trace, not a full panic backtrace.
  - Drive restore now grows its HotSync buffer only for an actual incoming database's largest block; an ordinary backup uses the small streaming buffer. Low-memory HTTPS checks stop the operation with an error instead of deliberately starting another TLS connection under pressure. If it still fails, copy the diagnostics trace and the Palm's HotSync log; device testing is still needed to establish the cause of a particular reboot.
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

## Google Drive backup

- **Memo, Note Pad, Expenses or Drive asks to set a HotSync user:** open **wifi.setup → HotSync settings (user name)**, enter the name and save. Disconnect Network and tap HotSync. The adapter fills a missing Palm name without replacing its numeric user identity. An existing name stays unchanged. For a reset with existing backups, use the original name and request Restore saved backups once.
- **`Drive backup upload was not confirmed (HTTP 404)` at the beginning:** update the firmware. The SDK's small receive buffer could truncate the long resumable-upload address. Upload setup now uses an 8 KiB receive buffer and a 404 session is restarted once using the same saved file ID and a rewound, validated source. Persistent failures retain the journal and include file/byte-range details; run HotSync again. This upload-session fix reuses the existing Google sign-in.

- **Access says OK, but there are no backup files:** enable Drive backup and run standalone HotSync. The access check does not create folders or transfer data. Check the Drive result in the portal or HotSync log.
- **HTTP 403:** enable Google Drive API in the OAuth client's Cloud project. If the Drive grant is missing, sign in again and allow it. Existing granted tokens can be reused.
- **Clock or identity error:** correct the Palm clock and check the HotSync user name and Google account. For a reset, unnamed Palm, enter the original folder user name and choose Restore saved backups once.
- **A removed Palm app does not come back:** its managed Drive backup is retained. Choose Restore saved backups once to reinstall missing backups. New, manually uploaded PDB/PRC files install automatically.
- **Interrupted installation:** use Retry interrupted install. Partial installs cannot become new backups. The previous version, when replacing existing data, is preserved under Recovery.
- **Folder moved, duplicate database, file/memory limit or unsupported format:** restore the saved folder path, keep a single file per internal database name, or address the reported limit. Sync stops instead of accepting an incomplete backup.

See [Drive setup, restore behavior and limits](google/drive.md).

## Expenses

- **Access check passes but nothing syncs:** enable Expenses and run standalone HotSync. Before the first sync the check verifies Drive access and scopes; creating the spreadsheet additionally needs Google Sheets API enabled. Read the `Expenses:` HotSync log or Last sync on its portal page.
- **Date or Amount is rejected:** use plain text, with `YYYY-MM-DD` dates through 2031. The adapter preserves amount strings without numeric conversion. Keep the original tab name, headers and column order.
- **A linked row is missing:** restore its row and Record ID using Sheets undo/version history. Set `Deleted` to `TRUE` for intentional deletion; removing a live row does not authorize deleting its Palm copy.
- **Both sides changed or the sheet changed during sync:** finish editing before HotSync. Reconcile competing fields or choose a conflict preference on the Expenses page. Changes after an upload can leave Sheet updates applied while Palm updates wait.
- **A creation is unconfirmed:** wait and retry HotSync first. Check Drive and the spreadsheet before explicitly retrying current pending creations. For an interrupted incoming row link, restore the exact Record ID reported by the sync result on that row.
- **Row/content limit:** remove audit rows marked `Deleted=TRUE` only after their deletion sync completes. Available ESP heap and shared settings storage also limit larger books; the existing partition layout is unchanged.

See [Expenses / Google Sheets](google/expenses.md) for field values, recovery and limits.

- **Expenses show currency `17`:** this is the Palm selector for New Zealand dollars. Updated firmware shows `NZD ($)` and upgrades existing numeric cells on successful sync. Use a specific currency label when editing in Sheets; `$` alone is ambiguous.

## Memo Pad and Note Pad

- **Access check passes but HotSync fails:** the check verifies Drive permissions/listing. Enable Google Docs API for Memo Pad; HotSync additionally checks Docs content, MemoDB or Palm OS 4 npadDB and image bounds. Read the conduit-specific result.
- **Both sides changed:** the default stops for review. Reconcile the edits or choose Prefer Palm edits / Prefer Google edits on that conduit's page. Titles and pixels merge independently for drawings.
- **Unconfirmed Google memo creation:** try HotSync again later so its saved marker can be found. Check Drive before choosing Retry unconfirmed creations; propagation delay can otherwise produce duplicates.
- **Formatted Docs or unsupported PNG:** use plain single-tab text documents for editable memos. Drawings require non-interlaced PNGs up to 160 pixels wide, 32,000 decoded bitmap bytes and 64 KiB per file; scrolling canvases are supported. Standard Palm OS 4 Note Pad is supported; other drawing applications need their own codecs.

See [Memo Pad](google/memos.md) and [Note Pad](google/notes.md) for formats, deletion rules and validation limits.

- **Memos report `HTTP -1` / `Not enough memory to receive Google HTTPS response`:** update the firmware. Memo reads now omit unused Google Docs metadata, and the shared HTTPS adapter collects small blocks before releasing TLS and assembling the complete reply. Request/response JSON and duplicate note metadata are released earlier. Existing document IDs and sync mappings are retained; keep your documents and retry. If memory is still insufficient, the error distinguishes receiving from final contiguous assembly and reports bytes received. Copy that full result plus Diagnostics; limits remain enforced.
- **Memos create Google Docs but report `Invalid Google Docs tab ID`:** update the firmware and sync again. Google uses dotted tab IDs such as `t.0`; older firmware applied the more restrictive Drive file ID validator. Saved creation mappings reuse the existing document on retry. Keep the existing documents and sync state.
- **Memos report HTTP 400 about legacy text-level Document fields while requesting tabs content:** update the firmware and retry HotSync. The response-memory optimization initially selected both a legacy top-level `body` and tab content. The corrected field mask selects the body only inside `tabs.documentTab`. Existing Google sign-in, documents and saved sync mappings can be reused.
- **Palm OS 4 Note Pad reports unsupported dimensions:** older firmware limited canvas height to 160 pixels. Update for scrolling canvases, bounded by decoded bitmap bytes instead of screen height. If it still fails, the result now distinguishes invalid dimensions, unsupported compression, truncated data and memory/buffer limits; copy the complete record-specific error.
- **Note Pad reports `Truncated Note Pad RLE pair`:** update the firmware. The decoder now accepts a lone trailing payload byte only if the preceding complete runs already produce every bitmap byte, matching pilot-link, and accepts zero-count no-op runs. It still rejects incomplete or oversized drawings. If the error remains, copy the full **Note Pad → Last sync** result, including record ID, received bytes and decoded/expected counts; it is not evidence that your Palm OS version is wrong.
- **Note Pad reports `Not enough memory to decode the PNG image`:** eight-bit grayscale is supported, but imports need file storage, decoder state and a decompression window as well as the Palm bitmap. Update the firmware for direct decoding from PNG chunks and smaller windows for small images. For the lowest memory use, export a non-interlaced one-bit black-and-white PNG no wider than 160 pixels; reducing compressed file size alone does not necessarily reduce decoder workspace. A failed import stops before note transfers. Keep the existing mappings; the updated error identifies the file and image dimensions/depth.
- **Adding a PNG causes `Not enough free/contiguous memory for Google HTTPS`:** update the firmware. Older downloads reserved the full image before TLS and opened a new connection per 4 KiB range. The adapter now downloads the fixed revision in one bounded request, assembles it after TLS closes, and releases enlarged Palm record buffers before further Google work. Duplicate drawing snapshots/upload copies are also reduced. The HTTPS admission checks remain enforced. Retry with the same Drive file and saved mappings. If it still fails, copy the complete Note Pad result (including the file and operation) and the latest Diagnostics free-heap/largest-block entries.
- **One image works, but adding another causes a PNG memory error, even for a one-bit image:** update the firmware. Earlier sync snapshots retained every decoded Palm and Google bitmap until HotSync finished. Note Pad now validates drawings individually, keeps compact metadata/hashes, and releases each bitmap before the next image. Needed drawings are reloaded individually for transfers; existing mappings and hashes are preserved. An error for an already monochrome PNG no longer suggests converting it to one-bit again. If it still fails, copy the complete Note Pad result and the latest Diagnostics entries, plus the affected image dimensions and file size.

## Related guides

- [ESP and network diagnostics](internet.md#notes)
- [Hardware and wiring](hardware.md)
- [Google sign-in](google/README.md)
