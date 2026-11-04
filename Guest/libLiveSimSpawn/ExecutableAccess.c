#include "ExecutableAccess.h"
#include "RuntimeDirectories.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool insideBootstrap(const LSBGuestRuntimeDirectories *directories, const char *path) {
    const char *base = LSBGuestStorageRootSpelling(directories->bootstrap, path);
    size_t length = directories->bootstrapLength - (size_t)(base - directories->bootstrap);
    return length && !strncmp(path, base, length) && (!path[length] || path[length] == '/');
}

int LSBResolveBootstrapAccess(const char *guest, const char **path, char resolved[PATH_MAX], bool nofollow) {
    const LSBGuestRuntimeDirectories *directories;
    int error = LSBGetRuntimeDirectories(&directories);
    if (error) return error;
    if (!LSBGuestPathIsCanonical(*path)) return EINVAL;
    if (!directories->bootstrapLength) return EACCES;

    const char *suffix = guest + sizeof("/var/jb") - 1;
    // For AT_SYMLINK_NOFOLLOW only resolve the parent; the native call must still see the final symlink.
    const char *leaf = nofollow ? strrchr(suffix, '/') : NULL;
    size_t length = leaf ? (size_t)(leaf - suffix) : strlen(suffix);
    char candidate[PATH_MAX];
    if (length >= sizeof(candidate) - directories->bootstrapLength) return ENAMETOOLONG;
    memcpy(candidate, directories->bootstrap, directories->bootstrapLength);
    memcpy(candidate + directories->bootstrapLength, suffix, length);
    candidate[directories->bootstrapLength + length] = 0;

    int saved = errno;
    if (!realpath(candidate, resolved)) return errno;
    if (!insideBootstrap(directories, resolved) || strchr(resolved, ':')) return EACCES;
    if (leaf && strlcat(resolved, leaf, PATH_MAX) >= PATH_MAX) return ENAMETOOLONG;
    *path = resolved;
    errno = saved;
    return 0;
}

static bool executableMode(const struct stat *metadata, bool effective) {
    uid_t user = effective ? geteuid() : getuid();
    gid_t group = effective ? getegid() : getgid();
    if (!user) return (metadata->st_mode & 0111) != 0;
    if (user == metadata->st_uid) return (metadata->st_mode & S_IXUSR) != 0;
    if (group == metadata->st_gid) return (metadata->st_mode & S_IXGRP) != 0;
    gid_t groups[NGROUPS_MAX];
    int count = getgroups(NGROUPS_MAX, groups);
    if (count < 0 || count > NGROUPS_MAX) return false;
    for (int index = 0; index < count; ++index) {
        if (groups[index] == metadata->st_gid) return (metadata->st_mode & S_IXGRP) != 0;
    }
    return (metadata->st_mode & S_IXOTH) != 0;
}

static bool resolveRuntimePath(const char *path, char canonical[PATH_MAX]) {
    const LSBGuestRuntimeDirectories *directories;
    if (LSBGetRuntimeDirectories(&directories) || !realpath(path, canonical)) return false;
    bool bootstrap = insideBootstrap(directories, canonical);
    if (path[0] == '/' && bootstrap != insideBootstrap(directories, path)) return false;
    if (bootstrap || LSBGuestPathInsideRoot(canonical, directories->runtime)) return !strchr(canonical, ':');
    return LSBGuestInstalledApplicationPath(directories->runtime, canonical) != NULL;
}

