#include "../Guest/LaunchdCompatibility.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static unsigned nativeCalls;
static unsigned environmentCalls;
static bool environmentAvailable;
static const char *expectedName;
static void *expectedOldValue, *expectedNewValue;
static size_t *expectedOldSize, expectedNewSize;
static unsigned filesystemCalls;
static const char *expectedPath, *expectedOtherPath, *expectedFileMode;
static mode_t expectedMode;
static int expectedFlags;
static struct stat *expectedStatus;
static bool realFilesystem;

static void checkPath(const char *path, const char *expected) {
    assert((!path && !expected) || (path && expected && !strcmp(path, expected)));
}

static int nativeMkdir(const char *path, mode_t mode) {
    if (realFilesystem) return mkdir(path, mode);
    ++filesystemCalls;
    checkPath(path, expectedPath);
    assert(mode == expectedMode);
    errno = EACCES;
    return -1;
}

static int nativeOpen(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (realFilesystem) return flags & O_CREAT ? open(path, flags, mode) : open(path, flags);
    ++filesystemCalls;
    checkPath(path, expectedPath);
    assert(flags == expectedFlags && (!(flags & O_CREAT) || mode == expectedMode));
    errno = EACCES;
    return -1;
}

static FILE *nativeFopen(const char *path, const char *mode) {
    if (realFilesystem) return fopen(path, mode);
    ++filesystemCalls;
    checkPath(path, expectedPath);
    assert(mode == expectedFileMode);
    errno = EACCES;
    return NULL;
}

static int nativeRename(const char *from, const char *to, unsigned flags) {
    if (realFilesystem) return renamex_np(from, to, flags);
    ++filesystemCalls;
    checkPath(from, expectedPath);
    checkPath(to, expectedOtherPath);
    assert(flags == (unsigned)expectedFlags);
    errno = EACCES;
    return -1;
}

static int nativeUnlink(const char *path) {
    if (realFilesystem) return unlink(path);
    ++filesystemCalls;
    checkPath(path, expectedPath);
    errno = EACCES;
    return -1;
}

static int nativeStat(const char *path, struct stat *status) {
    if (realFilesystem) return stat(path, status);
    ++filesystemCalls;
    checkPath(path, expectedPath);
    assert(status == expectedStatus);
    errno = EACCES;
    return -1;
}

static int nativeSysctlByName(const char *name, void *oldValue, size_t *oldSize, void *newValue, size_t newSize) {
    ++nativeCalls;
    assert(name == expectedName && oldValue == expectedOldValue && oldSize == expectedOldSize);
    assert(newValue == expectedNewValue && newSize == expectedNewSize);
    errno = EACCES;
    return -1;
}

static char *checkedGetenv(const char *name) {
    assert(environmentAvailable);
    ++environmentCalls;
    return getenv(name);
}

#define LSB_LAUNCHD_COMPATIBILITY_TESTING
#define sysctlbyname nativeSysctlByName
#define getenv checkedGetenv
#define mkdir nativeMkdir
#define open nativeOpen
#define fopen nativeFopen
#define renamex_np nativeRename
#define unlink nativeUnlink
#define stat(path, status) nativeStat(path, status)
#include "../Guest/LaunchdCompatibility.c"
#undef stat
#undef unlink
#undef renamex_np
#undef fopen
#undef open
#undef mkdir
#undef getenv
#undef sysctlbyname

static void expectPassthrough(const char *name, void *oldValue, size_t *oldSize, void *newValue, size_t newSize) {
    expectedName = name;
    expectedOldValue = oldValue;
    expectedOldSize = oldSize;
    expectedNewValue = newValue;
    expectedNewSize = newSize;
    unsigned before = nativeCalls;
    errno = EDOM;
    assert(LSBLaunchdSysctlByName(name, oldValue, oldSize, newValue, newSize) == -1 && errno == EACCES);
    assert(nativeCalls == before + 1);
}

