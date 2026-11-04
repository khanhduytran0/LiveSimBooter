#ifndef LSB_LAUNCHD_JOB_IDENTITY_H
#define LSB_LAUNCHD_JOB_IDENTITY_H

#include <stdint.h>
#include <xpc/xpc.h>

// Pinned launchd_sim OSLaunchdJob boundaries only. Native audit tokens and launchd's stored logical PIDs are unchanged.
void LSBLaunchdJobSetInt64(xpc_object_t dictionary, const char *key, int64_t value);
int64_t LSBLaunchdJobGetInt64(xpc_object_t dictionary, const char *key);

#endif
