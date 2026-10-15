# Google Drive backup and restore

[Documentation](../README.md) · [Shared Google sign-in](README.md)

Standalone HotSync backs up Palm RAM databases and applications to **My Drive → Backup → Palm → &lt;HotSync user name&gt;**. Standard `.pdb` and `.prc` files placed in that folder are installed on the Palm. The user name comes from the handheld, not the Google email address.

## Setup

1. Enable **Google Drive API** in the Cloud project owning your existing OAuth client.
2. Open **wifi.setup → Drive backup** on the Palm. The shared Google sign-in already requests Drive permission. Reuse it if granted; otherwise sign in from this page and allow Drive access.
3. Run **Check Drive access**, then enable **Drive backup**. Set the text encoding to match the Palm's database names.
4. If the Palm has no HotSync user name, save one in **wifi.setup → HotSync settings**. Set the Palm's clock correctly. Disconnect **Prefs → Network**, align the infrared ports, and tap HotSync with no desktop waiting.

The access check verifies authorization without creating folders or transferring databases. The first successful HotSync creates missing folders. Results appear in the Palm's HotSync log and on the Drive page.

## What HotSync does

Incoming Drive files are processed first, followed by enabled Mail, Tasks, Calendar, Contacts, Memo Pad, Note Pad and Expenses syncs. A final backup captures the resulting Palm data. A restore error stops the remaining conduits for that HotSync, so an incomplete installation cannot be synchronized as valid app data. A listening USB desktop retains priority.

| Situation | Result |
|:----------|:-------|
| Palm database has no Drive file | Upload a full `.pdb` or `.prc` backup |
| New, manually uploaded Palm file | Install it automatically |
| Database exists on both sides | Compare the Drive file modification time with the Palm database modification time; newest wins |
| Modification times are equal | Keep the Palm version |
| Drive version is newer | Save the previous Palm database in `Recovery` under the user's folder, then replace it |
| Backup is unchanged | Skip uploading it again |
| A previously backed-up database was removed from Palm | Retain its Drive backup without reinstalling it automatically |
| Drive changes during backup | Stop that update and retry on the next HotSync |

This is whole-database backup and restore. Choosing a newer file replaces the entire older database; individual records are synchronized separately by the application conduits. Drive's file modification time determines incoming precedence, even if the PDB header contains an older date. Keep one file per internal Palm database name; changing only a file's name does not change that identity.

The first backup includes all supported RAM databases, not only those with the Palm backup flag. Records, private records, resources, categories, app/sort blocks, record IDs, database type, creator, version, flags and dates are transferred. Deleted/archived record tombstones are omitted. ROM databases, expansion-card files, the operating-system image and the adapter's own Wi-Fi/OAuth settings are outside this backup.

## Full restore after data loss

On the Drive page, choose **Restore saved backups once**, then run HotSync. This also installs managed backup files absent from the Palm. Existing databases still use newest-wins comparison. The request clears after a successful complete sync.

A reset Palm may have no HotSync user name. Enter the original backup user name in **wifi.setup → HotSync settings** before requesting the restore. When this adapter already knows that Palm's previous identity, the explicit restore restores its HotSync user ID/name before automatic name setup, so application conduits retain their mappings. For a new adapter, the supplied name selects the folder in the connected Google account.

A saved binding protects against accidentally mixing another Google account or named Palm user into the same backup session. Renaming or moving saved folders stops sync until their original names and locations are restored. Distinct Palm users should use distinct HotSync user names.

Keep the network connected and the infrared link aligned throughout a restore. A database requesting reset is identified in the result; reset the Palm after HotSync to activate its installed updates.

## Interrupted transfers and recovery

Upload IDs and pending installations are saved in ESP settings before transfers, so a lost reply can be reconciled without creating duplicate files. An incomplete Palm installation is blocked from being backed up as a complete database.

Upload setup uses an 8 KiB HTTPS receive buffer because the ESP-IDF 4.4 parser can discard a header value after its first fragment. A truncated Drive `Location`/`upload_id` can make a newly opened session return HTTP 404. If a data upload returns 404, the adapter opens a new session once, retaining the saved file ID and rewinding/revalidating the source. This also covers expiry after a received chunk. Persistent failures show the file, byte range and Google error details, and keep the pending transfer for the next HotSync. Ambiguous network failures are not automatically resent. Google's [upload guide](https://developers.google.com/workspace/drive/api/guides/manage-uploads#resume-upload) documents restarting an expired 404 session.

After a disconnected installation, inspect the result and use **Retry interrupted install** on the Drive page. This explicitly permits replacing the partial database with the saved incoming source. If an installation fails while the link still works, the adapter attempts to restore the previous version from `Recovery`. Recovery files remain available for manual retrieval; they are not installed automatically.

To recover a particular older version, download its Recovery file, place it in the main user folder as the single file for that database, and give it a newer Drive modification time. Do not leave two files containing the same database there.

## Limits and validation

Files and record tables are streamed; a whole database does not need to fit in ESP RAM. Transfers use classic Palm DLP: each record, resource or app/sort block must fit within **65,489 bytes**. Stream/extended databases and chained PDB record lists are rejected explicitly. Available Palm RAM still limits installation size.

The adapter starts with the small streaming buffer and grows it only when an incoming database needs a larger block. Before replacing a database it checks capacity for both the incoming copy and rollback to the previous Palm version. Insufficient memory stops the restore before creating/deleting that database. The larger buffer is released before the other conduits run; HTTPS also checks available heap and contiguous space before connecting.

Defaults allow up to 512 Palm databases and 512 immediate Drive folder entries, with a 64,000-byte metadata budget. The budget can be reached before the count limit. Unsupported ordinary files are ignored, but folder entries still consume the listing budget. See [configuration](../configuration.md).

Palm database times are local wall time. The adapter infers the current UTC offset from the Palm and network clocks; incorrect clocks can prevent sync or choose the wrong version. Keep them correct, including daylight saving changes. Avoid editing a file while it is transferring.

Sanitizer-enabled host tests cover PDB/PRC transfers, private records, large multi-chunk uploads, repeat syncs, newest-wins conflicts, Recovery, rollback, lost replies, interrupted installs and account/storage failures. **Google Drive backup and restore still need verification on a real Palm and Google account.** Existing application-conduit device reports do not validate this new feature.

Protocol references: [Google Drive uploads](https://developers.google.com/workspace/drive/api/guides/manage-uploads), [downloads](https://developers.google.com/workspace/drive/api/guides/manage-downloads), and [pilot-link DLP](https://github.com/desrod/pilot-link/blob/main/libpisock/dlp.c).
