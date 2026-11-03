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
        int error = LSBResolveAbsoluteExecutablePath(root, path, resolved);
        if (error) return error;
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return errno;
        int length = snprintf(candidate, sizeof(candidate), "%s/%s", cwd, path);
        if (length < 0 || (size_t)length >= sizeof(candidate)) return ENAMETOOLONG;
        bool bootstrap;
        int error = LSBResolveBootstrapPath(root, candidate, resolved, &bootstrap);
        if (error) return error;
        if (!bootstrap) {
            if (!realpath(candidate, resolved)) return errno;
            if (!LSBGuestExecutableIsContained(root, resolved) || LSBGuestPathInBootstrap(root, resolved)) return EACCES;
        }
    }
    if (stat(resolved, &metadata)) return errno;
    if (!S_ISREG(metadata.st_mode)) return EINVAL;
    memcpy(output, resolved, strlen(resolved) + 1);
    return 0;
}
