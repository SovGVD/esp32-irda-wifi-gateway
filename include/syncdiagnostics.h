#pragma once
#include <stddef.h>
#include <stdint.h>

// Fixed-size, allocation-free breadcrumbs. No credentials, URLs or payloads.
struct SyncDiagnosticEvent {
    char stage[48];
    uint32_t free_heap,largest_block,stack_free,uptime_ms;
};
struct SyncDiagnosticState {
    uint32_t magic,active,failed,next,count;
    char service[24];
    uint32_t allocation_size,allocation_caps;
    char allocation_function[32];
    char outcome[96];
    SyncDiagnosticEvent events[8];
    uint32_t checksum;
};
bool syncdiag_valid(const SyncDiagnosticState &state);
void syncdiag_begin(SyncDiagnosticState &state);
void syncdiag_service(SyncDiagnosticState &state,const char *service);
void syncdiag_event(SyncDiagnosticState &state,const char *stage,uint32_t free_heap,
                    uint32_t largest,uint32_t stack_free,uint32_t uptime_ms);
void syncdiag_allocation_failed(SyncDiagnosticState &state,size_t bytes,uint32_t caps,const char *function);
void syncdiag_finish(SyncDiagnosticState &state,const char *outcome,bool failed);
