# Google Calendar on the Palm

[Documentation](../README.md)

Standalone HotSync synchronises the built-in **Date Book** (`DatebookDB`, Palm IIIx format) with one writable Google calendar. Enabled conduits run after incoming Drive restores: Mail, Tasks, Calendar, Contacts, Memo Pad and Note Pad, followed by the final Drive backup. A desktop listening on USB takes priority.

## Setup

1. Enable **Google Calendar API** in the Cloud project used by the existing Google OAuth client.
2. Open **Google Calendar** at `http://wifi.setup/`. If your shared Google sign-in already grants Calendar permission, it is reused without another authorization. Otherwise choose **Sign in with Google (from a PC)** and follow the [shared sign-in flow](README.md#shared-google-sign-in). Completing sign-in from Calendar enables Calendar and preserves Mail's previous switch.
3. Keep the calendar ID as `primary`, or enter the ID from Google Calendar's **Settings → Integrate calendar**. The calendar must be in the signed-in account's calendar list with owner or writer access. The default window is 30 days before and 90 days after today; choose a smaller window if the event limit is reached.
4. Choose **Turn Calendar on**, then **Check Calendar access** while Network is connected. This reports the calendar's time zone and event count. Set the Palm clock to that time zone: Date Book has no time-zone field.
5. Disconnect Network, keep the infrared ports aligned, and tap HotSync with no desktop waiting. The Palm log and Calendar portal page report uploads, downloads, deletions and skipped events.

Existing public Palm appointments in the window are uploaded and Google events in the window are imported. Matching titles or times do not deduplicate previously unrelated records; a dedicated calendar makes the first sync easier to review.

## Sync behavior and limits

- Titles, notes, dates, start/end times, alarms and supported recurrence sync both ways. A Palm edit wins for each field changed since its last successful sync; other fields take Google's current values. Categories stay on the Palm. Unchanged Google text, attendees, location, conferencing and other metadata are preserved.
- Single-day timed and all-day events are supported; Google's all-day end date is exclusive. Times use the selected calendar's time zone. Google resolves outgoing wall times, including daylight-saving offsets.
- Daily, weekly, monthly by date or ordinal weekday, and yearly repeat rules are supported when they fit Date Book, including intervals, inclusive end dates and up to 64 local-date exclusions. Timed repeat end dates use actual Calendar instances to resolve DST. Imported timed rules require a final occurrence within two days of their cutoff; their Palm end date becomes that occurrence's date. Rules whose cutoff cannot be resolved safely are skipped.
- Multi-day events, special event types, advanced recurrence such as `COUNT`, UTC exclusions and recurrence in another time zone are skipped. Modified/cancelled recurring instances returned in the window cause their series to be skipped as well. Overrides outside the window are not discovered; this is not a complete recurrence-exception editor. Skipped linked events retain their Palm copy and report a reason.
- Palm dates must be in 1904–2031. Titles and notes are limited to 255 and 4095 Palm bytes, respectively, with Western or Cyrillic text conversion. Unsupported characters are approximated. Unedited longer Google fields survive edits to other fields.
- One representable popup reminder becomes the Palm alarm. Editing it preserves Google email reminders. Google reminders are limited to four weeks. Invitations are read-only on the Palm unless Google allows guest changes; deleting a read-only invitation hides its Palm copy without cancelling the Google event.
- Explicit Palm tombstones delete linked writable Google events. Google cancellations or individually confirmed missing events remove linked Palm records. Absence from a rolling date window never implies deletion: linked events outside it are fetched individually. Missing Palm records without a tombstone are not treated as Google deletions.
- A complete paginated snapshot is required before mutation. Defaults allow at most 64 window events, 64 linked records and 64 relevant Palm records, with a combined 48 KB text budget and 32 KB response limit. Private and archived Palm records are skipped; new appointments outside the chosen window stay on the Palm. Previously linked appointments continue to sync outside it.
- Calendar state is saved as a compact versioned blob in the `ircalendar` NVS namespace; Google credentials remain in `irmail`. The first sync binds the canonical calendar ID and Palm HotSync user ID. A mismatch stops sync, and changing calendars with linked records is refused. Palms without a user ID cannot be distinguished.
- ETags stop updates if Google changes an event during sync. Outgoing creations save their chosen Google ID before the request, so a lost response retries that ID. Timed recurrence cutoffs require two writes; an interrupted update can temporarily include extra occurrences near the end until the next sync completes it. Cross-device writes are not atomic: power loss between importing a Palm record and saving its mapping can still cause a duplicate. Known mapping-storage failures stop sync and attempt to undo a new Palm import.

Calendar has passed simulated Palm/API tests and the firmware build. Calendar is reported working on the device. Restart persistence and the less common sync cases still need dedicated hardware validation; see [testing and validation](../development.md#validation-status). Google documents the [event format](https://developers.google.com/workspace/calendar/api/v3/reference/events) and [windowed event listing](https://developers.google.com/workspace/calendar/api/v3/reference/events/list).

## Related guides

- [Shared Google sign-in and permissions](README.md)
- [HotSync](../hotsync.md)
- [Troubleshooting](../troubleshooting.md)
