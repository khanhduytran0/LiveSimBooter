#include "ProcessDisplayName.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

int LSBSetProcessDisplayName(const char *path, pid_t guestPID, char *nativeArgv0, size_t argv0Capacity) {
    int savedErrno = errno;
    if (!path || !*path || guestPID < 0 || (argv0Capacity && !nativeArgv0)) return EINVAL;
    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    if (!*leaf) return EINVAL;
    char name[33], prefix[32] = {0};
    size_t prefixLength = guestPID ? (size_t)snprintf(prefix, sizeof(prefix), "LSB:%d ", guestPID) :
        (size_t)snprintf(prefix, sizeof(prefix), "LSBHelper: ");
    if (argv0Capacity && argv0Capacity < prefixLength + 2) { errno = savedErrno; return ENOSPC; }
    size_t leafLength = strlen(leaf), nameLength = sizeof(name) - prefixLength - 1;
    if (nameLength > leafLength) nameLength = leafLength;
    // Build the independent kernel name first: path can point into the native argv buffer rewritten below.
    memcpy(name, prefix, prefixLength);
    memcpy(name + prefixLength, leaf, nameLength);
    nameLength += prefixLength;
    name[nameLength] = 0;
    // Only touch the original argv[0] allocation. Adjacent native arguments, env and apple vectors remain intact.
    // The native argument window must also survive the simulator cache mapping for KERN_PROCARGS2/ps to retain this label.
    if (argv0Capacity) {
        size_t copied = argv0Capacity - prefixLength - 1;
        if (copied > leafLength) copied = leafLength;
        // Move the basename first: source may overlap the prefix's destination within argv[0].
        memmove(nativeArgv0 + prefixLength, leaf, copied);
        memcpy(nativeArgv0, prefix, prefixLength);
        copied += prefixLength;
        memset(nativeArgv0 + copied, 0, argv0Capacity - copied);
    }
    // XNU kern_sysctl.c sysctl_procname stores this in p_name, independently of the user stack and signed executable path.
    // sysctl_io_string supplies the terminator itself; including it would reject a full 32-byte name with EINVAL.
    int mib[] = {CTL_KERN, KERN_PROCNAME};
    int error = sysctl(mib, 2, NULL, NULL, name, nameLength) ? errno : 0;
    errno = savedErrno;
    return error;
}
