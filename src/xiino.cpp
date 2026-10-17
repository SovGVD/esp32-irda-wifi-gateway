// Adapted from nicl83/OpenXiino (MIT), e9ef5062095f66f7e731c5a0f831e394a4c02380.
// Retain upstream parsing/encoding quirks; adapt resource limits for the ESP32.
#include "xiino.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
#include "xiino_assets.inc"
struct Entity { const char *name, *value; };
#include "xiino_entities.inc"
std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string upper(std::string s) {
    for (char &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
bool space(char c) { return std::isspace((unsigned char)c); }
std::string trim(const std::string &s) {
    size_t a=0,b=s.size();
    while (a<b && space(s[a])) ++a;
    while (b>a && space(s[b-1])) --b;
    return s.substr(a,b-a);
}
void utf8(std::string &s, unsigned c) {
    if (c<128) s+=(char)c;
    else if (c<2048) { s+=(char)(192|(c>>6)); s+=(char)(128|(c&63)); }
    else if (c<65536) { s+=(char)(224|(c>>12)); s+=(char)(128|((c>>6)&63)); s+=(char)(128|(c&63)); }
    else { s+=(char)(240|(c>>18)); s+=(char)(128|((c>>12)&63)); s+=(char)(128|((c>>6)&63)); s+=(char)(128|(c&63)); }
}
std::string unescape(const std::string &s) {
    std::string out;
    for (size_t i=0;i<s.size();) {
        if (s[i]!='&') { out+=s[i++]; continue; }
        size_t at=i+1;
        if (at<s.size() && s[at]=='#') {
            ++at; int radix=10;
            if (at<s.size() && (s[at]=='x' || s[at]=='X')) { radix=16; ++at; }
            size_t start=at; unsigned code=0;
            while (at<s.size() && (radix==16 ? std::isxdigit((unsigned char)s[at]) : std::isdigit((unsigned char)s[at]))) {
                unsigned digit=s[at]<='9' ? s[at]-'0' : std::tolower((unsigned char)s[at])-'a'+10;
                code=std::min(0x110000u,code*(unsigned)radix+digit); ++at;
            }
            if (at!=start) {
                if (at<s.size() && s[at]==';') ++at;
                static const unsigned windows[] = {8364,129,8218,402,8222,8230,8224,8225,710,8240,352,8249,338,141,381,143,144,8216,8217,8220,8221,8226,8211,8212,732,8482,353,8250,339,157,382,376};
                if (code>=128 && code<=159) code=windows[code-128];
                if (!code || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) code=0xfffd;
                utf8(out,code); i=at; continue;
            }
        } else {
            size_t end=at;
            while (end<s.size() && end-at<32 && std::isalnum((unsigned char)s[end])) ++end;
            if (end<s.size() && s[end]==';') ++end;
            bool found=false;
            for (size_t n=end-at;n;--n) {
                std::string key=s.substr(at,n);
                auto e=std::lower_bound(std::begin(entities),std::end(entities),key,
                    [](const Entity &entry,const std::string &value) { return entry.name<value; });
                if (e!=std::end(entities) && key==e->name) {
                    out+=e->value; i=at+n; found=true; break;
                }
            }
            if (found) continue;
        }
        out+=s[i++];
    }
    return out;
}
std::string latin1(const std::string &s) {
    std::string out;
    for (size_t i=0;i<s.size();) {
        unsigned char c=s[i++]; unsigned code=c, count=0;
        if (c>=0xc2 && c<=0xdf) { code=c&31; count=1; }
        else if (c>=0xe0 && c<=0xef) { code=c&15; count=2; }
        else if (c>=0xf0 && c<=0xf4) { code=c&7; count=3; }
        else if (c>=128) continue;
        bool valid=true;
        while (count--) {
            if (i==s.size() || ((unsigned char)s[i]&192)!=128) { valid=false; break; }
            code=(code<<6)|((unsigned char)s[i++]&63);
        }
        if (valid && code<=255) out+=(char)code;
    }
    return out;
}
std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c=='&') out+="&amp;";
        else if (c=='<') out+="&lt;";
        else if (c=='>') out+="&gt;";
        else out+=c;
    }
    return out;
}
enum class PageEncoding { Utf8, Latin1, Windows1252 };
PageEncoding page_encoding(const XiinoResource &page) {
    std::string type=lower(page.content_type),encoding;
    size_t at=type.find("charset=");
    if (at!=type.npos) {
        encoding=trim(type.substr(at+8));
        if (!encoding.empty() && (encoding[0]=='\'' || encoding[0]=='"')) encoding.erase(0,1);
        encoding=encoding.substr(0,encoding.find_first_of(";\"' \t\r\n"));
    } else if (type.compare(0,5,"text/")==0) encoding="iso-8859-1"; // requests' HTTP default
    if (encoding=="windows-1252" || encoding=="cp1252") return PageEncoding::Windows1252;
    if (encoding=="iso-8859-1" || encoding=="latin-1" || encoding=="latin1" || encoding=="iso8859-1") return PageEncoding::Latin1;
    return PageEncoding::Utf8;
}
std::string source_text(const std::string &text,PageEncoding encoding) {
    if (encoding==PageEncoding::Utf8) return text;
    static const unsigned codes[]={8364,129,8218,402,8222,8230,8224,8225,710,8240,352,8249,338,141,381,143,144,8216,8217,8220,8221,8226,8211,8212,732,8482,353,8250,339,157,382,376};
    std::string out;
    for (unsigned char c : text) utf8(out,encoding==PageEncoding::Windows1252 && c>=128 && c<=159 ? codes[c-128] : c);
    return out;
}
// Search raw-text closing tags without allocating a lowercase page copy.
size_t find_case_insensitive(const std::string &text,const std::string &needle,size_t start) {
    auto found=std::search(text.begin()+start,text.end(),needle.begin(),needle.end(),
        [](unsigned char a,unsigned char b) { return std::tolower(a)==std::tolower(b); });
    return found==text.end() ? text.npos : size_t(found-text.begin());
}
std::string setting(const std::string &s, const char *prefix) {
    size_t at=s.find(std::string("/")+prefix);
    if (at==s.npos) return "";
    at+=strlen(prefix)+1; size_t end=s.find('/',at);
    return end==s.npos ? "" : s.substr(at,end-at);
}
bool supported(const std::string &tag) {
    static const char tags[]="|a|address|area|b|base|basefont|blink|blockquote|body|bgcolor|br|clear|center|caption|cite|code|dd|dir|div|dl|dt|font|form|frame|frameset|h1|h2|h3|h4|h5|h6|hr|i|img|input|isindex|kbd|li|map|meta|multicol|nobr|noframes|ol|option|p|plaintext|pre|s|select|small|strike|strong|style|sub|sup|table|title|td|th|tr|tt|u|ul|var|xmp|";
    return std::string(tags).find("|"+tag+"|")!=std::string::npos;
}
} // namespace

