# Google Contacts in Address Book

[Documentation](../README.md)

Standalone HotSync synchronises the classic Palm **Address Book** (`AddressDB`, v1 format) with the signed-in account's Google Contacts. It runs after Mail, Tasks and Calendar and has a separate switch. A listening USB desktop still takes priority.

## Setup

1. Enable **People API** in the Cloud project used by the existing Google OAuth client.
2. Open **Address Book** at `http://wifi.setup/`. If the shared sign-in already grants Contacts and basic profile permission, the stored token is reused. Otherwise add `https://www.googleapis.com/auth/userinfo.profile` to Google Auth Platform → Data Access, choose **Sign in with Google (from a PC)** and allow both Contacts and basic profile. Completing sign-in from this page enables Contacts and preserves Mail's previous switch. Basic profile permission identifies the account for saved contact mappings; it does not import your profile as a contact.
3. Choose **Turn Contacts on**, then **Check Contacts access** while Network is connected. Disconnect Network and tap HotSync with no desktop waiting. The Palm log and portal show a line such as `Contacts: 1 uploaded, 2 downloaded, 0 deleted`.

The first sync uploads existing public Palm contacts and imports Google contacts. Previously unrelated entries are not matched by name or email, so contacts already copied manually between the two books can appear twice. This conduit uses saved Google contacts, excluding Other Contacts, directory entries and profile-only data.

## Sync behavior and limits

- First/last name, company/title, postal street/city/region/postcode/country, notes and four custom fields sync both ways. The Palm wins for each component edited since the previous successful sync; unedited components use current Google data. Notes imported from HTML are shown as plain text. Unedited longer Google text is preserved.
- The five Palm phone slots hold phone numbers and email addresses, ordered with primary values first and then by a stable type/value ordering. Palm Work, Home, Fax, Other, E-mail, Main, Pager and Mobile labels map to Google types. Custom Google phone types appear as Other and keep their original type until their Palm label changes. Additional numbers/emails are preserved, even when visible slots are edited. If the API returns 100 phone/email entries, slot edits are refused because the API may have truncated that array.
- People fields have no immutable per-number IDs. If phone/email slots changed on both sides, sync stops with a conflict and leaves the Palm edit intact. Restore the Palm's previous slots to accept Google's changes, or resolve the contact in Google Contacts and restore the Palm slots before syncing again. Other fields use the usual per-component merge.
- The primary organization and postal address are shown; extra organizations/addresses and unrepresented fields such as middle names, departments and PO boxes survive edits. Birthdays, photos, group memberships and unrelated custom fields stay on Google. Palm categories and the preferred displayed phone slot stay on the Palm.
- The four custom slots use Google `userDefined` keys **Palm Custom 1** through **Palm Custom 4**. Renaming their labels in Palm app-info does not rename these keys. Other Google custom keys are preserved.
- Names are limited to 80 Palm bytes each, other fields to 255, and notes to 4095. Text uses Western or Cyrillic conversion; longer Google text is truncated only for display. Private/archived Palm records are skipped and their existing mappings retained.
- The classic record decoder ignores unused header padding and reserved field-mask bits, matching [pilot-link](https://github.com/desrod/pilot-link/blob/main/libpisock/address.c), and accepts zero padding after the fields. Unknown nonzero trailing data, missing string terminators, unsupported phone options and excessive field lengths stop sync with the record index, ID and reason. No contact transfers occur until the complete Palm snapshot is valid; see [troubleshooting](../troubleshooting.md).
- The complete Google and Palm snapshots are read before mutations. Defaults allow 64 Google contacts, 64 linked records and 64 relevant Palm records, with a 48 KB Google JSON snapshot budget, a combined 64 KB snapshot budget and a 32 KB HTTPS response limit. The combined public book must also fit 64 contacts. Exceeding a limit reports an error instead of syncing a partial book. There is no contact-group selector yet; larger accounts require changing the build limits and enough board memory/storage.
- Mappings and per-field hashes use a compact versioned blob under `ircontacts` in NVS; credentials remain in `irmail`. One Google account and one Palm HotSync user ID are bound on first sync. Google CONTACT source IDs retain links when a merged People resource name changes. Palms without a user ID cannot be distinguished. The board's existing NVS partition is shared with Wi-Fi, Mail, Tasks and Calendar; a full partition stops sync with a storage error.
- Explicit Palm deletion tombstones delete the linked Google contact. A Google contact missing from the list is checked individually before its Palm copy is removed. A Palm record missing without a tombstone is not a request to delete Google data. Update requests include the contact source ETag to reject concurrent Google edits. Deletion re-reads the contact immediately beforehand, but People API has no documented atomic conditional delete, so a change between that read and deletion remains possible.
- Outgoing creations save a unique `clientData` marker before POST. A lost reply waits for that marker to appear in a later complete snapshot; Google may delay propagation. No automatic second POST is sent for an unconfirmed creation. If a request never created the contact, **Retry unconfirmed creations** on the portal permits another attempt after checking the snapshot again; doing this before Google has finished propagating can cause a duplicate. Known rejected requests can retry automatically after correction.
- Cross-device sync is not atomic. Power loss between a new Palm import and saving its mapping can still cause a duplicate. Known mapping-storage failures stop sync and attempt to undo the new Palm import. Credentials and mappings are stored unencrypted in flash.

Host tests use the real HotSync protocol against simulated AddressDB and an in-memory People API. The user confirmed Contacts sync working on the device after the decoder compatibility fix on 14 October 2026. The firmware build and sanitizer-enabled host tests also passed. Restart persistence and the less common sync cases still need dedicated hardware validation; see [testing and validation](../development.md#validation-status). See Google's [contact management guide](https://developers.google.com/people/v1/contacts), [update rules](https://developers.google.com/people/api/rest/v1/people/updateContact) and [list propagation behavior](https://developers.google.com/people/api/rest/v1/people.connections/list).

## Related guides

- [Shared Google sign-in and permissions](README.md)
- [HotSync](../hotsync.md)
- [Troubleshooting](../troubleshooting.md)
