#ifndef LSB_PROCESS_EVENTS_H
#define LSB_PROCESS_EVENTS_H

#include <dispatch/dispatch.h>
#include <stdint.h>

// Ledger-managed PROC sources are real DATA_OR sources polled every 10 ms. Normal source activation, suspension,
// handlers and data coalescing remain libdispatch's. C callers must balance dispatch_retain/release normally.
// Each source pins its ledger generation through EXIT observation, including an earlier waitpid reap or SA_NOCLDWAIT.
// Objective-C ownership bridging remains unsupported.
dispatch_source_t LSBDispatchSourceCreate(dispatch_source_type_t type, uintptr_t handle, unsigned long mask, dispatch_queue_t queue);
uintptr_t LSBDispatchSourceGetHandle(dispatch_source_t source);
unsigned long LSBDispatchSourceGetMask(dispatch_source_t source);
void LSBDispatchSourceCancel(dispatch_source_t source);
void LSBDispatchRetain(dispatch_object_t object);
void LSBDispatchRelease(dispatch_object_t object);
int LSBProcPIDInfo(int pid, int flavor, uint64_t arg, void *buffer, int bufferSize);

#endif
