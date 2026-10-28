#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifndef LSB_FILE_LOCK_INTERPOSE_TESTING

static unsigned attributeReads, attributeWrites, descriptorStats;
static int readError, writeError;
static bool wrongOwner;
static const char *reportedPath;

static ssize_t readAttribute(int fd, const char *name, void *value, size_t size, uint32_t position, int options) {
    ++attributeReads;
    if (readError) { errno = readError; return -1; }
    return fgetxattr(fd, name, value, size, position, options);
}

static int writeAttribute(int fd, const char *name, const void *value, size_t size, uint32_t position, int options) {
    ++attributeWrites;
    assert(size == 1 && *(const unsigned char *)value == 0xff && !position && options == XATTR_CREATE);
    if (writeError) { errno = writeError; return -1; }
    return fsetxattr(fd, name, value, size, position, options);
}

static int descriptorStatus(int fd, struct stat *status) {
    ++descriptorStats;
    int result = fstat(fd, status);
    if (!result && wrongOwner) status->st_uid = geteuid() + 1;
    return result;
}

static int descriptorPath(int fd, int command, void *argument) {
    if (command == F_GETPATH && reportedPath) {
        assert(strlen(reportedPath) < PATH_MAX);
        strcpy(argument, reportedPath);
        return 0;
    }
    return fcntl(fd, command, argument);
}

// Keep every native lock call real. Only the scope's F_GETPATH call is redirected for alias/race fixtures.
#define LSB_FILE_LOCK_TESTING
#define fgetxattr readAttribute
#define fsetxattr writeAttribute
#define fstat descriptorStatus
#define fcntl(fd, command, argument) descriptorPath((fd), (command), (void *)(uintptr_t)(argument))
#include "../Guest/FileLockCompatibility.c"
#undef fcntl
#undef fstat
#undef fsetxattr
#undef fgetxattr

static char base[PATH_MAX], runtime[PATH_MAX], data[PATH_MAX], other[PATH_MAX], file[PATH_MAX], outside[PATH_MAX];

static void join(char output[PATH_MAX], const char *prefix, const char *suffix) {
    int count = snprintf(output, PATH_MAX, "%s/%s", prefix, suffix);
    assert(count > 0 && count < PATH_MAX);
}

static int createFile(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0);
    return fd;
}

static void clearAttribute(int fd) {
    if (fremovexattr(fd, LSB_FILE_LOCK_ATTRIBUTE, 0)) assert(errno == ENOATTR);
    attributeReads = attributeWrites = descriptorStats = 0;
}

static bool marked(int fd) {
    unsigned char value = 0;
    ssize_t size = fgetxattr(fd, LSB_FILE_LOCK_ATTRIBUTE, &value, 1, 0, 0);
    if (size < 0) assert(errno == ENOATTR);
    return size == 1 && value == 0xff;
}

static void prepare(int fd, bool expected) {
    errno = EDOM;
    LSBFileLockPrepare(fd);
    assert(errno == EDOM);
    assert(marked(fd) == expected);
}

