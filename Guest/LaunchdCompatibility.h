#ifndef LSB_LAUNCHD_COMPATIBILITY_H
#define LSB_LAUNCHD_COMPATIBILITY_H

#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

#define LSB_SIMULATOR_LAUNCHD_ENV "LSB_SIMULATOR_LAUNCHD"
#define LSB_SIMULATOR_BOOT_UUID_ENV "LSB_SIMULATOR_BOOT_UUID"

// The host sets the exact value "1" only for its explicit /sbin/launchd_sim plan and excludes it from the child environment.
// This is a compatibility opt-in, not an authentication/privilege boundary. No general sysctl or filesystem policy is replaced.
// A host-generated boot UUID is shared by all guests in one simulator boot. Its constructor snapshot services only
// read-only kern.bootsessionuuid queries; absent/invalid values and pre-constructor queries retain native behavior.
int LSBLaunchdSysctlByName(const char *name, void *oldValue, size_t *oldSize, void *newValue, size_t newSize);

// The fixed simulator launchd state subtree is redirected into SIMULATOR_SHARED_RESOURCES_DIRECTORY/tmp/launchd.
// These wrappers do not virtualize sockets: the legacy syslogsock bind keeps its native, nonfatal failure behavior.
int LSBLaunchdMkdir(const char *path, mode_t mode);
int LSBLaunchdOpen(const char *path, int flags, ...);
FILE *LSBLaunchdFopen(const char *path, const char *mode);
int LSBLaunchdRename(const char *from, const char *to, unsigned flags);
int LSBLaunchdUnlink(const char *path);
// Fish uses stat to verify that a /var/jb executable found by access(X_OK) is a regular file.
int LSBLaunchdStat(const char *path, struct stat *status);

#endif
