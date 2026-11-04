#include "NotificationSharedMemory.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char objectFile[] = "notification-center.shm";
#define WORKSPACE_OBJECT_FILE_CAPACITY 64

static bool workspaceObjectFile(const char *name, char file[WORKSPACE_OBJECT_FILE_CAPACITY]) {
    // iOS 26 FrontBoard's __pathForIdentifierAndBuffer uses /FBW1: and /FBW2: with identifierForName's first 25 characters.
    if (!name || (strncmp(name, "/FBW1:", 6) && strncmp(name, "/FBW2:", 6))) return false;
    const char *identifier = name + 6;
    size_t length = strnlen(identifier, 26);
    if (!length || length > 25) return false;
    for (size_t index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)identifier[index];
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-')) return false;
    }
    snprintf(file, WORKSPACE_OBJECT_FILE_CAPACITY, "frontboard-workspace-%c-%s.shm", name[4], identifier);
    return true;
}

static const char *virtualObject(const char *name, char file[WORKSPACE_OBJECT_FILE_CAPACITY]) {
    // Check the name before getenv: unrelated early libSystem initialization must not consult environment state.
    if (!name) return NULL;
    const char *selected;
    if (!strcmp(name, "apple.shm.notification_center") || !strcmp(name, "/apple.shm.notification_center")) selected = objectFile;
    // 23A343 CoreFoundation _initializeShmemPage:/getShmemName: use snprintf(size=31), truncating the user name's version digit.
    // __CFXPreferencesDaemon_main enables O_CREAT|O_RDWR; clients open O_RDONLY. The real daemon bypasses the local-daemon sandbox preflight.
    else if (!strcmp(name, "apple.sim.cfprefs.user.daemonv")) selected = "cfpreferences-user-v1.shm";
    else if (workspaceObjectFile(name, file)) selected = file;
    else return NULL;
    const char *enabled = getenv(LSB_NOTIFICATION_SHM_ENABLE_ENV);
    return enabled && !strcmp(enabled, "1") ? selected : NULL;
}

static int ownedDirectory(int descriptor) {
    struct stat status;
    if (fstat(descriptor, &status)) return errno;
    if (!S_ISDIR(status.st_mode)) return ENOTDIR;
    return status.st_uid == geteuid() ? 0 : EACCES;
}

static int openObjectDirectory(void) {
    const char *root = getenv(LSB_NOTIFICATION_SHM_ROOT_ENV);
    char canonical[PATH_MAX];
    if (!root || root[0] != '/' || !root[1]) { errno = EINVAL; return -1; }
    if (!realpath(root, canonical)) return -1;
    if (strcmp(root, canonical)) { errno = EINVAL; return -1; }
    int directory = open(canonical, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return -1;
    int error = ownedDirectory(directory), temporary = -1;
    if (!error) {
        temporary = openat(directory, "tmp", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        error = temporary < 0 ? errno : ownedDirectory(temporary);
    }
    close(directory);
    if (error) { if (temporary >= 0) close(temporary); errno = error; return -1; }
    return temporary;
}

static int ownedObject(const struct stat *status) {
    if (!S_ISREG(status->st_mode)) return S_ISLNK(status->st_mode) ? ELOOP : EINVAL;
    // An object unlinked concurrently with open can have zero links while its descriptor/mappings remain valid.
    return status->st_uid == geteuid() && status->st_nlink <= 1 ? 0 : EACCES;
}

int LSBNotificationShmOpen(const char *name, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    char file[WORKSPACE_OBJECT_FILE_CAPACITY];
    const char *selected = virtualObject(name, file);
    if (!selected) return flags & O_CREAT ? shm_open(name, flags, mode) : shm_open(name, flags);
    int access = flags & O_ACCMODE;
    if ((access != O_RDONLY && access != O_RDWR) || (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC))) {
        errno = EINVAL; return -1;
    }
    if ((flags & O_TRUNC) && access != O_RDWR) { errno = EINVAL; return -1; }
    int directory = openObjectDirectory();
    if (directory < 0) return -1;
    // Defer truncation until after validation, and use nonblocking open so a substituted FIFO cannot hang startup.
    int descriptor = openat(directory, selected, (flags & ~O_TRUNC) | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, mode);
    int error = descriptor < 0 ? errno : 0;
    close(directory);
    if (!error) {
        struct stat status;
        error = fstat(descriptor, &status) ? errno : ownedObject(&status);
        if (!error && (flags & O_TRUNC) && ftruncate(descriptor, 0)) error = errno;
        if (!error) {
            int current = fcntl(descriptor, F_GETFL);
            if (current < 0 || fcntl(descriptor, F_SETFL, current & ~O_NONBLOCK)) error = errno;
        }
    }
    if (error) { if (descriptor >= 0) close(descriptor); errno = error; return -1; }
    return descriptor;
}

int LSBNotificationShmUnlink(const char *name) {
    char file[WORKSPACE_OBJECT_FILE_CAPACITY];
    const char *selected = virtualObject(name, file);
    if (!selected) return shm_unlink(name);
    int directory = openObjectDirectory();
    if (directory < 0) return -1;
    struct stat status;
    int error = fstatat(directory, selected, &status, AT_SYMLINK_NOFOLLOW) ? errno : ownedObject(&status);
    if (!error && unlinkat(directory, selected, 0)) error = errno;
    close(directory);
    if (error) { errno = error; return -1; }
    return 0;
}

bool LSBNotificationShmAllowsSandbox(const char *name, const char *operation) {
    int savedError = errno;
    bool allowed = false;
    char file[WORKSPACE_OBJECT_FILE_CAPACITY];
    if (!operation || !workspaceObjectFile(name, file) || !virtualObject(name, file)) goto done;
    bool read = !strcmp(operation, "ipc-posix-shm-read-data");
    bool create = !strcmp(operation, "ipc-posix-shm-write-create");
    if (!read && !create && strcmp(operation, "ipc-posix-shm-write-unlink")) goto done;
    int directory = openObjectDirectory();
    if (directory < 0) goto done;
    // This predicts access to our backing files, not access to the native POSIX SHM namespace. An absent object is valid at preflight.
    if (!faccessat(directory, ".", X_OK | (read ? 0 : W_OK), AT_EACCESS)) {
        struct stat status;
        if (fstatat(directory, file, &status, AT_SYMLINK_NOFOLLOW)) allowed = errno == ENOENT;
        else if (!ownedObject(&status)) {
            allowed = !read && !create;
            if (!allowed) allowed = !faccessat(directory, file, read ? R_OK : W_OK, AT_EACCESS | AT_SYMLINK_NOFOLLOW);
        }
    }
    close(directory);
done:
    errno = savedError;
    return allowed;
}

#ifndef LSB_NOTIFICATION_SHM_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBNotificationShmOpen, (const void *)&shm_open}, {(const void *)&LSBNotificationShmUnlink, (const void *)&shm_unlink}
};
#endif