static void testBootUUID(void) {
    static const char value[] = "12345678-9ABC-4DEF-8123-456789ABCDEF";
    char output[40], original[sizeof(output)], replacement[] = "changed";
    memset(output, 'x', sizeof(output));
    memcpy(original, output, sizeof(output));
    size_t size = sizeof(output);
    environmentAvailable = false;
    unsigned beforeEnvironment = environmentCalls;
    // Even this exact name must forward without accessing environment or TLS-dependent compatibility state before initialization.
    assert(!setenv(LSB_SIMULATOR_BOOT_UUID_ENV, value, 1));
    expectPassthrough("kern.bootsessionuuid", output, &size, NULL, 0);
    assert(environmentCalls == beforeEnvironment);

    environmentAvailable = true;
    const char *invalid[] = {NULL, "", "12345678-9ABC-4DEF-8123-456789ABCDE", "12345678-9ABC-4DEF-8123-456789ABCDEFF",
                            "12345678_9ABC-4DEF-8123-456789ABCDEF", "12345678-9ABG-4DEF-8123-456789ABCDEF",
                            "12345678-9ABC-4DEF-8123-456789ABCDE/", " 12345678-9ABC-4DEF-8123-456789ABCDE"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        if (invalid[i]) assert(!setenv(LSB_SIMULATOR_BOOT_UUID_ENV, invalid[i], 1));
        else assert(!unsetenv(LSB_SIMULATOR_BOOT_UUID_ENV));
        initializeSimulatorBootUUID();
        assert(!atomic_load_explicit(&simulatorBootUUIDReady, memory_order_acquire));
        expectPassthrough("kern.bootsessionuuid", output, &size, NULL, 0);
    }
    // Every delimiter/digit position is checked; there is no accepted partial prefix or embedded non-hex character.
    for (size_t i = 0; i < sizeof(value) - 1; ++i) {
        char malformed[sizeof(value)];
        memcpy(malformed, value, sizeof(value));
        malformed[i] = value[i] == '-' ? '0' : '-';
        assert(!setenv(LSB_SIMULATOR_BOOT_UUID_ENV, malformed, 1));
        initializeSimulatorBootUUID();
        assert(!atomic_load_explicit(&simulatorBootUUIDReady, memory_order_acquire));
    }

    assert(!setenv(LSB_SIMULATOR_BOOT_UUID_ENV, value, 1));
    initializeSimulatorBootUUID();
    assert(atomic_load_explicit(&simulatorBootUUIDReady, memory_order_acquire));
    // The constructor snapshot remains stable if an application changes or removes its own environment later.
    assert(!setenv(LSB_SIMULATOR_BOOT_UUID_ENV, "87654321-abcd-4def-8123-fedcba987654", 1));
    environmentAvailable = false;
    beforeEnvironment = environmentCalls;
    unsigned beforeNative = nativeCalls;
    for (unsigned i = 0; i < 3; ++i) {
        size = i == 0 ? 0 : i == 1 ? 1 : SIZE_MAX;
        errno = EDOM;
        assert(!LSBLaunchdSysctlByName("kern.bootsessionuuid", NULL, &size, NULL, 0) && size == 37 && errno == EDOM);
    }
    errno = EDOM;
    assert(!LSBLaunchdSysctlByName("kern.bootsessionuuid", NULL, NULL, NULL, 0) && errno == EDOM);
    for (size_t capacity = 0; capacity < sizeof(value); ++capacity) {
        memcpy(output, original, sizeof(output));
        size = capacity;
        errno = EDOM;
        assert(LSBLaunchdSysctlByName("kern.bootsessionuuid", output, &size, NULL, 0) == -1 && errno == ENOMEM && size == 0);
        assert(!memcmp(output, original, sizeof(output)));
    }
    assert(LSBLaunchdSysctlByName("kern.bootsessionuuid", output, NULL, NULL, 0) == -1 && errno == ENOMEM);
    assert(!memcmp(output, original, sizeof(output)));
    for (size_t capacity = sizeof(value); capacity <= sizeof(output); ++capacity) {
        memcpy(output, original, sizeof(output));
        size = capacity;
        errno = ERANGE;
        assert(!LSBLaunchdSysctlByName("kern.bootsessionuuid", output, &size, NULL, 0) && size == sizeof(value) && errno == ERANGE);
        assert(!memcmp(output, value, sizeof(value)) && !memcmp(output + sizeof(value), original + sizeof(value), sizeof(output) - sizeof(value)));
    }
    assert(!unsetenv(LSB_SIMULATOR_BOOT_UUID_ENV));
    size = sizeof(output);
    assert(!LSBLaunchdSysctlByName("kern.bootsessionuuid", output, &size, NULL, 0) && !strcmp(output, value));
    assert(nativeCalls == beforeNative && environmentCalls == beforeEnvironment);

    const char *otherNames[] = {NULL, "", "KERN.BOOTSESSIONUUID", "kern.bootsessionuui", "kern.bootsessionuuid.extra", "kern.ostype"};
    for (unsigned i = 0; i < sizeof(otherNames) / sizeof(otherNames[0]); ++i)
        expectPassthrough(otherNames[i], output, &size, NULL, 0);
    expectPassthrough("kern.bootsessionuuid", output, &size, replacement, sizeof(replacement));
    expectPassthrough("kern.bootsessionuuid", output, &size, replacement, 0);
    expectPassthrough("kern.bootsessionuuid", output, &size, NULL, 1);
    expectPassthrough("kern.bootsessionuuid", NULL, NULL, replacement, sizeof(replacement));
    assert(environmentCalls == beforeEnvironment);
    size = sizeof(output);
    assert(!LSBLaunchdSysctlByName("kern.bootsessionuuid", output, &size, NULL, 0) && !strcmp(output, value));

    // Lowercase hexadecimal is also a canonical UUID spelling. Reset only in this single-threaded fixture.
    atomic_store_explicit(&simulatorBootUUIDReady, false, memory_order_release);
    environmentAvailable = true;
    const char *lowercase = "87654321-abcd-4def-8123-fedcba987654";
    assert(!setenv(LSB_SIMULATOR_BOOT_UUID_ENV, lowercase, 1));
    initializeSimulatorBootUUID();
    size = sizeof(output);
    assert(!LSBLaunchdSysctlByName("kern.bootsessionuuid", output, &size, NULL, 0) && !strcmp(output, lowercase));
    atomic_store_explicit(&simulatorBootUUIDReady, false, memory_order_release);
    assert(!unsetenv(LSB_SIMULATOR_BOOT_UUID_ENV));
}

