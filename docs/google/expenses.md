# Expenses / Google Sheets

[Google services](README.md) · [HotSync](../hotsync.md)

The adapter syncs the classic Palm **Expense** application (`ExpenseDB`) with a Google spreadsheet in both directions. Expense sync has its own switch and can run with Mail and Drive backup disabled.

## Setup

1. Enable **Google Drive API** and **Google Sheets API** in the Cloud project used for the [shared Google sign-in](README.md).
2. Open **wifi.setup → Expenses**. The existing sign-in works if it already grants Drive and Sheets access; sign in again only if either permission is missing.
3. Turn Expenses on, disconnect **Prefs → Network**, and start HotSync with no desktop waiting.
4. First sync creates **`Palm/<HotSync user name>/Expenses`** in My Drive. This is a spreadsheet with an **Expenses** tab. Use its link on the Expenses page or open it from Google Drive on a PC.

The Palm must have the classic Expense application installed. If its HotSync user name is missing, save one in **wifi.setup → HotSync settings**; the adapter writes it before syncing. The binding includes the Google account, Palm user ID and user name; changing identity stops sync rather than mixing expense books.

## Editing the spreadsheet

Keep the tab name, header row and A:L column order unchanged. The adapter sets these cells to **Plain text** to preserve dates and exact amounts. To add an expense, enter a new row with **Record ID empty**; copying an existing row and clearing its ID is convenient. Fill Date, Type, Payment and Currency explicitly. Category may be empty for Unfiled (slot 0). Remaining text fields can be empty.

| Column | Meaning |
|:-------|:--------|
| Record ID | Adapter-managed stable identity; leave empty for a new expense and keep existing IDs unchanged |
| Date | Text in `YYYY-MM-DD`, years 1904–2031, matching Palm's packed date format |
| Type | Palm type name, such as `Taxi`, `Hotel`, `Mileage` or `Other` |
| Payment | `AmEx`, `Cash`, `Check`, `CreditCard`, `MasterCard`, `Prepaid`, `VISA` or `Unfiled` |
| Currency | Readable label such as `NZD ($)`, `USD ($)` or `EUR (€)`; ISO text such as `NZD` or legacy Palm codes 0–255 are also accepted |
| Amount | Exact text, such as `0012.3400`; sync performs no floating-point or exchange-rate conversion |
| Vendor / City / Attendees / Note | Plain text; attendees remain one string, as on Palm |
| Category | Existing Palm category slot, 0–15; category definitions stay on Palm |
| Deleted | `TRUE` to delete; `FALSE` or empty to retain |

Type names: Airfare, Breakfast, Bus, Business Meals, Car Rental, Dinner, Entertainment, Fax, Gas, Gifts, Hotel, Incidentals, Laundry, Limo, Lodging, Lunch, Mileage, Other, Parking, Postage, Snack, Subway, Supplies, Taxi, Telephone, Tips, Tolls and Train. Unknown type/payment byte values are preserved as `#0` through `#255`.

Use the currency label exported from an expense in the desired currency. Existing numeric cells are upgraded on the next successful sync without changing their Palm currency: code **17 is NZD ($)**, code **23 is USD ($)**. A bare `$` is ambiguous and is rejected; specify `NZD`, `USD`, `AUD`, etc. Unknown/custom currency slots stay reversible as `#0` through `#255`; their definitions are not guessed. Existing sync identities and field hashes are preserved during this display upgrade. Custom-currency definitions, exchange rates, category names and the Mileage distance-unit preference remain on Palm and are not changed by this conduit. [Full Drive backup](drive.md) also preserves database app-info and preferences. No money, currency or mileage-unit conversion is performed.

The numeric format matters: **USD is decimal 23, hexadecimal `0x17`**; **NZD is decimal 17, hexadecimal `0x11`** in the classic Expense mapping. The adapter's old numeric Sheet cells were decimal, not hexadecimal. These values agree with both the [Palm OS 4 country constants](https://github.com/jichu4n/palm-os-sdk/blob/master/sdk-4/include/Core/System/PalmLocale.h#L254) and the [J-Pilot Expense table](https://github.com/juddmon/jpilot/blob/master/Expense/expense.c#L94). The SDK preserves these first 33 country IDs across its locale changes.

