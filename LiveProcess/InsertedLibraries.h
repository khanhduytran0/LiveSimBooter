#pragma once

#include "../ProcessSupport/NativeRuntimePolicy.h"
#include <limits.h>

enum { LSB_INSERT_LIBRARIES_BYTES = 3 * PATH_MAX + sizeof("DYLD_INSERT_LIBRARIES=") };

// environment comes from the authenticated broker plan, which replaces both optional flags with its session decision.
// Keep the always-loaded shim C-only. Optional helpers may load guest frameworks only for an explicitly enabled session.
static inline int LSBBuildGuestInsertedLibraries(const char *root, const char *const environment[], char *output, size_t capacity) {
    if (!output || !capacity) return EINVAL;
    output[0] = '\0';
    if (!root || root[0] != '/') return EINVAL;
    size_t rootLength = strnlen(root, PATH_MAX);
    if (rootLength == PATH_MAX) return ENAMETOOLONG;
    if (strchr(root, ':')) return EINVAL; // A colon would introduce another dyld insertion entry.
    const char *paths[] = {"/usr/lib/libLiveSimSpawn.dylib", "/usr/lib/libLiveSimAudio.dylib", "/usr/lib/libLiveSimHaptics.dylib"};
    bool enabled[] = {true, LSBRuntimeBooleanEnvironment(environment, "LSB_AUDIO_OUTPUT", false),
                     LSBRuntimeBooleanEnvironment(environment, "LSB_EXPERIMENTAL_HAPTICS", false)};
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
