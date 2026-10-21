> [!WARNING]
> **Use at your own risk.** This project is provided "as is," without warranty. The authors and contributors accept no responsibility for data loss, corruption, unintended changes to Palm or Google data, or hardware damage resulting from its use. Keep independent backups before flashing firmware, syncing or restoring data.

# ESP32-C3 IrDA Wi-Fi Gateway

An **ESP32-C3** and **Vishay TFBS4711** infrared transceiver connect a vintage Palm to modern Linux computers, Wi-Fi and Google services. The firmware handles IrDA itself, so it does not need the removed Linux IrDA kernel drivers. Standalone networking and sync need only USB power and Wi-Fi.

## Features

- **Desktop HotSync:** back up, install and restore Palm applications and databases over USB with `pilot-link` or `jpilot`.
- **Internet access:** connect the Palm over infrared to Wi-Fi, with a settings portal, web proxy and an embedded OpenXiino DataServer for Xiino pages and pictures.
- **Mail:** send and receive through IMAP/SMTP, including Gmail with Google sign-in, and sync read/deleted messages.
- **Google Tasks:** sync the Palm's To Do List in both directions.
- **Google Calendar:** sync Date Book appointments, notes, alarms and supported repeat rules.
- **Google Contacts:** sync Address Book names, phones, email, addresses, notes and custom fields.
- **Memo Pad:** sync classic memos with Google Docs in both directions.
- **Note Pad:** sync Palm OS 4 drawings with PNG files in Google Drive in both directions.
- **Expenses:** sync classic Palm expenses with Google Sheets in both directions.
- **Google Drive:** back up Palm databases/apps and install or restore `.pdb`/`.prc` files, keeping the newest version.
- **Diagnostics:** view ESP memory, uptime, firmware/storage information and network status from the Palm.

Mail, Tasks, Calendar, Contacts, Memo Pad, Note Pad, Expenses and Drive have separate switches and share the same Google sign-in where applicable. They run during HotSync when no desktop is waiting.

## Device compatibility

| Device or software | Support |
|:-------------------|:--------|
| Palm IIIx | Tested with desktop HotSync, networking and the classic built-in applications |
| Other Palm OS handhelds | Require IrDA and **IR to a PC/Handheld** (Palm OS 3.3+); standalone sync uses classic Mail, ToDoDB, DatebookDB, AddressDB, MemoDB and ExpenseDB formats; drawings target Palm OS 4 npadDB. Other models are not yet confirmed |
| ESP32-C3 SuperMini + Vishay TFBS4711 | Reference hardware; other ESP32-C3 boards require matching pin assignments |
| Linux with `pilot-link` / `jpilot` | Desktop HotSync host |
| Xiino and browsers that fetch HTTP directly | Palm portal and browsing; WAP gateway browsers are unsupported |

Wi-Fi uses 2.4 GHz personal or open networks. Detailed sync limits and device validation status are in the guides.

## Documentation

See the [documentation index](docs/README.md) for all setup and reference material.

- [Hardware and wiring](docs/hardware.md)
- [Building and flashing](docs/building.md)
- [HotSync](docs/hotsync.md)
- [Internet connection and diagnostics](docs/internet.md)
- [Mail](docs/mail.md)
- [Google services and sign-in](docs/google/README.md)
- [Configuration](docs/configuration.md), [troubleshooting](docs/troubleshooting.md) and [development](docs/development.md)