XiinoRequest xiino_request(const std::string &target) {
    XiinoRequest r;
    size_t at=target.find("/?");
    if (at==target.npos) return r;
    r.url=target.substr(at+2);
    if (r.url.compare(0,7,"http://") && r.url.compare(0,8,"https://")) { r.url.clear(); return r; }
    r.dataserver=true;
    const std::string settings=target.substr(0,at+1);
    r.colour_depth=setting(settings,"c"); r.grayscale_depth=setting(settings,"g");
    r.width=setting(settings,"w"); r.encoding=setting(settings,"de");
    if (r.encoding.empty()) r.encoding=setting(settings,"d");
    if (r.encoding.empty()) r.encoding=setting(settings,"e");
    return r;
}
const std::string &xiino_padding() {
    static const std::string padding=std::string(12,'\0')+"\r\n\r\n";
    return padding;
}

std::string xiino_resolve(const std::string &base, const std::string &reference) {
    if (reference.empty()) return base;
    size_t colon=reference.find(':'), delimiter=reference.find_first_of("/?#");
    if (colon!=reference.npos && (delimiter==reference.npos || colon<delimiter)) return reference;
    size_t scheme=base.find("://");
    if (scheme==base.npos) return reference;
    if (reference.compare(0,2,"//")==0) return base.substr(0,scheme+1)+reference;
    size_t path_start=base.find_first_of("/?#",scheme+3);
    std::string site=base.substr(0,path_start);
    std::string path=path_start==base.npos ? "/" : base.substr(path_start);
    if (reference[0]=='#') return base.substr(0,base.find('#'))+reference;
    if (reference[0]=='?') return base.substr(0,base.find_first_of("?#"))+reference;
    path=path.substr(0,path.find_first_of("?#"));
    if (path.empty() || path[0]!='/') path="/";
    path=reference[0]=='/' ? reference : path.substr(0,path.rfind('/')+1)+reference;
    size_t suffix=path.find_first_of("?#");
    std::string tail=suffix==path.npos ? "" : path.substr(suffix);
    path=path.substr(0,suffix);
    std::vector<std::string> parts;
    for (size_t at=1;at<=path.size();) {
        size_t end=path.find('/',at); if (end==path.npos) end=path.size();
        std::string part=path.substr(at,end-at);
        if (part=="..") { if (!parts.empty()) parts.pop_back(); }
        else if (part!=".") parts.push_back(part);
        if (end==path.size()) {
            if (part=="." || part=="..") parts.push_back("");
            break;
        }
        at=end+1;
    }
    std::string result=site+"/";
    for (size_t i=0;i<parts.size();++i) { if (i) result+='/'; result+=parts[i]; }
    return result+tail;
}

