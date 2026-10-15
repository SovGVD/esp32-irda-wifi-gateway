#include "hotsyncuser.h"

bool hotsync_user_validate(const HotSyncUserSetup &s,std::string &name,std::string &problem) {
    name.clear();
    if (s.name.size()>160 || s.name.find('\0')!=std::string::npos) { problem="Use a HotSync user name of at most 40 Palm characters"; return false; }
    for (unsigned char c:s.name) if (c<32 || c==127) { problem="HotSync user name cannot contain control characters"; return false; }
    name=palm_from_utf8(s.name,s.charset,41);
    if (name.size()>40 || palm_to_utf8(name,s.charset)!=s.name) { problem="HotSync user name must fit 40 characters in the selected Palm text encoding"; return false; }
    return true;
}
bool hotsync_user_prepare(const HotSyncUserSetup &s,HotSyncUserResult &r) {
    r=HotSyncUserResult(); std::string requested;
    if (!hotsync_user_validate(s,requested,r.problem)) return false;
    char name[128];
    if (!hotsync_read_user(&r.id,name,sizeof(name))) { r.problem="Could not read Palm HotSync user"; return false; }
    r.name=palm_to_utf8(name,s.charset);
    if (*name || requested.empty()) return true;
    if (!hotsync_write_user(r.id,requested.c_str())) { r.problem="Could not set Palm HotSync user; check HotSync settings in wifi.setup"; return false; }
    uint32_t confirmed;
    if (!hotsync_read_user(&confirmed,name,sizeof(name)) || confirmed!=r.id || requested!=name) { r.problem="Palm did not confirm the HotSync user name; sync stopped"; return false; }
    r.name=s.name; r.written=true; return true;
}