static void testScope(int fd, int outsideFD) {
    // Initialization is an explicit gate; unrelated early calls do not stat descriptors or consult attributes.
    configureFileLockScope(NULL, data);
    prepare(fd, false);
    assert(!descriptorStats && !attributeReads && !attributeWrites);
    const char *disabled[] = {"", "0", "true", "01"};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i) {
        configureFileLockScope(disabled[i], data);
        prepare(fd, false);
    }
    char path[PATH_MAX];
    const char *invalid[] = {NULL, "", "/", "relative", "/missing/data", base, other};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        configureFileLockScope("1", invalid[i]);
        prepare(fd, false);
    }
    const char *suffixes[] = {".", "../data", "/"};
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        join(path, data, suffixes[i]);
        configureFileLockScope("1", path);
        prepare(fd, false);
    }
    // A generic data directory alone is not sufficient: the paired runtime must be a real, owned directory.
    assert(!rmdir(runtime));
    configureFileLockScope("1", data);
    prepare(fd, false);
    assert(!symlink(other, runtime));
    configureFileLockScope("1", data);
    prepare(fd, false);
    assert(!unlink(runtime));
    int notDirectory = createFile(runtime);
    configureFileLockScope("1", data);
    prepare(fd, false);
    assert(!close(notDirectory) && !unlink(runtime) && !mkdir(runtime, 0700));
    wrongOwner = true;
    configureFileLockScope("1", data);
    prepare(fd, false);
    wrongOwner = false;
    configureFileLockScope("1", data);
    prepare(fd, true);
    assert(attributeWrites == 1);
    clearAttribute(fd);
    prepare(outsideFD, false);
    assert(!attributeWrites);
    int directory = open(data, O_RDONLY | O_DIRECTORY);
    assert(directory >= 0);
    errno = EDOM;
    LSBFileLockPrepare(directory);
    assert(errno == EDOM && !attributeWrites && !close(directory));
    wrongOwner = true;
    prepare(fd, false);
    wrongOwner = false;
    assert(!attributeWrites);

    join(path, data, "hardlink");
    assert(!link(file, path));
    prepare(fd, false);
    assert(!attributeWrites && !unlink(path));

    join(path, data, "escape");
    assert(!symlink(outside, path));
    int escaped = open(path, O_RDWR);
    assert(escaped >= 0);
    prepare(escaped, false);
    assert(!attributeWrites && !close(escaped) && !unlink(path));

    // F_GETPATH racing with a replacement must not annotate a different descriptor based on a matching string alone.
    reportedPath = file;
    prepare(outsideFD, false);
    assert(!attributeWrites);
    reportedPath = NULL;
    if (!strncmp(data, "/private/var/", 13)) {
        configureFileLockScope("1", data + 8);
        reportedPath = file + 8;
        prepare(fd, true);
        assert(attributeWrites == 1);
        reportedPath = NULL;
        clearAttribute(fd);
    }
    configureFileLockScope("1", data);
}

static void testExistingPolicy(int fd) {
    const unsigned char values[][2] = {{0}, {1}, {0xff}, {0x10, 0x20}};
    const size_t lengths[] = {0, 1, 1, 1, 2};
    const unsigned indexes[] = {0, 0, 1, 2, 3};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        assert(!fsetxattr(fd, LSB_FILE_LOCK_ATTRIBUTE, values[indexes[i]], lengths[i], 0, 0));
        attributeWrites = 0;
        errno = EDOM;
        LSBFileLockPrepare(fd);
        assert(errno == EDOM && !attributeWrites);
        unsigned char actual[2] = {0};
        assert(fgetxattr(fd, LSB_FILE_LOCK_ATTRIBUTE, actual, sizeof(actual), 0, 0) == (ssize_t)lengths[i]);
        assert(!memcmp(actual, values[indexes[i]], lengths[i]));
        clearAttribute(fd);
    }
    readError = EPERM;
    prepare(fd, false);
    assert(!attributeWrites);
    readError = 0;
    writeError = EACCES;
    prepare(fd, false);
    assert(attributeWrites == 1);
    writeError = 0;
    clearAttribute(fd);
}