static bool executableFile(const char *canonical, bool effective) {
    // O_NONBLOCK avoids waiting on special files before fstat rejects them; the final component must remain nonsymlink.
    int descriptor = open(canonical, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return false;
    struct stat metadata;
    struct mach_header_64 header;
    bool allowed = !fstat(descriptor, &metadata) && S_ISREG(metadata.st_mode) && executableMode(&metadata, effective);
    if (allowed) {
        ssize_t bytes;
        do { bytes = pread(descriptor, &header, sizeof(header), 0); } while (bytes < 0 && errno == EINTR);
        allowed = bytes == (ssize_t)sizeof(header) && header.magic == MH_MAGIC_64 && header.cputype == CPU_TYPE_ARM64 &&
                  header.cpusubtype == CPU_SUBTYPE_ARM64_ALL && header.filetype == MH_EXECUTE && (header.flags & MH_PIE) &&
                  header.ncmds && header.ncmds <= header.sizeofcmds / sizeof(struct load_command) &&
                  metadata.st_size >= (off_t)sizeof(header) && header.sizeofcmds <= (uint64_t)metadata.st_size - sizeof(header);
    }
    close(descriptor);
    return allowed;
}

static bool fallbackMode(int mode) { return (mode & X_OK) && !(mode & ~(R_OK | W_OK | X_OK)); }

int LSBAccess(const char *path, int mode) {
    char canonical[PATH_MAX];
    int error = LSBTranslateBootstrapAccessPath(&path, canonical, false);
    if (error) { errno = error; return -1; }
    // Calls from this image retain the native symbol, as with the existing signal and spawn interposers.
    if (!fallbackMode(mode) || !path) return access(path, mode);
    int savedErrno = errno;
    int result = access(path, mode);
    int nativeErrno = errno;
    if (!result || nativeErrno != EPERM) return result;
    // Keep native read/write and directory-traversal policy; manual loading additionally requires a readable file.
    bool allowed = (path == canonical || resolveRuntimePath(path, canonical)) && !access(path, (mode & ~X_OK) | R_OK) && executableFile(canonical, false);
    errno = allowed ? savedErrno : nativeErrno;
    return allowed ? 0 : result;
}

int LSBFAccessAt(int directory, const char *path, int mode, int flags) {
    char canonical[PATH_MAX];
    int error = LSBTranslateBootstrapAccessPath(&path, canonical, flags & AT_SYMLINK_NOFOLLOW);
    if (error) { errno = error; return -1; }
    if (!fallbackMode(mode) || (flags & ~AT_EACCESS) || !path || (path[0] != '/' && directory != AT_FDCWD)) return faccessat(directory, path, mode, flags);
    int savedErrno = errno;
    int result = faccessat(directory, path, mode, flags);
    int nativeErrno = errno;
    if (!result || nativeErrno != EPERM) return result;
    bool allowed = (path == canonical || resolveRuntimePath(path, canonical)) && !faccessat(directory, path, (mode & ~X_OK) | R_OK, flags) &&
                   executableFile(canonical, (flags & AT_EACCESS) != 0);
    errno = allowed ? savedErrno : nativeErrno;
    return allowed ? 0 : result;
}

// Procursus dpkg 1.22.6 scans CONFIGDIR before it parses --root, then opens the selected fragments using Darwin's unlimited-streams ABI.
extern FILE *darwinFopen(const char *path, const char *mode) __asm("_fopen$DARWIN_EXTSN");

static int translateDpkgConfig(const char **path, char resolved[PATH_MAX]) {
    const char *guest = LSBGuestVirtualBootstrapPath(*path);
    static const char prefix[] = "/var/jb/etc/dpkg/";
    if (!guest || strncmp(guest, prefix, sizeof(prefix) - 1)) return 0;
    return LSBResolveBootstrapAccess(guest, path, resolved, false);
}

int LSBDpkgConfigScandir(const char *path, struct dirent ***entries, int (*filter)(const struct dirent *),
                         int (*compare)(const struct dirent **, const struct dirent **)) {
    char resolved[PATH_MAX];
    int error = translateDpkgConfig(&path, resolved);
    if (error) { errno = error; return -1; }
    return scandir(path, entries, filter, compare);
}

FILE *LSBDpkgConfigFopen(const char *path, const char *mode) {
    char resolved[PATH_MAX];
    if (mode && mode[0] == 'r' && !mode[1]) {
        int error = translateDpkgConfig(&path, resolved);
        if (error) { errno = error; return NULL; }
    }
    return darwinFopen(path, mode);
}

#ifndef LSB_EXECUTABLE_ACCESS_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBAccess, (const void *)&access}, {(const void *)&LSBFAccessAt, (const void *)&faccessat},
    {(const void *)&LSBDpkgConfigScandir, (const void *)&scandir}, {(const void *)&LSBDpkgConfigFopen, (const void *)&darwinFopen}
};
#endif
