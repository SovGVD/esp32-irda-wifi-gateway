# Development, architecture and testing

[Documentation](README.md)

## Architecture and background

```
                                                     +---------------+      USB CDC ACM      +------------------+
                                                     |               | <===================> |   Linux Host     |
+----------------+        IrDA SIR (IR Light)        | ESP32-C3 with |    (/dev/ttyACM0)     | (pilot-xfer,     |
|   Palm IIIx    | <===============================> |   TFBS4711    |                       |     jpilot)      |
| (HotSync or    |      9600 - 57600 baud            |  Transceiver  |         Wi-Fi         +------------------+
|  Network / PPP)|   (IrLAP / IrLMP / IrCOMM)        |               | <===================>     Internet
+----------------+                                   +---------------+
```

Modern Linux kernels (starting with kernel 4.17 in 2018) completely removed the kernel IrDA subsystem (`irda.ko`, `ircomm`, `irattach`). Legacy USB-IrDA dongles (like KingSun or ACTiSYS) no longer work out of the box because there is no driver to handle the IrLAP/IrLMP/IrCOMM layers.

**This project solves that entirely in firmware:**

1. The **ESP32-C3** handles the IrDA protocol stack on-chip:
   - **IrPHY**: 3/16th bit-time SIR optical pulse encoding/decoding via hardware UART.
   - **IrLAP**: Device discovery (`XID`), link connection (`SNRM`), parameter negotiation, acknowledgment and retransmission.
   - **IrLMP / IAS**: Information Access Service responding to queries for `"IrDA:IrCOMM"`.
   - **TinyTP & IrCOMM**: Emulates a serial port over IR, with credit-based flow control.
2. For HotSync, the ESP32-C3 enumerates as a native USB CDC ACM serial device (`/dev/ttyACM0` or `/dev/palm-irda`) on Linux. To `pilot-link` (`pilot-xfer`) or `jpilot`, the Palm looks like a standard wired serial Palm.
3. For networking, the ESP32-C3 terminates the Palm's PPP session and forwards its traffic to Wi-Fi with NAT, so no computer is needed at all.
4. For standalone sync, the ESP32-C3 speaks the desktop's side of HotSync itself (SLP, PADP, CMP and DLP, the protocols `pilot-link` implements) and runs the Mail, Tasks, Calendar, Contacts, Memo Pad, Note Pad, Expenses and Drive conduits over Wi-Fi.

## Host-Side Tests

Run commands from the repository root.

The parts that do not need the ESP32 can be tried on a computer (needs `g++`, a C compiler, `python3` and zlib development headers/library). Google conduit tests use cJSON installed by PlatformIO after the first build. PNG memory tests also use the SDK's portable miniz sources in `components/esptool_py/esptool/flasher_stub`. For alternative source locations, set `CJSON_DIR` and `MINIZ_DIR` respectively:

```bash
test/host/run.sh
```

