#include "xiino.h"
#include <algorithm>
#include <cctype>

namespace {
bool space(char c) { return std::isspace((unsigned char)c); }
std::string lower(std::string value) {
    for (char &c : value) c=(char)std::tolower((unsigned char)c);
    return value;
}
bool keep_attribute(const std::string &name) {
    // Xiino's HTML specification: presentation, links, forms, frames and maps.
    // Modern class/style/data/event attributes only inflate the retained page.
    static const std::string names="|href|name|target|coords|shape|nohref|size|color|bgcolor|text|link|vlink|alink|ebdwidth|ebdheight|align|clear|compact|method|action|local|cols|rows|src|width|height|noshade|border|hspace|vspace|ismap|usemap|alt|value|type|maxlength|disabled|checked|selected|start|multiple|colspan|rowspan|nowrap|valign|cellpadding|cellspacing|content|http-equiv|charset|";
    return names.find("|"+name+"|")!=names.npos;
}
}

XiinoSourceFilter::XiinoSourceFilter(XiinoResource &page,size_t limit) : page_(page),limit_(limit) {
    page_.body.clear(); page_.truncated=false;
}
bool XiinoSourceFilter::append(const std::string &text) {
    if (text.size()>limit_-page_.body.size()) { page_.truncated=true; return false; }
    page_.body+=text; return true;
}
void XiinoSourceFilter::tag() {
    size_t pos=1;
    bool closing=pos<token_.size() && token_[pos]=='/';
    if (closing) ++pos;
    while (pos<token_.size() && space(token_[pos])) ++pos;
    size_t start=pos;
    while (pos<token_.size() && !space(token_[pos]) && token_[pos]!='/' && token_[pos]!='>') ++pos;
    std::string name=lower(token_.substr(start,pos-start));
    if (name.empty() || !std::isalpha((unsigned char)name[0])) return;
    if (!closing && (name=="script" || name=="style")) {
        // Empty tags preserve the upstream converter's supported-tag state.
        append("<"+name+"></"+name+">");
        raw_="</"+name; raw_match_=0; state_=Raw; return;
    }
    std::string result="<"+std::string(closing ? "/" : "")+name;
    if (!closing && !oversized_) while (pos<token_.size()) {
        while (pos<token_.size() && space(token_[pos])) ++pos;
        if (pos==token_.size() || token_[pos]=='>' || token_[pos]=='/') break;
        start=pos;
        while (pos<token_.size() && !space(token_[pos]) && token_[pos]!='=' && token_[pos]!='>') ++pos;
        if (pos==start) { ++pos; continue; }
        std::string attribute=lower(token_.substr(start,pos-start));
        while (pos<token_.size() && space(token_[pos])) ++pos;
        if (pos<token_.size() && token_[pos]=='=') {
            ++pos; while (pos<token_.size() && space(token_[pos])) ++pos;
            char quote=pos<token_.size() && (token_[pos]=='\'' || token_[pos]=='"') ? token_[pos++] : 0;
            while (pos<token_.size() && (quote ? token_[pos]!=quote : (!space(token_[pos]) && token_[pos]!='>'))) ++pos;
            if (quote && pos<token_.size()) ++pos;
        }
        if (keep_attribute(attribute)) result+=" "+token_.substr(start,pos-start);
    }
    bool selfclosing=token_.size()>1 && token_[token_.size()-2]=='/' && !oversized_;
    result+=selfclosing ? "/>" : ">";
    append(result);
    if (name=="pre" || name=="xmp" || name=="textarea") {
        if (closing) { if (preformatted_) --preformatted_; }
        else if (!selfclosing) ++preformatted_;
    }
}
void XiinoSourceFilter::feed(const char *bytes,size_t size) {
    for (size_t i=0;i<size && !full();++i) {
        char c=bytes[i];
        if (state_==Comment) {
            if (c=='-' && comment_dashes_<2) ++comment_dashes_;
            else if (c=='>' && comment_dashes_==2) { state_=Text; comment_dashes_=0; }
            else if (c!='-') comment_dashes_=0;
        } else if (state_==Raw || state_==RawEnd) {
            if (state_==RawEnd) {
                if (c=='>') { state_=Text; raw_match_=0; }
                else if (!space(c) && c!='/') { state_=Raw; raw_match_=c=='<' ? 1 : 0; }
            } else {
                char folded=(char)std::tolower((unsigned char)c);
                if (folded==raw_[raw_match_]) ++raw_match_;
                else raw_match_=c=='<' ? 1 : 0;
                if (raw_match_==raw_.size()) state_=RawEnd;
            }
        } else if (state_==Tag) {
            if (token_.size()<4096) token_+=c;
            else oversized_=true;
            if (token_=="<!--") { state_=Comment; token_.clear(); comment_dashes_=0; continue; }
            if (quote_) { if (c==quote_) quote_=0; }
            else if (c=='\'' || c=='"') quote_=c;
            else if (c=='>') {
                state_=Text; tag(); token_.clear(); oversized_=false;
            }
        } else if (c=='<') {
            whitespace_=false; state_=Tag; token_="<"; quote_=0; oversized_=false;
        } else if (!preformatted_ && space(c)) {
            whitespace_=true;
        } else {
            if (whitespace_ && !page_.body.empty() && page_.body.back()!='>') append(" ");
            whitespace_=false;
            if (!full()) append(std::string(1,c));
        }
    }
}
void XiinoSourceFilter::finish(bool source_limit) {
    if (source_limit) page_.truncated=true;
    // A partial final tag/comment is deliberately omitted. Plain text is
    // already retained, with no oversized raw block ever accumulated.
    token_.clear();
}
