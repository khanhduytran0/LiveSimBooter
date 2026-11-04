#ifndef LSB_GUEST_EXECUTABLE_PATH_H
#define LSB_GUEST_EXECUTABLE_PATH_H

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

// Path classification alone does not verify the opened executable. The broker also checks its descriptor and inode.
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

// RuntimeRoot and data are siblings. Installed app bundles and data/var/jb are separate executable locations.
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

// Exact guest prefix, including the directory itself. Never probe the native phone's /var/jb for an executable.
static inline const char *LSBGuestVirtualBootstrapPath(const char *path) {
    if (!path || path[0] != '/') return NULL;
    if (path[1] == 'p') {
        if (strncmp(path, "/private/var/jb", sizeof("/private/var/jb") - 1)) return NULL;
        path += sizeof("/private") - 1;
    } else if (path[1] != 'v' || strncmp(path, "/var/jb", sizeof("/var/jb") - 1)) {
        return NULL;
    }
    char boundary = path[sizeof("/var/jb") - 1];
    return !boundary || boundary == '/' ? path : NULL;
}

static inline const char *LSBGuestStorageRootSpelling(const char *root, const char *path) {
    // /var on iOS and /tmp in the macOS tests are aliases for their /private counterparts.
    if (!strncmp(root, "/private/", 9) && (!strncmp(path, "/var/", 5) || !strncmp(path, "/tmp/", 5))) return root + 8;
    return root;
}

static inline bool LSBGuestPathInBootstrap(const char *root, const char *path) {
    root = LSBGuestStorageRootSpelling(root, path);
    const char *leaf = strrchr(root, '/');
    if (!leaf || strcmp(leaf, "/RuntimeRoot")) return false;
    size_t parent = (size_t)(leaf - root);
    static const char suffix[] = "/data/var/jb";
    return !strncmp(path, root, parent) && !strncmp(path + parent, suffix, sizeof(suffix) - 1) &&
           (!path[parent + sizeof(suffix) - 1] || path[parent + sizeof(suffix) - 1] == '/');
}

// Procursus update-alternatives writes absolute /var/jb links inside the guest package tree.
// Run this walk only when ordinary realpath could not produce a path inside that same bootstrap.
static inline int LSBWalkBootstrapLinks(const char *root, const char *path, char resolved[PATH_MAX]) {
    const char *spelling = LSBGuestStorageRootSpelling(root, path);
    size_t baseLength = (size_t)(strrchr(spelling, '/') - spelling) + sizeof("/data/var/jb") - 1;
    char pending[PATH_MAX], target[PATH_MAX];
    if (baseLength >= sizeof(target) || strlen(path) < baseLength || strlcpy(pending, path + baseLength, sizeof(pending)) >= sizeof(pending)) return ENAMETOOLONG;
    memcpy(target, path, baseLength);
    target[baseLength] = 0;
    if (!realpath(target, resolved)) return errno;
    if (!LSBGuestPathInBootstrap(root, resolved)) return EACCES;
    baseLength = strlen(resolved);
    unsigned links = 0;
    for (char *part = pending; *part;) {
        if (*part == '/') { ++part; continue; }
        size_t length = strcspn(part, "/");
        char *rest = part + length;
        if (length == 1 && *part == '.') { part = rest; continue; }
        if (length == 2 && !memcmp(part, "..", 2)) {
            if (strlen(resolved) <= baseLength) return EACCES;
            *strrchr(resolved, '/') = 0;
            part = rest;
            continue;
        }
        size_t parent = strlen(resolved);
        if (length + 2 > PATH_MAX - parent) return ENAMETOOLONG;
        resolved[parent] = '/';
        memcpy(resolved + parent + 1, part, length);
        resolved[parent + length + 1] = 0;
        struct stat info;
        if (lstat(resolved, &info)) return errno;
        if (!S_ISLNK(info.st_mode)) {
            if (*rest && !S_ISDIR(info.st_mode)) return ENOTDIR;
            part = rest;
            continue;
        }
        if (++links > MAXSYMLINKS) return ELOOP;
        ssize_t n = readlink(resolved, target, sizeof(target));
        if (n < 0) return errno;
        if ((size_t)n == sizeof(target)) return ENAMETOOLONG;
        target[n] = 0;
        resolved[parent] = 0;
        const char *replacement = target;
        if (target[0] == '/') {
            const char *guest = LSBGuestVirtualBootstrapPath(target);
            resolved[baseLength] = 0;
            if (guest) replacement = guest + sizeof("/var/jb") - 1;
            else {
                const char *base = LSBGuestStorageRootSpelling(resolved, target);
                size_t bytes = strlen(base);
                if (strncmp(target, base, bytes) || (target[bytes] && target[bytes] != '/')) return EACCES;
                replacement = target + bytes;
            }
        }
        size_t prefix = strlen(replacement), tail = strlen(rest);
        if (prefix + tail >= sizeof(pending)) return ENAMETOOLONG;
        memmove(pending + prefix, rest, tail + 1);
        memcpy(pending, replacement, prefix);
        part = pending;
    }
    // Repeat the canonical containment check in case a directory was replaced during the walk.
    if (!realpath(resolved, target)) return errno;
    if (!LSBGuestPathInBootstrap(root, target) || !LSBGuestPathIsCanonical(target)) return EACCES;
    strcpy(resolved, target);
    return 0;
}

