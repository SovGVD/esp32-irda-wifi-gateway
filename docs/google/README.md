# Google services and shared sign-in

[Documentation](../README.md)

Mail, Tasks, Calendar, Contacts, Memo Pad, Note Pad, Expenses and Drive reuse one Google OAuth client and refresh token. Their sync switches are independent. Slides permission is prepared for future integrations; Slides sync is not implemented.

| Palm application | Google service | Guide |
|:-----------------|:---------------|:------|
| Mail | Gmail via IMAP/SMTP | [Mail](../mail.md) |
| To Do List | Google Tasks | [Tasks](tasks.md) |
| Date Book | Google Calendar | [Calendar](calendar.md) |
| Address Book | Google Contacts / People API | [Contacts](contacts.md) |
| Memo Pad | Google Docs | [Memos](memos.md) |
| Note Pad (Palm OS 4 drawings) | PNG files in Google Drive | [Notes](notes.md) |
| Expense | Google Sheets | [Expenses](expenses.md) |
| Full backup, install and restore | Google Drive | [Drive](drive.md) |

## Shared Google sign-in

The adapter logs in to Gmail with Google's own sign-in (OAuth, SASL XOAUTH2) instead of a password. Google's sign-in page needs a modern browser, so this is done once from a PC on the same Wi-Fi network. Each owner uses their own Google "OAuth client": an app used only by its owner needs no review by Google.

**A. Create the OAuth client** (once, at [console.cloud.google.com](https://console.cloud.google.com/), signed in with any Google account):

1. Create a project (top bar → project list → New project), for example "Palm mail".
2. APIs & Services → Library → enable **Gmail API**, **Google Tasks API**, **People API** (Contacts), **Google Calendar API**, **Google Drive API**, **Google Docs API**, **Google Sheets API** and **Google Slides API**.
3. Google Auth Platform (or "OAuth consent screen") → Get started: an app name, your e-mail address as support and contact address, Audience **External**.
4. Audience → **Publish app**, so that its status is **In production**. Leave it unverified. In "Testing" status Google ends the sign-in after 7 days.
5. Data Access → add all scopes in [Shared Google permissions](#shared-google-permissions), including basic profile access.
6. Clients → Create client → Application type **Desktop app** → Create. Keep the **Client ID** and **Client secret** it shows.

**B. Sign in:**

1. On the Palm, on the portal's Mail page, tap **Gmail: sign in with Google (from a PC)**. The page shows an address such as `http://192.168.1.50/google`. For the next 15 minutes the adapter answers that page on the Wi-Fi side; at all other times only the Palm reaches the portal.
2. Open that address on the PC. Enter the Gmail address, the client ID and the client secret, and press **Continue**.
3. Press **Open Google's sign-in page**, choose the Gmail account and allow access. Google warns that it "hasn't verified this app": it is your own client, so choose Continue (under "Advanced" in some versions).
4. Google then sends the browser to `http://127.0.0.1/?state=...&code=...`, a page that does not load. That is expected: copy the whole address from the address bar, paste it into the adapter's page and press **Finish**.
5. The page then logs in to both servers and shows the outcome. Mail sync is on; tap HotSync on the Palm.

**Notes:**

- The shared sign-in requests Mail, Tasks, Contacts, Calendar, Drive, Docs, Sheets and Slides permissions together, plus basic profile permission for the Contacts account identity check. Enable the corresponding APIs in the same Cloud project and add the [listed scopes](#shared-google-permissions) in Google Auth Platform → Data Access. Grant all requested permissions on the consent page to prepare the refresh token for their future integrations.
- Mail, Tasks, Calendar, Contacts, Memo Pad, Note Pad, Expenses and Drive sync are implemented. Expenses uses Sheets; Slides authorization is prepared for future features. Granting permission does not enable individual conduits.
- The adapter keeps the "refresh token" Google gives it, and gets a fresh access token from Google at the start of each sync. The token, client ID and secret are stored unencrypted in the ESP32's flash, like the Wi-Fi passwords. The token gives full access to the mailbox (IMAP and SMTP accept no narrower permission).
- Google ends a sign-in when the account's password is changed, when access is removed (Google account → Security → Your connections to third-party apps), when it has not been used for six months, or after 7 days if the app is still in "Testing". The HotSync log then says `Google sign-in no longer valid, sign in again from a PC`: repeat step B.
- **Sign out of Google** on the Mail page makes the adapter forget the token; the account's own security page removes the access on Google's side.
- Changing the e-mail address on the Palm ends the Google sign-in (it belonged to the other account).

## Shared Google permissions

The next sign-in requests the following read/write permissions in one consent. The completion page shows which permissions Google actually granted, and future clients can check each exact scope through the shared settings.

| Service | Scope | Access |
|:--------|:------|:-------|
| Mail | `https://mail.google.com/` | Gmail IMAP/SMTP |
| Tasks | `https://www.googleapis.com/auth/tasks` | Read and change task lists and tasks |
| Contacts | `https://www.googleapis.com/auth/contacts` | Read and change personal contacts through People API |
| Basic profile | `https://www.googleapis.com/auth/userinfo.profile` | Identify the account through People API `people/me`; only metadata is requested |
| Calendar | `https://www.googleapis.com/auth/calendar` | Read and manage calendars and events |
| Drive | `https://www.googleapis.com/auth/drive` | Read and manage all Drive files |
| Docs | `https://www.googleapis.com/auth/documents` | Read and edit Google Docs documents |
| Sheets | `https://www.googleapis.com/auth/spreadsheets` | Read and edit Google Sheets spreadsheets |
| Slides | `https://www.googleapis.com/auth/presentations` | Read and edit Google Slides presentations |

These scopes cover the named services' planned read/write integrations. Drive uses full-file access so backup/restore can work with existing uploaded files; `drive.file` would cover only files created or selected for this app. Special APIs such as Workspace Directory or Drive app-data may require separate scopes if added later.

Google documents these permissions in the [People API scope list](https://developers.google.com/identity/protocols/oauth2/scopes#people), [Calendar scope guide](https://developers.google.com/workspace/calendar/api/auth), [Drive scope guide](https://developers.google.com/workspace/drive/api/guides/api-specific-auth), [Docs scope guide](https://developers.google.com/workspace/docs/api/auth), [Sheets scope guide](https://developers.google.com/workspace/sheets/api/scopes) and [Slides scope guide](https://developers.google.com/workspace/slides/api/scopes).

Google's [People profile example](https://developers.google.com/apps-script/advanced/people#get_the_person_for_the_user) requires `userinfo.profile` separately from Contacts permission. Account identity prevents contact mappings from being applied to a different Google account. Tokens issued by older firmware, including tokens with all eight service permissions, need one more sign-in to add this missing profile grant; refreshing a token cannot add permissions.

## Related guides

- [Internet connection](../internet.md)
- [HotSync](../hotsync.md)
- [Troubleshooting](../troubleshooting.md)
