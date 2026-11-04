#include "RuntimeDirectories.h"
#include "../../ProcessSupport/GuestExecutablePath.h"

#include <stdatomic.h>
#include <sys/stat.h>

static LSBGuestRuntimeDirectories directories;
static int directoriesError;
static _Atomic(bool) directoriesReady;
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "Pre-constructor calls must not acquire a runtime lock");

static int configureRuntimeDirectories(const char *root) {
    if (!root || !*root) return ENOENT;
    if (root[0] != '/') return EINVAL;
    if (!realpath(root, directories.runtime)) return errno;
    if (!LSBGuestPathIsCanonical(directories.runtime)) return EINVAL;
    struct stat metadata;
    if (stat(directories.runtime, &metadata)) return errno;
    if (!S_ISDIR(metadata.st_mode)) return ENOTDIR;

    const char *leaf = strrchr(directories.runtime, '/');
    static const char suffix[] = "/data/var/jb";
    if (!strcmp(leaf, "/RuntimeRoot")) {
        size_t parent = (size_t)(leaf - directories.runtime);
        _Static_assert(sizeof(suffix) <= sizeof("/RuntimeRoot"), "The sibling path must fit in the runtime path buffer");
        memcpy(directories.bootstrap, directories.runtime, parent);
        memcpy(directories.bootstrap + parent, suffix, sizeof(suffix));
        directories.bootstrapLength = parent + sizeof(suffix) - 1;
    }
    return 0;
}

__attribute__((constructor)) static void initializeRuntimeDirectories(void) {
    int saved = errno;
    directoriesError = configureRuntimeDirectories(getenv("DYLD_ROOT_PATH"));
    errno = saved;
    atomic_store_explicit(&directoriesReady, true, memory_order_release);
}

int LSBGetRuntimeDirectories(const LSBGuestRuntimeDirectories **result) {
    if (!atomic_load_explicit(&directoriesReady, memory_order_acquire)) return EACCES;
    if (directoriesError) return directoriesError;
    *result = &directories;
    return 0;
}

#ifdef LSB_EXECUTABLE_ACCESS_TESTING
void LSBResetExecutableAccessForTesting(void) {
    directories = (LSBGuestRuntimeDirectories){0};
    initializeRuntimeDirectories();
}
#endif
