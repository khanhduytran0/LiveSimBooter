#ifndef LSB_GUEST_EXECUTABLE_PATH_H
#define LSB_GUEST_EXECUTABLE_PATH_H

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// String-only classification of already canonical paths. Callers resolving untrusted files must use realpath and verify the opened FD separately.
static inline bool LSBGuestPathIsCanonical(const char *path) {
    if (!path || path[0] != '/' || !path[1] || strnlen(path, PATH_MAX) == PATH_MAX || strchr(path, ':')) return false;
    for (const char *part = path + 1; *part;) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (!length || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (end && !end[1]) return false;
        part += length + (end != NULL);
    }
    return true;
}

static inline bool LSBGuestPathInsideRoot(const char *path, const char *root) {
    size_t length = strlen(root);
    return length > 1 && !strncmp(path, root, length) && path[length] == '/';
}

static inline int LSBGuestExecutableInputError(const char *root, const char *path) {
    if (!path || path[0] != '/') return EINVAL;
    const char *boundary = LSBGuestPathInsideRoot(path, root) ? path + strlen(root) : NULL;
    if (!boundary && !strncmp(root, "/private/var/", 13) && LSBGuestPathInsideRoot(path, root + 8)) boundary = path + strlen(root + 8);
    for (const char *part = path + 1; *part;) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        // Simulator launchd joins RuntimeRoot + "/" with the absolute xpcproxy path. Only that duplicated boundary is accepted.
        if (!length && boundary && part == boundary + 1 && part[1] && part[1] != '/') { ++part; continue; }
        if (!length || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.')) return EINVAL;
        if (end && !end[1]) return EINVAL;
        part += length + (end != NULL);
    }
    return 0;
}

// RuntimeRoot and data are siblings. Only installed bundle containers are executable; data homes, staging, and other runtimes are excluded.
// Return the top-level App.app component; nested appex/XPC/framework executables remain inside that same application boundary.
static inline const char *LSBGuestInstalledApplicationPath(const char *root, const char *path) {
    if (!LSBGuestPathIsCanonical(root) || !LSBGuestPathIsCanonical(path)) return NULL;
    const char *parentEnd = strrchr(root, '/');
    size_t parentLength = (size_t)(parentEnd - root);
    static const char location[] = "/data/Containers/Bundle/Application/";
    size_t locationLength = sizeof(location) - 1;
    if (strncmp(path, root, parentLength) || strncmp(path + parentLength, location, locationLength)) return NULL;
    const char *container = path + parentLength + locationLength, *separator = strchr(container, '/');
    if (!separator || separator == container) return NULL;
    const char *app = separator + 1;
    separator = strchr(app, '/');
    if (!separator || separator - app <= 4 || memcmp(separator - 4, ".app", 4) || !separator[1]) return NULL;
    return app;
}

static inline bool LSBGuestExecutableIsContained(const char *root, const char *path) {
    return LSBGuestPathIsCanonical(root) && LSBGuestPathIsCanonical(path) &&
        (LSBGuestPathInsideRoot(path, root) || LSBGuestInstalledApplicationPath(root, path));
}

// Recognize explicit physical inputs even if they are missing or their symlink resolves outside the allowed roots.
// Such inputs must never fall back to a different executable by prepending RuntimeRoot.
static inline bool LSBGuestPathUsesPhysicalStorage(const char *root, const char *path) {
    if (LSBGuestPathInsideRoot(path, root) || !strcmp(path, root)) return true;
    const char *parentEnd = strrchr(root, '/');
    size_t parentLength = (size_t)(parentEnd - root);
    if (!strncmp(path, root, parentLength) && !strncmp(path + parentLength, "/data", 5) &&
        (!path[parentLength + 5] || path[parentLength + 5] == '/')) return true;
    if (!strncmp(root, "/private/var/", 13)) return LSBGuestPathUsesPhysicalStorage(root + 8, path);
    return false;
}

// Broker identities retain virtual system paths for exact role checks. Installed app identities carry their physical path unchanged.
static inline int LSBGuestExecutablePhysicalPath(const char *root, const char *identity, char *output, size_t capacity) {
    if (!output || !LSBGuestPathIsCanonical(root) || !LSBGuestPathIsCanonical(identity)) return EINVAL;
    bool installed = LSBGuestInstalledApplicationPath(root, identity) != NULL;
    if (!installed && LSBGuestPathUsesPhysicalStorage(root, identity)) return EACCES;
    size_t rootLength = installed ? 0 : strlen(root), identityLength = strlen(identity);
    if (rootLength + identityLength >= capacity) return ENAMETOOLONG;
    if (rootLength) memcpy(output, root, rootLength);
    memcpy(output + rootLength, identity, identityLength + 1);
    return 0;
}

#endif
