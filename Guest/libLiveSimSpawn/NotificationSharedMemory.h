#ifndef LSB_NOTIFICATION_SHARED_MEMORY_H
#define LSB_NOTIFICATION_SHARED_MEMORY_H

#include <sys/types.h>
#include <stdbool.h>

#define LSB_NOTIFICATION_SHM_ENABLE_ENV "LSB_SIMULATE_NOTIFICATION_SHM"
#define LSB_NOTIFICATION_SHM_ROOT_ENV "SIMULATOR_SHARED_RESOURCES_DIRECTORY"

// Simulator notification object, exact 23A343 user cfprefs object, and /FBW1:/FBW2: names with 1–25 safe ASCII identifier characters.
// All unrelated names retain native POSIX shared-memory behavior.
// Enabled opens use an owned regular file in the canonical data directory's existing tmp subdirectory, with FD_CLOEXEC.
// Mode is required only with O_CREAT, matching the native variadic declaration. Errors use -1/errno.
int LSBNotificationShmOpen(const char *name, int flags, ...);
int LSBNotificationShmUnlink(const char *name);

// True only for managed FrontBoard workspace objects and the three POSIX SHM access operations backed by this adapter.
// The sandbox wrapper must separately validate its caller PID/filter ABI. This check preserves errno and does not create an object.
bool LSBNotificationShmAllowsSandbox(const char *name, const char *operation);

#endif
