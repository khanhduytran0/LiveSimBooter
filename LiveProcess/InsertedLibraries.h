#pragma once

#include "../ProcessSupport/NativeRuntimePolicy.h"
#include <limits.h>

enum { LSB_INSERT_LIBRARIES_BYTES = 4 * PATH_MAX + sizeof("DYLD_INSERT_LIBRARIES=") };

// environment comes from the authenticated broker plan, which replaces both optional flags with its session decision.
// root and executable are canonical physical paths resolved from that plan. Keep the always-loaded shim C-only; the installer
// and scene helpers load Objective-C only in their exact stock executable roles, never from an environment-selected role.
static inline int LSBBuildGuestInsertedLibraries(const char *root, const char *executable, const char *const environment[],
                                                char *output, size_t capacity) {
    if (!output || !capacity) return EINVAL;
    output[0] = '\0';
    if (!root || root[0] != '/' || !executable || executable[0] != '/') return EINVAL;
    size_t rootLength = strnlen(root, PATH_MAX);
    if (rootLength == PATH_MAX) return ENAMETOOLONG;
    if (strchr(root, ':')) return EINVAL; // A colon would introduce another dyld insertion entry.
    static const char installer[] = "/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/"
        "com.apple.MobileInstallationHelperService.xpc/com.apple.MobileInstallationHelperService";
    bool installerRole = !strncmp(executable, root, rootLength) && !strcmp(executable + rootLength, installer);
    bool springBoardRole = !strncmp(executable, root, rootLength) &&
        !strcmp(executable + rootLength, "/System/Library/CoreServices/SpringBoard.app/SpringBoard");
    const char *paths[] = {"/usr/lib/libLiveSimSpawn.dylib", "/usr/lib/libLiveSimAudio.dylib", "/usr/lib/libLiveSimHaptics.dylib",
                          "/usr/lib/libLiveSimInstall.dylib", "/usr/lib/libLiveSimScene.dylib"};
    bool enabled[] = {true, LSBRuntimeBooleanEnvironment(environment, "LSB_AUDIO_OUTPUT", false),
                     LSBRuntimeBooleanEnvironment(environment, "LSB_EXPERIMENTAL_HAPTICS", false), installerRole, springBoardRole};
    const char prefix[] = "DYLD_INSERT_LIBRARIES=";
    size_t required = sizeof(prefix), count = 0;
    for (unsigned index = 0; index < sizeof(paths) / sizeof(*paths); ++index) if (enabled[index]) {
        size_t length = strlen(paths[index]);
        if (rootLength >= PATH_MAX - length) return ENAMETOOLONG;
        required += rootLength + length + (count++ != 0);
    }
    if (required > capacity) return ENAMETOOLONG;
    memcpy(output, prefix, sizeof(prefix) - 1);
    char *cursor = output + sizeof(prefix) - 1;
    count = 0;
    for (unsigned index = 0; index < sizeof(paths) / sizeof(*paths); ++index) if (enabled[index]) {
        if (count++) *cursor++ = ':';
        memcpy(cursor, root, rootLength);
        cursor += rootLength;
        size_t length = strlen(paths[index]);
        memcpy(cursor, paths[index], length);
        cursor += length;
    }
    *cursor = '\0';
    return 0;
}
