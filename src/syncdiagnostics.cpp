#include "syncdiagnostics.h"
#include <cstring>
#include <algorithm>

static constexpr uint32_t magic=0x53444701;
static uint32_t checksum(const SyncDiagnosticState &s) {
    uint32_t h=2166136261u;
    const auto *p=reinterpret_cast<const uint8_t *>(&s);
    for (size_t n=0;n<offsetof(SyncDiagnosticState,checksum);n++) { h^=p[n]; h*=16777619u; }
    return h;
}
template<size_t N> static void copy(char (&dest)[N],const char *source) {
    size_t n=0;
    if (source) while (n+1<N && source[n]) { dest[n]=source[n]; ++n; }
    dest[n]=0;
}
bool syncdiag_valid(const SyncDiagnosticState &s) {
    return s.magic==magic && s.active<=1 && s.failed<=1 && s.next<8 && s.count<=8 && s.checksum==checksum(s);
}
void syncdiag_begin(SyncDiagnosticState &s) {
    memset(&s,0,sizeof(s)); s.magic=magic; s.active=1; s.checksum=checksum(s);
}
void syncdiag_service(SyncDiagnosticState &s,const char *service) {
    copy(s.service,service); s.checksum=checksum(s);
}
void syncdiag_event(SyncDiagnosticState &s,const char *stage,uint32_t heap,uint32_t largest,uint32_t stack,uint32_t ms) {
    auto &e=s.events[s.next]; memset(&e,0,sizeof(e)); copy(e.stage,stage);
    e.free_heap=heap; e.largest_block=largest; e.stack_free=stack; e.uptime_ms=ms;
    s.next=(s.next+1)%8; s.count=std::min<uint32_t>(s.count+1,8); s.checksum=checksum(s);
}
void syncdiag_allocation_failed(SyncDiagnosticState &s,size_t bytes,uint32_t caps,const char *function) {
    s.allocation_size=bytes; s.allocation_caps=caps; copy(s.allocation_function,function); s.checksum=checksum(s);
}
void syncdiag_finish(SyncDiagnosticState &s,const char *outcome,bool failed) {
    copy(s.outcome,outcome); s.failed=failed; s.active=0; s.checksum=checksum(s);
}
