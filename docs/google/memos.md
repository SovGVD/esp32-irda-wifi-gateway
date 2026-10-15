# Memo Pad and Google Docs

[Documentation](../README.md) · [Shared Google sign-in](README.md)

The Memo Pad conduit synchronizes classic **MemoDB** records in both directions with Google Docs in **My Drive → Palm → &lt;HotSync user name&gt; → Memos**. It has a separate switch from Mail and Drive backup. Each memo is one document; its entire text, including its first line, is the document body. CR/CRLF line endings are normalized to line feeds.

## Setup and use

1. Enable **Google Drive API** and **Google Docs API** in the Cloud project owning your OAuth client.
2. Open **wifi.setup → Memo Pad**. Reuse the existing Google sign-in if Drive and Docs permissions were granted; otherwise sign in from this page and allow them.
3. Set the Palm text encoding, enable sync and check Google access. The access check verifies Drive authorization and lists an existing folder; it does not create notes or prove the Palm codec or Docs API is working.
4. If the Palm has no HotSync user name, enter it in **wifi.setup → HotSync settings** and save. Disconnect Network and tap HotSync without a desktop waiting. The adapter sets the missing name before sync creates the folders, uploads public Palm memos and imports documents already placed there.

Create or edit a memo on either side, then HotSync to transfer it. To create a memo from a computer, create a Google Doc and move it into the Memos folder. Previously unrelated notes are not deduplicated by title or text; an empty dedicated folder makes the first sync easier to review. Text uses the Palm's selected Western or Cyrillic character set; unsupported characters are approximated on the Palm. Unedited Google text retains its original Unicode.

A newly exported document uses up to 80 characters of the memo's first line as its file name. Renaming that Google document does not change the memo body, and subsequent Palm edits preserve the Google file name.

## Edits, deletions and conflicts

Saved field hashes distinguish Palm edits from Google edits; MemoDB has no per-record modification timestamp. Changes on one side transfer to the other. If both sides changed the memo differently, the default **Stop for review** preserves both versions and reports a conflict. The portal also offers **Prefer Palm edits** and **Prefer Google edits**. These settings persist and apply to conflicting changed fields; they do not turn the conduit into a one-way mirror.

An explicit Palm deletion tombstone moves its linked Google document to Trash. A confirmed Google deletion removes its Palm counterpart. If deletion conflicts with an edit, sync stops unless a conflict preference chooses the surviving side. An absent Palm record without a tombstone is reimported. Moving a Google file out of the Memos folder pauses that link instead of deleting Palm data. Private and archived records stay out of this application sync and retain their existing mappings.

Google Docs revision guards reject a concurrent update occurring after the document was read. Drive file versions are checked again before deletion, with conditional headers when available. Cross-device operations are not atomic. Google document creation cannot use a preassigned Drive ID, so a private creation marker is saved before the request. If the response is lost, later HotSyncs look for that marker. If it remains unconfirmed, check Drive and try again later before using **Retry unconfirmed creations**. The retry authorizes only currently unconfirmed creations, not later failures. An early explicit retry can create a duplicate; duplicate creation markers stop sync for review. Incoming Palm IDs are reserved in saved state before writing, allowing interrupted imports to resume without creating a second Palm record.

## Limits and validation

- Classic MemoDB text is limited to **4095 Palm bytes**, plus its NUL terminator. Oversized documents or malformed Palm records stop sync before note transfers.
- One Google document tab and text paragraphs are supported, including Google's standard `t.0` tab ID. Tab IDs are carried unchanged into revision-guarded writes, separately from Drive file ID validation. Existing documents created before a tab-ID failure are reused on retry through the saved mapping; do not delete them or reset sync state. Tables, images, embedded objects and additional tabs are rejected. Formatted text can be read, but Palm replacement of a formatted document is refused to avoid silently destroying Google formatting; use plain documents in this dedicated folder.
- Memo reads request only document/revision/tab IDs, text, text bounds and formatting indicators using a Google Docs field mask. With `includeTabsContent=true`, body fields are selected only within `tabs.documentTab`; requesting a legacy top-level `body` at the same time causes HTTP 400. Unused document styles and metadata are omitted. Tables, embedded objects and child tabs remain detectable after filtering and are rejected. Writes still require the fetched revision ID.
- HTTPS receives bounded 1 KiB blocks, closes TLS, then assembles the response once. It releases old reply capacity before connecting; request JSON trees are freed before upload, and encoded response JSON is released after parsing. The conduit also frees duplicate listing metadata and avoids copying the complete remote note for every edit. These changes reduce peak heap use without raising the response/content limits or disabling low-memory checks.
- Defaults allow **64 records**, including private/deleted records during the Palm snapshot, a combined **64,000-byte content budget**, and a **32 KiB HTTPS response limit**. A complete folder and Palm snapshot are required before transfers. Folder metadata has its own 24,000-byte budget.
- Identity bindings and per-note mappings are stored in the adapter's shared NVS settings partition. Account/user changes or storage failures stop sync. Do not rename/move the bound folders or change the Palm user identity while links exist.
- All conduits run sequentially: shared identity setup, incoming Drive restore, Mail, Tasks, Calendar, Contacts, Memo Pad, Note Pad, Expenses, final Drive backup. Desktop USB HotSync retains priority. Native database backup and document sync are separate features.

The user confirmed Memo sync working on 15 October 2026 after the response-memory and tab field-mask fixes. Firmware builds and sanitizer-enabled simulated Palm/API tests also cover two-way creation/editing/deletion, repeat syncs, conflict preferences, Unicode projection, private records, moved files, malformed content, revision conflicts, lost creation replies and interrupted state writes, including rejection of mixed legacy/tab field masks. The device report confirms normal use; larger collections and recovery/conflict cases have not each been separately confirmed on hardware.

References: [Google Docs field masks](https://developers.google.com/workspace/docs/api/how-tos/field-masks), [Google Docs tabs](https://developers.google.com/workspace/docs/api/how-tos/tabs), [Google Docs write controls](https://developers.google.com/workspace/docs/api/how-tos/best-practices), [Drive creation and ID restrictions](https://developers.google.com/workspace/drive/api/guides/create-file).

Tab ID reference: [Google Docs JSON example](https://developers.google.com/workspace/docs/api/samples/output-json).
