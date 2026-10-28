#include "LaunchdCompatibility.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static char simulatorBootUUID[37];
static _Atomic(bool) simulatorBootUUIDReady;
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "Early sysctl forwarding must not acquire a runtime lock");

#ifndef LSB_LAUNCHD_COMPATIBILITY_TESTING
__attribute__((constructor))
#endif
static void initializeSimulatorBootUUID(void) {
    // libSystem is ready before this constructor; readers never inspect environ during its earlier bootstrap.
    const char *value = getenv(LSB_SIMULATOR_BOOT_UUID_ENV);
    if (!value || strnlen(value, sizeof(simulatorBootUUID)) != sizeof(simulatorBootUUID) - 1) return;
    for (size_t i = 0; i < sizeof(simulatorBootUUID) - 1; ++i) {
        char character = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (character != '-') return;
        } else if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
                     (character >= 'A' && character <= 'F'))) return;
    }
    memcpy(simulatorBootUUID, value, sizeof(simulatorBootUUID));
    atomic_store_explicit(&simulatorBootUUIDReady, true, memory_order_release);
}

static int readSimulatorBootUUID(void *oldValue, size_t *oldSize) {
    int savedErrno = errno;
    // Match Darwin's string sysctl: no partial output on ENOMEM; oldSize reports bytes actually copied (zero).
    if (oldValue && (!oldSize || *oldSize < sizeof(simulatorBootUUID))) {
        if (oldSize) *oldSize = 0;
        errno = ENOMEM;
        return -1;
    }
    if (oldValue) memcpy(oldValue, simulatorBootUUID, sizeof(simulatorBootUUID));
    if (oldSize) *oldSize = sizeof(simulatorBootUUID);
    errno = savedErrno;
    return 0;
}

int LSBLaunchdSysctlByName(const char *name, void *oldValue, size_t *oldSize, void *newValue, size_t newSize) {
    // 23A343 PosterBoard's pbf_bootInstanceIdentifier keeps a 36-NUL string after native EPERM, producing a nil
    // transient-info URL. Supply the simulator's own boot identity, never an entitlement grant or a write override.
    if (name && !strcmp(name, "kern.bootsessionuuid") && !newValue && !newSize &&
        atomic_load_explicit(&simulatorBootUUIDReady, memory_order_acquire)) return readSimulatorBootUUID(oldValue, oldSize);
    // libSystem itself queries sysctls before environ is initialized. Only launchd's later bootargs read may inspect its marker.
    if (!name || strcmp(name, "kern.bootargs") || newValue || newSize)
        return sysctlbyname(name, oldValue, oldSize, newValue, newSize);
    int savedErrno = errno;
    const char *marker = getenv(LSB_SIMULATOR_LAUNCHD_ENV);
    if (!marker || strcmp(marker, "1"))
        return sysctlbyname(name, oldValue, oldSize, newValue, newSize);
    // launchd needs a bootargs string before reading its bootstrap plist; this isolated simulator has no configured boot arguments.
    // Darwin's sysctl_old_user reports zero bytes copied on an undersized output, rather than the required query length.
    if (oldValue && (!oldSize || !*oldSize)) {
        if (oldSize) *oldSize = 0;
        errno = ENOMEM;
        return -1;
    }
    if (oldValue) *(char *)oldValue = '\0';
    if (oldSize) *oldSize = 1;
    errno = savedErrno;
    return 0;
}

static bool dotComponent(const char *path) {
    for (const char *component = path; *component;) {
        while (*component == '/') ++component;
        const char *end = component;
        while (*end && *end != '/') ++end;
        size_t length = (size_t)(end - component);
        if ((length == 1 && component[0] == '.') || (length == 2 && component[0] == '.' && component[1] == '.')) return true;
        component = end;
    }
    return false;
}

