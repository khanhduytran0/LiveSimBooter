#include "ExecutablePath.h"
#include "../ProcessSupport/GuestExecutablePath.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int LSBResolveGuestExecutable(const char *runtimeRoot, const char *path, char output[PATH_MAX]) {
    if (!runtimeRoot || runtimeRoot[0] != '/' || !path || !path[0] || !output) return EINVAL;
    if (strnlen(runtimeRoot, PATH_MAX) == PATH_MAX || strnlen(path, PATH_MAX) == PATH_MAX) return ENAMETOOLONG;
    char root[PATH_MAX], resolved[PATH_MAX], candidate[PATH_MAX];
    if (!realpath(runtimeRoot, root)) return errno;
    if (!root[1]) return EINVAL;
    struct stat metadata;
    if (stat(root, &metadata)) return errno;
    if (!S_ISDIR(metadata.st_mode)) return ENOTDIR;
    if (path[0] == '/') {
        int syntaxError = LSBGuestExecutableInputError(root, path);
        if (syntaxError) return syntaxError;
        // LaunchServices can return /var/... for a /private/var/... RuntimeRoot. Resolve physical aliases before adding the guest root.
        bool physical = realpath(path, resolved) != NULL;
        int physicalError = errno;
        if (physical && LSBGuestExecutableIsContained(root, resolved)) {
            if (stat(resolved, &metadata)) return errno;
            if (!S_ISREG(metadata.st_mode)) return EINVAL;
            memcpy(output, resolved, strlen(resolved) + 1);
            return 0;
        }
        // Never reinterpret an explicit physical path that is missing or escapes through a symlink as a guest absolute path.
        if (LSBGuestPathUsesPhysicalStorage(root, path) || (physical && !strcmp(resolved, root)))
            return physical ? EACCES : physicalError;
        int length = snprintf(candidate, sizeof(candidate), "%s%s", root, path);
        if (length < 0 || (size_t)length >= sizeof(candidate)) return ENAMETOOLONG;
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return errno;
        int length = snprintf(candidate, sizeof(candidate), "%s/%s", cwd, path);
        if (length < 0 || (size_t)length >= sizeof(candidate)) return ENAMETOOLONG;
    }
    if (!realpath(candidate, resolved)) return errno;
    if (!LSBGuestExecutableIsContained(root, resolved)) return EACCES;
    if (stat(resolved, &metadata)) return errno;
    if (!S_ISREG(metadata.st_mode)) return EINVAL;
    memcpy(output, resolved, strlen(resolved) + 1);
    return 0;
}
