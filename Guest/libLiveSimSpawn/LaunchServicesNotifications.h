#ifndef LSB_LAUNCH_SERVICES_NOTIFICATIONS_H
#define LSB_LAUNCH_SERVICES_NOTIFICATIONS_H

#include <mach/mach.h>

// Only the pinned guest CoreServices self preflight for its installation-observer port uses guest namespace reachability.
// All other sandbox queries retain the native API's complete calling convention, policy and errno.
int LSBLaunchServicesSandboxCheck(audit_token_t token, const char *operation, int type, ...);

#endif
