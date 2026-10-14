#pragma once

// Shared by firmware configuration and portable OAuth tests. Request planned
// services together so their future clients can reuse the same refresh token.
#define GOOGLE_MAIL_SCOPE         "https://mail.google.com/"
#define GOOGLE_TASKS_SCOPE        "https://www.googleapis.com/auth/tasks"
#define GOOGLE_CONTACTS_SCOPE     "https://www.googleapis.com/auth/contacts"
// People.get(people/me) requires profile permission even when requesting only
// metadata. Its canonical account ID keeps contact mappings account-bound.
#define GOOGLE_PROFILE_SCOPE      "https://www.googleapis.com/auth/userinfo.profile"
#define GOOGLE_CALENDAR_SCOPE     "https://www.googleapis.com/auth/calendar"
#define GOOGLE_DRIVE_SCOPE        "https://www.googleapis.com/auth/drive"
#define GOOGLE_DOCS_SCOPE         "https://www.googleapis.com/auth/documents"
#define GOOGLE_SHEETS_SCOPE       "https://www.googleapis.com/auth/spreadsheets"
#define GOOGLE_SLIDES_SCOPE       "https://www.googleapis.com/auth/presentations"

#define GOOGLE_DEFAULT_SCOPES     GOOGLE_MAIL_SCOPE " " GOOGLE_TASKS_SCOPE " " \
                                  GOOGLE_CONTACTS_SCOPE " " GOOGLE_CALENDAR_SCOPE " " GOOGLE_DRIVE_SCOPE " " \
                                  GOOGLE_DOCS_SCOPE " " GOOGLE_SHEETS_SCOPE " " GOOGLE_SLIDES_SCOPE " " GOOGLE_PROFILE_SCOPE