static int launchdStatePath(const char *path, char translated[PATH_MAX], const char **result) {
    static const char prefix[] = "/private/var/tmp/com.kdt.LiveSimBooter.launchd";
    static const char replacement[] = "/tmp/launchd";
    *result = path;
    // As with sysctlbyname, early libc calls cannot safely inspect environ. Match the complete fixed component first.
    if (!path || strncmp(path, prefix, sizeof(prefix) - 1)) return 0;
    const char *suffix = path + sizeof(prefix) - 1;
    if (*suffix && *suffix != '/') return 0;
    const char *marker = getenv(LSB_SIMULATOR_LAUNCHD_ENV);
    if (!marker || strcmp(marker, "1")) return 0;
    const char *root = getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY");
    if (!root || root[0] != '/') return EINVAL;
    size_t rootLength = strnlen(root, PATH_MAX), suffixLength = strnlen(suffix, PATH_MAX);
    if (rootLength == PATH_MAX || suffixLength == PATH_MAX) return ENAMETOOLONG;
    if (dotComponent(root) || dotComponent(suffix)) return EINVAL;
    while (rootLength > 1 && root[rootLength - 1] == '/') --rootLength;
    // The native host supplies the canonical build's data directory. Reject an accidentally missing container path.
    if (rootLength == 1) return EINVAL;
    size_t replacementLength = sizeof(replacement) - 1;
    if (rootLength + replacementLength + suffixLength >= PATH_MAX) return ENAMETOOLONG;
    memcpy(translated, root, rootLength);
    memcpy(translated + rootLength, replacement, replacementLength);
    memcpy(translated + rootLength + replacementLength, suffix, suffixLength + 1);
    *result = translated;
    return 0;
}

int LSBLaunchdMkdir(const char *path, mode_t mode) {
    char translated[PATH_MAX];
    int error = launchdStatePath(path, translated, &path);
    if (error) { errno = error; return -1; }
    return mkdir(path, mode);
}

int LSBLaunchdOpen(const char *path, int flags, ...) {
    char translated[PATH_MAX];
    int error = launchdStatePath(path, translated, &path);
    if (error) { errno = error; return -1; }
    if (!(flags & O_CREAT)) return open(path, flags);
    va_list arguments;
    va_start(arguments, flags);
    mode_t mode = (mode_t)va_arg(arguments, int);
    va_end(arguments);
    return open(path, flags, mode);
}

FILE *LSBLaunchdFopen(const char *path, const char *mode) {
    char translated[PATH_MAX];
    int error = launchdStatePath(path, translated, &path);
    if (error) { errno = error; return NULL; }
    return fopen(path, mode);
}

int LSBLaunchdRename(const char *from, const char *to, unsigned flags) {
    char translatedFrom[PATH_MAX], translatedTo[PATH_MAX];
    int error = launchdStatePath(from, translatedFrom, &from);
    if (!error) error = launchdStatePath(to, translatedTo, &to);
    if (error) { errno = error; return -1; }
    return renamex_np(from, to, flags);
}

int LSBLaunchdUnlink(const char *path) {
    char translated[PATH_MAX];
    int error = launchdStatePath(path, translated, &path);
    if (error) { errno = error; return -1; }
    return unlink(path);
}

int LSBLaunchdStat(const char *path, struct stat *status) {
    char translated[PATH_MAX];
    int error = launchdStatePath(path, translated, &path);
    if (error) { errno = error; return -1; }
    return stat(path, status);
}

#ifndef LSB_LAUNCHD_COMPATIBILITY_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBLaunchdSysctlByName, (const void *)&sysctlbyname},
    {(const void *)&LSBLaunchdMkdir, (const void *)&mkdir}, {(const void *)&LSBLaunchdOpen, (const void *)&open},
    {(const void *)&LSBLaunchdFopen, (const void *)&fopen}, {(const void *)&LSBLaunchdRename, (const void *)&renamex_np},
    {(const void *)&LSBLaunchdUnlink, (const void *)&unlink}, {(const void *)&LSBLaunchdStat, (const void *)&stat}
};
#endif
