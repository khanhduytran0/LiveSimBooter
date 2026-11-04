#include "../Guest/libLiveSimSpawn/NotificationSharedMemory.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LSB_NOTIFICATION_SHM_INTERPOSE_TESTING
static const char *expectedName;
static int expectedFlags;
static mode_t expectedMode;
static unsigned forwards;
static int nativeOpen(const char *name, int flags, ...) {
    assert(name == expectedName && flags == expectedFlags);
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        assert((mode_t)va_arg(arguments, int) == expectedMode);
        va_end(arguments);
    }
    ++forwards;
    errno = ENOSYS;
    return -1;
}
static int nativeUnlink(const char *name) { assert(name == expectedName); ++forwards; errno = ENOSYS; return -1; }
#define shm_open nativeOpen
#define shm_unlink nativeUnlink
#define LSB_NOTIFICATION_SHM_TESTING
#include "../Guest/libLiveSimSpawn/NotificationSharedMemory.c"
#undef shm_open
#undef shm_unlink
#define objectOpen LSBNotificationShmOpen
#define objectUnlink LSBNotificationShmUnlink

static void testForwarding(void) {
    const char *names[] = {NULL, "", "unrelated", "apple.shm.notification_center.extra", "//apple.shm.notification_center",
                          "/apple.shm.notification_center/nested", "APPLE.shm.notification_center", "/FBW1:", "/FBW2:",
                          "FBW1:com.apple.frontboard", "//FBW1:com.apple.frontboard", "/FBW0:com.apple.frontboard",
                          "/FBW3:com.apple.frontboard", "/FBW1:com.apple.frontboard.systX", "/FBW1:../other",
                          "/FBW2:name/nested", "/FBW1:name:other", "/FBW1:name with space", "/FBW1:\xc3\xa9",
                          "/apple.sim.cfprefs.user.daemonv", "apple.sim.cfprefs.user.daemonv1", "apple.sim.cfprefs.user.daemon",
                          "apple.sim.cfprefs.user.daemonv.extra", "apple.sim.cfprefs.system.daemo", "apple.sim.cfprefs.system.daemonv1",
                          "test.sim.cfprefs.user.daemonv1", "apple.cfprefs.user.daemonv1"};
    assert(!setenv(LSB_NOTIFICATION_SHM_ENABLE_ENV, "1", 1));
    initializeNotificationShm();
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        expectedName = names[i]; expectedFlags = O_CREAT | O_RDWR | O_EXCL; expectedMode = 0621;
        unsigned before = forwards;
        assert(objectOpen(expectedName, expectedFlags, expectedMode) == -1 && errno == ENOSYS);
        assert(objectUnlink(expectedName) == -1 && errno == ENOSYS && forwards == before + 2);
    }
    const char *disabledNames[] = {"apple.shm.notification_center", "/FBW1:com.apple.frontboard.syst", "/FBW2:other-domain",
                                   "apple.sim.cfprefs.user.daemonv"};
    expectedFlags = O_RDONLY;
    for (unsigned index = 0; index < sizeof(disabledNames) / sizeof(*disabledNames); ++index) {
        expectedName = disabledNames[index];
        for (unsigned i = 0; i < 4; ++i) {
            if (!i) assert(!unsetenv(LSB_NOTIFICATION_SHM_ENABLE_ENV));
            else assert(!setenv(LSB_NOTIFICATION_SHM_ENABLE_ENV, i == 1 ? "" : i == 2 ? "0" : "true", 1));
            initializeNotificationShm();
            unsigned before = forwards;
            assert(objectOpen(expectedName, expectedFlags) == -1 && errno == ENOSYS);
            assert(objectUnlink(expectedName) == -1 && errno == ENOSYS && forwards == before + 2);
        }
    }
}
#else
#define objectOpen shm_open
#define objectUnlink shm_unlink
#endif

static const char *name = "apple.shm.notification_center", *slashName = "/apple.shm.notification_center";
static const char *preferencesName = "apple.sim.cfprefs.user.daemonv";