- **HotSync protocols:** the firmware's code talks to a simulated Palm (`test/host/fake_palm.py`) over a pseudo terminal. The requests it sends must be byte for byte those that `pilot-link` sent to the same simulated Palm for the same steps (recorded in `test/host/data/`). The sync is repeated with every single packet lost once, in either direction.
- **Mail text:** records of the Mail database, character sets, MIME encodings, HTML to text, headers, dates.
- **IMAP and SMTP clients:** against scripted servers that answer as Gmail does, with the answers arriving whole and one byte at a time; with passwords and with Google sign-in (XOAUTH2).
- **Google sign-in:** the consent address, the PKCE challenge (RFC 7636 example), taking the code from a pasted address, and the token server's answers (tokens, revoked token, wrong client).
- **Google HTTPS transport:** the actual ESP32 adapter runs against an SDK shim reproducing the 512-byte request-line limit and the SDK's misleading redirect error for non-2xx responses. Checks cover long pagination URLs, complete streamed bodies without duplication, real HTTP error status, People API error reasons, failed setup, bounded responses, ETags, keep-alives, partial writes, truncated responses, no automatic redirects/retries and Tasks compatibility.
- **Shared HotSync identity:** global portal setup fills an absent Palm user name, preserving numeric IDs, last-desktop fields and sync dates; named Palms stay unchanged. The native-protocol test covers blank/nonblank names, ID zero/nonzero, Western text, unsupported characters and length/control validation before Memo/Note sync. Settings save the name and encoding together, and explicit Drive recovery runs before automatic naming.
- **Memory and reset diagnostics:** allocation-free, checksummed breadcrumbs record service stages, internal free/largest heap, stack headroom and allocation failures in RTC no-init memory. Early boot archives valid interrupted/failed traces to NVS, independently of USB debug logging. Host tests cover ring wraparound, interrupted snapshots, corrupt/stale state rejection, bounded strings and failure/completion transitions. HTTPS shim tests cover low total/contiguous heap, 32,000-byte reception using 1 KiB blocks while the live TLS heap is fragmented, assembly after cleanup, assembly failure with no write retries, a >56 KiB binary GET using its own bounded response cap while ordinary JSON stays limited to 32 KiB, and ordinary error-body/status handling. Drive tests start with a small buffer, grow for actual records, and reject insufficient capacity before new installations, replacement and interrupted-install deletion. Actual warm-reset retention and the reported startup freeze still require ESP/Palm testing.
- **Whole mail syncs:** the conduit between the simulated Palm and the scripted servers, with a password, with Google sign-in, and with messages read, deleted, filed and archived on either side; what the Palm holds afterwards is compared with the expected contents.
- **Google Tasks:** two-way and repeated HotSyncs against the simulated ToDoDB and an in-memory Google API, pagination, completion, due-date clearing, long-field preservation, conflicts, private/archive exclusion, deletion and failure handling. Tasks authorization also exercises the existing OAuthClient with a scripted token server.
- **Google Calendar:** real HotSync against simulated DatebookDB and an in-memory Calendar API, independent Palm record fixture, maximum record size, compact state validation, timed/all-day appointments, recurrence and DST, reminders, long-field preservation, conflict handling, window absence, retry after a lost creation response, invitations and two-way deletion.
- **Google Contacts:** real HotSync against simulated AddressDB and an in-memory People API, independent Palm record fixture and bounds, reserved header bytes/bits and zero padding, actionable record errors before any transfers, persisted state, component merges, additional Google values, slot type conversion, source ID changes, source ETag conflicts, missing-list checks, delayed/failed creations, privacy and two-way deletion. API cases also cover missing profile permission without changing either address book, empty books, oversized email arrays, HTML/Cyrillic display and allocation failures while constructing updates.

- **Google Drive backup:** real HotSync against multiple simulated RAM databases and resources, standard PDB/PRC offset tables, private records, app/sort blocks, uploads larger than 256 KiB, unchanged snapshots, newest-wins conflicts, recovery-before-replace, rollback after a failed payload download, explicit interrupted-install retries, full-restore identity recovery, lost folder/upload replies, concurrent Drive edits, malformed/duplicate incoming files and account/storage/clock failures. New regressions cover fresh-session and post-chunk 404 expiry, exact stream rewind, bounded restart with unchanged file IDs and persistent journal recovery. The SDK shim reproduces a truncated long Location with the old RX buffer and verifies the enlarged upload-setup buffer preserves it.

- **Memo Pad and Note Pad:** simulated Palm OS 4 MemoDB/npadDB over real HotSync, Docs creation/revision guards, PNG image import/export and RGB conversion, unchanged syncs, title-only image preservation, alarms, private records, deletion-versus-edit conflicts, explicit conflict preferences, moved files, lost creation replies, incoming ID reservation, failed state commits and malformed records/documents/images/state blobs. RLE regressions include an independently encoded 320-row native record with zero-count no-ops and an odd nonzero trailer, trailer-value checks, normalized native round trips, and rejection of incomplete pairs/bitmaps, overruns and data outside the payload. The user confirmed Note Pad working on Palm OS 4 on 15 October 2026. The scripted Docs server rejects legacy top-level text fields when tab content is requested, then applies the actual requested field mask to a response with 45 KB of unused metadata, and still rejects masked-out tables/objects and child tabs while preserving formatting/revision checks. Regressions use real `t.0`/dotted Docs tab IDs, reject invalid tab IDs, decode an independent 480-row raw Palm drawing with ink below the first screen, resize truncated native reads, and round-trip dense scrolling drawings and 320/480/1600-row PNGs. The sync tests use zlib, while a dedicated codec test runs the miniz path with SDK sources and checks bounded workspace, no compressed C++ copy, split/empty IDAT chunks, wrapping, malformed streams and allocation failures. The >56 KiB 160×352 grayscale sync regression verifies one revision-locked download without image preallocation, rejection of short files without transfers, and native-buffer release before network work; firmware executes ROM miniz.

