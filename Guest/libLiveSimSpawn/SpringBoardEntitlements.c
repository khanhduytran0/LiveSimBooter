#include "SpringBoardEntitlements.h"

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool isSpringBoardExecutable(void) {
    const char *enabled = getenv(LSB_SPRINGBOARD_ENTITLEMENTS_ENABLE_ENV);
    const char *root = getenv("DYLD_ROOT_PATH");
    if (!enabled || strcmp(enabled, "1") || !root || root[0] != '/') return false;
    char canonicalRoot[PATH_MAX], executable[PATH_MAX] = {0}, canonicalExecutable[PATH_MAX], expected[PATH_MAX];
    struct stat info;
    if (!realpath(root, canonicalRoot) || !canonicalRoot[1] || stat(canonicalRoot, &info) || !S_ISDIR(info.st_mode)) return false;
    int count = snprintf(expected, sizeof(expected), "%s/System/Library/CoreServices/SpringBoard.app/SpringBoard", canonicalRoot);
    if (count < 0 || (size_t)count >= sizeof(expected)) return false;
    uint32_t capacity = sizeof(executable);
    if (_NSGetExecutablePath(executable, &capacity) || !memchr(executable, 0, sizeof(executable)) || executable[0] != '/' ||
        !realpath(executable, canonicalExecutable) || strcmp(canonicalExecutable, expected)) return false;
    return !stat(canonicalExecutable, &info) && S_ISREG(info.st_mode);
}

bool LSBSpringBoardSelfTaskHasEntitlement(CFStringRef entitlement) {
    // Original calls from this image bind to BaseBoard, not this replacement. Missing weak imports fail closed.
    if (!BSSelfTaskHasEntitlement) return false;
    bool result = BSSelfTaskHasEntitlement(entitlement);
    if (result) return true;
    int savedError = errno;
    if (entitlement && CFEqual(entitlement, CFSTR(LSB_SPRINGBOARD_UI_ENTITLEMENT))) result = isSpringBoardExecutable();
    errno = savedError;
    return result;
}

#ifndef LSB_SPRINGBOARD_ENTITLEMENTS_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interposition = {
    (const void *)&LSBSpringBoardSelfTaskHasEntitlement, (const void *)&BSSelfTaskHasEntitlement
};
#endif
