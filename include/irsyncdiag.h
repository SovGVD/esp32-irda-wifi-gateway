#pragma once
#include "syncdiagnostics.h"

void irsyncdiag_init();
void irsyncdiag_begin();
void irsyncdiag_service(const char *service);
void irsyncdiag_checkpoint(const char *stage);
void irsyncdiag_finish(const char *outcome,bool failed);
bool irsyncdiag_snapshot(SyncDiagnosticState &state,bool previous);
// A conservative admission check, not a reservation against other tasks.
bool irsyncdiag_memory_available(size_t allocation,size_t reserve);