- **Expenses / Sheets:** independent classic ExpenseDB wire fixtures over real HotSync, exact amount strings, packed dates and selector values, two-way edits and inserts, complete-row reordering, field merges and conflict preferences, Unicode preservation, explicit deletion, excluded private/archive records, unchanged category/currency app-info, lost spreadsheet/row-write replies, reserved incoming IDs and failed commits. Cases also cover preflight/postflight concurrent edits, formulas versus literal text, malformed/duplicate/oversized snapshots, API, identity and storage failures, and one-time retries. Raw sparse value batches and early release of parsed JSON reduce peak heap usage. Currency coverage verifies all 256 Palm selectors, readable NZD/USD labels, unknown/custom slots, legacy numeric-cell upgrades and unchanged v1 hashes.

## Validation status

The project has been used with a Palm IIIx and Xiino for desktop backup/install, PPP, the portal and Wi-Fi setup. Mail, Google Tasks and Calendar are reported working on the user's device after the shared HTTPS fixes. The user confirmed Contacts sync working after the profile-permission and record-decoder fixes on 14 October 2026. The firmware build and sanitizer-enabled host tests passed for these changes.

The user reported Memo documents being created but sync failing on Google Docs tab IDs, followed by response-memory exhaustion and an HTTP 400 caused by selecting legacy body fields with tab content. The user confirmed Memo sync working on 15 October 2026 after those fixes. The user also confirmed Note Pad working on Palm OS 4 after the scrolling-canvas and RLE fixes, then reported PNG import allocation failures. Direct IDAT decoding and smaller decompression windows have firmware-build and sanitizer-enabled host coverage. A later 160×352 grayscale import hit HTTPS admission limits; the single bounded download, native-buffer shrinking and reduced drawing-copy changes have build/host coverage, and the user subsequently confirmed Note Pad working after these fixes on 15 October 2026. A later second-image import exposed retained whole-book bitmaps. Compact drawing snapshots, per-image reload/release and allocation-free bitmap hashing now have firmware/host coverage, including eight-image repeat syncs with live C++ allocation measurements and a changed-Palm-record guard. The user subsequently reported the multi-image sync working on 15 October 2026. HTTPS memory checks remain enforced; the drawing content budget now covers retained metadata plus one active bitmap, while Memo text retains its combined budget.

The user reported ExpenseDB/Google Sheets sync working on 15 October 2026, with currency shown as a numeric selector. The readable currency upgrade has firmware-build and sanitizer-enabled host coverage; its device retest and heap usage with larger books remain pending.

The Google Drive backup/restore implementation has firmware-build and sanitizer-enabled host-test coverage; real Palm/Google Drive verification remains pending.

The user reported missing HotSync names in Memo/Notes/Expenses and a Drive data-upload HTTP 404. The global-name setup and upload-header/session-restart fixes require a device retest; host regressions do not establish that those reported device failures are resolved.

That device report covers normal use; restart persistence, interrupted writes, larger data sets and uncommon recurrence/conflict cases have not each been separately confirmed on hardware. The web proxy's recorded verification is host-side testing. Keep automated test coverage separate from those device checks.

## Project Structure

