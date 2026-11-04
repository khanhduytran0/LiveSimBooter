#ifndef LSB_GUEST_RUNTIME_DIRECTORIES_H
#define LSB_GUEST_RUNTIME_DIRECTORIES_H

#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    char runtime[PATH_MAX];
    char bootstrap[PATH_MAX]; // Sibling data/var/jb. The directory can be installed after launch.
    size_t bootstrapLength;
} LSBGuestRuntimeDirectories;

// The loader supplies DYLD_ROOT_PATH before guest initialization. All readers share a single canonical snapshot.
int LSBGetRuntimeDirectories(const LSBGuestRuntimeDirectories **result);

#define LSB_SYSTEM_VERSION_PATH "/System/Library/CoreServices/SystemVersion.plist"
static inline bool LSBIsSystemVersionPath(const char *path) {
    return path && path[0] == '/' && path[1] == 'S' && !strcmp(path, LSB_SYSTEM_VERSION_PATH);
}
static inline bool LSBIsSystemVersionFopen(const char *path, const char *mode) {
    return LSBIsSystemVersionPath(path) && mode && mode[0] == 'r' && mode[1 + strspn(mode + 1, "be")] == 0;
}
// Only called for exact, read-only matches. Other callers and pre-constructor reads retain the original filesystem view.
int LSBProjectBootstrapSystemVersion(const char **path, char translated[PATH_MAX]);

#endif