static bool convert_page(const XiinoResource &page, const XiinoFetch &fetch,
                   const XiinoLimits &limits, std::string &html, std::string &problem,const XiinoSink *sink) {
    html.clear(); problem.clear();
    std::vector<std::string> embedded;
    size_t embedded_bytes=0;
    const std::string shortened_notice="\n<P>[Page shortened to fit the Palm]</P>\n</BODY></HTML>\n";
    if (limits.page_bytes<=shortened_notice.size()) {
        problem="Xiino page limit is too small"; return false;
    }
    const size_t payload_limit=limits.page_bytes-shortened_notice.size();
    bool stopped=false,shortened=page.truncated;
    size_t emitted=0;
    auto write=[&](const std::string &bytes) {
        for (size_t at=0;at<bytes.size();at+=512) {
            if (!(*sink)(bytes.data()+at,std::min(size_t(512),bytes.size()-at))) {
                problem="Palm disconnected during Xiino response"; return false;
            }
        }
        return true;
    };
    auto flush=[&]() {
        if (!sink || html.empty()) return true;
        if (!write(html)) return false;
        emitted+=html.size(); html.clear(); return true;
    };
    unsigned slot=0; bool parsing_supported=true;
    auto append=[&](const std::string &text,bool atomic=true) {
        std::string bytes=latin1(text);
        if (stopped) return true;
        size_t remaining=payload_limit-emitted-html.size()-embedded_bytes;
        if (bytes.size()>remaining) {
            if (!atomic) html.append(bytes,0,remaining);
            shortened=true; stopped=true; return flush();
        }
        html+=bytes;
        return html.size()<512 || flush();
    };
    const PageEncoding encoding=page_encoding(page);
    auto data=[&](const std::string &text, bool refs) {
        if (!parsing_supported || text.empty()) return true;
        std::string decoded=source_text(text,encoding);
        return append(trim(refs ? unescape(decoded) : decoded)+"\n",false);
    };
    // HTML structure is ASCII in all supported encodings. Convert text and
    // attribute values only when consumed, never retain another full page
    // beside the compacted source, TLS state and decoded image workspace.
    const std::string &source=page.body;
    std::string rawtag;
    for (size_t at=0;at<source.size() && !stopped;) {
        if (!rawtag.empty()) {
            size_t end=find_case_insensitive(source,"</"+rawtag,at);
            if (end==source.npos) break; // HTMLParser buffers unfinished script/style content.
            if (!data(source.substr(at,end-at),false)) return false;
            at=end; rawtag.clear();
        }
        if (source.compare(at,4,"<!--")==0) {
            size_t end=source.find("-->",at+4); if (end==source.npos) break;
            at=end+3; continue;
        }
        if (source[at]!='<') {
            size_t end=source.find('<',at); if (end==source.npos) end=source.size();
            if (!data(source.substr(at,end-at),true)) return false;
            at=end; continue;
        }
        size_t pos=at+1; bool closing=pos<source.size() && source[pos]=='/';
        if (closing) ++pos;
        if (pos==source.size()) break;
        if (source[pos]=='!' || source[pos]=='?') {
            size_t end=source.find('>',pos); if (end==source.npos) break;
            at=end+1; continue;
        }
        if (!std::isalpha((unsigned char)source[pos])) {
            if (!data("<",true)) return false;
            ++at; continue;
        }
        size_t start=pos;
        while (pos<source.size() && !space(source[pos]) && source[pos]!='/' && source[pos]!='>') ++pos;
        std::string tag=lower(source.substr(start,pos-start));
        std::vector<std::pair<std::string,std::string>> attrs;
        bool selfclosing=false, complete=false;
        while (pos<source.size()) {
            while (pos<source.size() && space(source[pos])) ++pos;
            if (pos==source.size()) break;
            if (source[pos]=='>') { ++pos; complete=true; break; }
            if (source[pos]=='/' && pos+1<source.size() && source[pos+1]=='>') {
                pos+=2; complete=true; selfclosing=true; break;
            }
            start=pos;
            while (pos<source.size() && !space(source[pos]) && source[pos]!='=' && source[pos]!='>') ++pos;
            if (start==pos) { ++pos; continue; }
            std::string name=lower(source.substr(start,pos-start)),value="None";
            while (pos<source.size() && space(source[pos])) ++pos;
            if (pos<source.size() && source[pos]=='=') {
                ++pos; while (pos<source.size() && space(source[pos])) ++pos;
                char quote=pos<source.size() && (source[pos]=='\'' || source[pos]=='"') ? source[pos++] : 0;
                start=pos;
                while (pos<source.size() && (quote ? source[pos]!=quote : (!space(source[pos]) && source[pos]!='>'))) ++pos;
                value=unescape(source_text(source.substr(start,pos-start),encoding));
                if (quote && pos<source.size()) ++pos;
            }
            attrs.emplace_back(name,value);
        }
        if (!complete) break;
        at=pos;
        if (closing) {
            if (supported(tag) && !append("</"+upper(tag)+">\n")) return false;
            continue;
        }
        if (!supported(tag)) parsing_supported=false;
        else if (tag=="img") {
            std::string src;
            for (const auto &attr : attrs) if (attr.first=="src") { src=attr.second; break; }
            if (!src.empty()) {
                if (!flush()) return false; // Text reaches the Palm before optional network/image work.
                XiinoResource image; std::string img,ebd,error;
                if (!fetch(xiino_resolve(page.url,src),limits.image_bytes,image,error)) {
                    std::string why=error.find("memory")!=error.npos ? "memory limit" :
                        error.find("budget")!=error.npos ? "page image budget" :
                        error.find("time limit")!=error.npos ? "download timeout" : "download failed";
                    if (!append("<p>[Image unavailable: "+why+"]</p>")) return false;
                    continue;
                }
                if (xiino_image(image.body,slot+1,limits,img,ebd,error)) {
                    size_t total=img.size()+ebd.size()+2;
                    if (total>payload_limit-emitted-html.size()-embedded_bytes) {
                        if (!append("<p>[Image omitted: page limit]</p>")) return false;
                        continue;
                    }
                    embedded_bytes+=ebd.size()+1; embedded.push_back(std::move(ebd)); ++slot;
                    if (!append(img+"\n")) return false;
                } else {
                    std::string why=error.find("memory")!=error.npos || error.find("Thumbnail")!=error.npos ?
                        "Image omitted: memory limit" : "Image omitted: "+error;
                    if (!append("<p>["+escape(why)+"]</p>")) return false;
                }
            }
        } else {
            parsing_supported=true;
            std::string output="<"+upper(tag)+" ";
            for (size_t i=0;i<attrs.size();++i) {
                if (i) output+=' ';
                std::string value=attrs[i].second;
                if (tag=="a" && attrs[i].first=="href") {
                    value=xiino_resolve(page.url,value);
                    if (value.compare(0,6,"https:")==0) value.replace(0,6,"http:");
                }
                output+=upper(attrs[i].first)+"=\""+value+"\"";
            }
            if (!append(output+">\n")) return false;
        }
        if (selfclosing && supported(tag) && !append("</"+upper(tag)+">\n")) return false;
        if (!selfclosing && (tag=="script" || tag=="style")) rawtag=tag;
    }
    if (shortened) html+=shortened_notice;
    if (sink) {
        if (!flush()) return false;
        for (const auto &image : embedded) if (!write(image) || !write("\n")) return false;
    } else for (const auto &image : embedded) { html+=image; html+='\n'; }
    return true;
}

