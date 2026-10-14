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
4. For mail, the ESP32-C3 speaks the desktop's side of HotSync itself (SLP, PADP, CMP and DLP, the protocols `pilot-link` implements) and acts as the Mail conduit, with an IMAP and an SMTP client on the Wi-Fi side.

## Host-Side Tests

Run commands from the repository root.

The parts that do not need the ESP32 can be tried on a computer (needs `g++`, a C compiler and `python3`). Google conduit tests use cJSON installed by PlatformIO after the first build; set `CJSON_DIR` to another cJSON source directory for host-only testing:

```bash
test/host/run.sh
```

- **HotSync protocols:** the firmware's code talks to a simulated Palm (`test/host/fake_palm.py`) over a pseudo terminal. The requests it sends must be byte for byte those that `pilot-link` sent to the same simulated Palm for the same steps (recorded in `test/host/data/`). The sync is repeated with every single packet lost once, in either direction.
- **Mail text:** records of the Mail database, character sets, MIME encodings, HTML to text, headers, dates.
- **IMAP and SMTP clients:** against scripted servers that answer as Gmail does, with the answers arriving whole and one byte at a time; with passwords and with Google sign-in (XOAUTH2).
- **Google sign-in:** the consent address, the PKCE challenge (RFC 7636 example), taking the code from a pasted address, and the token server's answers (tokens, revoked token, wrong client).
- **Google HTTPS transport:** the actual ESP32 adapter runs against an SDK shim reproducing the 512-byte request-line limit and the SDK's misleading redirect error for non-2xx responses. Checks cover long pagination URLs, complete streamed bodies without duplication, real HTTP error status, People API error reasons, failed setup, bounded responses, ETags, keep-alives, partial writes, truncated responses, no automatic redirects/retries and Tasks compatibility.
- **Whole mail syncs:** the conduit between the simulated Palm and the scripted servers, with a password, with Google sign-in, and with messages read, deleted, filed and archived on either side; what the Palm holds afterwards is compared with the expected contents.
- **Google Tasks:** two-way and repeated HotSyncs against the simulated ToDoDB and an in-memory Google API, pagination, completion, due-date clearing, long-field preservation, conflicts, private/archive exclusion, deletion and failure handling. Tasks authorization also exercises the existing OAuthClient with a scripted token server.
- **Google Calendar:** real HotSync against simulated DatebookDB and an in-memory Calendar API, independent Palm record fixture, maximum record size, compact state validation, timed/all-day appointments, recurrence and DST, reminders, long-field preservation, conflict handling, window absence, retry after a lost creation response, invitations and two-way deletion.
- **Google Contacts:** real HotSync against simulated AddressDB and an in-memory People API, independent Palm record fixture and bounds, reserved header bytes/bits and zero padding, actionable record errors before any transfers, persisted state, component merges, additional Google values, slot type conversion, source ID changes, source ETag conflicts, missing-list checks, delayed/failed creations, privacy and two-way deletion. API cases also cover missing profile permission without changing either address book, empty books, oversized email arrays, HTML/Cyrillic display and allocation failures while constructing updates.

## Validation status

The project has been used with a Palm IIIx and Xiino for desktop backup/install, PPP, the portal and Wi-Fi setup. Mail, Google Tasks and Calendar are reported working on the user's device after the shared HTTPS fixes. Contacts access is confirmed after adding profile permission, but actual Contacts transfers stopped on a Palm record-decoding error. The decoder compatibility fix needs a device retest. The shared HTTPS and OAuth changes passed the firmware build and sanitizer-enabled host tests.

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
│   ├── irtasks.h          # Tasks settings and ESP32 conduit adapter
│   ├── tasksync.h         # Two-way Palm To Do List conduit
│   ├── googletasks.h      # Google Tasks REST client
│   ├── ircalendar.h       # Calendar settings and ESP32 conduit adapter
│   ├── calendarsync.h     # Two-way Palm Date Book conduit and persistent mapping
│   ├── calendartext.h     # Date Book record codec and recurrence conversion
│   ├── googlecalendar.h   # Calendar REST client
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
│   ├── irtasks.cpp        # Tasks NVS state, HTTPS and portal settings
│   ├── tasksync.cpp       # ToDoDB conversion and synchronization
│   ├── googletasks.cpp    # Google API requests
│   ├── ircalendar.cpp     # Calendar NVS state and conduit adapter
│   ├── calendarsync.cpp   # DatebookDB synchronization
│   ├── calendartext.cpp   # Appointment records, Gregorian dates and RRULEs
│   ├── googlecalendar.cpp # Calendar API requests
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
