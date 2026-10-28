#include "../Host/SessionControlQueue.h"
#include "../ProcessSupport/RuntimeLayout.h"

#include <assert.h>
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

typedef struct {
    char container[PATH_MAX], base[PATH_MAX], data[PATH_MAX], queue[PATH_MAX];
    int root, directory;
} Fixture;

static void path(char *result, const char *parent, const char *name) {
    int size = snprintf(result, PATH_MAX, "%s/%s", parent, name);
    assert(size > 0 && size < PATH_MAX);
}

static void removeDirectory(int parent, const char *name) {
    int descriptor = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(descriptor >= 0);
    DIR *directory = fdopendir(descriptor);
    assert(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat metadata;
        assert(!fstatat(descriptor, entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW));
        if (S_ISDIR(metadata.st_mode)) removeDirectory(descriptor, entry->d_name);
        else assert(!unlinkat(descriptor, entry->d_name, 0));
    }
    assert(!closedir(directory));
    assert(!unlinkat(parent, name, AT_REMOVEDIR));
}

static Fixture fixture(bool queue) {
    Fixture value = {.root = -1, .directory = -1};
    char temporary[] = "/tmp/lsb-control-queue.XXXXXX";
    assert(mkdtemp(temporary));
    assert(realpath(temporary, value.container));
    path(value.base, value.container, LSB_STORAGE_BUILD_RELATIVE_PATH);
    int parent = open(value.container, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(parent >= 0);
    const char *components[] = {LSB_STORAGE_DIRECTORY, LSB_RUNTIME_PLATFORM, LSB_RUNTIME_BUILD};
    for (unsigned index = 0; index < sizeof(components) / sizeof(*components); ++index) {
        assert(!mkdirat(parent, components[index], 0700));
        int next = openat(parent, components[index], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        assert(next >= 0);
        close(parent);
        parent = next;
    }
    close(parent);
    path(value.data, value.base, LSB_DATA_DIRECTORY);
    path(value.queue, value.data, "var/run/launchctl-control");
    value.root = open(value.base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(value.root >= 0);
    if (queue) {
        assert(!mkdirat(value.root, LSB_DATA_DIRECTORY, 0700));
        int data = openat(value.root, LSB_DATA_DIRECTORY, O_RDONLY | O_DIRECTORY);
        assert(data >= 0 && !mkdirat(data, "var", 0700));
        int var = openat(data, "var", O_RDONLY | O_DIRECTORY);
        assert(var >= 0 && !mkdirat(var, "run", 0700));
        int run = openat(var, "run", O_RDONLY | O_DIRECTORY);
        assert(run >= 0 && !mkdirat(run, "launchctl-control", 0700));
        value.directory = openat(run, "launchctl-control", O_RDONLY | O_DIRECTORY);
        assert(value.directory >= 0);
        close(run); close(var); close(data);
    }
    return value;
}

static void finish(Fixture *value) {
    if (value->directory >= 0) close(value->directory);
    close(value->root);
    removeDirectory(AT_FDCWD, value->container);
}

static void writeFile(int directory, const char *name, const void *bytes, size_t size) {
    int descriptor = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    assert(descriptor >= 0 && write(descriptor, bytes, size) == (ssize_t)size);
    assert(!close(descriptor));
}

static void assertBytes(int directory, const char *name, const void *bytes, size_t size) {
    char result[8192];
    assert(size < sizeof(result));
    int descriptor = openat(directory, name, O_RDONLY | O_NOFOLLOW);
    assert(descriptor >= 0);
    assert(read(descriptor, result, sizeof(result)) == (ssize_t)size && !memcmp(result, bytes, size));
    assert(!close(descriptor));
}

static bool exists(int directory, const char *name) {
    struct stat metadata;
    if (!fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW)) return true;
    assert(errno == ENOENT);
    return false;
}

static unsigned archives(int queue, char result[PATH_MAX]) {
    int descriptor = openat(queue, ".", O_RDONLY | O_DIRECTORY);
    assert(descriptor >= 0);
    DIR *directory = fdopendir(descriptor);
    assert(directory);
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) if (!strncmp(entry->d_name, "reboot-interrupted.", 19)) {
        ++count;
        if (result) strcpy(result, entry->d_name);
    }
    assert(!closedir(directory));
    return count;
}

static int quarantine(const Fixture *value) {
    errno = EDOM;
    int error = LSBQuarantineSessionControlQueue(value->data);
    assert(errno == EDOM);
    return error;
}

static void makeClientLock(int directory) {
    assert(!mkdirat(directory, "client.lock", 0700));
    int lock = openat(directory, "client.lock", O_RDONLY | O_DIRECTORY);
    assert(lock >= 0);
    const char owner[] = "{\"id\":\"0123456789abcdef0123456789abcdef\",\"arguments\":[\"reboot\",\"userspace\"]}";
    writeFile(lock, "owner.json", owner, sizeof(owner) - 1);
    close(lock);
}

static void testMissingAndValidation(void) {
    Fixture value = fixture(false);
    assert(!quarantine(&value));
    assert(!exists(value.root, LSB_DATA_DIRECTORY));
    assert(!mkdirat(value.root, LSB_DATA_DIRECTORY, 0700));
    assert(!quarantine(&value));
    assert(!exists(value.root, LSB_DATA_DIRECTORY "/var"));
    assert(!mkdirat(value.root, LSB_DATA_DIRECTORY "/var", 0700));
    assert(!quarantine(&value));
    assert(!exists(value.root, LSB_DATA_DIRECTORY "/var/run"));
    assert(!mkdirat(value.root, LSB_DATA_DIRECTORY "/var/run", 0700));
    assert(!quarantine(&value));
    assert(!exists(value.root, LSB_DATA_DIRECTORY "/var/run/launchctl-control"));
    const char *invalid[] = {NULL, "", "/", LSB_DATA_DIRECTORY, "/tmp/" LSB_DATA_DIRECTORY "/", "/tmp/" LSB_DATA_DIRECTORY "-other", value.base};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        errno = ENOSPC;
        assert(LSBQuarantineSessionControlQueue(invalid[i]) == EINVAL && errno == ENOSPC);
    }
    char alias[PATH_MAX];
    path(alias, value.base, "alias");
    assert(!symlink(value.base, alias));
    path(alias, value.base, "alias/" LSB_DATA_DIRECTORY);
    assert(LSBQuarantineSessionControlQueue(alias) == EINVAL);
    finish(&value);
}