static void pathJoin(char result[PATH_MAX], const char *root, const char *suffix) {
    int count = snprintf(result, PATH_MAX, "%s/%s", root, suffix);
    assert(count > 0 && count < PATH_MAX);
}

static void testMappings(const char *root, const char *name, const char *slashName) {
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, root, 1));
    assert(objectOpen(name, O_RDWR) == -1 && errno == ENOENT);
    mode_t originalMask = umask(0027);
    int writer = objectOpen(name, O_RDWR | O_CREAT | O_EXCL, 0666);
    umask(originalMask);
    assert(writer >= 0 && (fcntl(writer, F_GETFD) & FD_CLOEXEC) && !(fcntl(writer, F_GETFL) & O_NONBLOCK));
    struct stat status;
    assert(!fstat(writer, &status) && S_ISREG(status.st_mode) && status.st_size == 0 && (status.st_mode & 0777) == 0640);
    assert(objectOpen(slashName, O_RDWR | O_CREAT | O_EXCL, 0600) == -1 && errno == EEXIST);
    assert(!ftruncate(writer, 4096));
    int reader = objectOpen(slashName, O_RDONLY);
    assert(reader >= 0 && (fcntl(reader, F_GETFD) & FD_CLOEXEC) && (fcntl(reader, F_GETFL) & O_ACCMODE) == O_RDONLY);
    volatile uint32_t *writeMap = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, writer, 0);
    volatile const uint32_t *readMap = mmap(NULL, 4096, PROT_READ, MAP_SHARED, reader, 0);
    assert(writeMap != MAP_FAILED && readMap != MAP_FAILED);
    writeMap[17] = 0x12345678;
    assert(readMap[17] == 0x12345678);
    assert(write(reader, "x", 1) == -1 && errno == EBADF);
    assert(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, reader, 0) == MAP_FAILED);
    assert(!objectUnlink(slashName));
    assert(objectOpen(name, O_RDONLY) == -1 && errno == ENOENT);
    assert(objectUnlink(name) == -1 && errno == ENOENT);
    assert(!close(writer) && !close(reader));
    writeMap[17] = 0x87654321; // POSIX unlink and descriptor close cannot invalidate existing mappings.
    assert(readMap[17] == 0x87654321);
    int replacement = objectOpen(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(replacement >= 0 && !ftruncate(replacement, 4096));
    volatile const uint32_t *newMap = mmap(NULL, 4096, PROT_READ, MAP_SHARED, replacement, 0);
    assert(newMap != MAP_FAILED && !newMap[17] && readMap[17] == 0x87654321);
    assert(!munmap((void *)newMap, 4096) && !munmap((void *)writeMap, 4096) && !munmap((void *)readMap, 4096));
    assert(!close(replacement));
    int truncated = objectOpen(name, O_RDWR | O_TRUNC);
    assert(truncated >= 0 && !fstat(truncated, &status) && !status.st_size && !close(truncated));
    assert(!objectUnlink(name));
}

