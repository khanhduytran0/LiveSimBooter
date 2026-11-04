#include "../Guest/libLiveSimSpawn/SpringBoardEntitlements.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool (*originalQuery)(CFStringRef);
static int executablePath(char *, uint32_t *);
#define BSSelfTaskHasEntitlement originalQuery
#define _NSGetExecutablePath executablePath
#define LSB_SPRINGBOARD_ENTITLEMENTS_TESTING
#include "../Guest/libLiveSimSpawn/SpringBoardEntitlements.c"
#undef BSSelfTaskHasEntitlement
#undef _NSGetExecutablePath

static unsigned originalCalls, executableCalls, checks;
static bool originalResult;
static int pathMode;
static const char *currentExecutable;
static CFStringRef lastKey;

static bool original(CFStringRef key) {
    ++originalCalls;
    lastKey = key;
    errno = EPERM;
    return originalResult;
}

static int executablePath(char *buffer, uint32_t *size) {
    ++executableCalls;
    if (pathMode == 1) { *size = PATH_MAX + 1; return -1; }
    if (pathMode == 2) { memset(buffer, 'x', *size); return 0; }
    assert(strlen(currentExecutable) + 1 <= *size);
    strcpy(buffer, currentExecutable);
    return 0;
}

static void query(CFStringRef key, bool expected, unsigned paths) {
    originalCalls = executableCalls = 0;
    lastKey = NULL;
    errno = EDOM;
    assert(LSBSpringBoardSelfTaskHasEntitlement(key) == expected);
    assert(originalCalls == (originalQuery ? 1U : 0U));
    assert(!originalQuery || lastKey == key);
    assert(errno == (originalQuery ? EPERM : EDOM));
    assert(executableCalls == paths);
    ++checks;
}

static void makeFile(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !close(fd));
}

int main(void) {
    char directory[] = "/tmp/LSBSpringBoardEntitlements.XXXXXX", root[PATH_MAX], path[PATH_MAX], executable[PATH_MAX], outside[PATH_MAX];
    assert(mkdtemp(directory) && realpath(directory, root));
    const char *components[] = {"System", "System/Library", "System/Library/CoreServices", "System/Library/CoreServices/SpringBoard.app"};
    for (size_t i = 0; i < sizeof(components) / sizeof(*components); ++i) {
        assert(snprintf(path, sizeof(path), "%s/%s", root, components[i]) < (int)sizeof(path));
        assert(!mkdir(path, 0700));
    }
    assert(snprintf(executable, sizeof(executable), "%s/%s/SpringBoard", root, components[3]) < (int)sizeof(executable));
    assert(snprintf(outside, sizeof(outside), "%s/other", root) < (int)sizeof(outside));
    makeFile(executable);
    makeFile(outside);
    currentExecutable = executable;
    originalQuery = original;
    assert(!setenv("DYLD_ROOT_PATH", directory, 1)); // /tmp spelling canonicalizes to /private/tmp on macOS.
    assert(!setenv(LSB_SPRINGBOARD_ENTITLEMENTS_ENABLE_ENV, "1", 1));
    CFStringRef key = CFSTR(LSB_SPRINGBOARD_UI_ENTITLEMENT);
    query(key, true, 1);
    query(CFSTR("com.apple.springboard-ui.client.other"), false, 0);
    query(CFSTR("com.apple.private.MobileContainerManager.allowed"), false, 0);
    query(NULL, false, 0);
    originalResult = true;
    query(key, true, 0);
    query(CFSTR("original"), true, 0);
    originalResult = false;
    originalQuery = NULL;
    query(key, false, 0);
    originalQuery = original;

    const char *disabled[] = {"", "0", "true", "01", "1 "};
    for (size_t i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) {
        assert(!setenv(LSB_SPRINGBOARD_ENTITLEMENTS_ENABLE_ENV, disabled[i], 1));
        query(key, false, 0);
    }
    assert(!unsetenv(LSB_SPRINGBOARD_ENTITLEMENTS_ENABLE_ENV));
    query(key, false, 0);
    assert(!setenv(LSB_SPRINGBOARD_ENTITLEMENTS_ENABLE_ENV, "1", 1));
    const char *invalidRoots[] = {"relative", "/", "/definitely-missing-LSBSpringBoard-root", outside};
    for (size_t i = 0; i < sizeof(invalidRoots) / sizeof(*invalidRoots); ++i) {
        assert(!setenv("DYLD_ROOT_PATH", invalidRoots[i], 1));
        query(key, false, 0);
    }
    assert(!unsetenv("DYLD_ROOT_PATH"));
    query(key, false, 0);
    assert(!setenv("DYLD_ROOT_PATH", root, 1));
    currentExecutable = outside;
    query(key, false, 1);
    currentExecutable = "System/Library/CoreServices/SpringBoard.app/SpringBoard";
    query(key, false, 1);
    currentExecutable = executable;
    for (pathMode = 1; pathMode <= 2; ++pathMode) query(key, false, 1);
    pathMode = 0;
    assert(!unlink(executable));
    query(key, false, 1);
    assert(!symlink(outside, executable));
    query(key, false, 1); // A nominal SpringBoard path must not escape to a different executable.
    assert(!unlink(executable) && !mkdir(executable, 0700));
    query(key, false, 1);
    assert(!rmdir(executable));
    makeFile(executable);
    query(key, true, 1); // No cached grant or cached failure survives a policy/filesystem change.
    assert(!unlink(executable) && !unlink(outside));
    for (size_t i = sizeof(components) / sizeof(*components); i--;) {
        assert(snprintf(path, sizeof(path), "%s/%s", root, components[i]) < (int)sizeof(path));
        assert(!rmdir(path));
    }
    assert(!rmdir(root));
    printf("SpringBoard entitlement tests passed (%u queries)\n", checks);
    return 0;
}