A `$` symbol on the handheld alone does not identify USD. To verify the actual selected country, open an expense's **Details → Currency → Edit currencies** and inspect the country assigned to that entry. If a record explicitly set to United States still exports decimal `17`, that is a device/record discrepancy to investigate; changing the standard mapping to make decimal `17` mean USD would misidentify New Zealand records. The user's reported country/code mismatch is not yet resolved by the reference lookup.

Sort or move **whole rows including Record ID**, between syncs. Linked records are matched by ID, not saved row numbers. Keep reports and formulas on separate tabs; formula cells in A:L stop sync rather than being overwritten. Literal text starting with `=` is supported and is written as text. If you change Amount or Date to a numeric/date-formatted cell, convert it back to plain text before sync.

## Merging and deleting

Changes to different fields merge. If both sides change the same field differently, the default **Stop for review** leaves transfers pending. Choose **Prefer Palm edits** or **Prefer Google edits** on the Expenses page to resolve competing fields; independent edits still merge. Unedited Google characters that Palm cannot display remain in Sheets.

Deleting a linked Palm expense sets its Sheet row's **Deleted** cell to `TRUE`. Setting that cell to `TRUE` removes its Palm counterpart at the next successful HotSync. The Sheet row remains as an audit record; after that HotSync completes, you can remove the deleted row to free space. A deletion competing with an edit follows the conflict preference.

Removing an active linked row, clearing its Record ID or deleting/moving the spreadsheet stops sync. Restore the original row/ID/file, then use `Deleted=TRUE` for intentional deletions. A Palm record that disappears without a deletion tombstone is reimported from its linked live row. Private and archived Palm records are excluded and are not reimported through an existing public mapping.

## Recovery and limits

Finish editing the spreadsheet before HotSync. The adapter compares a fresh complete table before uploading and checks affected rows again before writing to Palm. Sheets does not provide the Docs API's revision precondition: these checks cannot make simultaneous spreadsheet/Palm edits atomic. A post-upload conflict can leave Sheet updates applied while Palm updates wait for the next sync.

Spreadsheet creation uses a persisted private Drive marker. Appends use persisted row IDs; an unanswered append is searched for on the next sync and is never automatically appended again while absent. If a creation remains unconfirmed, check Drive and the sheet, wait, then retry HotSync. **Retry unconfirmed creations** applies once to currently pending operations; using it before Google's result becomes visible can create duplicates.

Incoming Palm IDs are saved before writing. If assigning an incoming row's Record ID is interrupted and the ID is still absent, the sync result tells you which ID to restore on that row. This pauses ambiguous imports rather than creating duplicate Palm records. Settings/storage failures stop further transfers; retain the ESP32's saved mappings.

Defaults are **64 Palm records**, **64 nonempty Sheet rows including deleted rows**, **24,000 combined Palm/Sheets content bytes**, a **48,000-byte HTTP response limit**, and **8,000 Palm bytes per record** (each text field at most 4,095 bytes). All Palm records count toward its snapshot limit, including private/archive entries. Malformed, duplicate, oversized or incomplete snapshots stop before expense transfers. Limits are configurable as described in [Configuration](../configuration.md); available heap and shared NVS storage can impose lower limits.

**Check Google access** does not create files. Before the first HotSync it checks Drive access and granted scopes; after initialization it also reads the actual Sheets tab. Access success does not establish that Palm decoding or transfers succeeded. Read **Last sync** or the `Expenses:` line in the Palm HotSync log.

The user reported Expense/Google Sheets sync working on 15 October 2026. Firmware-build and sanitizer host tests also cover Expense records and scripted Sheets/Drive services. The readable currency-label upgrade and larger books still need a device retest.

## Protocol references

The record layout and selector names follow the [pilot-link Expense codec](https://github.com/desrod/pilot-link/blob/master/libpisock/expense.c) and [Expense definitions](https://github.com/desrod/pilot-link/blob/master/include/pi-expense.h). Google calls use [Drive file creation](https://developers.google.com/workspace/drive/api/reference/rest/v3/files/create), [Sheets values reads](https://developers.google.com/workspace/sheets/api/reference/rest/v4/spreadsheets.values/get) and [Sheets batch updates](https://developers.google.com/workspace/sheets/api/reference/rest/v4/spreadsheets/batchUpdate) and [raw value updates](https://developers.google.com/workspace/sheets/api/reference/rest/v4/spreadsheets.values/batchUpdate).

Currency selector reference: [J-Pilot Expense country IDs](https://github.com/juddmon/jpilot/blob/master/Expense/expense.c).
