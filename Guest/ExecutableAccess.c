#include "ExecutableAccess.h"
#include "../ProcessSupport/GuestExecutablePath.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    const char *configuredRoot = getenv("DYLD_ROOT_PATH");
    char root[PATH_MAX];
    struct stat rootMetadata;
    if (!configuredRoot || !*configuredRoot || !realpath(configuredRoot, root) || !realpath(path, canonical) ||
        !LSBGuestExecutableIsContained(root, canonical))
        return false;
    return !stat(root, &rootMetadata) && S_ISDIR(rootMetadata.st_mode);
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
    int savedErrno = errno;
    // Calls from this image retain the native symbol, as with the existing signal and spawn interposers.
    int result = access(path, mode);
    int nativeErrno = errno;
    if (!result || nativeErrno != EPERM || !fallbackMode(mode) || !path) return result;
    // Keep native read/write and directory-traversal policy; manual loading additionally requires a readable file.
    char canonical[PATH_MAX];
    bool allowed = resolveRuntimePath(path, canonical) && !access(path, (mode & ~X_OK) | R_OK) && executableFile(canonical, false);
    errno = allowed ? savedErrno : nativeErrno;
    return allowed ? 0 : result;
}

int LSBFAccessAt(int directory, const char *path, int mode, int flags) {
    int savedErrno = errno;
    int result = faccessat(directory, path, mode, flags);
    int nativeErrno = errno;
    if (!result || nativeErrno != EPERM || !fallbackMode(mode) || (flags & ~AT_EACCESS) || !path ||
        (path[0] != '/' && directory != AT_FDCWD)) return result;
    char canonical[PATH_MAX];
    bool allowed = resolveRuntimePath(path, canonical) && !faccessat(directory, path, (mode & ~X_OK) | R_OK, flags) &&
                   executableFile(canonical, (flags & AT_EACCESS) != 0);
    errno = allowed ? savedErrno : nativeErrno;
    return allowed ? 0 : result;
}

#ifndef LSB_EXECUTABLE_ACCESS_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBAccess, (const void *)&access}, {(const void *)&LSBFAccessAt, (const void *)&faccessat}
};
#endif
