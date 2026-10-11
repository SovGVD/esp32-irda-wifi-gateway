#include "mailsync.h"
#include "hotsync.h"
#include "mailoauth.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <vector>

#define MAIL_CREATOR          0x6D61696CUL  // 'mail'
#define MAIL_DATABASE         "MailDB"
#define MAIL_PREF_LOCAL_SYNC  1             // HotSync Options for a local HotSync
#define MAIL_PREF_SIGNATURE   3

static const MailSyncSetup *s_setup = nullptr;
static MailSyncHost *s_host = nullptr;
static std::string s_access_token;  // Google sign-in: fetched once per sync, when first needed
static std::string s_sign_in_error; // ... or why that failed (not tried again in the same sync)

static std::string number(unsigned long value) {
    char text[12];
    snprintf(text, sizeof(text), "%lu", value);
    return text;
}

static void note_problem(MailSyncResult &outcome, const std::string &problem) {
    s_host->trace(problem);
    if (outcome.problem.empty()) {
        outcome.problem = problem;
    }
}

static bool link_lost() {
    return hotsync_error() == HOTSYNC_ERR_LINK;
}

static bool google_login() {
    return !s_setup->oauth_refresh_token.empty();
}

// The password for the servers: with Google sign-in, an access token fetched
// from the token server first (over the same connection, before the mail server)
static bool sign_in(std::string &error) {
    if (!google_login() || !s_access_token.empty()) {
        return true;
    }
    if (!s_sign_in_error.empty()) {
        error = s_sign_in_error;
        return false;
    }
    OAuthClient google(s_host->connection(), hotsync_keep_alive);
    OAuthTokens tokens;
    if (!google.refresh(s_setup->oauth_client_id, s_setup->oauth_client_secret, s_setup->oauth_refresh_token, tokens)) {
        error = s_sign_in_error = google.error();
        return false;
    }
    s_access_token = tokens.access_token;
    return true;
}

static const std::string &password() {
    return google_login() ? s_access_token : s_setup->password;
}

static std::string capped(const std::string &text, size_t max_len) {
    return text.length() > max_len ? text.substr(0, max_len) : text;
}

// Sends what is in the Outbox; each message leaves the Palm once the server took it
static void send_outbox(uint8_t db, const std::string &signature, int64_t now, int offset_minutes, MailSyncResult &outcome) {
    // Note the records first: deleting while stepping through the category could skip some
    std::vector<uint32_t> ids;
    HotSync_Record record;
    while (ids.size() < s_setup->max_send && hotsync_read_next_in_category(db, MAIL_CATEGORY_OUTBOX, &record)) {
        ids.push_back(record.id);
    }
    if (ids.empty() || link_lost()) {
        return;
    }
    s_host->trace(number(ids.size()) + " in the Outbox");
    if (!s_host->online()) {
        outcome.not_sent = ids.size();
        note_problem(outcome, "Wi-Fi is not connected");
        return;
    }

    std::string error;
    if (!sign_in(error)) {
        outcome.not_sent = ids.size();
        note_problem(outcome, error);
        return;
    }
    SmtpClient smtp(s_host->connection(), hotsync_keep_alive);
    if (!smtp.open(s_setup->smtp_host, s_setup->smtp_port, s_setup->login, password(), google_login())) {
        outcome.not_sent = ids.size();
        note_problem(outcome, smtp.error());
        return;
    }

    MailIdentity sender;
    sender.address = s_setup->address;
    sender.name = s_setup->name;
    size_t at_sign = sender.address.find('@');
    std::string domain = (at_sign == std::string::npos) ? "localhost" : sender.address.substr(at_sign + 1);

    for (size_t i = 0; i < ids.size() && !link_lost(); i++) {
        MailMessage message;
        if (!hotsync_read_record(db, ids[i], &record) || !mail_unpack_record(record.data, record.len, message)) {
            continue;
        }
        std::vector<std::string> recipients;
        mail_extract_addresses(message.to, recipients);
        mail_extract_addresses(message.cc, recipients);
        mail_extract_addresses(message.bcc, recipients);
        if (recipients.empty()) {
            outcome.not_sent++;
            note_problem(outcome, "a message has no e-mail address to send to");
            continue;
        }

        char id[48];
        snprintf(id, sizeof(id), "%08lx.%08lx@", (unsigned long)now, (unsigned long)s_host->random());
        std::string text = mail_compose(message, sender, s_setup->charset, signature, now, offset_minutes, id + domain);
        if (!smtp.send(sender.address, recipients, text)) {
            outcome.not_sent++;
            note_problem(outcome, smtp.error());
            continue;
        }
        outcome.sent++;
        if (!hotsync_delete_record(db, ids[i])) {
            // Still in the Outbox, so it would go out again next time: stop here
            note_problem(outcome, "a sent message could not be removed from the Outbox");
            break;
        }
    }
    smtp.close();
}