static void testRejections(const char *root, const char *name, const char *file) {
    char object[PATH_MAX], temporary[PATH_MAX], unrelated[PATH_MAX], linked[PATH_MAX];
    pathJoin(temporary, root, "tmp"); pathJoin(object, temporary, file);
    pathJoin(unrelated, root, "untouched"); pathJoin(linked, root, "tmp-link");
    int fd = open(unrelated, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0 && write(fd, "keep", 4) == 4 && !close(fd));
    assert(!symlink(unrelated, object));
    assert(objectOpen(name, O_RDWR | O_TRUNC) == -1 && errno == ELOOP);
    assert(objectUnlink(name) == -1 && errno == ELOOP);
    struct stat status;
    assert(!stat(unrelated, &status) && status.st_size == 4);
    assert(!unlink(object) && !mkfifo(object, 0600));
    assert(objectOpen(name, O_RDWR | O_TRUNC) == -1 && errno == EINVAL);
    assert(objectUnlink(name) == -1 && errno == EINVAL);
    assert(!unlink(object) && !mkdir(object, 0700));
    assert(objectOpen(name, O_RDONLY) == -1 && errno == EINVAL);
    assert(objectUnlink(name) == -1 && errno == EINVAL);
    assert(!rmdir(object) && !link(unrelated, object));
    assert(objectOpen(name, O_RDWR | O_TRUNC) == -1 && errno == EACCES);
    assert(objectUnlink(name) == -1 && errno == EACCES);
    assert(!stat(unrelated, &status) && status.st_size == 4 && !unlink(object));
    assert(objectOpen(name, O_WRONLY | O_CREAT, 0600) == -1 && errno == EINVAL);
    assert(objectOpen(name, O_RDONLY | O_TRUNC) == -1 && errno == EINVAL);
    assert(objectOpen(name, O_RDWR | O_APPEND) == -1 && errno == EINVAL);

    assert(!rename(temporary, linked) && !symlink(linked, temporary));
    assert(objectOpen(name, O_RDWR | O_CREAT, 0600) == -1);
    assert(objectUnlink(name) == -1);
    assert(!unlink(temporary) && !rename(linked, temporary));
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, "/", 1));
    assert(objectOpen(name, O_RDWR | O_CREAT, 0600) == -1 && errno == EINVAL);
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, "relative", 1));
    assert(objectUnlink(name) == -1 && errno == EINVAL);
    pathJoin(linked, root, ".");
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, linked, 1));
    assert(objectOpen(name, O_RDONLY) == -1 && errno == EINVAL);
    assert(!unsetenv(LSB_NOTIFICATION_SHM_ROOT_ENV));
    assert(objectUnlink(name) == -1 && errno == EINVAL);
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, root, 1));
    if (geteuid()) {
        assert(!chmod(unrelated, 0000) && !rename(unrelated, object));
        assert(objectOpen(name, O_RDONLY) == -1 && errno == EACCES);
        assert(!chmod(object, 0600) && !rename(object, unrelated));
    }
    assert(!unlink(unrelated));
}

static void testWorkspaceIsolation(const char *root, const char *second) {
    const char *objects[] = {"/FBW1:com.apple.frontboard.syst", "/FBW2:com.apple.frontboard.syst", "/FBW1:other_domain-2"};
    int descriptors[3];
    for (unsigned index = 0; index < 3; ++index) {
        descriptors[index] = objectOpen(objects[index], O_CREAT | O_EXCL | O_RDWR, 0600);
        assert(descriptors[index] >= 0 && !ftruncate(descriptors[index], (index + 1) * 4096));
    }
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, second, 1));
    for (unsigned index = 0; index < 3; ++index) assert(objectOpen(objects[index], O_RDONLY) == -1 && errno == ENOENT);
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, root, 1));
    for (unsigned index = 0; index < 3; ++index) {
        struct stat status;
        assert(!fstat(descriptors[index], &status) && status.st_size == (index + 1) * 4096);
        assert(!close(descriptors[index]) && !objectUnlink(objects[index]));
    }
}

