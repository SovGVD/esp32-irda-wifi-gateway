#include "irdrive.h"
#include "irmail.h"
#include "irhotsync.h"
#include "irgooglehttp.h"
#include "irdebug.h"
#include "irsyncdiag.h"
#include "cJSON.h"
#include <Preferences.h>
#include <WiFi.h>
#include <memory>
#include <cstring>
#include <time.h>

static IrDriveSettings s_settings;
static char s_result[384]="";
static DriveBackupResult s_restore_result;
static std::string json_field(cJSON *j,const char *key) { auto *p=cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsString(p) ? p->valuestring : ""; }
void irdrive_init() {
    Preferences p; if (!p.begin("irdrive",true)) return;
    s_settings.enabled=p.getBool("on",false); s_settings.cyrillic=p.getBool("cset",false);
    s_settings.restore_saved=p.getBool("restore",false); s_settings.retry_interrupted=p.getBool("retry",false);
    s_settings.username=p.getString("user",""); p.end();
}
const IrDriveSettings &irdrive_settings() { return s_settings; }
bool irdrive_store_settings(const IrDriveSettings &s) {
    if (s.username.length()>128 || s.username.indexOf('\r')>=0 || s.username.indexOf('\n')>=0) return false;
    Preferences p; if (!p.begin("irdrive",false)) return false;
    bool ok=p.putBool("on",s.enabled)==1 && p.putBool("cset",s.cyrillic)==1 && p.putBool("restore",s.restore_saved)==1 &&
        p.putBool("retry",s.retry_interrupted)==1 && p.putString("user",s.username)==s.username.length();
    p.end(); if (ok) s_settings=s; return ok;
}
bool irdrive_ready() {
    const auto &g=irmail_settings();
    return ENABLE_WIFI_GATEWAY && s_settings.enabled && g.google_scope(GOOGLE_DRIVE_SCOPE) &&
        !g.google_client_id.isEmpty() && !g.google_client_secret.isEmpty();
}
class EspDriveHost : public DriveBackupHost {
public:
    explicit EspDriveHost(const std::function<bool(size_t)> &reserve={}) : reserve_(reserve) {}
    bool reserve_record_buffer(size_t bytes) override {
        return reserve_ ? reserve_(bytes) : DriveBackupHost::reserve_record_buffer(bytes);
    }
    GoogleHttp &http() override { return http_; }
    bool online() override { return WiFi.status()==WL_CONNECTED; }
    int64_t now() override { return (int64_t)time(nullptr); }
    bool access_token(std::string &token,std::string &problem) override { return irmail_google_access_token(token,problem,true); }
    void trace(const std::string &text) override { irsyncdiag_checkpoint(text.rfind("Installing ",0)==0 ? "Installing database" : text.rfind("Backing up ",0)==0 ? "Uploading database" : "Saving recovery database"); irdbg_printf("Drive: %s",text.c_str()); }
    bool load(DriveBackupState &state) override {
        state=DriveBackupState(); Preferences p;
        if (!p.begin("irdrive",false)) return false;
        bool exists=p.isKey("state"); String text=p.getString("state",""); p.end();
        if (!exists) return true;
        if (!text.length() || text.length()>4096) return false;
        std::unique_ptr<cJSON,decltype(&cJSON_Delete)> j(cJSON_ParseWithOpts(text.c_str(),nullptr,true),cJSON_Delete);
        auto *version=cJSON_GetObjectItemCaseSensitive(j.get(),"v"),*id=cJSON_GetObjectItemCaseSensitive(j.get(),"palm");
        if (!cJSON_IsObject(j.get()) || !cJSON_IsNumber(version) || version->valuedouble!=1 ||
            !cJSON_IsNumber(id) || id->valuedouble<0 || id->valuedouble>4294967295.0 || id->valuedouble!=(uint32_t)id->valuedouble) return false;
        state.palm_id=(uint32_t)id->valuedouble;
        std::string *fields[]={&state.account,&state.username,&state.backup_folder,&state.palm_folder,&state.user_folder,&state.pending_file,&state.pending_db,&state.recovery_folder,&state.recovery_file};
        const char *names[]={"account","user","backup","folder","target","file","db","recovery","previous"};
        for (unsigned n=0;n<9;n++) { *fields[n]=json_field(j.get(),names[n]); if (fields[n]->size()>(n==6 ? 31 : 256)) return false; }
        auto pending=json_field(j.get(),"pending");
        if (pending.size()>1 || (!pending.empty() && pending!="u" && pending!="r" && pending!="s")) return false;
        state.pending=pending.empty() ? 0 : pending[0];
        return state.pending ? !state.pending_file.empty() && !state.pending_db.empty() : state.pending_file.empty() && state.pending_db.empty();
    }
    bool store(const DriveBackupState &state) override {
        std::unique_ptr<cJSON,decltype(&cJSON_Delete)> j(cJSON_CreateObject(),cJSON_Delete); if (!j) return false;
        bool ok=cJSON_AddNumberToObject(j.get(),"v",1) && cJSON_AddNumberToObject(j.get(),"palm",state.palm_id);
        const std::string *fields[]={&state.account,&state.username,&state.backup_folder,&state.palm_folder,&state.user_folder,&state.pending_file,&state.pending_db,&state.recovery_folder,&state.recovery_file};
        const char *names[]={"account","user","backup","folder","target","file","db","recovery","previous"};
        for (unsigned n=0;n<9;n++) if (!cJSON_AddStringToObject(j.get(),names[n],fields[n]->c_str())) ok=false;
        std::string pending=state.pending ? std::string(1,state.pending) : "";
        if (!cJSON_AddStringToObject(j.get(),"pending",pending.c_str())) ok=false;
        if (!ok) return false;
        char *data=cJSON_PrintUnformatted(j.get()); if (!data) return false;
        Preferences p; ok=p.begin("irdrive",false);
        if (ok) { ok=p.putString("state",data)==strlen(data); p.end(); }
        free(data); return ok;
    }
private:
    std::function<bool(size_t)> reserve_;
    EspGoogleHttp http_{true,DRIVE_HTTP_TIMEOUT_MS,DRIVE_HTTP_RESPONSE_MAX};
};
String irdrive_check_login() {
    const auto &g=irmail_settings();
    if (!g.google_scope(GOOGLE_DRIVE_SCOPE)) return "Sign in with Google and allow Drive access first.";
    if (WiFi.status()!=WL_CONNECTED) return "Wi-Fi is not connected.";
    std::string token,problem; if (!irmail_google_access_token(token,problem)) return problem.c_str();
    EspGoogleHttp http(false,DRIVE_HTTP_TIMEOUT_MS,DRIVE_HTTP_RESPONSE_MAX); GoogleDriveClient drive(http,token); std::string root;
    if (!drive.root(root)) return drive.error().c_str();
    return "OK, Google Drive access. HotSync selects Backup/Palm/<Palm user name>.";
}
String irdrive_last_result() { return s_result; }
void irdrive_record_result(const DriveBackupResult &r) {
    auto summary=drivebackup_summary(r); strlcpy(s_result,summary.c_str(),sizeof(s_result)); irdbg_printf("Drive: %s",s_result);
}
String irdrive_folder() {
    EspDriveHost host; DriveBackupState state;
    if (host.load(state) && !state.username.empty()) return "Backup/Palm/"+String(state.username.c_str());
    return "Backup/Palm/"+(irhotsync_settings().username.length() ? irhotsync_settings().username : String("<HotSync user name>"));
}
String irdrive_interrupted_name() {
    EspDriveHost host; DriveBackupState state;
    return host.load(state) && state.pending=='r' ? String(state.pending_db.c_str()) : String("");
}
bool irdrive_restore_once() { auto s=s_settings; s.enabled=true; s.restore_saved=true; return irdrive_store_settings(s); }
bool irdrive_retry_interrupted() {
    if (!irdrive_interrupted_name().length()) return false;
    auto s=s_settings; s.enabled=true; s.retry_interrupted=true; return irdrive_store_settings(s);
}
static void run_drive(DriveBackupResult &r,bool restore,const std::function<bool(size_t)> &reserve={}) {
    EspDriveHost host(reserve); DriveBackupSetup setup;
    setup.max_databases=DRIVE_MAX_DATABASES; setup.max_files=DRIVE_MAX_FILES; setup.folder_memory=DRIVE_FOLDER_MEMORY;
    setup.charset=s_settings.cyrillic ? PALM_CYRILLIC : PALM_WESTERN; setup.username=irhotsync_settings().username.c_str();
    setup.restore_saved=s_settings.restore_saved; setup.retry_interrupted=s_settings.retry_interrupted;
    setup.restore_only=restore; setup.backup_only=!restore;
    drivebackup_run(setup,host,r);
    if (!restore) {
        r.restored+=s_restore_result.restored; r.reset_needed=r.reset_needed||s_restore_result.reset_needed;
    }
    if (!restore && r.problem.empty()) {
        auto s=s_settings; s.restore_saved=s.retry_interrupted=false;
        if (!irdrive_store_settings(s)) r.problem="Sync completed, but could not clear the one-time restore request";
    }
    irdrive_record_result(r);
}
void irdrive_restore(DriveBackupResult &r,const std::function<bool(size_t)> &reserve) { run_drive(r,true,reserve); s_restore_result=r; }
void irdrive_sync(DriveBackupResult &r) {
    if (!s_restore_result.problem.empty()) { r=s_restore_result; return; }
    run_drive(r,false);
}
