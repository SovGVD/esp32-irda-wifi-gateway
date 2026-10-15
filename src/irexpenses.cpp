#include "irexpenses.h"
#include "irmail.h"
#include "irgooglehttp.h"
#include "irdebug.h"
#include <Preferences.h>
#include <WiFi.h>

static IrExpenseSettings s_settings;
static char s_result[384]={};
void irexpenses_init() {
    Preferences p; if (!p.begin("irexpenses",true)) return;
    s_settings.enabled=p.getBool("on",false); s_settings.cyrillic=p.getBool("cset",false); s_settings.conflicts=p.getUChar("conflict",0); p.end();
}
const IrExpenseSettings &irexpenses_settings() { return s_settings; }
bool irexpenses_store_settings(const IrExpenseSettings &s) {
    if (s.conflicts>2) return false;
    Preferences p; if (!p.begin("irexpenses",false)) return false;
    bool ok=p.putBool("on",s.enabled)==1 && p.putBool("cset",s.cyrillic)==1 && p.putUChar("conflict",s.conflicts)==1;
    p.end(); if (ok) s_settings=s; return ok;
}
bool irexpenses_ready() {
    const auto &g=irmail_settings();
    return ENABLE_WIFI_GATEWAY && s_settings.enabled && g.google_scope(GOOGLE_DRIVE_SCOPE) && g.google_scope(GOOGLE_SHEETS_SCOPE) && !g.google_client_id.isEmpty() && !g.google_client_secret.isEmpty();
}
class EspExpenseHost : public ExpenseSyncHost {
public:
    GoogleHttp &http() override { return http_; }
    bool online() override { return WiFi.status()==WL_CONNECTED; }
    uint32_t random() override { return esp_random(); }
    bool access_token(std::string &token,std::string &problem) override { return irmail_google_access_token(token,problem,true); }
    bool load(ExpenseSyncState &out) override {
        out=ExpenseSyncState(); Preferences p; if (!p.begin("irexpenses",false)) return false;
        bool exists=p.isKey("state"); String text=p.getString("state",""); p.end();
        return !exists || (text.length() && expensesync_decode(text.c_str(),EXPENSES_MAX_PER_SYNC,out));
    }
    bool store(const ExpenseSyncState &s) override {
        std::string text; if (!expensesync_encode(s,text)) return false;
        Preferences p; if (!p.begin("irexpenses",false)) return false;
        bool ok=p.putString("state",text.c_str())==text.size(); p.end(); return ok;
    }
private:
    EspGoogleHttp http_{true,EXPENSES_HTTP_TIMEOUT_MS,EXPENSES_HTTP_RESPONSE_MAX};
};
String irexpenses_check_login() {
    const auto &g=irmail_settings();
    if (!g.google_scope(GOOGLE_DRIVE_SCOPE) || !g.google_scope(GOOGLE_SHEETS_SCOPE)) return "Sign in with Google and allow Drive and Sheets access first.";
    if (WiFi.status()!=WL_CONNECTED) return "Wi-Fi is not connected.";
    std::string token,problem; if (!irmail_google_access_token(token,problem)) return problem.c_str();
    EspGoogleHttp http(false,EXPENSES_HTTP_TIMEOUT_MS,EXPENSES_HTTP_RESPONSE_MAX); GoogleSheetsClient google(http,token,EXPENSES_MAX_PER_SYNC,EXPENSES_CONTENT_MEMORY); std::string root;
    if (!google.drive.root(root)) return google.drive.error().c_str();
    EspExpenseHost host; ExpenseSyncState state;
    if (!host.load(state)) return "Could not load Expense sync state.";
    if (!state.account.empty() && state.account!=root) return "Google account differs from the saved Expense binding.";
    if (state.spreadsheet.empty()) return "OK, Drive access and requested Sheets permission. First HotSync creates the spreadsheet and checks the Sheets API and ExpenseDB.";
    GoogleDriveFile file; bool found;
    if (!google.drive.get(state.spreadsheet,file,found)) return google.drive.error().c_str();
    if (!found || file.trashed) return "The Expenses spreadsheet is missing or trashed.";
    if (!google.initialize(state.spreadsheet,false)) return google.error().c_str();
    std::vector<GoogleExpenseRow> rows; if (!google.read(state.spreadsheet,rows)) return google.error().c_str();
    return "OK, Google Sheets access; "+String(rows.size())+" expense rows. HotSync checks Palm records and transfers changes.";
}
String irexpenses_last_result() { return s_result; }
String irexpenses_sheet_url() {
    EspExpenseHost host; ExpenseSyncState state;
    return host.load(state) && !state.spreadsheet.empty() ? String("https://docs.google.com/spreadsheets/d/")+state.spreadsheet.c_str() : String();
}
bool irexpenses_retry_creations() {
    EspExpenseHost host; ExpenseSyncState state;
    return host.load(state) && expensesync_retry_unconfirmed(state) && host.store(state);
}
void irexpenses_sync(ExpenseSyncResult &r) {
    ExpenseSyncSetup setup; setup.charset=s_settings.cyrillic ? PALM_CYRILLIC : PALM_WESTERN; setup.conflicts=s_settings.conflicts;
    setup.max_expenses=EXPENSES_MAX_PER_SYNC; setup.content_memory=EXPENSES_CONTENT_MEMORY;
    EspExpenseHost host; expensesync_run(setup,host,r);
    auto summary=expensesync_summary(r); strlcpy(s_result,summary.c_str(),sizeof(s_result)); irdbg_printf("Expenses: %s",summary.c_str());
}
