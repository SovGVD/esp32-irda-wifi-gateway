#include "irnotes.h"
#include "irmail.h"
#include "irgooglehttp.h"
#include "irdebug.h"
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <cstring>

static IrNotesSettings s_settings[2];
static char s_results[2][384]={};
static const char *space(bool drawing) { return drawing ? "irnotes" : "irmemos"; }
void irnotes_init() {
    for (unsigned i=0;i<2;i++) {
        Preferences p; if (!p.begin(space(i),true)) continue;
        auto &s=s_settings[i]; s.enabled=p.getBool("on",false); s.cyrillic=p.getBool("cset",false);
        s.conflicts=p.getUChar("conflict",0); p.end();
    }
}
const IrNotesSettings &irnotes_settings(bool drawing) { return s_settings[drawing]; }
bool irnotes_store_settings(bool drawing,const IrNotesSettings &s) {
    if (s.conflicts>2) return false;
    Preferences p; if (!p.begin(space(drawing),false)) return false;
    bool ok=p.putBool("on",s.enabled)==1 && p.putBool("cset",s.cyrillic)==1 && p.putUChar("conflict",s.conflicts)==1;
    p.end(); if (ok) s_settings[drawing]=s; return ok;
}
bool irnotes_ready(bool drawing) {
    const auto &g=irmail_settings();
    return ENABLE_WIFI_GATEWAY && s_settings[drawing].enabled && g.google_scope(GOOGLE_DRIVE_SCOPE) &&
        (drawing || g.google_scope(GOOGLE_DOCS_SCOPE)) && !g.google_client_id.isEmpty() && !g.google_client_secret.isEmpty();
}
class EspNotesHost : public NoteSyncHost {
public:
    explicit EspNotesHost(bool drawing,const std::function<bool(size_t)> &reserve={},const std::function<bool()> &release={})
        : drawing_(drawing),reserve_(reserve),release_(release) {}
    bool reserve_record_buffer(size_t bytes) override { return reserve_ ? reserve_(bytes) : NoteSyncHost::reserve_record_buffer(bytes); }
    bool release_record_buffer() override { return !release_ || release_(); }
    GoogleHttp &http() override { return http_; }
    bool online() override { return WiFi.status()==WL_CONNECTED; }
    int64_t now() override { return (int64_t)time(nullptr); }
    uint32_t random() override { return esp_random(); }
    bool access_token(std::string &token,std::string &problem) override { return irmail_google_access_token(token,problem,true); }
    bool load(NoteSyncState &out) override {
        out=NoteSyncState(); Preferences p; if (!p.begin(space(drawing_),false)) return false;
        bool exists=p.isKey("state"); String text=p.getString("state",""); p.end();
        return !exists || (text.length() && notesync_decode(text.c_str(),NOTES_MAX_PER_SYNC,out));
    }
    bool store(const NoteSyncState &s) override {
        std::string text; if (!notesync_encode(s,text)) return false;
        Preferences p; if (!p.begin(space(drawing_),false)) return false;
        bool ok=p.putString("state",text.c_str())==text.size(); p.end(); return ok;
    }
private:
    bool drawing_;
    std::function<bool(size_t)> reserve_;
    std::function<bool()> release_;
    EspGoogleHttp http_{true,NOTES_HTTP_TIMEOUT_MS,NOTES_HTTP_RESPONSE_MAX};
};
String irnotes_check_login(bool drawing) {
    const auto &g=irmail_settings();
    if (!g.google_scope(GOOGLE_DRIVE_SCOPE) || (!drawing && !g.google_scope(GOOGLE_DOCS_SCOPE))) return "Sign in with Google and allow Drive and Docs access first.";
    if (WiFi.status()!=WL_CONNECTED) return "Wi-Fi is not connected.";
    std::string token,problem; if (!irmail_google_access_token(token,problem)) return problem.c_str();
    EspGoogleHttp http(false,NOTES_HTTP_TIMEOUT_MS,NOTES_HTTP_RESPONSE_MAX); GoogleDriveClient drive(http,token,NOTES_MAX_PER_SYNC+8,24000); std::string root;
    if (!drive.root(root)) return drive.error().c_str();
    EspNotesHost host(drawing); NoteSyncState state;
    if (!host.load(state)) return "Could not load notes sync state.";
    unsigned count=0;
    if (!state.account.empty() && state.account!=root) return "Google account differs from the saved notes binding.";
    if (!state.folder.empty()) {
        std::vector<GoogleDriveFile> files; if (!drive.list(state.folder,files)) return drive.error().c_str();
        count=files.size();
    }
    return "OK, Drive access and requested permissions; "+String(count)+" folder entries. HotSync checks the Palm database and document/image contents.";
}
String irnotes_last_result(bool drawing) { return s_results[drawing]; }
String irnotes_folder(bool drawing) {
    EspNotesHost host(drawing); NoteSyncState state;
    String user=host.load(state) && !state.username.empty() ? String(state.username.c_str()) : String("<HotSync user name>");
    return "Palm/"+user+(drawing ? "/Notes" : "/Memos");
}
bool irnotes_retry_creations(bool drawing) {
    EspNotesHost host(drawing); NoteSyncState state;
    return host.load(state) && notesync_retry_unconfirmed(state) && host.store(state);
}
void irnotes_sync(bool drawing,NoteSyncResult &r,const std::function<bool(size_t)> &reserve,const std::function<bool()> &release) {
    const auto &s=s_settings[drawing]; NoteSyncSetup setup;
    setup.drawing=drawing; setup.charset=s.cyrillic ? PALM_CYRILLIC : PALM_WESTERN; setup.conflicts=s.conflicts;
    setup.max_notes=NOTES_MAX_PER_SYNC; setup.content_memory=NOTES_CONTENT_MEMORY;
    EspNotesHost host(drawing,reserve,release); notesync_run(setup,host,r);
    auto summary=notesync_summary(r); strlcpy(s_results[drawing],summary.c_str(),sizeof(s_results[drawing]));
    irdbg_printf("%s: %s",drawing ? "Note Pad" : "Memos",summary.c_str());
}
