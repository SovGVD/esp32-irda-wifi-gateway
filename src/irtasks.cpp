#include "irtasks.h"
#include "config.h"
#include "irmail.h"
#include "hotsync.h"
#include "irdebug.h"
#include <Preferences.h>
#include <WiFi.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <cstdlib>
#include <cstring>

static IrTasksSettings s_settings;
static char s_result[200] = "";

static GoogleTasksSetup tasks_setup() {
    GoogleTasksSetup out;
    out.list_id = s_settings.list_id.c_str();
    out.max_tasks = TASKS_MAX_PER_SYNC;
    return out;
}
void irtasks_init() {
    Preferences p;
    if (p.begin("irtasks", true)) {
        s_settings.enabled = p.getBool("on", false);
        s_settings.cyrillic = p.getBool("cset", false);
        s_settings.list_id = p.getString("list", "@default");
        p.end();
    }
}
const IrTasksSettings &irtasks_settings() { return s_settings; }
bool irtasks_ready() {
    const IrMailSettings &google = irmail_settings();
    return ENABLE_WIFI_GATEWAY && s_settings.enabled && google.google() && google.google_tasks() &&
           google.google_client_id.length() && google.google_client_secret.length() && s_settings.list_id.length();
}
bool irtasks_store_settings(const IrTasksSettings &s) {
    Preferences p;
    if (!p.begin("irtasks", false)) return false;
    if (s.list_id != s_settings.list_id && p.getString("map", "[]") != "[]") { p.end(); return false; }
    bool ok = p.putString("list", s.list_id) == s.list_id.length() &&
              p.putBool("on", s.enabled) == 1 && p.putBool("cset", s.cyrillic) == 1;
    p.end();
    if (ok) s_settings = s;
    return ok;
}
class EspTasksHttp : public TasksHttp {
public:
    explicit EspTasksHttp(bool keepalive = false) : keepalive_(keepalive) {}
    int request(const std::string &url, const char *method, const std::string &auth,
                const char *type, const std::string &body, std::string &reply) override {
        reply.clear(); response_ = &reply; too_large_ = false;
        esp_http_client_config_t config = {};
        config.url = url.c_str(); config.crt_bundle_attach = esp_crt_bundle_attach;
        config.timeout_ms = TASKS_HTTP_TIMEOUT_MS; config.disable_auto_redirect = true;
        config.event_handler = event; config.user_data = this;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return -1;
        esp_http_client_method_t m = HTTP_METHOD_GET;
        if (!strcmp(method, "POST")) m = HTTP_METHOD_POST;
        else if (!strcmp(method, "PATCH")) m = HTTP_METHOD_PATCH;
        else if (!strcmp(method, "DELETE")) m = HTTP_METHOD_DELETE;
        esp_http_client_set_method(client, m);
        esp_http_client_set_header(client, "Content-Type", type);
        if (!auth.empty()) esp_http_client_set_header(client, "Authorization", auth.c_str());
        if (!body.empty()) esp_http_client_set_post_field(client, body.data(), body.size());
        if (keepalive_) hotsync_keep_alive();
        esp_err_t err = esp_http_client_perform(client);
        int code = err == ESP_OK && !too_large_ ? esp_http_client_get_status_code(client) : -1;
        esp_http_client_cleanup(client); response_ = nullptr;
        if (keepalive_) hotsync_keep_alive();
        return code;
    }
private:
    static esp_err_t event(esp_http_client_event_t *e) {
        auto *self = (EspTasksHttp *)e->user_data;
        if (self->keepalive_) hotsync_keep_alive();
        if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
            if (self->response_->size() + e->data_len > TASKS_HTTP_RESPONSE_MAX) {
                self->too_large_ = true; return ESP_FAIL;
            }
            self->response_->append((const char *)e->data, e->data_len);
        }
        return ESP_OK;
    }
    bool keepalive_, too_large_ = false;
    std::string *response_ = nullptr;
};
class EspTasksHost : public TaskSyncHost {
public:
    TasksHttp &http() override { return http_; }
    bool online() override { return WiFi.status() == WL_CONNECTED; }
    bool access_token(std::string &token, std::string &problem) override {
        return irmail_google_access_token(token, problem, true);
    }
    bool bind_list(const std::string &id) override {
        uint32_t palm_id;
        if (!hotsync_read_user_id(&palm_id)) return false;
        Preferences p;
        if (!p.begin("irtasks", false)) return false;
        String bound = p.getString("bound", "");
        bool ok = (bound.length() == 0 || bound == id.c_str()) &&
                  (!p.isKey("palm") || p.getULong("palm") == palm_id);
        if (ok) ok = p.putString("bound", id.c_str()) == id.size() && p.putULong("palm", palm_id) == 4;
        p.end(); return ok;
    }
    bool load_map(std::vector<TaskSyncMap> &map) override {
        Preferences p;
        if (!p.begin("irtasks", false)) return false;
        String raw = p.getString("map", "[]"); p.end();
        cJSON *j = cJSON_Parse(raw.c_str());
        if (!cJSON_IsArray(j) || cJSON_GetArraySize(j) > TASKS_MAX_PER_SYNC) { cJSON_Delete(j); return false; }
        bool ok = true; const cJSON *item;
        cJSON_ArrayForEach(item, j) {
            TaskSyncMap m;
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "p");
            const cJSON *google = cJSON_GetObjectItemCaseSensitive(item, "g");
            const cJSON *f = cJSON_GetObjectItemCaseSensitive(item, "f");
            if (!cJSON_IsNumber(id) || id->valuedouble < 1 || id->valuedouble > 0xffffff ||
                !cJSON_IsString(google) || !google->valuestring[0] || !cJSON_IsArray(f) || cJSON_GetArraySize(f) != 4) { ok = false; break; }
            m.palm_id = (uint32_t)id->valuedouble; m.google_id = google->valuestring;
            for (unsigned n = 0; n < 4; ++n) {
                const cJSON *v = cJSON_GetArrayItem(f, n);
                if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > UINT32_MAX) { ok = false; break; }
                m.fields[n] = (uint32_t)v->valuedouble;
            }
            for (const auto &old : map) if (old.palm_id == m.palm_id || old.google_id == m.google_id) ok = false;
            if (!ok) break;
            map.push_back(m);
        }
        cJSON_Delete(j); return ok;
    }
    bool store_map(const std::vector<TaskSyncMap> &map) override {
        cJSON *j = cJSON_CreateArray();
        for (const auto &m : map) {
            cJSON *item = cJSON_CreateObject(); cJSON_AddNumberToObject(item, "p", m.palm_id);
            cJSON_AddStringToObject(item, "g", m.google_id.c_str());
            cJSON *f = cJSON_AddArrayToObject(item, "f");
            for (uint32_t h : m.fields) cJSON_AddItemToArray(f, cJSON_CreateNumber(h));
            cJSON_AddItemToArray(j, item);
        }
        char *raw = cJSON_PrintUnformatted(j); cJSON_Delete(j);
        if (!raw) return false;
        Preferences p; bool ok = p.begin("irtasks", false);
        if (ok) { ok = p.putString("map", raw) == strlen(raw); p.end(); }
        free(raw); return ok;
    }
private:
    EspTasksHttp http_{true};
};
String irtasks_last_result() { return s_result; }
String irtasks_check_login() {
    if (!irtasks_ready()) return "Sign in with Google for Tasks and enable Tasks first.";
    if (WiFi.status() != WL_CONNECTED) return "Wi-Fi is not connected.";
    std::string token, problem;
    if (!irmail_google_access_token(token, problem)) return problem.c_str();
    EspTasksHttp http; auto config = tasks_setup(); GoogleTasksClient client(http, config, token);
    std::vector<GoogleTask> tasks;
    if (!client.list(tasks)) return client.error().c_str();
    return "OK, " + String((unsigned)tasks.size()) + " tasks (including deleted tasks).";
}
void irtasks_sync(TaskSyncResult &result) {
    EspTasksHost host;
    tasksync_run(tasks_setup(), s_settings.cyrillic ? PALM_CYRILLIC : PALM_WESTERN, host, result);
    std::string summary = tasksync_summary(result);
    strlcpy(s_result, summary.c_str(), sizeof(s_result));
    irdbg_printf("Tasks: %s", s_result);
}