bool xiino_convert(const XiinoResource &page,const XiinoFetch &fetch,const XiinoLimits &limits,
                   std::string &html,std::string &problem) {
    return convert_page(page,fetch,limits,html,problem,nullptr);
}
bool xiino_convert_stream(const XiinoResource &page,const XiinoFetch &fetch,const XiinoLimits &limits,
                          const XiinoSink &sink,std::string &problem) {
    std::string buffer;
    return convert_page(page,fetch,limits,buffer,problem,&sink);
}

std::string xiino_builtin(const XiinoRequest &r, const std::string &headers) {
    if (r.url=="http://about/") return about_page;
    if (r.url=="http://about2/") return about2_page;
    if (r.url=="http://github/") return github_page;
    if (r.url!="http://deviceinfo/") return "";
    std::string out="<html><title>Device Info</title><body><h1>Device Info</h1><ul>";
    if (!r.colour_depth.empty()) out+="<li><b>"+escape(r.colour_depth)+"-bit colour </b>reported by Xiino.</li>";
    else if (!r.grayscale_depth.empty()) out+="<li><b>"+escape(r.grayscale_depth)+"-bit grayscale </b>reported by Xiino.</li>";
    else out+="<li>Your device isn't reporting a colour depth! Please tell the OpenXiino devs about your Xiino version.</li>";
    if (!r.width.empty()) {
        out+="<li><b>"+escape(r.width)+"px </b>viewport reported by Xiino. ";
        if (std::atoi(r.width.c_str())>153) out+="This is a high-density device.";
        out+="</li>";
    } else out+="<li>Your device isn't reporting a screen width! Please tell the OpenXiino devs about your Xiino version.</li>";
    if (!r.encoding.empty()) out+="<li>Your text encoding is set to <br/><b>"+escape(r.encoding)+"</b></li>";
    else out+="<li>Your device isn't reporting an encoding! Please tell the OpenXiino devs about your Xiino version.</li>";
    return out+"</ul><h2>Request Headers</h2><pre>"+escape(headers)+"</pre></body></html>";
}