static inline int LSBRealpathBootstrap(const char *root, const char *path, char resolved[PATH_MAX]) {
    if (realpath(path, resolved) && LSBGuestPathInBootstrap(root, resolved)) return LSBGuestPathIsCanonical(resolved) ? 0 : EACCES;
    return LSBWalkBootstrapLinks(root, path, resolved);
}

static inline int LSBResolveBootstrapPath(const char *root, const char *path, char resolved[PATH_MAX], bool *handled) {
    const char *guest = LSBGuestVirtualBootstrapPath(path);
    const char *spelling = LSBGuestStorageRootSpelling(root, path);
    if (!guest && LSBGuestPathInsideRoot(path, spelling)) {
        const char *tail = path + strlen(spelling);
        guest = LSBGuestVirtualBootstrapPath(tail[1] == '/' ? tail + 1 : tail);
    }
    *handled = guest || LSBGuestPathInBootstrap(root, path);
    if (!*handled) return 0;
    char candidate[PATH_MAX];
    if (guest) {
        const char *leaf = strrchr(root, '/');
        if (!leaf || strcmp(leaf, "/RuntimeRoot")) return EACCES;
        int length = snprintf(candidate, sizeof(candidate), "%.*s/data%s", (int)(leaf - root), root, guest);
        if (length < 0 || (size_t)length >= sizeof(candidate)) return ENAMETOOLONG;
        path = candidate;
    }
    return LSBRealpathBootstrap(root, path, resolved);
}

static inline bool LSBGuestExecutableIsContained(const char *root, const char *path) {
    return LSBGuestPathIsCanonical(root) && LSBGuestPathIsCanonical(path) &&
        (LSBGuestPathInsideRoot(path, root) || LSBGuestPathInBootstrap(root, path) || LSBGuestInstalledApplicationPath(root, path));
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

// Both the broker and the new LiveProcess resolve absolute inputs here. Only an explicitly selected
// bootstrap path may enter its subtree, and symlinks within it may never select a system or installed-app identity.
static inline int LSBResolveAbsoluteExecutablePath(const char *root, const char *path, char resolved[PATH_MAX]) {
    int error = LSBGuestExecutableInputError(root, path);
    if (error) return error;
    bool bootstrap;
    error = LSBResolveBootstrapPath(root, path, resolved, &bootstrap);
    if (bootstrap) return error;
    bool physical = realpath(path, resolved) != NULL;
    int physicalError = errno;
    if (physical && LSBGuestExecutableIsContained(root, resolved))
        return LSBGuestPathInBootstrap(root, resolved) ? EACCES : 0;
    if (LSBGuestPathUsesPhysicalStorage(root, path) || (physical && !strcmp(resolved, root))) return physical ? EACCES : physicalError;
    char candidate[PATH_MAX];
    int length = snprintf(candidate, sizeof(candidate), "%s%s", root, path);
    if (length < 0 || (size_t)length >= sizeof(candidate)) return ENAMETOOLONG;
    if (!realpath(candidate, resolved)) return errno;
    return LSBGuestExecutableIsContained(root, resolved) && !LSBGuestPathInBootstrap(root, resolved) ? 0 : EACCES;
}

// System identities use virtual paths for exact role checks. Installed apps and bootstrap executables use their canonical physical path.
static inline int LSBGuestExecutablePhysicalPath(const char *root, const char *identity, char *output, size_t capacity) {
    if (!output || !LSBGuestPathIsCanonical(root) || !LSBGuestPathIsCanonical(identity)) return EINVAL;
    bool physical = LSBGuestPathInBootstrap(root, identity) || LSBGuestInstalledApplicationPath(root, identity);
    if (!physical && LSBGuestPathUsesPhysicalStorage(root, identity)) return EACCES;
    size_t rootLength = physical ? 0 : strlen(root), identityLength = strlen(identity);
    if (rootLength + identityLength >= capacity) return ENAMETOOLONG;
    if (rootLength) memcpy(output, root, rootLength);
    memcpy(output + rootLength, identity, identityLength + 1);
    return 0;
}

#endif
