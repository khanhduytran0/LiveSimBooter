#include "SharedCachePathCompatibility.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static _Atomic(bool) sharedCachePathReady;
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "Early dlopen forwarding must not require runtime initialization");

#ifndef LSB_SHARED_CACHE_PATH_TESTING
__attribute__((constructor))
#endif
static void initializeSharedCachePath(void) {
    atomic_store_explicit(&sharedCachePathReady, true, memory_order_release);
}

static bool unambiguousPath(const char *path) {
    if (!path || path[0] != '/' || !path[1] || strnlen(path, PATH_MAX) == PATH_MAX || strchr(path, ':')) return false;
    for (const char *component = path + 1; *component;) {
        const char *end = strchr(component, '/');
        size_t length = end ? (size_t)(end - component) : strlen(component);
        if (!length || (length == 1 && component[0] == '.') || (length == 2 && component[0] == '.' && component[1] == '.')) return false;
        if (end && !end[1]) return false;
        component += length + (end != NULL);
    }
    return true;
}

static bool canonicalOrVarAlias(const char *path, const char *canonical) {
    return !strcmp(path, canonical) || (!strncmp(canonical, "/private/var/", 13) && !strcmp(path, canonical + 8));
}

static const char *relativeToRoot(const char *path, const char *root) {
    size_t length = strlen(root);
    return !strncmp(path, root, length) && path[length] == '/' && path[length + 1] ? path + length : NULL;
}

static bool absentWithoutSymlinkParents(char path[PATH_MAX], size_t rootLength) {
    struct stat metadata;
    // A loose override, dangling final symlink, permission failure or non-directory component keeps native dlopen behavior.
    if (!lstat(path, &metadata) || errno != ENOENT) return false;
    for (char *separator = strchr(path + rootLength + 1, '/'); separator; separator = strchr(separator + 1, '/')) {
        *separator = '\0';
        int result = lstat(path, &metadata), error = errno;
        *separator = '/';
        if (result) return error == ENOENT;
        if (!S_ISDIR(metadata.st_mode)) return false;
    }
    return true;
}

static const char *sharedCachePath(const char *path) {
    if (!unambiguousPath(path)) return NULL;
    const char *configured = getenv("DYLD_ROOT_PATH");
    if (!unambiguousPath(configured)) return NULL;
    char root[PATH_MAX];
    struct stat metadata;
    if (!realpath(configured, root) || !canonicalOrVarAlias(configured, root) ||
        strcmp(strrchr(root, '/'), "/RuntimeRoot") || stat(root, &metadata) || !S_ISDIR(metadata.st_mode)) return NULL;
    const char *logical = relativeToRoot(path, root);
    if (!logical && !strncmp(root, "/private/var/", 13)) logical = relativeToRoot(path, root + 8);
    bool physicalInput = logical != NULL;
    if (!logical) logical = path;
    // In 23A343 this stock symlink targets Network.framework. Our canonical-image cache omits the alias, but Security dlopens it.
    const char *aliasTarget = !strcmp(logical, "/usr/lib/libnetwork.dylib") ? "/System/Library/Frameworks/Network.framework/Network" : NULL;
    if (!physicalInput && !aliasTarget) return NULL;
    size_t rootLength = strlen(root), logicalLength = strlen(logical);
    if (logicalLength >= PATH_MAX - rootLength) return NULL;
    char physical[PATH_MAX];
    memcpy(physical, root, rootLength);
    memcpy(physical + rootLength, logical, logicalLength + 1);
    if (!absentWithoutSymlinkParents(physical, rootLength)) return NULL;
    if (_dyld_shared_cache_contains_path(logical)) return logical;
    if (!aliasTarget || !_dyld_shared_cache_contains_path(aliasTarget)) return NULL;
    size_t targetLength = strlen(aliasTarget);
    if (targetLength >= PATH_MAX - rootLength) return NULL;
    memcpy(physical + rootLength, aliasTarget, targetLength + 1);
    if (!absentWithoutSymlinkParents(physical, rootLength)) return NULL;
    return aliasTarget; // Returned paths point into the caller's string or a literal, never temporary storage used by the tail call.
}

void *LSBSharedCacheDlopen(const char *path, int mode) {
    if (atomic_load_explicit(&sharedCachePathReady, memory_order_acquire)) {
        int savedErrno = errno;
        const char *logical = sharedCachePath(path);
        if (logical) path = logical;
        errno = savedErrno;
    }
    // CUTWeakLinkSymbol supplies physical RuntimeRoot paths even for cache-only images. Preserve dlopen's caller image too:
    // dyld uses its return address for @loader_path, @rpath and caller-sensitive modes, including all unrelated passthrough calls.
    __attribute__((musttail)) return dlopen(path, mode);
}

#ifndef LSB_SHARED_CACHE_PATH_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interposition = {
    (const void *)&LSBSharedCacheDlopen, (const void *)&dlopen
};
#endif
