# Documentation

[Project overview](../README.md)

Start with [hardware and wiring](hardware.md), then [build and flash the firmware](building.md). Set up [desktop HotSync](hotsync.md) or [Internet access](internet.md), depending on how you want to use the adapter. Run shell commands from the repository root unless a guide says otherwise.

## Setup and use

| Category | Contents |
|:---------|:---------|
| [Hardware](hardware.md) | Parts, board/transceiver pinouts, wiring options and status LED |
| [Building and flashing](building.md) | Software requirements, PlatformIO build and upload |
| [HotSync](hotsync.md) | Linux permissions/udev, backup/install/restore and standalone sync |
| [Internet connection](internet.md) | Palm PPP, Wi-Fi setup, web proxy, Xiino and ESP diagnostics |
| [Mail](mail.md) | Gmail or other IMAP/SMTP accounts, sync behavior and limits |
| [Google services](google/README.md) | Cloud project, shared OAuth, API permissions and reauthorization |

## Google sync guides

- [Tasks / To Do List](google/tasks.md)
- [Calendar / Date Book](google/calendar.md)
- [Contacts / Address Book](google/contacts.md)
- [Memo Pad / Google Docs](google/memos.md)
- [Note Pad drawings / Drive PNG](google/notes.md)
- [Expenses / Google Sheets](google/expenses.md)
- [Drive / full backup and restore](google/drive.md)

Slides permission is prepared in the shared sign-in for future features; Slides sync is not implemented.

## Reference

- [Configuration](configuration.md)
- [Troubleshooting](troubleshooting.md), including HTTPS and missing profile permission
- [Development, architecture and testing](development.md), including device validation status
