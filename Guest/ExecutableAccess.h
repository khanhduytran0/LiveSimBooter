#ifndef LSB_EXECUTABLE_ACCESS_H
#define LSB_EXECUTABLE_ACCESS_H

#include "../ProcessSupport/GuestExecutablePath.h"
#include <dirent.h>

// Replace a literal guest /var/jb or /private/var/jb path with a canonical path under this simulator's data/var/jb.
// Unrelated inputs pass through. On a translation error return errno without forwarding the original path.
int LSBResolveBootstrapAccess(const char *guest, const char **path, char resolved[PATH_MAX], bool nofollow);
static inline int LSBTranslateBootstrapAccessPath(const char **path, char resolved[PATH_MAX], bool nofollow) {
    const char *guest = LSBGuestVirtualBootstrapPath(*path);
    return guest ? LSBResolveBootstrapAccess(guest, path, resolved, nofollow) : 0;
}

#ifdef LSB_EXECUTABLE_ACCESS_TESTING
void LSBResetExecutableAccessForTesting(void);
#endif

// Preserve native access checks, with a constrained executable preflight after sandbox EPERM in guest executable locations.
// faccessat fallback supports absolute paths and AT_FDCWD-relative paths with flags 0 or AT_EACCESS only.
// This checks readable executable metadata, not code signatures; the broker and loader validate the eventual launch.
int LSBAccess(const char *path, int mode);
int LSBFAccessAt(int directory, const char *path, int mode, int flags);
int LSBDpkgConfigScandir(const char *path, struct dirent ***entries, int (*filter)(const struct dirent *),
                         int (*compare)(const struct dirent **, const struct dirent **));
FILE *LSBDpkgConfigFopen(const char *path, const char *mode);

#endif