static void testPreferenceCounters(const char *root, const char *second) {
    // Match the daemon's 16 KiB writable counter page and a fresh client's separate read-only open/mapping.
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, root, 1));
    assert(objectOpen(preferencesName, O_RDONLY) == -1 && errno == ENOENT);
    int producer = objectOpen(preferencesName, O_CREAT | O_RDWR, 0644);
    assert(producer >= 0 && !ftruncate(producer, 16384));
    volatile uint32_t *counters = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, producer, 0);
    assert(counters != MAP_FAILED);
    counters[3393] = 41;
    int ready[2], update[2];
    assert(!pipe(ready) && !pipe(update));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(!close(ready[0]) && !close(update[1]) && !close(producer) && !munmap((void *)counters, 16384));
        int client = objectOpen(preferencesName, O_RDONLY);
        assert(client >= 0 && (fcntl(client, F_GETFL) & O_ACCMODE) == O_RDONLY);
        const volatile uint32_t *observed = mmap(NULL, 16384, PROT_READ, MAP_SHARED, client, 0);
        assert(observed != MAP_FAILED && observed[3393] == 41);
        assert(mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, client, 0) == MAP_FAILED);
        char token = 'r';
        assert(write(ready[1], &token, 1) == 1 && read(update[0], &token, 1) == 1 && observed[3393] == 42);
        assert(!munmap((void *)observed, 16384) && !close(client) && !close(ready[1]) && !close(update[0]));
        _exit(0);
    }
    assert(!close(ready[1]) && !close(update[0]));
    char token;
    assert(read(ready[0], &token, 1) == 1);
    counters[3393] = 42;
    assert(write(update[1], &token, 1) == 1 && !close(ready[0]) && !close(update[1]));
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(!close(producer));
    int reopened = objectOpen(preferencesName, O_CREAT | O_RDWR, 0644);
    uint32_t value = 0;
    assert(reopened >= 0 && !ftruncate(reopened, 16384) && pread(reopened, &value, sizeof(value), 3393 * sizeof(value)) == sizeof(value));
    assert(value == 42 && !close(reopened)); // A daemon restart reopens the existing object without replacing its counter storage.
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, second, 1));
    assert(objectOpen(preferencesName, O_RDONLY) == -1 && errno == ENOENT);
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, root, 1));
    assert(!objectUnlink(preferencesName) && counters[3393] == 42 && !munmap((void *)counters, 16384));
}

#ifndef LSB_NOTIFICATION_SHM_INTERPOSE_TESTING
static void checkSandbox(const char *name, const char *operation, bool expected) {
    errno = EDOM;
    assert(LSBNotificationShmAllowsSandbox(name, operation) == expected && errno == EDOM);
}

static void testWorkspaceSandbox(const char *root) {
    const char *workspace = "/FBW1:com.apple.frontboard.syst";
    const char *operations[] = {"ipc-posix-shm-read-data", "ipc-posix-shm-write-create", "ipc-posix-shm-write-unlink"};
    char temporary[PATH_MAX], object[PATH_MAX], linked[PATH_MAX];
    pathJoin(temporary, root, "tmp"); pathJoin(object, temporary, "frontboard-workspace-1-com.apple.frontboard.syst.shm");
    pathJoin(linked, root, "sandbox-object");
    for (unsigned index = 0; index < 3; ++index) {
        checkSandbox(workspace, operations[index], true); // Preflight also works before the first object is created.
        checkSandbox("/FBW2:com.apple.frontboard.syst", operations[index], true);
        checkSandbox(name, operations[index], false); // Notification support does not grant arbitrary SHM preflights.
        checkSandbox(preferencesName, operations[index], false); // The real preference daemon does not need an access-check exception.
        checkSandbox("/FBW3:com.apple.frontboard.syst", operations[index], false);
        checkSandbox("/FBW1:com.apple.frontboard.systX", operations[index], false);
        checkSandbox("/FBW1:../outside", operations[index], false);
        checkSandbox(NULL, operations[index], false);
    }
    checkSandbox(workspace, NULL, false);
    checkSandbox(workspace, "file-read-data", false);
    checkSandbox(workspace, "ipc-posix-shm-read-data.extra", false);
    checkSandbox(preferencesName, "ipc-posix-shm-write-data", false);
    assert(access(object, F_OK) == -1 && errno == ENOENT); // Access queries have no creation side effect.
    assert(!setenv(LSB_NOTIFICATION_SHM_ENABLE_ENV, "0", 1));
    initializeNotificationShm();
    checkSandbox(workspace, operations[0], false);
    assert(!setenv(LSB_NOTIFICATION_SHM_ENABLE_ENV, "1", 1));
    initializeNotificationShm();
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, "/", 1));
    checkSandbox(workspace, operations[0], false);
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, root, 1));

    int descriptor = objectOpen(workspace, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(descriptor >= 0 && !close(descriptor));
    for (unsigned index = 0; index < 3; ++index) checkSandbox(workspace, operations[index], true);
    if (geteuid()) {
        assert(!chmod(object, 0400));
        checkSandbox(workspace, operations[0], true);
        checkSandbox(workspace, operations[1], false);
        checkSandbox(workspace, operations[2], true);
        assert(!chmod(object, 0200));
        checkSandbox(workspace, operations[0], false);
        checkSandbox(workspace, operations[1], true);
        assert(!chmod(object, 0600));
        assert(!chmod(temporary, 0500));
        checkSandbox(workspace, operations[0], true);
        checkSandbox(workspace, operations[1], false);
        checkSandbox(workspace, operations[2], false);
        assert(!chmod(temporary, 0700));
    }
    assert(!link(object, linked));
    for (unsigned index = 0; index < 3; ++index) checkSandbox(workspace, operations[index], false);
    assert(!unlink(linked) && !rename(object, linked) && !symlink(linked, object));
    for (unsigned index = 0; index < 3; ++index) checkSandbox(workspace, operations[index], false);
    assert(!unlink(object) && !unlink(linked) && !mkfifo(object, 0600));
    for (unsigned index = 0; index < 3; ++index) checkSandbox(workspace, operations[index], false);
    assert(!unlink(object) && !rename(temporary, linked) && !symlink(linked, temporary));
    for (unsigned index = 0; index < 3; ++index) checkSandbox(workspace, operations[index], false);
    assert(!unlink(temporary) && !rename(linked, temporary));
}
#endif

