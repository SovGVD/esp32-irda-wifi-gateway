# Mail on the Palm

[Documentation](README.md)

The Palm's built-in **Mail** application has no network code: it only exchanges messages during a HotSync, through a "conduit" on the desktop. The adapter can be that desktop. When a HotSync starts and no program on the USB host answers it, the adapter takes the HotSync over and synchronises Mail with a mail account over Wi-Fi:

- Messages in the **Outbox** are sent (SMTP) and then removed from the Palm.
- Messages that arrived in the account's inbox since the last sync are copied to the Palm's **Inbox** (IMAP).
- What became of messages copied earlier is passed on, both ways: read on one side, read on the other; deleted on the Palm, moved to the account's trash; gone from the account's inbox (deleted or archived elsewhere), gone from the Palm's Inbox.

No computer is involved; the adapter only needs USB power and a saved Wi-Fi network.

> [!NOTE]
> Mail is reported working on the device. Automated checks cover a simulated Palm, `pilot-link` request compatibility and scripted mail servers; see [testing and validation](development.md#validation-status).

## 1. Enter the account

Connect the Palm using the [Internet connection guide](internet.md), open **`http://wifi.setup/`** and follow **Mail**:

- **Gmail:** sign in with Google from a PC, once (see [Google sign-in](google/README.md)). This needs no app password.
- **Enter the account** (any provider, or Gmail with an app password): e-mail address, password, and the name recipients should see.
- With **Gmail** a password must be an *app password*: Google account → Security → 2-Step Verification → App passwords. Google refuses the normal account password here. It is 16 lower-case letters. Accounts without 2-Step Verification, with Advanced Protection, or managed by an organisation that blocks them cannot create one: use "Sign in with Google" instead.
- For **other providers**, open "Servers" and enter the IMAP and SMTP server names. Both are contacted with TLS from the first byte, on ports 993 and 465 (another port can be given as `server:port`; STARTTLS on ports 143 / 587 is not supported).
- **Check the login now** tries both servers and shows what they answer.

Then tap **Disconnect** in Prefs → Network: HotSync needs the infrared link for itself.

For Gmail OAuth, follow [Google sign-in and shared permissions](google/README.md).

## 2. Synchronise

In the **HotSync** application choose "IR to a PC/Handheld" and tap the HotSync icon, exactly as for a desktop sync.

- The adapter passes the HotSync to USB first. A desktop program that is waiting (`pilot-xfer`, `jpilot`) answers within milliseconds and gets the session, as before.
- If nothing answers within 1.5 seconds, the adapter runs the enabled Mail, Tasks, Calendar and Contacts conduits itself.

The outcome is written to the **HotSync log** on the Palm (HotSync → Log), for example `Mail: 1 sent, 3 received`, or the reason something failed. The portal's Mail page shows the same line.

## What is synchronised

- The choice under **Options → HotSync Options** in the Mail application is respected: All, Send only, Filter, Unread, and the Truncate length.
- Only the 15 newest messages of the inbox are looked at in one sync. If more than that arrived since the last sync, the older ones are passed over for good. The first sync fetches the 15 newest.
- Messages arrive as text. Of a message with several parts the plain text is taken; a message that only exists as HTML is reduced to its text; attachments are left out.
- Text is converted to the Palm's character set (Windows-1252). Accented letters it lacks become plain letters, other scripts become `?`. With a Cyrillic localisation on the Palm, switch "Text" on the Mail page to Cyrillic (Windows-1251).
- **Read and deleted, both ways.** A message read on the Palm is marked read in the account at the next sync, and one read elsewhere becomes read on the Palm. A message deleted on the Palm (it goes to Mail's Deleted folder) is moved to the account's trash (Gmail: "Trash", in the account's language) and purged from the Palm; a server without a trash folder deletes it. A message deleted or archived elsewhere leaves the Palm's Inbox; one filed on the Palm stays there.
- This applies to messages the adapter copied to the Palm, and to the newest 200 of them; messages from before this version are not followed. Without such changes the inbox is opened read-only.
- A sent message leaves the Outbox once the server has accepted it. Gmail keeps its own copy under "Sent"; other providers may not.
- Messages deleted on the Palm are purged from its memory during the sync.
- The signature set in Mail's preferences is appended to messages that ask for it.
- Dates are shown in the Palm's time. The adapter gets the time from the network and takes the time zone from the Palm's clock, so set that clock correctly.

## Notes

- The password (or the Google sign-in token) is stored unencrypted in the ESP32's flash, like the Wi-Fi passwords. Both can be revoked at any time; a mail account used only for the Palm limits the exposure further.
- The mail servers' certificates are checked (unlike the web proxy's).
- Keep the IR ports aligned until the Palm reports that the HotSync is complete.
- "Fetch the newest messages again" on the Mail page makes the next sync start over, for example after emptying the Palm's Inbox.
- To sync with a computer while mail is set up, start `pilot-xfer` / `jpilot` **before** tapping HotSync, or turn mail sync off on the Mail page.

## Related guides

- [Google sign-in](google/README.md)
- [HotSync](hotsync.md)
- [Troubleshooting](troubleshooting.md)