// ------------------------------------------------------------------------------
// Read and Deleted, Both Ways
// ------------------------------------------------------------------------------

// What became of a linked message on the Palm
struct PalmState {
    bool gone = false;     // Deleted, or purged since
    uint8_t category = MAIL_CATEGORY_INBOX;
    bool read = false;
};

// Only the start of each record is read: the read flag is in its fifth byte
static bool read_palm_states(uint8_t db, const std::vector<MailLink> &links, std::vector<PalmState> &states) {
    states.assign(links.size(), PalmState());
    for (size_t i = 0; i < links.size(); i++) {
        HotSync_Record record;
        if (!hotsync_read_record_start(db, links[i].record_id, 6, &record)) {
            if (link_lost()) {
                return false;
            }
            states[i].gone = true;
            continue;
        }
        states[i].gone = (record.attr & HOTSYNC_ATTR_DELETED) != 0;
        states[i].category = record.category;
        states[i].read = record.len >= 5 && (record.data[4] & 0x80) != 0;
    }
    return true;
}

// Sets the read flag of a record in the Palm's Inbox
static bool mark_read_on_palm(uint8_t db, uint32_t record_id) {
    HotSync_Record record;
    MailMessage message;
    if (!hotsync_read_record(db, record_id, &record) || record.truncated || !mail_unpack_record(record.data, record.len, message)) {
        return false;
    }
    uint8_t attr = record.attr & HOTSYNC_ATTR_SECRET;
    uint8_t category = record.category;
    message.read = true;
    size_t room;
    uint8_t *buffer = hotsync_record_buffer(&room);
    size_t len = mail_pack_record(message, buffer, room);
    return len > 0 && hotsync_write_record(db, record_id, attr, category, len, nullptr);
}

