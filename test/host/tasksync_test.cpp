// End-to-end Tasks conduit against fake_palm.py and an in-memory Google API.
// Reuses the real SLP/PADP/DLP implementation; no live credentials needed.
#include "hotsync.h"
#include "tasksync.h"
#include "cJSON.h"
#include "mailoauth.h"
#include "scripted_server.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static int fd = -1;
static int port_read(uint8_t *dest, size_t max_len, uint32_t timeout) {
    pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, timeout) <= 0) return 0;
    int n = read(fd, dest, max_len); return n > 0 ? n : -1;
}
static bool port_write(const uint8_t *data, size_t len) { return write(fd, data, len) == (ssize_t)len; }
static uint32_t port_millis() {
    timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static std::string json_text(cJSON *j) {
    char *p = cJSON_PrintUnformatted(j); assert(p); std::string out(p); free(p); cJSON_Delete(j); return out;
}
static std::string field(cJSON *j, const char *name) {
    auto *v = cJSON_GetObjectItemCaseSensitive(j, name); return cJSON_IsString(v) ? v->valuestring : "";
}
static cJSON *task_json(const GoogleTask &t) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", t.id.c_str()); cJSON_AddStringToObject(j, "title", t.title.c_str());
    cJSON_AddStringToObject(j, "notes", t.notes.c_str()); cJSON_AddStringToObject(j, "due", t.due.c_str());
    cJSON_AddStringToObject(j, "status", t.status.c_str()); cJSON_AddBoolToObject(j, "deleted", t.deleted);
    return j;
}
class Server : public TasksHttp {
public:
    std::map<std::string, GoogleTask> tasks;
    std::string list_id = "list-one";
    unsigned posts = 0, patches = 0, deletes = 0, next = 0;
    bool bad_auth = false, bad_page = false, malformed_page = false, fail_patch = false, pages = false;
    std::string last_patch;
    int request(const std::string &url, const char *method, const std::string &auth,
                const char *type, const std::string &body, std::string &reply) override {
        // Tasks never calls the token endpoint: the shared OAuth client does.
        assert(url.find("oauth2.googleapis.com") == std::string::npos);
        (void)type;
        assert(auth == "Bearer test-token");
        if (url.find("/users/@me/lists/") != std::string::npos) {
            reply = "{\"id\":\"" + list_id + "\"}"; return 200;
        }
        if (!strcmp(method, "GET")) {
            assert(url.find("showHidden=true") != std::string::npos);
            assert(url.find("showDeleted=true") != std::string::npos);
            if (malformed_page) { reply = "{}"; return 200; }
            bool second = url.find("pageToken=") != std::string::npos;
            if (second) assert(url.find("pageToken=next%2B%26") != std::string::npos);
            if (bad_page && second) { reply = "{}"; return 503; }
            cJSON *j = cJSON_CreateObject(), *items = cJSON_AddArrayToObject(j, "items");
            cJSON_AddStringToObject(j, "kind", "tasks#tasks");
            unsigned i = 0;
            for (auto &entry : tasks) {
                if (!pages || (second ? i >= 1 : i < 1)) cJSON_AddItemToArray(items, task_json(entry.second));
                ++i;
            }
            if (pages && !second) cJSON_AddStringToObject(j, "nextPageToken", "next+&");
            reply = json_text(j); return 200;
        }
        auto split = url.rfind("/tasks");
        std::string id = url.substr(split + 6);
        if (!id.empty() && id[0] == '/') id.erase(0,1);
        if (!strcmp(method, "DELETE")) {
            deletes++; if (!tasks.count(id)) return 404;
            tasks[id].deleted = true; reply.clear(); return 204;
        }
        if (!strcmp(method, "PATCH")) {
            patches++; last_patch = body;
            if (fail_patch) { reply = "{}"; return 503; }
            assert(tasks.count(id));
        } else {
            assert(!strcmp(method, "POST")); posts++;
            id = "new" + std::to_string(++next);
            GoogleTask t; t.id = id; t.status = "needsAction"; tasks[id] = t;
        }
        cJSON *j = cJSON_Parse(body.c_str()); assert(j);
        auto &t = tasks[id];
        for (const char *name : {"title", "notes", "due", "status"}) {
            if (!cJSON_GetObjectItemCaseSensitive(j, name)) continue;
            auto value = field(j, name);
            if (!strcmp(name,"title")) t.title = value;
            if (!strcmp(name,"notes")) t.notes = value;
            if (!strcmp(name,"due")) t.due = value;
            if (!strcmp(name,"status")) t.status = value;
        }
        cJSON_Delete(j); reply = json_text(task_json(t)); return 200;
    }
};
class Host : public TaskSyncHost {
public:
    Server server;
    std::vector<TaskSyncMap> map;
    std::string bound;
    bool is_online = true, writable = true;
    TasksHttp &http() override { return server; }
    bool online() override { return is_online; }
    bool access_token(std::string &token, std::string &problem) override {
        std::string form = "grant_type=refresh_token&refresh_token=refresh%2Btoken%26&client_id=client&client_secret=secret";
        std::string request = "POST /token HTTP/1.0\r\nHost: oauth2.googleapis.com\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n"
            "Content-Length: " + std::to_string(form.size()) + "\r\nConnection: close\r\n\r\n" + form;
        std::string body = server.bad_auth ? "{\"error\":\"invalid_grant\"}" : "{\"access_token\":\"test-token\",\"scope\":\"https://mail.google.com/ https://www.googleapis.com/auth/tasks\"}";
        std::string response = std::string("HTTP/1.0 ") + (server.bad_auth ? "400 Bad Request" : "200 OK") +
            "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        ScriptedServer connection({{"", ""}, {request, response}}, 7);
        OAuthClient google(connection, hotsync_keep_alive);
        OAuthTokens tokens;
        bool ok = google.refresh("client", "secret", "refresh+token&", tokens);
        assert(connection.finished("Tasks shared OAuth") && connection.closed_);
        if (!ok) { problem = google.error(); return false; }
        assert(tokens.scope.find("https://www.googleapis.com/auth/tasks") != std::string::npos);
        token = tokens.access_token; return true;
    }
    bool bind_list(const std::string &id) override {
        uint32_t palm_id; assert(hotsync_read_user_id(&palm_id) && palm_id == 1);
        if (!bound.empty() && bound != id) return false;
        bound = id; return true;
    }
    bool load_map(std::vector<TaskSyncMap> &out) override { out = map; return true; }
    bool store_map(const std::vector<TaskSyncMap> &in) override { if (!writable) return false; map = in; return true; }
};

