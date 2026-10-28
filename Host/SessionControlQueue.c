#include "SessionControlQueue.h"
#include "../ProcessSupport/RuntimeLayout.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uuid/uuid.h>

typedef struct {
    const char *name;
    bool directory, present;
    struct stat metadata;
} LSBQueuedControlEntry;

static bool sameFile(const struct stat *first, const struct stat *second) {
    return first->st_dev == second->st_dev && first->st_ino == second->st_ino && first->st_mode == second->st_mode &&
        first->st_uid == second->st_uid && first->st_nlink == second->st_nlink && first->st_size == second->st_size &&
        first->st_mtimespec.tv_sec == second->st_mtimespec.tv_sec && first->st_mtimespec.tv_nsec == second->st_mtimespec.tv_nsec;
}

static int regularOwnedFile(const struct stat *metadata) {
    if (S_ISLNK(metadata->st_mode)) return ELOOP;
    if (!S_ISREG(metadata->st_mode) || metadata->st_nlink != 1) return EINVAL;
    // The optional external SSH client can submit as mobile or root. Renaming within our owned queue never changes its file contents.
    return metadata->st_uid == geteuid() || metadata->st_uid == 0 ? 0 : EACCES;
}

static int openOwnedDirectory(int parent, const char *name) {
    int descriptor = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return -1;
    struct stat metadata;
    int error = fstat(descriptor, &metadata) ? errno : 0;
    if (!error && (!S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid() || (metadata.st_mode & 0022))) error = EACCES;
    if (error) { close(descriptor); errno = error; return -1; }
    return descriptor;
}

static int validateClientLock(int queue, const struct stat *expected) {
    int descriptor = openat(queue, "client.lock", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return errno;
    struct stat metadata;
    int error = fstat(descriptor, &metadata) ? errno : 0;
    if (!error && !sameFile(&metadata, expected)) error = ESTALE;
    if (!error && (metadata.st_mode & 0022)) error = EACCES;
    if (!error && metadata.st_uid != geteuid() && metadata.st_uid != 0) error = EACCES;
    DIR *directory = !error ? fdopendir(descriptor) : NULL;
    if (!directory) { if (!error) error = errno; close(descriptor); return error; }
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) error = errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (strcmp(entry->d_name, "owner.json")) { error = EINVAL; break; }
        if (fstatat(descriptor, entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW)) { error = errno; break; }
        error = regularOwnedFile(&metadata);
        if (!error && metadata.st_size > 4096) error = EFBIG;
        if (error) break;
    }
    if (closedir(directory) && !error) error = errno;
    return error;
}

static int inspectEntry(int queue, LSBQueuedControlEntry *entry) {
    if (fstatat(queue, entry->name, &entry->metadata, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? 0 : errno;
    entry->present = true;
    if (!entry->directory) return regularOwnedFile(&entry->metadata);
    if (S_ISLNK(entry->metadata.st_mode)) return ELOOP;
    if (!S_ISDIR(entry->metadata.st_mode)) return EINVAL;
    return validateClientLock(queue, &entry->metadata);
}

static int quarantineQueue(int queue) {
    int lock = openat(queue, "worker.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (lock < 0) return errno;
    struct stat metadata;
    int error = fstat(lock, &metadata) ? errno : 0;
    if (!error) error = regularOwnedFile(&metadata);
    if (!error && flock(lock, LOCK_EX | LOCK_NB)) error = errno;
    LSBQueuedControlEntry entries[] = {{.name = "active"}, {.name = "request"}, {.name = "client.lock", .directory = true}};
    bool present = false;
    for (unsigned i = 0; !error && i < sizeof(entries) / sizeof(*entries); ++i) {
        error = inspectEntry(queue, &entries[i]);
        present |= entries[i].present;
    }
    int archive = -1;
    char archiveName[80] = {0};
    if (!error && present) {
        uuid_t identifier;
        char formatted[37];
        uuid_generate_random(identifier);
        uuid_unparse_lower(identifier, formatted);
        snprintf(archiveName, sizeof(archiveName), "reboot-interrupted.%s", formatted);
        if (mkdirat(queue, archiveName, 0700)) error = errno;
        if (!error) { archive = openOwnedDirectory(queue, archiveName); if (archive < 0) error = errno; }
    }
    for (unsigned i = 0; !error && archive >= 0 && i < sizeof(entries) / sizeof(*entries); ++i) {
        const LSBQueuedControlEntry *entry = &entries[i];
        if (!entry->present) continue;
        // Keep the external client's lock in place until its active/queued requests have moved, so a new submission cannot start midway.
        if (fstatat(queue, entry->name, &metadata, AT_SYMLINK_NOFOLLOW)) { error = errno; break; }
        if (!sameFile(&metadata, &entry->metadata)) { error = ESTALE; break; }
        if (renameatx_np(queue, entry->name, archive, entry->name, RENAME_EXCL)) { error = errno; break; }
        if (fstatat(archive, entry->name, &metadata, AT_SYMLINK_NOFOLLOW)) { error = errno; break; }
        if (!sameFile(&metadata, &entry->metadata)) { error = ESTALE; break; }
        if (entry->directory) error = validateClientLock(archive, &metadata);
    }
    if (!error) for (unsigned i = 0; i < sizeof(entries) / sizeof(*entries); ++i) {
        if (!fstatat(queue, entries[i].name, &metadata, AT_SYMLINK_NOFOLLOW)) { error = EBUSY; break; }
        if (errno != ENOENT) { error = errno; break; }
    }
    if (!error && archive >= 0) fprintf(stderr, "[broker] Preserved interrupted launchctl queue in %s (not replayed)\n", archiveName);
    if (archive >= 0) close(archive);
    close(lock); // Release the existing worker lock without removing its inode.
    return error;
}

int LSBQuarantineSessionControlQueue(const char *data) {
    int saved = errno, error = 0;
    char canonical[PATH_MAX];
    size_t size = data ? strnlen(data, PATH_MAX) : 0;
    static const char suffix[] = "/" LSB_DATA_DIRECTORY;
    if (!data || !size || size >= PATH_MAX || data[0] != '/' || size < sizeof(suffix) - 1 ||
        strcmp(data + size - (sizeof(suffix) - 1), suffix)) error = EINVAL;
    if (!error && !realpath(data, canonical)) error = errno == ENOENT ? 0 : errno;
    else if (!error && strcmp(data, canonical)) error = EINVAL;
    else if (!error) {
        int directory = openOwnedDirectory(AT_FDCWD, canonical);
        if (directory < 0) error = errno;
        const char *components[] = {"var", "run", "launchctl-control"};
        for (unsigned i = 0; !error && directory >= 0 && i < 3; ++i) {
            int next = openOwnedDirectory(directory, components[i]);
            int openedError = next < 0 ? errno : 0;
            close(directory);
            directory = next;
            if (openedError) { error = openedError == ENOENT ? 0 : openedError; break; }
        }
        if (!error && directory >= 0) error = quarantineQueue(directory);
        if (directory >= 0) close(directory);
    }
    errno = saved;
    return error;
}