// The first `count` links are messages copied in earlier syncs: whatever
// happened to one of them on one side since the last sync is done on the other.
// Messages read on the Palm become read on the server and the other way round;
// deleted on the Palm, they go to the server's trash; gone from the server's
// inbox (deleted or archived there), they leave the Palm's Inbox. Links that
// end are taken out of `links`.
static bool reconcile(ImapClient &imap, uint8_t db, std::vector<MailLink> &links, size_t count,
                      const std::vector<PalmState> &palm, MailSyncResult &outcome) {
    std::vector<uint32_t> uids;
    for (size_t i = 0; i < count; i++) {
        uids.push_back(links[i].uid);
    }
    std::vector<ImapMessage> server;
    if (!imap.fetch_flags(uids, server)) {
        note_problem(outcome, imap.error());
        return false;
    }

    std::vector<uint32_t> to_mark, to_remove;
    std::vector<uint32_t> palm_read, palm_delete;
    std::vector<bool> ended(count, false);
    std::vector<bool> removed(count, false); // Waits for the server to remove it
    for (size_t i = 0; i < count; i++) {
        const ImapMessage *found = nullptr;
        for (size_t k = 0; k < server.size() && found == nullptr; k++) {
            if (server[k].uid == links[i].uid) {
                found = &server[k];
            }
        }
        if (palm[i].gone || palm[i].category == MAIL_CATEGORY_DELETED) {
            if (found != nullptr) {
                to_remove.push_back(links[i].uid);
                removed[i] = true;
            } else if (!palm[i].gone) {
                palm_delete.push_back(links[i].record_id);
            }
            ended[i] = found == nullptr;
        } else if (found == nullptr) {
            // Gone from the inbox: a message the owner filed on the Palm stays
            if (palm[i].category == MAIL_CATEGORY_INBOX) {
                palm_delete.push_back(links[i].record_id);
                outcome.changed_on_palm++;
            }
            ended[i] = true;
        } else if (palm[i].read != links[i].read) {
            if (palm[i].read && !found->seen) {
                to_mark.push_back(links[i].uid);
            }
            links[i].read = palm[i].read;
        } else if (found->seen != links[i].read) {
            if (found->seen && !palm[i].read) {
                palm_read.push_back(links[i].record_id);
            }
            links[i].read = found->seen;
        }
    }

    bool ok = true;
    if (!to_mark.empty()) {
        if (imap.mark_read(to_mark)) {
            outcome.changed_on_server += to_mark.size();
        } else {
            note_problem(outcome, imap.error());
            ok = false;
        }
    }
    if (ok && !to_remove.empty()) {
        std::string trash;
        if (imap.find_trash(trash) && imap.remove(to_remove, trash)) {
            outcome.changed_on_server += to_remove.size();
            for (size_t i = 0; i < count; i++) {
                if (removed[i]) {
                    ended[i] = true;
                    if (!palm[i].gone) {
                        palm_delete.push_back(links[i].record_id); // Purged as a desktop sync would
                    }
                }
            }
        } else {
            note_problem(outcome, imap.error());
            ok = false;
        }
    }
    s_host->trace(number(to_mark.size()) + " marked read and " + number(to_remove.size()) + " deleted on the server, " +
                  number(palm_read.size()) + " marked read on the Palm");

    for (size_t i = 0; i < palm_delete.size() && !link_lost(); i++) {
        hotsync_delete_record(db, palm_delete[i]);
    }
    for (size_t i = 0; i < palm_read.size() && !link_lost(); i++) {
        if (mark_read_on_palm(db, palm_read[i])) {
            outcome.changed_on_palm++;
        }
    }

    std::vector<MailLink> kept;
    for (size_t i = 0; i < links.size(); i++) {
        if (i >= count || !ended[i]) {
            kept.push_back(links[i]);
        }
    }
    links.swap(kept);
    return ok;
}

// ------------------------------------------------------------------------------
// Inbox
// ------------------------------------------------------------------------------

