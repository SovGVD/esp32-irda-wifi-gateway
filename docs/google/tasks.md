# Google Tasks on the Palm

[Documentation](../README.md)

The adapter synchronises the Palm's built-in **To Do List** (`ToDoDB`) with **one Google task list** during standalone HotSync. Mail, Tasks, Calendar, Contacts, Memo Pad, Note Pad, Expenses and Drive have separate switches and can run together. A desktop listening on USB still takes priority.

> [!NOTE]
> Google Tasks is reported working on the device and has passed simulated Palm/API tests and the firmware build. See [testing and validation](../development.md#validation-status) for the evidence and remaining checks.

## 1. Use the existing Google sign-in

Tasks shares the adapter's **Sign in with Google** flow with Mail: the same Desktop OAuth client, portal pages, PKCE exchange, NVS refresh token and token-refresh client. No separate script or credential header is needed.

1. Enable **Google Tasks API** in the Google Cloud project that owns your existing OAuth client.
2. On the Palm, open **Google Tasks** at `http://wifi.setup/` and choose **Sign in with Google (from a PC)**. This opens the existing `/google` setup page on the Wi-Fi side for 15 minutes.
3. On a PC on the same Wi-Fi, open the address shown on the Palm. The form reuses the stored Google account and client ID; leave the secret blank to keep the stored secret for that client.
4. Continue to Google's consent page and allow **Mail, Tasks, Contacts, Calendar, Drive, Docs, Sheets and Slides**, plus **basic profile** access. Copy the loopback address Google redirects to and paste it into the adapter's page, as described in [Shared Google sign-in](README.md#shared-google-sign-in).
5. Finishing from the Tasks page enables Tasks and preserves Mail's previous on/off setting. You can also turn Tasks on or off separately on the Palm.

**Existing authorizations without the full grant must sign in again to add the missing permissions:** firmware changes cannot extend an already-issued token. Once all eight service permissions and basic profile permission are granted, Tasks, Calendar, Contacts, Drive, Memo Pad, Note Pad and Expenses, plus future Slides features can use the same refresh token for operations covered by those scopes. Token expiry, revocation or additional scopes can still require another sign-in. Until Tasks permission has been granted, the Tasks page explains that reauthorization is needed; Mail can continue using its existing token.

The default list is `@default`. To use another list, enter its Google API ID (not its displayed name) on the Tasks page. A dedicated Google task list makes the first sync easier to review. Existing public Palm tasks are uploaded, and existing Google tasks are added to the Palm; matching titles are not treated as the same task.

## 2. Check access and sync

Choose **Check Google access** while Network is connected. Disconnect Network, align the infrared ports, and tap HotSync with no desktop waiting. The Palm HotSync log and portal show an outcome such as `Tasks: 1 uploaded, 2 downloaded, 0 deleted`.

Google credentials stay in the existing `irmail` NVS namespace (`gid`, `gsecret`, `gtoken`); Tasks stores only its switch, character set, list and mapping state in `irtasks`. Signing out of Google also stops Tasks until another shared sign-in. To replace an expired token, repeat the existing portal sign-in.

## Sync behavior and limits

- Titles, notes, completion and due **dates** sync in both directions. On simultaneous edits, the Palm wins for each field edited since its last successful sync; other fields take Google's current values.
- Task IDs and per-field baselines are saved in flash. Repeated successful syncs update the same records.
- Explicit Palm deletion deletes the linked Google task. Google deletion removes its linked Palm record. A full paginated snapshot is required before any changes; a failed page never implies deletion.
- Private and archived Palm records are skipped. Palm priority and category stay on the Palm. Google subtasks appear as flat Palm tasks; their Google hierarchy and links are preserved. Assigned tasks are excluded.
- Imported descriptions are limited to 255 Palm bytes and notes to 4095. Text uses Windows-1252 or the separate Cyrillic setting. Unsupported characters are approximated or omitted. Unedited long or unrepresentable Google fields are preserved when other fields change.
- Palm due years are 1904–2031. Dates outside that range appear undated; they stay intact on Google unless the Palm due-date field changes. The [Tasks API](https://developers.google.com/workspace/tasks/reference/rest/v1/tasks) exposes date information only.
- A snapshot accepts up to 100 records on each side (including Google deletion tombstones), with a 48 KB text budget per side and 24 KB maximum HTTPS response per page. These bounds protect memory; exceeding a limit reports a problem.
- Use one Palm and one Google list per adapter. The first sync binds the canonical Google list ID and Palm HotSync user ID; a mismatch stops syncing. Palms without a HotSync user ID cannot be distinguished. Changing the configured task list after linking records is refused. The shared Google sign-in can be renewed; a different account/list ID will be refused by the saved mapping binding.
- Credentials and mappings are stored unencrypted in flash, like Wi-Fi and mail settings. HTTPS certificates are checked. Network failures preserve pending Palm edits. Power loss or an ambiguous failure during creation before an ID is saved can leave a duplicate on retry; cross-device sync is not atomic.

See Google's [desktop OAuth guide](https://developers.google.com/identity/protocols/oauth2/native-app) and [device-flow scopes](https://developers.google.com/identity/protocols/oauth2/limited-input-device#allowedscopes).

## Related guides

- [Shared Google sign-in and permissions](README.md)
- [HotSync](../hotsync.md)
- [Troubleshooting](../troubleshooting.md)