static void checkFilesystemPath(const char *path, const char *expected) {
    expectedPath = expected;
    expectedMode = 0713;
    unsigned before = filesystemCalls;
    assert(LSBLaunchdMkdir(path, expectedMode) == -1 && errno == EACCES);
    expectedFlags = O_RDONLY | O_CLOEXEC;
    assert(LSBLaunchdOpen(path, expectedFlags) == -1 && errno == EACCES);
    expectedFlags = O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW;
    expectedMode = 0641;
    assert(LSBLaunchdOpen(path, expectedFlags, expectedMode) == -1 && errno == EACCES);
    expectedFileMode = "w+";
    assert(LSBLaunchdFopen(path, expectedFileMode) == NULL && errno == EACCES);
    assert(LSBLaunchdUnlink(path) == -1 && errno == EACCES);
    struct stat status;
    expectedStatus = &status;
    assert(LSBLaunchdStat(path, &status) == -1 && errno == EACCES);
    assert(filesystemCalls == before + 6);
}

static void checkRejectedPath(const char *path, int error) {
    unsigned before = filesystemCalls;
    struct stat status;
    assert(LSBLaunchdMkdir(path, 0700) == -1 && errno == error);
    assert(LSBLaunchdOpen(path, O_RDONLY) == -1 && errno == error);
    assert(LSBLaunchdOpen(path, O_CREAT | O_WRONLY, 0600) == -1 && errno == error);
    assert(LSBLaunchdFopen(path, "w") == NULL && errno == error);
    assert(LSBLaunchdUnlink(path) == -1 && errno == error);
    assert(LSBLaunchdStat(path, &status) == -1 && errno == error);
    assert(LSBLaunchdRename(path, "/unrelated", 0) == -1 && errno == error);
    assert(LSBLaunchdRename("/unrelated", path, 0) == -1 && errno == error);
    assert(filesystemCalls == before);
}