// Copies the messages that arrived since the last sync into the Palm's Inbox
// (unless fetch_new is false), and passes on what became of the ones copied
// before. Without such changes the mailbox is opened read-only.
static void sync_inbox(uint8_t db, const MailSyncOptions &options, bool fetch_new, int offset_minutes, MailSyncResult &outcome) {
    PalmCharset charset = s_setup->charset;
    std::vector<MailLink> links;
    s_host->load_links(links);
    if (!fetch_new && links.empty()) {
        return;
    }
    if (!s_host->online()) {
        note_problem(outcome, "Wi-Fi is not connected");
        return;
    }
    std::string error;
    if (!sign_in(error)) {
        note_problem(outcome, error);
        return;
    }
    // The Palm side first, so that the server is not kept waiting
    std::vector<PalmState> palm;
    if (!read_palm_states(db, links, palm)) {
        return;
    }

    ImapClient imap(s_host->connection(), hotsync_keep_alive);
    uint32_t uid_validity = 0, exists = 0;
    std::vector<ImapMessage> newest;
    bool opened = imap.open(s_setup->imap_host, s_setup->imap_port, s_setup->login, password(), google_login()) &&
                  (links.empty() ? imap.examine_inbox(&uid_validity, &exists) : imap.select_inbox(&uid_validity, &exists));
    if (!opened || (fetch_new && !imap.list_newest(exists, s_setup->max_fetch, newest))) {
        note_problem(outcome, imap.error());
        return;
    }

    uint32_t known_validity, last_uid;
    s_host->load_position(known_validity, last_uid);
    size_t stored_links = links.size();
    if (known_validity != uid_validity) {
        last_uid = 0; // Another mailbox, or the server renumbered it
        links.clear();
    }
    size_t earlier = links.size();
    bool broken = false;
    uint32_t done_uid = last_uid;
    size_t truncate = options.truncate < MAILSYNC_BODY_MAX ? options.truncate : MAILSYNC_BODY_MAX;
    s_host->trace("inbox has " + number(exists) + ", looking at the newest " + number(newest.size()) + " after UID " +
                  number(last_uid));

    for (size_t i = 0; i < newest.size() && !link_lost(); i++) {
        uint32_t uid = newest[i].uid;
        if (uid <= last_uid) {
            continue;
        }
        if (options.type == MAIL_SYNC_UNREAD && newest[i].seen) {
            done_uid = uid;
            continue;
        }

        std::string headers, internal_date;
        ImapPart part;
        if (!imap.fetch_summary(uid, headers, internal_date, part)) {
            note_problem(outcome, imap.error());
            broken = true;
            break;
        }
        MailMessage message;
        message.subject = capped(mail_decode_header(mail_header_field(headers, "Subject"), charset), MAILSYNC_FIELD_MAX);
        message.from = capped(mail_decode_header(mail_header_field(headers, "From"), charset), MAILSYNC_FIELD_MAX);
        message.to = capped(mail_decode_header(mail_header_field(headers, "To"), charset), MAILSYNC_FIELD_MAX);
        message.cc = capped(mail_decode_header(mail_header_field(headers, "Cc"), charset), MAILSYNC_FIELD_MAX);
        message.reply_to = capped(mail_decode_header(mail_header_field(headers, "Reply-To"), charset), MAILSYNC_FIELD_MAX);
        message.read = newest[i].seen;
        std::string priority = mail_header_field(headers, "X-Priority");
        std::string importance = mail_header_field(headers, "Importance");
        if (priority[0] == '1' || priority[0] == '2' || strcasecmp(importance.c_str(), "high") == 0) {
            message.priority = MAIL_PRIORITY_HIGH;
        }
        if (!mail_filter_accepts(options, message.from, message.to, message.subject, message.priority == MAIL_PRIORITY_HIGH)) {
            done_uid = uid;
            continue;
        }
        int64_t when;
        if (mail_parse_date(mail_header_field(headers, "Date"), &when) || mail_parse_date(internal_date, &when)) {
            mail_set_date(message, when, offset_minutes);
        }

        if (part.section.empty()) {
            message.body = "(This message has no text.)";
        } else {
            // Encoded text is longer than what it decodes to, markup much longer
            uint32_t max_bytes = part.html ? s_setup->html_fetch_max : (uint32_t)truncate * 6 + 1024;
            if (part.size > 0 && part.size < max_bytes) {
                max_bytes = part.size;
            }
            MailTextDecoder decoder(part.charset, part.encoding, part.html, charset, truncate);
            if (!imap.fetch_text(uid, part, max_bytes, decoder)) {
                note_problem(outcome, imap.error());
                broken = true;
                break;
            }
            decoder.finish();
            message.body = decoder.text();
        }

        size_t room;
        uint8_t *buffer = hotsync_record_buffer(&room);
        size_t len = mail_pack_record(message, buffer, room);
        uint32_t record_id = 0;
        if (len == 0 || !hotsync_write_record(db, 0, 0, MAIL_CATEGORY_INBOX, len, &record_id)) {
            if (!link_lost()) {
                note_problem(outcome, "the Palm did not take a message (memory full?)");
            }
            break;
        }
        MailLink link = {record_id, uid, message.read};
        links.push_back(link);
        outcome.received++;
        done_uid = uid;
        s_host->trace("UID " + number(uid) + " to the Palm, " + number(len) + " bytes");
    }

    if (done_uid != last_uid || known_validity != uid_validity) {
        s_host->store_position(uid_validity, done_uid);
    }
    if (!broken && earlier > 0 && !link_lost()) {
        reconcile(imap, db, links, earlier, palm, outcome);
    }
    if (links.size() > MAILSYNC_LINKS_MAX) {
        links.erase(links.begin(), links.begin() + (links.size() - MAILSYNC_LINKS_MAX));
    }
    if (links.size() > 0 || stored_links > 0) {
        s_host->store_links(links);
    }
    imap.close();
}

