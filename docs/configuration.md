# Configuration reference

[Documentation](README.md)

**HotSync user name:** configure it once under **wifi.setup → HotSync settings**. It is shared by Memo Pad, Note Pad, Expenses and Drive, and applied to an unnamed Palm at the next standalone HotSync. Existing names and numeric identities are preserved. See [HotSync user setup](hotsync.md#set-the-hotsync-user-name-from-the-palm).

Firmware settings are in [`include/config.h`](../include/config.h). Google scope definitions are in [`include/googlescopes.h`](../include/googlescopes.h).

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
| `PROXY_MAX_PAGE_BYTES` | 48000 | Direct proxy truncation limit; Xiino compacted-source and output byte budgets (oversized pages are shortened with a notice) |
| `XIINO_DATASERVER_HOST` / `XIINO_DATASERVER_LEGACY_HOST` | `pds.mobirus.com` / `pds.ilinx.com` | Names resolved locally for the OpenXiino DataServer, also with the general proxy off |
| `XIINO_DATASERVER_PORT` | 4040 | Additional DataServer listener; port 80 also accepts Xiino requests |
| `XIINO_MAX_SOURCE_BYTES` | `2 * 1024 * 1024` | Raw page scan budget; streamed through a filter, not retained in RAM |
| `XIINO_MAX_IMAGE_BYTES` | 64000 | Maximum compressed image download |
| `XIINO_MAX_IMAGE_PIXELS` | `153 * 1024` | Resized image pixel cap; shared 192 KiB decoder allocation budget and free-heap checks can reject smaller images |
| `XIINO_PAGE_TIMEOUT_MS` | 20000 | Total origin page download deadline, including redirects and reads |
| `XIINO_IMAGE_TIMEOUT_MS` | 3000 | Deadline for each optional image download |
| `XIINO_IMAGES_TIMEOUT_MS` / `XIINO_MAX_IMAGES` | 8000 / 8 | Shared elapsed image-attempt budget and maximum attempted images per page |
| `PPP_GATEWAY_IP` / `PPP_PALM_IP` | `192.168.99.1` / `.2` | Addresses on the link between adapter and Palm |
| `MAIL_PC_ANSWER_TIMEOUT_MS` | 1500 | How long a HotSync waits for a desktop on USB before standalone Drive / Mail / Tasks / Calendar / Contacts sync |
| `MAIL_DEFAULT_IMAP_SERVER` / `MAIL_DEFAULT_SMTP_SERVER` | Gmail's | Mail servers until others are entered in the portal |
| `MAIL_MAX_PER_SYNC` / `MAIL_MAX_SEND_PER_SYNC` | 15 / 10 | Newest inbox messages looked at, and Outbox messages sent, per sync |
| `MAIL_HTML_FETCH_MAX` | 96000 | Bytes read of a message that only exists as HTML |
| `GOOGLE_SCOPES` | Mail + Tasks + Contacts + Calendar + Drive + Docs + Sheets + Slides + basic profile | Access asked for by the shared Google sign-in |
| `GOOGLE_SETUP_WINDOW_MS` | 15 minutes | How long the sign-in page answers on the Wi-Fi side once opened from the Palm |
| `MAIL_NTP_SERVER` | `pool.ntp.org` | Time server (for message dates) |
| `TASKS_MAX_PER_SYNC` | 100 | Maximum Palm records and Google tasks in a snapshot |
| `TASKS_HTTP_TIMEOUT_MS` / `TASKS_HTTP_RESPONSE_MAX` | 12000 / 24000 | HTTPS timeout and response byte limit |
| `CALENDAR_MAX_PER_SYNC` | 64 | Maximum Calendar window, linked records and relevant Palm appointments |
| `CALENDAR_PAST_DAYS` / `CALENDAR_FUTURE_DAYS` | 30 / 90 | Default rolling date window; adjustable on the Calendar page |
| `CALENDAR_HTTP_TIMEOUT_MS` / `CALENDAR_HTTP_RESPONSE_MAX` | 12000 / 32000 | Calendar HTTPS timeout and response byte limit |
| `CONTACTS_MAX_PER_SYNC` | 64 | Maximum Google, linked and relevant Palm contacts; the combined public book must also fit |
| `CONTACTS_HTTP_TIMEOUT_MS` / `CONTACTS_HTTP_RESPONSE_MAX` | 12000 / 32000 | Contacts HTTPS timeout and response byte limit |
| `NOTES_MAX_PER_SYNC` / `NOTES_CONTENT_MEMORY` | 64 / 64000 | Memo/Note record count and retained-content budget, per conduit; Memo holds combined text, while Note Pad holds metadata plus one active bitmap |
| `NOTES_HTTP_TIMEOUT_MS` / `NOTES_HTTP_RESPONSE_MAX` | 20000 / 32768 | Memo/Note HTTPS timeout and response byte limit |
| `EXPENSES_MAX_PER_SYNC` / `EXPENSES_CONTENT_MEMORY` | 64 / 24000 | Expense record/row count and combined Palm/Sheets content byte budget |
| `EXPENSES_HTTP_TIMEOUT_MS` / `EXPENSES_HTTP_RESPONSE_MAX` | 20000 / 48000 | Expenses HTTPS timeout and response byte limit |
| `DRIVE_MAX_DATABASES` / `DRIVE_MAX_FILES` | 512 / 512 | Maximum RAM databases and immediate Drive folder entries |
| `DRIVE_FOLDER_MEMORY` | 64000 | Approximate in-memory Drive listing metadata budget in bytes |
| `DRIVE_HTTP_TIMEOUT_MS` / `DRIVE_HTTP_RESPONSE_MAX` | 20000 / 32000 | Drive HTTPS timeout and response byte limit |
| `ENABLE_CDC_DEBUG` | 0 | Print a protocol trace on USB instead of serial data |
| `CURRENT_OP_MODE` | `MODE_IRCOMM` | `MODE_RAW_SIR` turns the adapter into a plain IR pulse bridge without the IrDA stack |
