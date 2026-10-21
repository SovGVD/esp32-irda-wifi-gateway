#include "expensetext.h"
#include <cstring>
#include <cstdio>

static const char *types[]={"Airfare","Breakfast","Bus","Business Meals","Car Rental","Dinner","Entertainment","Fax","Gas","Gifts","Hotel","Incidentals","Laundry","Limo","Lodging","Lunch","Mileage","Other","Parking","Postage","Snack","Subway","Supplies","Taxi","Telephone","Tips","Tolls","Train"};
static const char *payments[]={"AmEx","Cash","Check","CreditCard","MasterCard","Prepaid","VISA","Unfiled"};
// Palm Expense IDs are country selectors, not ISO numeric currency codes.
// Country IDs verified against J-Pilot's Expense/expense.c. Retain obsolete
// currencies distinctly; no conversion or replacement with EUR is implied.
static const char *currency_iso[]={"AUD","ATS","BEF","BRL","CAD","DKK","FIM","FRF","DEM","HKD","ISK","IEP","ITL","JPY","LUF","MXN","NLG","NZD","NOK","ESP","SEK","CHF","GBP","USD","INR","IDR","KRW","MYR","CNY","PHP","SGD","THB","TWD"};
std::string expense_currency_label(uint8_t code) {
    if (code>=33 && code!=133) return "#"+std::to_string(code);
    std::string iso=code==133 ? "EUR" : currency_iso[code];
    if (code==0 || code==4 || code==9 || code==17 || code==23 || code==30 || code==32) return iso+" ($)";
    if (code==22) return iso+" (\xc2\xa3)";
    if (code==13 || code==28) return iso+" (\xc2\xa5)";
    if (code==133) return iso+" (\xe2\x82\xac)";
    return iso;
}
bool expense_currency_code(const std::string &label,uint8_t &code) {
    unsigned value=0; size_t at=!label.empty() && label[0]=='#' ? 1 : 0;
    bool numeric=at<label.size();
    for (size_t i=at;numeric && i<label.size();i++) {
        if (label[i]<'0' || label[i]>'9' || value>25 || (value==25 && label[i]>'5')) numeric=false;
        else value=value*10+label[i]-'0';
    }
    if (numeric) { code=value; return true; }
    for (unsigned i=0;i<33;i++) if (label==currency_iso[i] || label==expense_currency_label(i)) { code=i; return true; }
    if (label=="EUR" || label==expense_currency_label(133)) { code=133; return true; }
    return false; // '$' alone cannot distinguish NZD, USD, AUD, CAD, etc.
}
static bool date_ok(unsigned year,unsigned month,unsigned day) {
    if (year<1904 || year>2031 || month<1 || month>12 || !day) return false;
    const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return day<=days[month-1]+(month==2 && year%4==0 && (year%100 || year%400==0));
}
static bool selector(const std::string &s,unsigned max,uint8_t &out) {
    if (s.empty() || s.size()>3) return false;
    unsigned n=0; for (char c:s) { if (c<'0' || c>'9') return false; n=n*10+c-'0'; }
    if (n>max) return false;
    out=n; return true;
}
static bool label(const std::string &s,const char *const *names,unsigned count,uint8_t &out) {
    for (unsigned i=0;i<count;i++) if (s==names[i]) { out=i; return true; }
    return s.size()>1 && s[0]=='#' && selector(s.substr(1),255,out);
}
bool expense_unpack(const uint8_t *data,size_t size,uint8_t category,PalmExpense &e,std::string &problem) {
    e=PalmExpense();
    auto fail=[&](const char *s) { problem=s; return false; };
    if (!data || size<11 || size>8000 || category>15) return fail("Expense record is truncated or too large");
    e.date=uint16_t(data[0])<<8|data[1]; e.type=data[2]; e.payment=data[3]; e.currency=data[4]; e.category=category;
    if (!date_ok(1904+(e.date>>9),(e.date>>5)&15,e.date&31)) return fail("Invalid Expense date");
    size_t at=6;
    for (auto &s:e.text) {
        auto *end=(const uint8_t *)memchr(data+at,0,size-at);
        if (!end || size_t(end-data-at)>4095) return fail("Expense string is unterminated or too large");
        s.assign((const char *)data+at,end-data-at); at=end-data+1;
    }
    // pilot-link ignores the reserved byte; real devices may also pad records.
    for (;at<size;at++) if (data[at]) return fail("Unexpected data after Expense strings");
    return true;
}
size_t expense_pack(const PalmExpense &e,uint8_t *data,size_t room) {
    if (!date_ok(1904+(e.date>>9),(e.date>>5)&15,e.date&31) || e.category>15) return 0;
    size_t n=11;
    for (const auto &s:e.text) { if (s.size()>4095 || s.find('\0')!=std::string::npos) return 0; n+=s.size(); }
    if (!data || n>room || n>8000) return 0;
    data[0]=e.date>>8; data[1]=e.date; data[2]=e.type; data[3]=e.payment; data[4]=e.currency; data[5]=0;
    size_t at=6; for (const auto &s:e.text) { memcpy(data+at,s.c_str(),s.size()+1); at+=s.size()+1; }
    return n;
}
ExpenseFields expense_fields(const PalmExpense &e,PalmCharset charset) {
    ExpenseFields f;
    char date[16]; snprintf(date,sizeof(date),"%04u-%02u-%02u",1904+(e.date>>9),(e.date>>5)&15,e.date&31); f[0]=date;
    f[1]=e.type<28 ? types[e.type] : "#"+std::to_string(e.type);
    f[2]=e.payment<8 ? payments[e.payment] : "#"+std::to_string(e.payment);
    f[3]=expense_currency_label(e.currency);
    for (unsigned i=0;i<5;i++) f[i+4]=palm_to_utf8(e.text[i],charset);
    f[9]=std::to_string(e.category); return f;
}
bool expense_project(const ExpenseFields &f,PalmCharset charset,PalmExpense &e,std::string &problem) {
    auto fail=[&](const char *s) { problem=s; return false; };
    unsigned y=0,m=0,d=0;
    if (f[0].size()!=10 || f[0][4]!='-' || f[0][7]!='-') return fail("Date must be text in YYYY-MM-DD format (1904-2031)");
    for (unsigned i=0;i<10;i++) if (i!=4 && i!=7) {
        if (f[0][i]<'0' || f[0][i]>'9') return fail("Invalid Expense date");
        auto &n=i<4 ? y : (i<7 ? m : d); n=n*10+f[0][i]-'0';
    }
    if (!date_ok(y,m,d)) return fail("Invalid Expense date (supported years: 1904-2031)");
    e=PalmExpense(); e.date=((y-1904)<<9)|(m<<5)|d;
    if (!label(f[1],types,28,e.type)) return fail("Unknown Expense type; use a documented name or #0 through #255");
    if (!label(f[2],payments,8,e.payment)) return fail("Unknown payment; use a documented name or #0 through #255");
    if (!expense_currency_code(f[3],e.currency)) return fail("Use a currency such as NZD ($), USD ($), EUR, or a Palm code 0-255; $ alone is ambiguous");
    if (!selector(f[9],15,e.category)) return fail("Category must be a Palm slot 0-15");
    size_t total=11;
    for (unsigned i=0;i<5;i++) {
        if (f[i+4].find('\0')!=std::string::npos) return fail("Expense text contains a NUL character");
        e.text[i]=palm_from_utf8(f[i+4],charset,4097); total+=e.text[i].size();
        if (e.text[i].size()>4095) return fail("Expense text exceeds the Palm record limit");
    }
    if (total>8000) return fail("Expense record exceeds 8000 Palm bytes");
    return true;
}
std::array<uint32_t,10> expense_hashes(const ExpenseFields &f) {
    std::array<uint32_t,10> h;
    for (unsigned i=0;i<10;i++) {
        // Preserve v1 sync hashes: changing the cell's display must not look
        // like a currency edit on either side of an existing sync mapping.
        uint8_t code;
        std::string canonical;
        if (i==3 && expense_currency_code(f[i],code)) canonical=std::to_string(code);
        const auto &value=canonical.empty() ? f[i] : canonical;
        h[i]=2166136261u; for (unsigned char c:value) h[i]=(h[i]^c)*16777619u;
    }
    return h;
}