static void testFilesystem(void) {
    static const char original[] = "/private/var/tmp/com.kdt.LiveSimBooter.launchd";
    static const char originalFile[] = "/private/var/tmp/com.kdt.LiveSimBooter.launchd/disabled.plist";
    environmentAvailable = false;
    unsigned beforeEnvironment = environmentCalls;
    const char *unrelated[] = {NULL, "/dev/null", "/private/var/tmp/com.kdt.LiveSimBooter.launchd-extra/disabled.plist",
                              "/private/var/tmp/com.kdt.LiveSimBooter.launch", "private/var/tmp/com.kdt.LiveSimBooter.launchd"};
    for (unsigned i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) checkFilesystemPath(unrelated[i], unrelated[i]);
    expectedPath = "/early-a";
    expectedOtherPath = "/early-b";
    expectedFlags = RENAME_EXCL;
    assert(LSBLaunchdRename(expectedPath, expectedOtherPath, RENAME_EXCL) == -1 && errno == EACCES);
    assert(environmentCalls == beforeEnvironment);
    environmentAvailable = true;
    assert(!unsetenv(LSB_SIMULATOR_LAUNCHD_ENV));
    checkFilesystemPath(originalFile, originalFile);
    assert(!setenv(LSB_SIMULATOR_LAUNCHD_ENV, "01", 1));
    checkFilesystemPath(originalFile, originalFile);
    assert(!setenv(LSB_SIMULATOR_LAUNCHD_ENV, "1", 1));
    assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", "/group/data", 1));
    checkFilesystemPath(original, "/group/data/tmp/launchd");
    checkFilesystemPath(originalFile, "/group/data/tmp/launchd/disabled.plist");
    checkFilesystemPath("/private/var/tmp/com.kdt.LiveSimBooter.launchd/", "/group/data/tmp/launchd/");
    checkFilesystemPath("/private/var/tmp/com.kdt.LiveSimBooter.launchd/.hidden", "/group/data/tmp/launchd/.hidden");
    checkFilesystemPath("/private/var/tmp/com.kdt.LiveSimBooter.launchd/...", "/group/data/tmp/launchd/...");
    const char *traversals[] = {"/../other", "/.", "/a/..", "/a/../other", "/a//./b"};
    for (unsigned i = 0; i < sizeof(traversals) / sizeof(traversals[0]); ++i) {
        char path[PATH_MAX];
        assert(snprintf(path, sizeof(path), "%s%s", original, traversals[i]) > 0);
        checkRejectedPath(path, EINVAL);
    }
    expectedPath = "/group/data/tmp/launchd/disabled.plist";
    expectedOtherPath = "/group/data/tmp/launchd";
    expectedFlags = RENAME_SWAP;
    unsigned before = filesystemCalls;
    assert(LSBLaunchdRename(originalFile, original, RENAME_SWAP) == -1 && errno == EACCES);
    expectedOtherPath = "/elsewhere";
    expectedFlags = 0;
    assert(LSBLaunchdRename(originalFile, expectedOtherPath, 0) == -1 && errno == EACCES);
    expectedPath = "/elsewhere";
    expectedOtherPath = "/group/data/tmp/launchd/disabled.plist";
    assert(LSBLaunchdRename(expectedPath, originalFile, 0) == -1 && errno == EACCES);
    assert(filesystemCalls == before + 3);
    assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", "/group/data///", 1));
    checkFilesystemPath(originalFile, "/group/data/tmp/launchd/disabled.plist");
    const char *badRoots[] = {"", "relative", "/", "/group/../other", "/group/./data"};
    for (unsigned i = 0; i < sizeof(badRoots) / sizeof(badRoots[0]); ++i) {
        assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", badRoots[i], 1));
        checkRejectedPath(original, EINVAL);
    }
    assert(!unsetenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY"));
    checkRejectedPath(original, EINVAL);
    char largeRoot[PATH_MAX + 1], translated[PATH_MAX];
    memset(largeRoot, 'r', sizeof(largeRoot));
    largeRoot[0] = '/';
    largeRoot[PATH_MAX] = '\0';
    assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", largeRoot, 1));
    checkRejectedPath(original, ENAMETOOLONG);
    largeRoot[PATH_MAX - sizeof("/tmp/launchd")] = '\0';
    assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", largeRoot, 1));
    assert(snprintf(translated, sizeof(translated), "%s/tmp/launchd", largeRoot) == PATH_MAX - 1);
    checkFilesystemPath(original, translated);
    checkRejectedPath(originalFile, ENAMETOOLONG);
    assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", "/group", 1));
    char largePath[PATH_MAX + sizeof(original)];
    memcpy(largePath, original, sizeof(original) - 1);
    memset(largePath + sizeof(original) - 1, 'x', PATH_MAX);
    largePath[sizeof(original) - 1] = '/';
    largePath[sizeof(largePath) - 1] = '\0';
    checkRejectedPath(largePath, ENAMETOOLONG);

    // Exercise the actual create/read/write/metadata/rotation/delete lifecycle in a temporary simulator data directory.
    char directory[] = "/tmp/LSB-launchd-state.XXXXXX", temporary[PATH_MAX], state[PATH_MAX];
    assert(mkdtemp(directory));
    assert(snprintf(temporary, sizeof(temporary), "%s/tmp", directory) > 0);
    assert(!mkdir(temporary, 0700));
    assert(!setenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY", directory, 1));
    realFilesystem = true;
    assert(!LSBLaunchdMkdir(original, 0700));
    int descriptor = LSBLaunchdOpen(originalFile, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(descriptor >= 0 && write(descriptor, "first", 5) == 5 && !close(descriptor));
    FILE *file = LSBLaunchdFopen(originalFile, "w");
    assert(file && fputs("replacement", file) >= 0 && !fclose(file));
    struct stat status;
    assert(!LSBLaunchdStat(originalFile, &status) && status.st_size == 11 && S_ISREG(status.st_mode));
    const char *rotated = "/private/var/tmp/com.kdt.LiveSimBooter.launchd/disabled.plist.1";
    assert(!LSBLaunchdRename(originalFile, rotated, RENAME_EXCL));
    descriptor = LSBLaunchdOpen(rotated, O_RDONLY);
    char contents[12] = {0};
    assert(descriptor >= 0 && read(descriptor, contents, sizeof(contents)) == 11 && !strcmp(contents, "replacement"));
    assert(!close(descriptor) && !LSBLaunchdUnlink(rotated));
    realFilesystem = false;
    assert(snprintf(state, sizeof(state), "%s/tmp/launchd", directory) > 0);
    assert(!rmdir(state) && !rmdir(temporary) && !rmdir(directory));
    assert(!unsetenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY") && !unsetenv(LSB_SIMULATOR_LAUNCHD_ENV));
}

int main(void) {
    char output[4] = {'a', 'b', 'c', 'd'}, replacement[] = "changed";
    size_t size = sizeof(output);
    // libSystem's host compatibility checks run before environ exists; unrelated queries must never touch getenv.
    expectPassthrough("kern.osrelease", output, &size, NULL, 0);
    expectPassthrough("hw.optional.arm64", output, &size, NULL, 0);
    expectPassthrough(NULL, output, &size, NULL, 0);
    expectPassthrough("kern.bootargs", output, &size, replacement, sizeof(replacement));
    assert(environmentCalls == 0);
    environmentAvailable = true;
    assert(!unsetenv(LSB_SIMULATOR_LAUNCHD_ENV));
    expectPassthrough("kern.bootargs", output, &size, NULL, 0);
    const char *inactive[] = {"", "0", "true", "01", "1x", " 1", "1 "};
    for (unsigned i = 0; i < sizeof(inactive) / sizeof(inactive[0]); ++i) {
        assert(!setenv(LSB_SIMULATOR_LAUNCHD_ENV, inactive[i], 1));
        expectPassthrough("kern.bootargs", output, &size, NULL, 0);
    }
    assert(!setenv(LSB_SIMULATOR_LAUNCHD_ENV, "1", 1));
    unsigned priorEnvironmentCalls = environmentCalls;
    const char *otherNames[] = {"kern.osrelease", "kern.bootargs.extra", "kern.bootarg", "KERN.BOOTARGS", "", NULL};
    for (unsigned i = 0; i < sizeof(otherNames) / sizeof(otherNames[0]); ++i)
        expectPassthrough(otherNames[i], output, &size, NULL, 0);
    // No write operation, including ambiguous zero-length arguments, is converted into a successful fake read.
    expectPassthrough("kern.bootargs", output, &size, replacement, sizeof(replacement));
    expectPassthrough("kern.bootargs", output, &size, replacement, 0);
    expectPassthrough("kern.bootargs", output, &size, NULL, 1);
    expectPassthrough("kern.bootargs", NULL, NULL, replacement, sizeof(replacement));
    assert(environmentCalls == priorEnvironmentCalls);

    unsigned before = nativeCalls;
    for (unsigned i = 0; i < 3; ++i) {
        size = i == 0 ? 0 : i == 1 ? 1 : (size_t)-1;
        errno = EDOM;
        assert(!LSBLaunchdSysctlByName("kern.bootargs", NULL, &size, NULL, 0) && size == 1 && errno == EDOM);
    }
    size = sizeof(output);
    errno = ERANGE;
    assert(!LSBLaunchdSysctlByName("kern.bootargs", output, &size, NULL, 0) && size == 1 && errno == ERANGE);
    assert(!memcmp(output, "\0bcd", sizeof(output)));
    output[0] = 'a';
    size = 1;
    assert(!LSBLaunchdSysctlByName("kern.bootargs", output, &size, NULL, 0) && size == 1 && !output[0]);
    output[0] = 'a';
    size = 0;
    assert(LSBLaunchdSysctlByName("kern.bootargs", output, &size, NULL, 0) == -1 && errno == ENOMEM && size == 0);
    assert(!memcmp(output, "abcd", sizeof(output)));
    assert(LSBLaunchdSysctlByName("kern.bootargs", output, NULL, NULL, 0) == -1 && errno == ENOMEM);
    assert(!memcmp(output, "abcd", sizeof(output)));
    errno = EDOM;
    assert(!LSBLaunchdSysctlByName("kern.bootargs", NULL, NULL, NULL, 0) && errno == EDOM);
    assert(nativeCalls == before);
    assert(!unsetenv(LSB_SIMULATOR_LAUNCHD_ENV));
    expectPassthrough("kern.bootargs", output, &size, NULL, 0);
    testBootUUID();
    testFilesystem();
    puts("LaunchdCompatibility: early-init bypass, bootargs/boot UUID semantics, exact state subtree and file lifecycle passed");
    return 0;
}