static void testEvidenceAndIdempotence(void) {
    Fixture value = fixture(true);
    assert(!quarantine(&value) && !archives(value.directory, NULL));
    struct stat lockBefore, lockAfter, activeBefore, activeAfter;
    assert(!fstatat(value.directory, "worker.lock", &lockBefore, 0));
    const char active[] = "0123456789abcdef0123456789abcdef\0reboot\0userspace\0";
    const char request[] = "malformed\0preserve bytes\0";
    writeFile(value.directory, "active", active, sizeof(active));
    writeFile(value.directory, "request", request, sizeof(request));
    writeFile(value.directory, "old.json", "receipt", 7);
    writeFile(value.directory, "old.log", "output", 6);
    makeClientLock(value.directory);
    assert(!fstatat(value.directory, "active", &activeBefore, 0));
    assert(!quarantine(&value));
    assert(!exists(value.directory, "active") && !exists(value.directory, "request") && !exists(value.directory, "client.lock"));
    char name[PATH_MAX];
    assert(archives(value.directory, name) == 1);
    int archive = openat(value.directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(archive >= 0);
    assertBytes(archive, "active", active, sizeof(active));
    assertBytes(archive, "request", request, sizeof(request));
    assert(exists(archive, "client.lock/owner.json"));
    assert(!fstatat(archive, "active", &activeAfter, 0));
    assert(activeBefore.st_ino == activeAfter.st_ino && activeBefore.st_mode == activeAfter.st_mode);
    assert(!fstatat(value.directory, "worker.lock", &lockAfter, 0) && lockBefore.st_ino == lockAfter.st_ino);
    assertBytes(value.directory, "old.json", "receipt", 7);
    assertBytes(value.directory, "old.log", "output", 6);
    close(archive);
    assert(!quarantine(&value) && archives(value.directory, NULL) == 1);
    makeClientLock(value.directory); // A later interrupted session receives a distinct archive, even with no published request yet.
    assert(!quarantine(&value) && archives(value.directory, NULL) == 2);
    finish(&value);
}

static void testBusyWorker(void) {
    Fixture value = fixture(true);
    int lock = openat(value.directory, "worker.lock", O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(lock >= 0 && !flock(lock, LOCK_EX | LOCK_NB));
    writeFile(value.directory, "active", "running", 7);
    int error = quarantine(&value);
    assert(error == EWOULDBLOCK || error == EAGAIN);
    assertBytes(value.directory, "active", "running", 7);
    assert(!archives(value.directory, NULL));
    assert(!close(lock));
    assert(!quarantine(&value));
    finish(&value);
}

static void testUnsafeEntries(void) {
    enum { ActiveSymlink, RequestHardlink, ActiveDirectory, ActiveFIFO, ClientSymlink, OwnerSymlink, OwnerHardlink,
           OwnerOversize, UnknownClientEntry, WritableClient, WorkerSymlink, WorkerHardlink, WorkerFIFO, Count };
    for (unsigned mode = 0; mode < Count; ++mode) {
        Fixture value = fixture(true);
        writeFile(value.directory, "sentinel", "untouched", 9);
        if (mode == RequestHardlink || (mode >= ClientSymlink && mode <= WritableClient))
            writeFile(value.directory, "active", "preserve before invalid later entry", 35);
        int client = -1;
        if (mode >= OwnerSymlink && mode <= WritableClient) {
            assert(!mkdirat(value.directory, "client.lock", 0700));
            client = openat(value.directory, "client.lock", O_RDONLY | O_DIRECTORY);
            assert(client >= 0);
        }
        switch (mode) {
            case ActiveSymlink: assert(!symlinkat("sentinel", value.directory, "active")); break;
            case RequestHardlink: assert(!linkat(value.directory, "sentinel", value.directory, "request", 0)); break;
            case ActiveDirectory: assert(!mkdirat(value.directory, "active", 0700)); break;
            case ActiveFIFO: assert(!mkfifoat(value.directory, "active", 0600)); break;
            case ClientSymlink: assert(!symlinkat(".", value.directory, "client.lock")); break;
            case OwnerSymlink: assert(!symlinkat("../sentinel", client, "owner.json")); break;
            case OwnerHardlink: assert(!linkat(value.directory, "sentinel", client, "owner.json", 0)); break;
            case OwnerOversize: { char bytes[4097] = {0}; writeFile(client, "owner.json", bytes, sizeof(bytes)); break; }
            case UnknownClientEntry: writeFile(client, "unexpected", "", 0); break;
            case WritableClient: assert(!fchmod(client, 0777)); break;
            case WorkerSymlink: assert(!symlinkat("sentinel", value.directory, "worker.lock")); break;
            case WorkerHardlink: assert(!linkat(value.directory, "sentinel", value.directory, "worker.lock", 0)); break;
            case WorkerFIFO: assert(!mkfifoat(value.directory, "worker.lock", 0600)); break;
            default: assert(false);
        }
        if (client >= 0) close(client);
        assert(quarantine(&value));
        assert(!archives(value.directory, NULL));
        assertBytes(value.directory, "sentinel", "untouched", 9);
        if (mode == RequestHardlink || (mode >= ClientSymlink && mode <= WritableClient))
            assertBytes(value.directory, "active", "preserve before invalid later entry", 35);
        finish(&value);
    }
}

static void testUnsafeParents(void) {
    const char *names[] = {"var", "var/run", "var/run/launchctl-control"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        Fixture value = fixture(true);
        int data = open(value.data, O_RDONLY | O_DIRECTORY);
        assert(data >= 0);
        char moved[PATH_MAX];
        assert(snprintf(moved, sizeof(moved), "%s.real", names[i]) < (int)sizeof(moved));
        assert(!renameat(data, names[i], data, moved));
        assert(!symlinkat("/tmp", data, names[i]));
        assert(quarantine(&value));
        assert(!archives(value.directory, NULL));
        close(data);
        finish(&value);
    }
    Fixture value = fixture(true);
    assert(!fchmod(value.directory, 0777));
    assert(quarantine(&value) == EACCES);
    finish(&value);
}

int main(void) {
    testMissingAndValidation();
    testEvidenceAndIdempotence();
    testBusyWorker();
    testUnsafeEntries();
    testUnsafeParents();
    puts("Session control queue tests passed");
    return 0;
}
