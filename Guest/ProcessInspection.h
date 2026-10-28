#ifndef LSB_GUEST_PROCESS_INSPECTION_H
#define LSB_GUEST_PROCESS_INSPECTION_H
#include <stdbool.h>
#include <stdint.h>
// The shared proc_pidpath wrapper calls this before its existing RBS policy. Authorized records return a physical RuntimeRoot path;
// the broker's stored identity stays virtual. False means forward normally, without touching TLS or output before a snapshot exists.
bool LSBInspectionPIDPath(int pid, void *buffer, uint32_t size, int *result);
// After an authorized snapshot, unsupported known peers return ENOTSUP and unknown logical PIDs return ESRCH.
// Inactive inspection or true self returns zero; the caller must still translate guest self before invoking a kernel API.
int LSBInspectionUnsupportedPID(int pid);
#endif