static void testFlock(int fd) {
    errno = EDOM;
    assert(!LSBFileLockFlock(fd, LOCK_UN) && errno == EDOM && !attributeWrites);
    errno = EDOM;
    assert(!LSBFileLockFlock(fd, LOCK_EX | LOCK_NB) && errno == EDOM && marked(fd));
    int contender = open(file, O_RDWR);
    assert(contender >= 0);
    assert(LSBFileLockFlock(contender, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    assert(!LSBFileLockFlock(fd, LOCK_UN));
    assert(!LSBFileLockFlock(contender, LOCK_SH | LOCK_NB));
    assert(!LSBFileLockFlock(contender, LOCK_UN) && !close(contender));
    assert(LSBFileLockFlock(-1, LOCK_EX) == -1 && errno == EBADF);
    clearAttribute(fd);
    writeError = EACCES;
    errno = EDOM;
    assert(!LSBFileLockFlock(fd, LOCK_EX | LOCK_NB) && errno == EDOM && !marked(fd));
    assert(!LSBFileLockFlock(fd, LOCK_UN));
    writeError = 0;
    clearAttribute(fd);
}

static void testFCntl(int fd) {
    struct flock lock = {.l_whence = SEEK_SET, .l_type = F_UNLCK};
    errno = EDOM;
    assert(!LSBFileLockFCntl(fd, F_SETLK, &lock) && errno == EDOM && !attributeWrites);
    lock.l_type = F_WRLCK;
    assert(!LSBFileLockFCntl(fd, F_GETLK, &lock) && !attributeWrites);
    assert(LSBFileLockFCntl(fd, F_SETLK, (void *)1) == -1 && errno == EFAULT && !attributeWrites);
    lock.l_type = F_WRLCK;
    errno = EDOM;
    assert(!LSBFileLockFCntl(fd, F_SETLK, &lock) && errno == EDOM && marked(fd));
    lock.l_type = F_UNLCK;
    assert(!LSBFileLockFCntlNoCancel(fd, F_SETLK, &lock));
    clearAttribute(fd);
    lock.l_type = F_RDLCK;
    assert(!LSBFileLockFCntlNoCancel(fd, F_SETLK, &lock) && marked(fd));
    lock.l_type = F_UNLCK;
    assert(!LSBFileLockFCntlRaw(fd, F_SETLK, (uintptr_t)&lock));
    clearAttribute(fd);
    lock.l_type = F_WRLCK;
    assert(!LSBFileLockFCntlRaw(fd, F_SETLK, (uintptr_t)&lock) && marked(fd));
    lock.l_type = F_UNLCK;
    assert(!LSBFileLockFCntlRawNoCancel(fd, F_SETLK, (uintptr_t)&lock));
    clearAttribute(fd);
    lock.l_type = F_RDLCK;
    assert(!LSBFileLockFCntlRawNoCancel(fd, F_SETLK, (uintptr_t)&lock) && marked(fd));
    lock.l_type = F_UNLCK;
    assert(!LSBFileLockFCntl(fd, F_SETLK, &lock));
    clearAttribute(fd);

    // Exercise the naked arm64 wrappers with no variadic argument, an integer, and a pointer.
    errno = EDOM;
    assert(LSBFileLockFCntl(fd, F_GETFD) == FD_CLOEXEC && errno == EDOM);
    assert(LSBFileLockFCntlNoCancel(fd, F_GETFL) == fcntl(fd, F_GETFL));
    assert(!LSBFileLockFCntl(fd, F_SETFD, 0) && !fcntl(fd, F_GETFD));
    assert(!LSBFileLockFCntlNoCancel(fd, F_SETFD, FD_CLOEXEC) && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    char path[PATH_MAX];
    assert(!LSBFileLockFCntl(fd, F_GETPATH, path) && !strcmp(path, file));
    assert(LSBFileLockFCntl(-1, F_GETFD) == -1 && errno == EBADF);
    assert(LSBFileLockFCntlNoCancel(fd, F_SETLK, (void *)1) == -1 && errno == EFAULT);
    assert(!attributeWrites);
}

static void testRecordLockVariants(int fd) {
    const int commands[] = {F_SETLKW, F_SETLKWTIMEOUT, F_OFD_SETLK, F_OFD_SETLKW, F_OFD_SETLKWTIMEOUT};
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        // Timeout variants carry struct flock first; ordinary variants read only that prefix.
        struct flocktimeout request = {.fl = {.l_whence = SEEK_SET, .l_type = F_WRLCK}, .timeout = {.tv_nsec = 1000000}};
        errno = EDOM;
        assert(!LSBFileLockFCntl(fd, commands[i], &request) && errno == EDOM && marked(fd));
        clearAttribute(fd);
        request.fl.l_type = F_UNLCK;
        errno = EDOM;
        assert(!LSBFileLockFCntlNoCancel(fd, commands[i], &request) && errno == EDOM && !attributeWrites && !marked(fd));
    }
}

int main(void) {
    char candidate[PATH_MAX];
    join(candidate, getenv("TMPDIR") ?: "/tmp", "LiveSimBooter-file-lock.XXXXXX");
    assert(mkdtemp(candidate) && realpath(candidate, base));
    join(data, base, "data"); join(other, base, "data-other"); join(runtime, base, "RuntimeRoot");
    assert(!mkdir(data, 0700) && !mkdir(other, 0700) && !mkdir(runtime, 0700));
    join(file, data, "worker.lock"); join(outside, other, "worker.lock");
    int fd = createFile(file), outsideFD = createFile(outside);
    testScope(fd, outsideFD);
    testExistingPolicy(fd);
    testFlock(fd);
    testFCntl(fd);
    testRecordLockVariants(fd);
    assert(!close(fd) && !close(outsideFD));
    assert(!unlink(file) && !unlink(outside) && !rmdir(data) && !rmdir(other) && !rmdir(runtime) && !rmdir(base));
    puts("FileLockCompatibility: canonical scope, descriptor identity, existing policy, native locks and arm64 fcntl ABI passed");
    return 0;
}

#else

#include "../Guest/FileLockCompatibility.h"

extern int nativeNoCancel(int descriptor, int command, ...) __asm("_fcntl$NOCANCEL");
extern int __fcntl(int descriptor, int command, uintptr_t argument);
extern int __fcntl_nocancel(int descriptor, int command, uintptr_t argument);

static void checkAttribute(int fd, bool expected) {
    unsigned char value = 0;
    ssize_t length = fgetxattr(fd, LSB_FILE_LOCK_ATTRIBUTE, &value, sizeof(value), 0, 0);
    if (expected) assert(length == 1 && value == 0xff);
    else assert(length == -1 && errno == ENOATTR);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    bool enabled = !strcmp(getenv(LSB_FILE_LOCK_ENABLE_ENV), "1");
    char file[PATH_MAX], outside[PATH_MAX];
    int count = snprintf(file, sizeof(file), "%s/worker.lock", argv[1]);
    assert(count > 0 && (size_t)count < sizeof(file));
    count = snprintf(outside, sizeof(outside), "%s-outside", argv[1]);
    assert(count > 0 && (size_t)count < sizeof(outside));
    int fd = open(file, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    int outsideFD = open(outside, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    assert(fd >= 0 && outsideFD >= 0);
    assert(!flock(outsideFD, LOCK_EX | LOCK_NB));
    checkAttribute(outsideFD, false);
    assert(!flock(outsideFD, LOCK_UN));
    assert(!flock(fd, LOCK_EX | LOCK_NB));
    checkAttribute(fd, enabled);
    assert(!flock(fd, LOCK_UN));
    for (unsigned variant = 0; variant < 4; ++variant) {
        if (enabled) assert(!fremovexattr(fd, LSB_FILE_LOCK_ATTRIBUTE, 0));
        struct flock lock = {.l_whence = SEEK_SET, .l_type = F_WRLCK};
        int result;
        errno = EDOM;
        switch (variant) {
            case 0: result = fcntl(fd, F_SETLK, &lock); break;
            case 1: result = nativeNoCancel(fd, F_SETLK, &lock); break;
            case 2: result = __fcntl(fd, F_SETLK, (uintptr_t)&lock); break;
            default: result = __fcntl_nocancel(fd, F_SETLK, (uintptr_t)&lock); break;
        }
        assert(!result && errno == EDOM);
        checkAttribute(fd, enabled);
        lock.l_type = F_UNLCK;
        assert(!fcntl(fd, F_SETLK, &lock));
    }
    assert(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    assert(!fcntl(fd, F_SETFD, 0) && !nativeNoCancel(fd, F_GETFD));
    assert(!close(fd) && !close(outsideFD) && !unlink(file) && !unlink(outside));
    puts(enabled ? "FileLockCompatibility interpose: constructor, flock and all fcntl symbols passed" :
                   "FileLockCompatibility interpose: disabled scope preserves native behavior");
    return 0;
}

#endif
