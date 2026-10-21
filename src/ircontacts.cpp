#include "ircontacts.h"
#include "irmail.h"
#include "irgooglehttp.h"
#include "irdebug.h"
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>

static IrContactsSettings s_settings;
static char s_result[256]="";
static GoogleContactsSetup contacts_setup() { GoogleContactsSetup s; s.max_contacts=CONTACTS_MAX_PER_SYNC; return s; }
void ircontacts_init() {
    Preferences p;
    if (!p.begin("ircontacts",true)) return;
    s_settings.enabled=p.getBool("on",false); s_settings.cyrillic=p.getBool("cset",false); p.end();
}
const IrContactsSettings &ircontacts_settings() { return s_settings; }
bool ircontacts_store_settings(const IrContactsSettings &s) {
    Preferences p;
    if (!p.begin("ircontacts",false)) return false;
    bool ok=p.putBool("on",s.enabled)==1 && p.putBool("cset",s.cyrillic)==1;
    p.end(); if (ok) s_settings=s; return ok;
}
bool ircontacts_ready() {
    const auto &google=irmail_settings();
    return ENABLE_WIFI_GATEWAY && s_settings.enabled && google.google_contacts() &&
        !google.google_client_id.isEmpty() && !google.google_client_secret.isEmpty();
}
class EspContactsHost : public ContactSyncHost {
public:
    GoogleHttp &http() override { return http_; }
    bool online() override { return WiFi.status()==WL_CONNECTED; }
    uint32_t random() override { return esp_random(); }
    bool access_token(std::string &token,std::string &problem) override { return irmail_google_access_token(token,problem,true); }
    bool bind_account(const std::string &id,uint32_t palm) override {
        Preferences p;
        if (!p.begin("ircontacts",false)) return false;
        String bound=p.getString("bound","");
        bool ok=(bound.isEmpty() || bound==id.c_str()) && (!p.isKey("palm") || p.getULong("palm")==palm);
        if (ok) ok=p.putString("bound",id.c_str())==id.size() && p.putULong("palm",palm)==4;
        p.end(); return ok;
    }
    bool load_map(std::vector<ContactSyncMap> &map) override {
        map.clear(); Preferences p;
        if (!p.begin("ircontacts",false)) return false;
        if (!p.isKey("links")) { p.end(); return true; }
        size_t length=p.getBytesLength("links");
        if (length<6 || length>CONTACTS_MAX_PER_SYNC*(71+640)+6) { p.end(); return false; }
        std::string data(length,'\0'); bool ok=p.getBytes("links",&data[0],length)==length; p.end();
        return ok && contactsync_decode_map(data,CONTACTS_MAX_PER_SYNC,map);
    }
    bool store_map(const std::vector<ContactSyncMap> &map) override {
        std::string data;
        if (!contactsync_encode_map(map,data)) return false;
        Preferences p;
        if (!p.begin("ircontacts",false)) return false;
        bool ok=p.putBytes("links",data.data(),data.size())==data.size(); p.end(); return ok;
    }
private:
    EspGoogleHttp http_{true,CONTACTS_HTTP_TIMEOUT_MS,CONTACTS_HTTP_RESPONSE_MAX};
};
bool ircontacts_has_pending() {
    EspContactsHost host; std::vector<ContactSyncMap> map;
    if (!host.load_map(map)) return false;
    for (const auto &m:map) if (m.pending==2) return true;
    return false;
}
bool ircontacts_retry_pending() {
    // The portal is used while the Palm is on Network, outside HotSync.
    EspContactsHost host; std::vector<ContactSyncMap> map;
    if (!host.load_map(map)) return false;
    for (auto &m:map) if (m.pending==2) m.pending=1;
    return host.store_map(map);
}
String ircontacts_check_login() {
    const auto &settings=irmail_settings();
    if (settings.google_scope(GOOGLE_CONTACTS_SCOPE) && !settings.google_scope(GOOGLE_PROFILE_SCOPE))
        return "Sign in with Google again and allow basic profile access to identify the Contacts account.";
    if (!ircontacts_ready()) return "Enable Contacts and sign in with Contacts permission first.";
    if (WiFi.status()!=WL_CONNECTED) return "Wi-Fi is not connected.";
    std::string token,problem;
    if (!irmail_google_access_token(token,problem)) return problem.c_str();
    EspGoogleHttp http(false,CONTACTS_HTTP_TIMEOUT_MS,CONTACTS_HTTP_RESPONSE_MAX);
    auto setup=contacts_setup(); GoogleContactsClient google(http,setup,token);
    std::string account; std::vector<GoogleContact> contacts;
    if (!google.identity(account) || !google.list(contacts)) return google.error().c_str();
    return "OK, "+String((unsigned)contacts.size())+" Google contacts.";
}
String ircontacts_last_result() { return s_result; }
void ircontacts_sync(ContactSyncResult &r) {
    EspContactsHost host; contactsync_run(contacts_setup(),s_settings.cyrillic ? PALM_CYRILLIC : PALM_WESTERN,host,r);
    std::string summary=contactsync_summary(r); strlcpy(s_result,summary.c_str(),sizeof(s_result)); irdbg_printf("Contacts: %s",s_result);
}
