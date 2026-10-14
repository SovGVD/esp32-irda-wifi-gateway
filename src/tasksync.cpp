#include "tasksync.h"
#include "hotsync.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

static bool date_parts(const std::string &due, unsigned &y, unsigned &m, unsigned &d) {
    int used = 0;
    if (sscanf(due.c_str(), "%4u-%2u-%2u%n", &y, &m, &d, &used) != 3 || used != 10 ||
        due.size() != 10 || y < 1904 || y > 2031 || m < 1 || m > 12) return false;
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned limit = days[m-1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return d > 0 && d <= limit;
}
bool tasks_unpack_record(const uint8_t *data, size_t len, PalmTask &t) {
    if (len < 5) return false;
    const uint8_t *end = data + len;
    const uint8_t *title_end = (const uint8_t *)memchr(data + 3, 0, len - 3);
    if (!title_end) return false;
    const uint8_t *notes = title_end + 1;
    const uint8_t *note_end = (const uint8_t *)memchr(notes, 0, end - notes);
    if (!note_end || note_end + 1 != end) return false;
    t.title.assign((const char *)data + 3, title_end - data - 3);
    t.notes.assign((const char *)notes, note_end - notes);
    if (t.title.size() > TASKSYNC_TITLE_MAX || t.notes.size() > TASKSYNC_NOTE_MAX) return false;
    t.completed = (data[2] & 0x80) != 0; t.priority = data[2] & 0x7f;
    if (t.priority < 1 || t.priority > 5) return false;
    unsigned date = (data[0] << 8) | data[1];
    t.due.clear();
    if (date != 0xffff) {
        char due[16]; snprintf(due, sizeof(due), "%04u-%02u-%02u", 1904 + (date >> 9), (date >> 5) & 15, date & 31);
        t.due = due; unsigned y, m, d;
        if (!date_parts(t.due, y, m, d)) return false;
    }
    return true;
}
size_t tasks_pack_record(const PalmTask &t, uint8_t *data, size_t max_len) {
    size_t len = 5 + t.title.size() + t.notes.size();
    if (len > max_len || t.title.size() > TASKSYNC_TITLE_MAX || t.notes.size() > TASKSYNC_NOTE_MAX ||
        t.priority < 1 || t.priority > 5 || t.title.find('\0') != std::string::npos || t.notes.find('\0') != std::string::npos) return 0;
    unsigned date = 0xffff;
    if (!t.due.empty()) {
        unsigned y, m, d; if (!date_parts(t.due, y, m, d)) return 0;
        date = ((y - 1904) << 9) | (m << 5) | d;
    }
    data[0] = date >> 8; data[1] = date; data[2] = t.priority | (t.completed ? 0x80 : 0);
    memcpy(data + 3, t.title.c_str(), t.title.size() + 1);
    memcpy(data + 4 + t.title.size(), t.notes.c_str(), t.notes.size() + 1);
    return len;
}
static uint32_t hash(const std::string &s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}
static std::array<uint32_t,4> fields(const PalmTask &t) {
    return {{hash(t.title), hash(t.notes), hash(t.due), t.completed ? 1u : 0u}};
}
static std::string palm_text(const std::string &s, PalmCharset charset, size_t limit) {
    return palm_from_utf8(s, charset, limit);
}
static PalmTask display(const GoogleTask &g, PalmCharset charset, uint8_t priority) {
    PalmTask t;
    t.title = palm_text(g.title, charset, TASKSYNC_TITLE_MAX);
    t.notes = palm_text(g.notes, charset, TASKSYNC_NOTE_MAX);
    t.completed = g.status == "completed"; t.priority = priority;
    unsigned y, m, d;
    if (g.due.size() >= 10 && date_parts(g.due.substr(0,10), y, m, d)) t.due = g.due.substr(0,10);
    return t;
}
static GoogleTaskPatch patch(const PalmTask &t, const TaskSyncMap *map, PalmCharset charset) {
    auto f = fields(t); GoogleTaskPatch p;
    p.title_set = !map || f[0] != map->fields[0]; p.title = palm_to_utf8(t.title, charset);
    p.notes_set = !map || f[1] != map->fields[1]; p.notes = palm_to_utf8(t.notes, charset);
    p.due_set = !map || f[2] != map->fields[2]; p.due = t.due.empty() ? "" : t.due + "T00:00:00.000Z";
    p.status_set = !map || f[3] != map->fields[3]; p.status = t.completed ? "completed" : "needsAction";
    return p;
}
struct LocalTask { uint32_t id; uint8_t attr, category; PalmTask task; };
static bool write_palm(uint8_t db, uint32_t id, uint8_t attr, uint8_t category, const PalmTask &t, uint32_t &saved) {
    size_t room; uint8_t *buffer = hotsync_record_buffer(&room);
    size_t len = tasks_pack_record(t, buffer, room);
    return len && hotsync_write_record(db, id, attr & ~HOTSYNC_ATTR_DIRTY, category, len, &saved);
}
void tasksync_run(const GoogleTasksSetup &setup, PalmCharset charset, TaskSyncHost &host, TaskSyncResult &r) {
    r = TaskSyncResult();
    if (!host.online()) { r.problem = "Wi-Fi is not connected"; return; }
    std::vector<TaskSyncMap> map;
    if (!host.load_map(map) || map.size() > setup.max_tasks) { r.problem = "Task sync state could not be loaded"; return; }
    std::string token;
    if (!host.access_token(token, r.problem)) return;
    GoogleTasksClient google(host.http(), setup, token);
    std::vector<GoogleTask> remote;
    std::string list_id;
    if (!google.resolve_list(list_id) || !google.list(remote)) { r.problem = google.error(); return; }
    if (!host.bind_list(list_id)) { r.problem = "Task list or Palm identity does not match the saved sync state"; r.link_lost = hotsync_error() == HOTSYNC_ERR_LINK; return; }
    if (!hotsync_open_conduit()) { r.problem = "Could not open the Tasks conduit"; r.link_lost = hotsync_error() == HOTSYNC_ERR_LINK; return; }
    uint8_t db;
    if (!hotsync_open_db("ToDoDB", HOTSYNC_MODE_READ | HOTSYNC_MODE_WRITE, &db)) {
        r.problem = "Could not open ToDoDB"; r.link_lost = hotsync_error() == HOTSYNC_ERR_LINK; return;
    }
    auto work = [&]() {
        std::vector<LocalTask> local;
        // Collect all indices before mutation; a partial/malformed database
        // must never be mistaken for a collection of deleted records.
        size_t text_bytes = 0;
        for (unsigned index = 0; index <= setup.max_tasks; ++index) {
            HotSync_Record rec;
            if (!hotsync_read_record_by_index(db, index, &rec)) {
                if (hotsync_error() == HOTSYNC_DLP_NOT_FOUND) break;
                r.problem = "Could not read the To Do List"; return;
            }
            if (index == setup.max_tasks) { r.problem = "Palm To Do List exceeds the configured task limit"; return; }
            LocalTask t; t.id = rec.id; t.attr = rec.attr; t.category = rec.category;
            if (!(rec.attr & (HOTSYNC_ATTR_DELETED | HOTSYNC_ATTR_ARCHIVED | HOTSYNC_ATTR_SECRET))) {
                if (rec.truncated || !tasks_unpack_record(rec.data, rec.len, t.task)) { r.problem = "A Palm task is too large or invalid"; return; }
                text_bytes += t.task.title.size() + t.task.notes.size();
                if (text_bytes > 48000) { r.problem = "Palm task text exceeds the sync memory limit"; return; }
            }
            local.push_back(t);
        }
        // Stale Palm IDs may have been purged by another desktop. Reimport.
        map.erase(std::remove_if(map.begin(), map.end(), [&](const TaskSyncMap &m) {
            return std::none_of(local.begin(), local.end(), [&](const LocalTask &t) { return t.id == m.palm_id; });
        }), map.end());
        size_t projected = std::count_if(remote.begin(), remote.end(), [](const GoogleTask &g) { return !g.deleted; });
        for (const auto &t : local) {
            if (!(t.attr & (HOTSYNC_ATTR_DELETED | HOTSYNC_ATTR_ARCHIVED | HOTSYNC_ATTR_SECRET)) &&
                std::none_of(map.begin(), map.end(), [&](const TaskSyncMap &m) { return m.palm_id == t.id; })) projected++;
        }
        if (projected > setup.max_tasks) { r.problem = "Combined task list exceeds the configured task limit"; return; }
        // Do not upload private or archived records. Keep their mappings so
        // they cannot be reimported as public tasks on the next sync.
        for (auto &t : local) {
            auto it = std::find_if(map.begin(), map.end(), [&](const TaskSyncMap &m) { return m.palm_id == t.id; });
            if (t.attr & (HOTSYNC_ATTR_SECRET | HOTSYNC_ATTR_ARCHIVED)) continue;
            if (t.attr & HOTSYNC_ATTR_DELETED) {
                if (it != map.end()) {
                    if (!google.remove(it->google_id)) { r.problem = google.error(); return; }
                    for (auto &g : remote) if (g.id == it->google_id) g.deleted = true;
                    map.erase(it);
                    if (!host.store_map(map)) { r.problem = "Could not save task sync state"; return; }
                }
                if (!hotsync_delete_record(db, t.id)) { r.problem = "Could not remove a deleted Palm task"; return; }
                r.deleted++; continue;
            }
            bool fresh = it == map.end();
            if (fresh && map.size() >= setup.max_tasks) { r.problem = "Task sync map is full"; return; }
            auto g = remote.end();
            if (!fresh) g = std::find_if(remote.begin(), remote.end(), [&](const GoogleTask &v) { return v.id == it->google_id; });
            // A complete snapshot proves a mapped remote task was deleted.
            if (!fresh && (g == remote.end() || g->deleted)) {
                if (!hotsync_delete_record(db, t.id)) { r.problem = "Could not delete a Palm task"; return; }
                map.erase(it);
                if (!host.store_map(map)) { r.problem = "Could not save task sync state"; return; }
                r.deleted++; continue;
            }
            GoogleTask saved;
            auto changes = patch(t.task, fresh ? nullptr : &*it, charset);
            if (fresh || changes.title_set || changes.notes_set || changes.due_set || changes.status_set) {
                // Palm wins conflicts only for fields edited since the baseline.
                if (!google.save(fresh ? "" : it->google_id, changes, saved)) { r.problem = google.error(); return; }
                if (fresh) {
                    TaskSyncMap entry; entry.palm_id = t.id; entry.google_id = saved.id; entry.fields = fields(t.task);
                    map.push_back(entry); it = map.end() - 1;
                    // Save identity before any further Palm transaction.
                    if (!host.store_map(map)) { r.problem = "Could not save task sync state"; return; }
                }
                r.uploaded++;
            } else saved = *g;
            PalmTask target = display(saved, charset, t.task.priority);
            uint32_t id = t.id;
            if (fields(target) != fields(t.task) || (t.attr & HOTSYNC_ATTR_DIRTY)) {
                if (!write_palm(db, t.id, t.attr, t.category, target, id)) { r.problem = "Could not write a Palm task"; return; }
                r.downloaded++;
            }
            it->fields = fields(target);
            if (!host.store_map(map)) { r.problem = "Could not save task sync state"; return; }
        }
        for (const auto &g : remote) {
            if (g.deleted || std::any_of(map.begin(), map.end(), [&](const TaskSyncMap &m) { return m.google_id == g.id; })) continue;
            // Palm records removed by another desktop are imported again; only
            // explicit Palm deletion tombstones propagate to Google.
            if (map.size() >= setup.max_tasks) { r.problem = "Task sync map is full"; return; }
            PalmTask t = display(g, charset, 1); uint32_t id;
            if (!write_palm(db, 0, 0, 0, t, id)) { r.problem = "Could not add a Palm task"; return; }
            TaskSyncMap m; m.palm_id = id; m.google_id = g.id; m.fields = fields(t);
            map.push_back(m);
            if (!host.store_map(map)) { r.problem = "Could not save task sync state"; return; }
            r.downloaded++;
        }
    };
    work();
    r.link_lost = hotsync_error() == HOTSYNC_ERR_LINK;
    if (!r.link_lost && !hotsync_close_db(db) && r.problem.empty()) r.problem = "Could not close ToDoDB";
    r.link_lost = r.link_lost || hotsync_error() == HOTSYNC_ERR_LINK;
}
std::string tasksync_summary(const TaskSyncResult &r) {
    std::string s = std::to_string(r.uploaded) + " uploaded, " + std::to_string(r.downloaded) + " downloaded, " + std::to_string(r.deleted) + " deleted";
    if (!r.problem.empty()) s += ". " + r.problem;
    return s;
}
