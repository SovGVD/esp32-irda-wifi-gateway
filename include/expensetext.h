#pragma once
#include "mailtext.h"
#include <array>

// Classic ExpenseDB: packed Palm date, three byte-sized selectors, one reserved
// byte, then five NUL-terminated strings. Category lives in the DLP record.
using ExpenseFields=std::array<std::string,10>;
struct PalmExpense {
    uint16_t date=0;
    uint8_t type=0,payment=0,currency=0,category=0;
    std::array<std::string,5> text; // amount, vendor, city, attendees, note
};
bool expense_unpack(const uint8_t *data,size_t size,uint8_t category,PalmExpense &out,std::string &problem);
size_t expense_pack(const PalmExpense &expense,uint8_t *data,size_t room);
ExpenseFields expense_fields(const PalmExpense &expense,PalmCharset charset);
bool expense_project(const ExpenseFields &fields,PalmCharset charset,PalmExpense &expense,std::string &problem);
std::array<uint32_t,10> expense_hashes(const ExpenseFields &fields);
std::string expense_currency_label(uint8_t code);
bool expense_currency_code(const std::string &label,uint8_t &code);
