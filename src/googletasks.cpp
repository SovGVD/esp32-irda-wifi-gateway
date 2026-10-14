#include "googletasks.h"
#include "mailoauth.h"
#include "cJSON.h"
#include <memory>
#include <cstdlib>

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
static Json parse(const std::string &s) { return Json(cJSON_ParseWithOpts(s.c_str(), nullptr, true), cJSON_Delete); }
static std::string field(const cJSON *j, const char *name) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool unpack(const cJSON *j, GoogleTask &t) {
    if (!cJSON_IsObject(j)) return false;
    for (const char *name : {"id", "title", "notes", "due", "status", "etag"}) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
        if (v && !cJSON_IsString(v)) return false;
    }
    const cJSON *deleted = cJSON_GetObjectItemCaseSensitive(j, "deleted");
    if (deleted && !cJSON_IsBool(deleted)) return false;
    t.id = field(j, "id"); t.title = field(j, "title"); t.notes = field(j, "notes");
    t.due = field(j, "due"); t.status = field(j, "status"); t.etag = field(j, "etag");
    t.deleted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "deleted"));
    return !t.id.empty() && (t.deleted || t.status == "completed" || t.status == "needsAction");
}
std::string tasks_url_encode(const std::string &s) { return oauth_encode(s); }
bool GoogleTasksClient::call(const std::string &path, const char *method, const std::string &body, std::string &reply) {
    if (token_.empty() || token_.find_first_of("\r\n") != std::string::npos) { error_ = "Google Tasks is not authorized"; return false; }
    int code = http_.request("https://tasks.googleapis.com/tasks/v1/lists/" + tasks_url_encode(setup_.list_id) +
                            "/tasks" + path, method, "Bearer " + token_, "application/json", body, reply);
    if (code < 200 || code >= 300) {
        error_ = "Google Tasks request failed (HTTP " + std::to_string(code) + ")";
        return false;
    }
    return true;
}
bool GoogleTasksClient::resolve_list(std::string &id) {
    std::string reply;
    int code = http_.request("https://tasks.googleapis.com/tasks/v1/users/@me/lists/" + tasks_url_encode(setup_.list_id),
                            "GET", "Bearer " + token_, "application/json", "", reply);
    auto j = parse(reply);
    id = field(j.get(), "id");
    if (code != 200 || id.empty()) {
        error_ = "Could not identify the Google task list (HTTP " + std::to_string(code) + ")";
        return false;
    }
    return true;
}
bool GoogleTasksClient::list(std::vector<GoogleTask> &tasks) {
    tasks.clear(); std::string page; size_t text_bytes = 0;
    // Bounded, complete snapshot: never use a partial list to infer deletions.
    for (unsigned n = 0; n < 100; ++n) {
        std::string reply;
        std::string path = "?maxResults=10&showCompleted=true&showHidden=true&showDeleted=true";
        if (!page.empty()) path += "&pageToken=" + tasks_url_encode(page);
        if (!call(path, "GET", "", reply)) return false;
        auto j = parse(reply);
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(j.get(), "items");
        const cJSON *next_token = cJSON_GetObjectItemCaseSensitive(j.get(), "nextPageToken");
        if (!j || field(j.get(), "kind") != "tasks#tasks" || (items && !cJSON_IsArray(items)) ||
            (next_token && !cJSON_IsString(next_token))) {
            error_ = "Invalid Google Tasks list response"; return false;
        }
        const cJSON *item;
        cJSON_ArrayForEach(item, items) {
            GoogleTask t;
            if (!unpack(item, t)) { error_ = "Invalid Google task"; return false; }
            for (const auto &old : tasks) if (old.id == t.id) { error_ = "Duplicate Google task ID"; return false; }
            // Bound UTF-8 storage; unchanged original fields survive a PATCH.
            if (t.title.size() > 4096 || t.notes.size() > 32768) { error_ = "Google task text is too large"; return false; }
            text_bytes += t.title.size() + t.notes.size() + t.id.size() + t.etag.size();
            if (text_bytes > 48000) { error_ = "Google task text exceeds the sync memory limit"; return false; }
            tasks.push_back(t);
            if (tasks.size() > setup_.max_tasks) { error_ = "Task list exceeds the configured task limit"; return false; }
        }
        std::string next = field(j.get(), "nextPageToken");
        if (next.empty()) return true;
        if (next == page) { error_ = "Google Tasks repeated a page"; return false; }
        page = next;
    }
    error_ = "Too many Google Tasks pages"; return false;
}
bool GoogleTasksClient::save(const std::string &id, const GoogleTaskPatch &p, GoogleTask &saved) {
    Json j(cJSON_CreateObject(), cJSON_Delete);
    if (p.title_set) cJSON_AddStringToObject(j.get(), "title", p.title.c_str());
    if (p.notes_set) cJSON_AddStringToObject(j.get(), "notes", p.notes.c_str());
    if (p.status_set) cJSON_AddStringToObject(j.get(), "status", p.status.c_str());
    if (p.due_set) {
        if (p.due.empty()) cJSON_AddNullToObject(j.get(), "due");
        else cJSON_AddStringToObject(j.get(), "due", p.due.c_str());
    }
    char *raw = cJSON_PrintUnformatted(j.get());
    if (!raw) { error_ = "Not enough memory for Google task"; return false; }
    std::string body(raw); free(raw);
    std::string reply;
    if (!call(id.empty() ? "" : "/" + tasks_url_encode(id), id.empty() ? "POST" : "PATCH", body, reply)) return false;
    auto answer = parse(reply);
    if (!unpack(answer.get(), saved) || saved.deleted || (!id.empty() && saved.id != id)) {
        error_ = "Invalid Google task save response"; return false;
    }
    return true;
}
bool GoogleTasksClient::remove(const std::string &id) {
    std::string reply;
    if (call("/" + tasks_url_encode(id), "DELETE", "", reply)) return true;
    // An already deleted task is the desired state (including a retry).
    if (error_ == "Google Tasks request failed (HTTP 404)" || error_ == "Google Tasks request failed (HTTP 410)") {
        error_.clear(); return true;
    }
    return false;
}
