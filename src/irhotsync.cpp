#include "irhotsync.h"
#include "irdrive.h"
#include <Preferences.h>

static IrHotSyncSettings s_settings;
static char s_result[256]={};
static HotSyncUserSetup user_setup(const IrHotSyncSettings &s) {
    HotSyncUserSetup out; out.name=s.username.c_str(); out.charset=s.cyrillic ? PALM_CYRILLIC : PALM_WESTERN; return out;
}
void irhotsync_init() {
    Preferences p; bool saved=false;
    if (p.begin("irhotsync",true)) {
        saved=p.isKey("prefs"); String value=p.getString("prefs",""); p.end();
        if (saved && value.length() && (value[0]=='W' || value[0]=='C')) { s_settings.username=value.substring(1); s_settings.cyrillic=value[0]=='C'; }
    }
    // Older firmware kept an unnamed Palm's folder name on the Drive page.
    // Reuse it without deleting its settings, backup binding or numeric ID.
    if (!saved) {
        IrHotSyncSettings legacy; legacy.username=irdrive_settings().username; legacy.cyrillic=irdrive_settings().cyrillic;
        std::string encoded,problem;
        if (hotsync_user_validate(user_setup(legacy),encoded,problem)) s_settings=legacy;
        else strlcpy(s_result,"Set the shared HotSync name; the old Drive fallback does not fit the Palm name limit.",sizeof(s_result));
    }
}
const IrHotSyncSettings &irhotsync_settings() { return s_settings; }
bool irhotsync_store_settings(const IrHotSyncSettings &s,String &problem) {
    std::string encoded,error;
    if (!hotsync_user_validate(user_setup(s),encoded,error)) { problem=error.c_str(); return false; }
    Preferences p; if (!p.begin("irhotsync",false)) { problem="Could not open HotSync settings storage"; return false; }
    // One nonempty value makes the name/encoding update atomic, including
    // saving an empty name to explicitly turn automatic setup off.
    String value=String(s.cyrillic ? "C" : "W")+s.username;
    bool ok=p.putString("prefs",value)==value.length(); p.end();
    if (ok) s_settings=s; else problem="Could not save HotSync settings";
    return ok;
}
bool irhotsync_prepare(String &problem) {
    HotSyncUserResult result; bool ok=hotsync_user_prepare(user_setup(s_settings),result);
    std::string message=ok ? (result.name.empty() ? "Palm has no HotSync user name. Set it in wifi.setup -> HotSync settings." :
        (result.written ? "HotSync user name set: " : "Palm HotSync user: ")+result.name) : result.problem;
    strlcpy(s_result,message.c_str(),sizeof(s_result));
    if (!ok) problem=message.c_str();
    return ok;
}
String irhotsync_last_result() { return s_result; }
