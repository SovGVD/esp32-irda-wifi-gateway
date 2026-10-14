#pragma once
#include "googletasks.h"
#include "mailtext.h"
#include <array>

#define TASKSYNC_TITLE_MAX 255
#define TASKSYNC_NOTE_MAX 4095
#define TASKSYNC_RECORD_MAX (TASKSYNC_TITLE_MAX + TASKSYNC_NOTE_MAX + 5)
struct PalmTask {
    std::string title, notes, due; // due YYYY-MM-DD, or empty
    bool completed = false;
    uint8_t priority = 1; // Remains on the Palm; Google has no priority field
};
bool tasks_unpack_record(const uint8_t *data, size_t len, PalmTask &task);
size_t tasks_pack_record(const PalmTask &task, uint8_t *data, size_t max_len);
struct TaskSyncMap {
    uint32_t palm_id = 0;
    std::string google_id;
    // Baseline of each displayed field, so a completion edit never overwrites
    // Google's longer/unrepresentable title or notes.
    std::array<uint32_t, 4> fields = {};
};
class TaskSyncHost {
public:
    virtual ~TaskSyncHost() {}
    virtual TasksHttp &http() = 0;
    virtual bool online() = 0;
    // Refuse a different Google list or handheld before touching records.
    virtual bool access_token(std::string &token, std::string &problem) = 0;
    virtual bool bind_list(const std::string &id) = 0;
    virtual bool load_map(std::vector<TaskSyncMap> &map) = 0;
    virtual bool store_map(const std::vector<TaskSyncMap> &map) = 0;
};
struct TaskSyncResult {
    unsigned uploaded = 0, downloaded = 0, deleted = 0;
    std::string problem;
    bool link_lost = false;
};
void tasksync_run(const GoogleTasksSetup &setup, PalmCharset charset, TaskSyncHost &host, TaskSyncResult &result);
std::string tasksync_summary(const TaskSyncResult &result);
