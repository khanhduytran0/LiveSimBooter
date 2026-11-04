#ifndef LSB_GUEST_RUNTIME_DIRECTORIES_H
#define LSB_GUEST_RUNTIME_DIRECTORIES_H

#include <limits.h>
#include <stddef.h>

typedef struct {
    char runtime[PATH_MAX];
    char bootstrap[PATH_MAX]; // Sibling data/var/jb. The directory can be installed after launch.
    size_t bootstrapLength;
} LSBGuestRuntimeDirectories;

// The loader supplies DYLD_ROOT_PATH before guest initialization. All readers share a single canonical snapshot.
int LSBGetRuntimeDirectories(const LSBGuestRuntimeDirectories **result);

#endif
