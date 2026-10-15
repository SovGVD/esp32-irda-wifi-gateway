#include "syncdiagnostics.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
    SyncDiagnosticState state={}; assert(!syncdiag_valid(state));
    syncdiag_begin(state); assert(syncdiag_valid(state) && state.active);
    syncdiag_service(state,"Drive restore");
    for (unsigned i=0;i<12;i++) syncdiag_event(state,"Google HTTPS connect",80000-i,32000,4096,i*1000);
    assert(syncdiag_valid(state) && state.count==8 && state.next==4);
    assert(state.events[state.next].uptime_ms==4000);
    syncdiag_allocation_failed(state,65535,0x804,"heap_caps_malloc");
    assert(syncdiag_valid(state) && state.allocation_size==65535);
    // An interrupted record remains readable after a warm reset/copy. Starting
    // another sync must not mutate the separately archived failure.
    auto previous=state;
    syncdiag_begin(state); assert(syncdiag_valid(state) && !state.allocation_size && !state.count);
    assert(syncdiag_valid(previous) && previous.active && previous.count==8 && !strcmp(previous.service,"Drive restore"));
    assert(previous.events[3].free_heap==79989 && !strcmp(previous.allocation_function,"heap_caps_malloc"));
    syncdiag_finish(state,"Not enough memory to create HotSync task",true);
    assert(syncdiag_valid(state) && !state.active && state.failed);
    auto damaged=previous; damaged.events[0].largest_block^=1; assert(!syncdiag_valid(damaged));
    damaged=previous; damaged.next=99; assert(!syncdiag_valid(damaged));
    damaged=previous; damaged.magic++; assert(!syncdiag_valid(damaged));
    damaged=previous; damaged.checksum=0; assert(!syncdiag_valid(damaged));
    // Bounded strings, including from the allocator callback, are terminated.
    char long_text[512]; memset(long_text,'a',sizeof(long_text)); long_text[511]=0;
    syncdiag_service(state,long_text); syncdiag_event(state,long_text,1,2,3,4);
    syncdiag_allocation_failed(state,123,456,long_text); syncdiag_finish(state,long_text,false);
    assert(syncdiag_valid(state) && !state.active && !state.failed);
    assert(strlen(state.service)==23 && strlen(state.events[0].stage)==47 &&
           strlen(state.outcome)==95 && strlen(state.allocation_function)==31);
    puts("syncdiagnostics: all passed");
}
