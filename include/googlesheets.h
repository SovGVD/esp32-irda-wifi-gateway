#pragma once
#include "googledrive.h"
#include "expensetext.h"

struct GoogleExpenseRow {
    std::string id;
    ExpenseFields fields;
    unsigned row=0; // zero-based grid index; identity is id, never this index
    bool deleted=false;
    bool operator==(const GoogleExpenseRow &v) const { return id==v.id && fields==v.fields && row==v.row && deleted==v.deleted; }
};
struct GoogleExpenseChange {
    GoogleExpenseRow after;
    unsigned before_row=0;
    bool append=false;
    GoogleExpenseChange(const GoogleExpenseRow &b,const GoogleExpenseRow &a,bool add) : after(a),before_row(b.row),append(add) {}
};
class GoogleSheetsClient {
public:
    GoogleSheetsClient(GoogleHttp &http,const std::string &token,unsigned max=64,size_t budget=24000)
        : drive(http,token,max+8,budget),http_(http),token_(token),max_(max),budget_(budget) {}
    GoogleDriveClient drive;
    bool create(const std::string &parent,const std::string &marker,std::string &id);
    bool initialize(const std::string &id,bool create_if_missing=true);
    bool read(const std::string &id,std::vector<GoogleExpenseRow> &rows);
    bool apply(const std::string &id,const std::vector<GoogleExpenseRow> &snapshot,const std::vector<GoogleExpenseChange> &changes,
               const std::function<bool()> &before_write);
    const std::string &error() const { return error_; }
private:
    bool call(const std::string &url,const char *method,const std::string &body,std::string &reply);
    bool literal(const std::string &id,unsigned row,unsigned col,const std::string &value);
    GoogleHttp &http_; std::string token_,error_; unsigned max_; size_t budget_;
};