int main(void) {
    alarm(10);
#ifndef LSB_NOTIFICATION_SHM_INTERPOSE_TESTING
    testForwarding();
    assert(!setenv(LSB_NOTIFICATION_SHM_ENABLE_ENV, "1", 1));
    initializeNotificationShm();
#endif
    assert(!unsetenv(LSB_NOTIFICATION_SHM_ENABLE_ENV)); // Both direct and inserted hooks keep the captured setting.
    char directory[] = "/tmp/LiveSimBooter-notification-shm.XXXXXX", root[PATH_MAX], temporary[PATH_MAX], second[PATH_MAX], secondTmp[PATH_MAX];
    assert(mkdtemp(directory) && realpath(directory, root));
    pathJoin(temporary, root, "tmp");
    pathJoin(second, root, "other-runtime"); pathJoin(secondTmp, second, "tmp");
    assert(!mkdir(temporary, 0700) && !mkdir(second, 0700) && !mkdir(secondTmp, 0700));
    testMappings(root, name, slashName);
    testMappings(root, "/FBW1:com.apple.frontboard.syst", "/FBW1:com.apple.frontboard.syst");
    testMappings(root, "/FBW2:com.apple.frontboard.syst", "/FBW2:com.apple.frontboard.syst");
    testMappings(root, preferencesName, preferencesName);
    testRejections(root, name, "notification-center.shm");
    testRejections(root, "/FBW1:com.apple.frontboard.syst", "frontboard-workspace-1-com.apple.frontboard.syst.shm");
    testRejections(root, preferencesName, "cfpreferences-user-v1.shm");
    testWorkspaceIsolation(root, second);
    testPreferenceCounters(root, second);
#ifndef LSB_NOTIFICATION_SHM_INTERPOSE_TESTING
    testWorkspaceSandbox(root);
#endif
    int first = objectOpen(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(first >= 0 && !ftruncate(first, 1234));
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, second, 1));
    assert(objectOpen(name, O_RDONLY) == -1 && errno == ENOENT);
    int other = objectOpen(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    struct stat status;
    assert(other >= 0 && !fstat(other, &status) && !status.st_size);
    assert(!close(other) && !objectUnlink(name));
    assert(!setenv(LSB_NOTIFICATION_SHM_ROOT_ENV, root, 1));
    assert(!fstat(first, &status) && status.st_size == 1234 && !close(first) && !objectUnlink(name));
    assert(!rmdir(secondTmp) && !rmdir(second) && !rmdir(temporary) && !rmdir(root));
    puts("NotificationSharedMemory: notification/FBW/cfprefs mappings, cross-process counters, unlink lifetime and safe sandbox preflights passed");
    return 0;
}