static uint32_t put(const PalmTask &t, uint32_t id = 0, uint8_t attr = HOTSYNC_ATTR_DIRTY, uint8_t category = 0) {
    uint8_t db; assert(hotsync_open_db("ToDoDB", HOTSYNC_MODE_READ | HOTSYNC_MODE_WRITE, &db));
    size_t max; auto *buf = hotsync_record_buffer(&max);
    size_t len = tasks_pack_record(t, buf, max); assert(len);
    uint32_t saved; assert(hotsync_write_record(db, id, attr, category, len, &saved));
    assert(hotsync_close_db(db)); return saved;
}
static PalmTask get(uint32_t id, uint8_t *category = nullptr) {
    uint8_t db; assert(hotsync_open_db("ToDoDB", HOTSYNC_MODE_READ | HOTSYNC_MODE_WRITE, &db));
    HotSync_Record rec; assert(hotsync_read_record(db, id, &rec));
    PalmTask t; assert(tasks_unpack_record(rec.data, rec.len, t));
    if (category) *category = rec.category;
    assert(hotsync_close_db(db)); return t;
}
static uint32_t mapped(Host &h, const std::string &id) {
    for (auto &m : h.map) if (m.google_id == id) return m.palm_id;
    assert(false); return 0;
}
static TaskSyncResult sync(Host &h, const GoogleTasksSetup &s, bool ok = true) {
    TaskSyncResult r; tasksync_run(s, PALM_WESTERN, h, r);
    printf("Tasks: %s\n", tasksync_summary(r).c_str());
    assert(!r.link_lost); assert(r.problem.empty() == ok); return r;
}
static void unit_tests() {
    uint8_t data[TASKSYNC_RECORD_MAX]; PalmTask t;
    t.title = "two  spaces "; t.notes = "tab\t\n\n\nend "; t.due = "2028-02-29"; t.priority = 5; t.completed = true;
    size_t n = tasks_pack_record(t, data, sizeof(data)); assert(n && data[2] == 0x85);
    PalmTask read; assert(tasks_unpack_record(data, n, read) && read.due == t.due && read.notes == t.notes);
    assert(!tasks_unpack_record(data, n-1, read));
    t.due = "2027-02-29"; assert(!tasks_pack_record(t, data, sizeof(data)));
    t.due = "2032-01-01"; assert(!tasks_pack_record(t, data, sizeof(data)));
    t.due.clear(); assert(tasks_pack_record(t, data, sizeof(data)) && data[0] == 255 && data[1] == 255);
    assert(palm_from_utf8(" a\t\n\n\nend ", PALM_WESTERN, 100) == " a\t\n\n\nend ");
    assert(palm_to_utf8(std::string("\xe9"), PALM_WESTERN) == "\xc3\xa9");
    assert(palm_from_utf8("\xd0\xaf", PALM_CYRILLIC, 100) == std::string("\xdf"));
    assert(tasks_url_encode("a/+&?") == "a%2F%2B%26%3F");
}
int main(int argc, char **argv) {
    unit_tests(); assert(argc == 2);
    fd = open(argv[1], O_RDWR | O_NOCTTY); assert(fd >= 0);
    uint8_t memory[HOTSYNC_BUFFER_SIZE(TASKSYNC_RECORD_MAX)];
    HotSync_Io io = {port_read, port_write, port_millis};
    assert(hotsync_accept(&io, memory, sizeof(memory), 10000));
    Host host; GoogleTasksSetup config;
    GoogleTask cloud; cloud.id = "cloud"; cloud.title = "Caf\xc3\xa9  "; cloud.notes = std::string(5000, 'n') + "\xf0\x9f\x98\x80";
    cloud.due = "2026-10-20T00:00:00.000Z"; cloud.status = "needsAction";
    host.server.tasks[cloud.id] = cloud;
    GoogleTask hidden = cloud; hidden.id = "hidden"; hidden.title = "Completed"; hidden.notes = ""; hidden.status = "completed";
    host.server.tasks[hidden.id] = hidden; host.server.pages = true;
    PalmTask local; local.title = "Local task"; local.notes = "new\nnote"; local.priority = 4;
    uint32_t local_id = put(local, 0, HOTSYNC_ATTR_DIRTY, 3);
    PalmTask secret = local; secret.title = "Private"; uint32_t secret_id = put(secret, 0, HOTSYNC_ATTR_SECRET | HOTSYNC_ATTR_DIRTY);
    put(secret, 0, HOTSYNC_ATTR_ARCHIVED | HOTSYNC_ATTR_DIRTY);
    auto r = sync(host, config);
    assert(r.uploaded == 1 && r.downloaded == 3 && host.server.posts == 1 && host.map.size() == 3);
    uint8_t category; auto kept = get(local_id, &category); assert(category == 3 && kept.priority == 4);
    uint32_t cloud_id = mapped(host, "cloud");
    auto task = get(cloud_id); assert(task.title == std::string("Caf\xe9  ") && task.notes.size() == 4095 && task.due == "2026-10-20");
    assert(get(mapped(host, "hidden")).completed);
    r = sync(host, config); assert(r.uploaded == 0 && r.downloaded == 0 && host.server.posts == 1);

    Host restarted;
    restarted.map = host.map; restarted.bound = host.bound; restarted.server = host.server;
    r = sync(restarted, config); assert(r.uploaded == 0 && r.downloaded == 0 && restarted.server.posts == 1);

    // A completion-only edit preserves the full cloud note and unedited title.
    task.completed = true; task.due.clear(); put(task, cloud_id);
    host.server.tasks["cloud"].title = "Remote renamed"; // Concurrent field change is merged.
    r = sync(host, config); assert(r.uploaded == 1);
    assert(host.server.tasks["cloud"].notes == cloud.notes && host.server.tasks["cloud"].title == "Remote renamed");
    cJSON *patch = cJSON_Parse(host.server.last_patch.c_str()); assert(patch);
    assert(!cJSON_GetObjectItemCaseSensitive(patch, "notes") && !cJSON_GetObjectItemCaseSensitive(patch, "title"));
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(patch, "due"))); cJSON_Delete(patch);

    // A failed second page must cause no writes or inferred deletions.
    host.server.bad_page = true; auto before = host.map;
    unsigned writes = host.server.posts + host.server.patches + host.server.deletes;
    sync(host, config, false);
    assert(host.map.size() == before.size() && writes == host.server.posts + host.server.patches + host.server.deletes);
    host.server.bad_page = false;
    host.server.malformed_page = true; sync(host, config, false); host.server.malformed_page = false;
    auto small = config; small.max_tasks = 1; sync(host, small, false);
    host.server.bad_auth = true; sync(host, config, false); host.server.bad_auth = false;
    host.is_online = false; sync(host, config, false); host.is_online = true;
    host.server.list_id = "other-account"; sync(host, config, false); host.server.list_id = "list-one";

    // Failed PATCH retains the pending Palm edit for the next sync.
    task = get(cloud_id); task.title = "Palm edited"; put(task, cloud_id);
    host.server.fail_patch = true; sync(host, config, false);
    assert(get(cloud_id).title == "Palm edited");
    host.server.fail_patch = false; r = sync(host, config); assert(r.uploaded == 1);

    // Explicit Palm deletion propagates, and is not reimported.
    auto deletion = get(local_id); put(deletion, local_id, HOTSYNC_ATTR_DELETED | HOTSYNC_ATTR_DIRTY);
    r = sync(host, config); assert(r.deleted == 1 && host.server.deletes == 1 && host.map.size() == 2);
    r = sync(host, config); assert(r.downloaded == 0 && host.map.size() == 2);

    // Cloud deletion propagates to the Palm; private local record survives.
    host.server.tasks["cloud"].deleted = true;
    r = sync(host, config); assert(r.deleted == 1 && host.map.size() == 1);
    assert(get(secret_id).title == "Private");
    host.server.tasks["hidden"].status = "needsAction";
    host.server.tasks["hidden"].due = "2032-01-01T00:00:00Z"; // Outside Palm date range, preserved on Google.
    r = sync(host, config); assert(!get(mapped(host,"hidden")).completed);
    auto remaining = get(mapped(host, "hidden")); remaining.completed = true; put(remaining, mapped(host, "hidden"));
    sync(host, config); assert(host.server.tasks["hidden"].due == "2032-01-01T00:00:00Z");
    assert(hotsync_end(0)); close(fd); puts("done");
}
