#ifndef LSB_LAUNCHD_IDENTITY_H
#define LSB_LAUNCHD_IDENTITY_H

#include <mach/mach.h>
#include <sys/types.h>

// Called by the signed launchd adapter. The original audit token remains untouched for native authorization checks.
__attribute__((visibility("default"))) pid_t LSBLaunchdResolveAuditPID(const audit_token_t *audit);

#endif