// Minutes the Palm's clock is ahead of UTC, to the quarter hour; 0 if either clock cannot be trusted
static int palm_utc_offset(int64_t now) {
    HotSync_Time palm;
    if (now == 0 || !hotsync_get_time(&palm)) {
        return 0;
    }
    int64_t difference = mail_seconds(palm.year, palm.month, palm.day, palm.hour, palm.minute, palm.second) - now;
    int quarters = (int)((difference + (difference >= 0 ? 450 : -450)) / 900);
    return (quarters >= -14 * 4 && quarters <= 14 * 4) ? quarters * 15 : 0;
}

void mailsync_run(const MailSyncSetup &setup, MailSyncHost &host, MailSyncResult &outcome) {
    s_setup = &setup;
    s_host = &host;
    s_access_token.clear();
    s_sign_in_error.clear();
    outcome = MailSyncResult();
    hotsync_open_conduit();

    // What the user chose under Options > HotSync Options in the Mail application
    MailSyncOptions options;
    std::string signature;
    const uint8_t *data;
    size_t len;
    if (hotsync_read_preference(MAIL_CREATOR, MAIL_PREF_LOCAL_SYNC, true, &data, &len) ||
        hotsync_read_preference(MAIL_CREATOR, MAIL_PREF_LOCAL_SYNC, false, &data, &len)) {
        mail_unpack_sync_options(data, len, options);
    }
    if (hotsync_read_preference(MAIL_CREATOR, MAIL_PREF_SIGNATURE, true, &data, &len) ||
        hotsync_read_preference(MAIL_CREATOR, MAIL_PREF_SIGNATURE, false, &data, &len)) {
        signature.assign((const char *)data, strnlen((const char *)data, len));
    }
    host.trace("HotSync option " + number(options.type) + ", truncate at " + number(options.truncate));

    // The clock comes from the network; the Palm's own tells us its time zone
    int64_t now = host.now();
    int offset_minutes = palm_utc_offset(now);

    uint8_t db;
    if (!hotsync_open_db(MAIL_DATABASE, HOTSYNC_MODE_READ | HOTSYNC_MODE_WRITE, &db)) {
        note_problem(outcome, "the Mail database could not be opened");
        outcome.link_lost = link_lost();
        return;
    }
    send_outbox(db, signature, now, offset_minutes, outcome);
    if (!link_lost()) {
        sync_inbox(db, options, options.type != MAIL_SYNC_SEND_ONLY, offset_minutes, outcome);
    }

    // Purge what was deleted on the Palm, as a desktop sync would
    if (!link_lost()) {
        hotsync_cleanup_db(db);
        hotsync_reset_sync_flags(db);
        hotsync_close_db(db);
    }
    outcome.link_lost = link_lost();
}

std::string mailsync_summary(const MailSyncResult &result) {
    std::string text = number(result.sent) + " sent, " + number(result.received) + " received";
    if (result.not_sent > 0) {
        text += ", " + number(result.not_sent) + " not sent";
    }
    if (result.changed_on_server > 0 || result.changed_on_palm > 0) {
        text += ", " + number(result.changed_on_server + result.changed_on_palm) + " read or deleted";
    }
    if (!result.problem.empty()) {
        text += ". " + result.problem;
    }
    return text;
}