```
esp32-irda-wifi-gateway/
├── platformio.ini         # PlatformIO environment (Arduino on top of ESP-IDF, native USB CDC)
├── sdkconfig.defaults     # ESP-IDF options: PPP, IP forwarding, NAT, quiet USB port
├── partitions.csv         # Flash layout (3 MB application, settings storage)
├── CMakeLists.txt         # ESP-IDF project file used by the PlatformIO build
├── README.md              # Project overview, features and compatibility
├── docs/                  # Setup guides, service details and reference
├── include/
│   ├── config.h           # Pin assignments, baud rates, Wi-Fi gateway and debug settings
│   ├── googlescopes.h     # Shared Google scope definitions, including planned services
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
│   ├── hotsyncuser.h      # Shared name validation and identity-preserving DLP setup
│   ├── irhotsync.h        # Global HotSync user settings and last observed status
│   ├── irtasks.h          # Tasks settings and ESP32 conduit adapter
│   ├── tasksync.h         # Two-way Palm To Do List conduit
│   ├── googletasks.h      # Google Tasks REST client
│   ├── ircalendar.h       # Calendar settings and ESP32 conduit adapter
│   ├── calendarsync.h     # Two-way Palm Date Book conduit and persistent mapping
│   ├── calendartext.h     # Date Book record codec and recurrence conversion
│   ├── googlecalendar.h   # Calendar REST client
│   ├── irnotes.h          # Memo/Note settings and ESP32 conduit adapter
│   ├── notesync.h         # Two-way notes coordinator, mappings and journals
│   ├── notetext.h         # MemoDB/OS 4 npadDB codecs and monochrome PNG conversion
│   ├── googlenotes.h      # Google Docs text and Drive PNG operations
│   ├── irexpenses.h       # Expense settings and ESP32 conduit adapter
│   ├── expensesync.h      # Two-way ExpenseDB/Sheets coordinator and durable mappings
│   ├── expensetext.h      # Classic ExpenseDB record codec and field projection
│   ├── googlesheets.h     # Sheets table reads, initialization and sparse raw updates
│   ├── irdrive.h          # Drive settings and ESP32 conduit adapter
│   ├── drivebackup.h      # Backup/restore coordinator and persistent transfer journal
│   ├── palmbackup.h       # Streaming classic PDB/PRC export and installation
│   ├── googledrive.h      # Drive folders, resumable uploads and revision downloads
│   ├── googlehttp.h       # Shared Google HTTP interface
│   ├── irgooglehttp.h     # Shared ESP32 HTTPS transport
│   ├── ircontacts.h       # Contacts settings and ESP32 conduit adapter
│   ├── contactsync.h      # Two-way Address Book conduit and persistent mapping
│   ├── contacttext.h      # Classic AddressDB record codec
│   ├── googlecontacts.h   # Google People API client
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
│   ├── hotsyncuser.cpp    # Fill an unnamed Palm without resetting its numeric ID
│   ├── irhotsync.cpp      # Atomic name/encoding settings and legacy Drive fallback
│   ├── irtasks.cpp        # Tasks NVS state, HTTPS and portal settings
│   ├── tasksync.cpp       # ToDoDB conversion and synchronization
│   ├── googletasks.cpp    # Google API requests
│   ├── ircalendar.cpp     # Calendar NVS state and conduit adapter
│   ├── calendarsync.cpp   # DatebookDB synchronization
│   ├── calendartext.cpp   # Appointment records, Gregorian dates and RRULEs
│   ├── googlecalendar.cpp # Calendar API requests
│   ├── irnotes.cpp        # Memo/Note NVS state and conduit wrappers
│   ├── notesync.cpp       # Two-way notes sync, conflicts and durable creation IDs
│   ├── notetext.cpp       # Classic memo and OS 4 drawing record codecs
│   ├── noteimage.cpp      # Bounded PNG conversion and ROM inflation
│   ├── googlenotes.cpp    # Docs revision-controlled edits and PNG operations
│   ├── irexpenses.cpp     # Expense NVS settings, Google checks and conduit wrapper
│   ├── expensesync.cpp    # Field merges, explicit deletions and transfer journals
│   ├── expensetext.cpp    # Expense dates, selectors, strings and compact field hashes
│   ├── googlesheets.cpp   # Stable row IDs, complete snapshots and bounded JSON requests
│   ├── irdrive.cpp        # Drive NVS settings, state and conduit phases
│   ├── drivebackup.cpp    # Newest-wins backup/restore and recovery copies
│   ├── palmbackup.cpp     # PDB/PRC streaming and DLP database transfers
│   ├── googledrive.cpp    # Google Drive API client
│   ├── irgooglehttp.cpp   # Bounded Google HTTPS with HotSync keep-alive
│   ├── ircontacts.cpp     # Contacts NVS state and conduit adapter
│   ├── contactsync.cpp    # AddressDB synchronization
│   ├── contacttext.cpp    # Address Book fields, labels and baselines
│   ├── googlecontacts.cpp # People API requests and field preservation
│   ├── mailsync.cpp       # Steps of a mail sync
│   ├── mailnet.cpp        # IMAP and SMTP
│   ├── mailoauth.cpp      # Google's token server, consent address, PKCE
│   ├── mailtext.cpp       # Conversion between Mail records and Internet mail
│   ├── irdebug.cpp        # Protocol trace buffer
│   └── main.cpp           # IrLAP state machine; routes sessions to USB, Wi-Fi or conduits
├── test/host/             # Tests that run on a computer (see Host-Side Tests above)
└── udev/
    └── 99-esp32-irda.rules# Linux udev rules (disables ModemManager probing)
```

## Related guides

- [Building and flashing](building.md)
- [Configuration](configuration.md)
