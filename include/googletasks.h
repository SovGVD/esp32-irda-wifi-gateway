#pragma once
#include <string>
#include <vector>
#include <stdint.h>

#include "googlehttp.h"
// Compatibility name for the existing Tasks host interface.
using TasksHttp = GoogleHttp;
struct GoogleTasksSetup {
    std::string list_id = "@default";
    unsigned max_tasks = 100;
};
struct GoogleTask {
    std::string id, title, notes, due, status, etag;
    bool deleted = false;
};
// Fields absent from a PATCH are preserved on Google (including hierarchy,
// links, and text too long for the Palm). due may be explicitly cleared.
struct GoogleTaskPatch {
    bool title_set = false, notes_set = false, due_set = false, status_set = false;
    std::string title, notes, due, status;
};
class GoogleTasksClient {
public:
    GoogleTasksClient(TasksHttp &http, const GoogleTasksSetup &setup, const std::string &token)
        : http_(http), setup_(setup), token_(token) {}
    bool resolve_list(std::string &id);
    bool list(std::vector<GoogleTask> &tasks);
    bool save(const std::string &id, const GoogleTaskPatch &patch, GoogleTask &saved);
    bool remove(const std::string &id);
    const std::string &error() const { return error_; }
private:
    bool call(const std::string &path, const char *method, const std::string &body, std::string &reply);
    TasksHttp &http_;
    const GoogleTasksSetup &setup_;
    std::string token_, error_;
};
std::string tasks_url_encode(const std::string &text);
