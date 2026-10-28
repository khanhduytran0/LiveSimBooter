#include "../LiveProcess/InsertedLibraries.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    char output[LSB_INSERT_LIBRARIES_BYTES];
    const char *base = "DYLD_INSERT_LIBRARIES=/RuntimeRoot/usr/lib/libLiveSimSpawn.dylib";
    assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", NULL, output, sizeof(output)) && !strcmp(output, base));
    for (unsigned flags = 0; flags < 4; ++flags) {
        const char *environment[] = {flags & 1 ? "LSB_AUDIO_OUTPUT=1" : "LSB_AUDIO_OUTPUT=0",
            flags & 2 ? "LSB_EXPERIMENTAL_HAPTICS=1" : "LSB_EXPERIMENTAL_HAPTICS=0", "DYLD_INSERT_LIBRARIES=/untrusted.dylib", NULL};
        assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", environment, output, sizeof(output)));
        char expected[512];
        snprintf(expected, sizeof(expected), "%s%s%s", base, flags & 1 ? ":/RuntimeRoot/usr/lib/libLiveSimAudio.dylib" : "",
                 flags & 2 ? ":/RuntimeRoot/usr/lib/libLiveSimHaptics.dylib" : "");
        assert(!strcmp(output, expected));
        size_t size = strlen(expected) + 1;
        memset(output, 0x5a, sizeof(output));
        assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", environment, output, size));
        assert(!strcmp(output, expected) && output[size] == 0x5a);
        assert(LSBBuildGuestInsertedLibraries("/RuntimeRoot", environment, output, size - 1) == ENAMETOOLONG && !output[0]);
        assert(output[size] == 0x5a);
    }
    const char *invalid[] = {"LSB_AUDIO_OUTPUT_SUFFIX=1", "LSB_AUDIO_OUTPUT=true", "LSB_AUDIO_OUTPUT=1",
                            "LSB_EXPERIMENTAL_HAPTICS=01", "LSB_EXPERIMENTAL_HAPTICS=1", NULL};
    assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", invalid, output, sizeof(output)) && !strcmp(output, base));
    assert(LSBBuildGuestInsertedLibraries(NULL, NULL, output, sizeof(output)) == EINVAL && !output[0]);
    assert(LSBBuildGuestInsertedLibraries("relative", NULL, output, sizeof(output)) == EINVAL && !output[0]);
    assert(LSBBuildGuestInsertedLibraries("/root:/another", NULL, output, sizeof(output)) == EINVAL && !output[0]);
    assert(LSBBuildGuestInsertedLibraries("/RuntimeRoot", NULL, NULL, sizeof(output)) == EINVAL);
    assert(LSBBuildGuestInsertedLibraries("/RuntimeRoot", NULL, output, 0) == EINVAL);
    char root[PATH_MAX + 1];
    memset(root, 'a', sizeof(root));
    root[0] = '/'; root[PATH_MAX] = '\0';
    assert(LSBBuildGuestInsertedLibraries(root, NULL, output, sizeof(output)) == ENAMETOOLONG && !output[0]);
    const char *both[] = {"LSB_AUDIO_OUTPUT=1", "LSB_EXPERIMENTAL_HAPTICS=1", NULL};
    size_t longest = strlen("/usr/lib/libLiveSimHaptics.dylib");
    root[PATH_MAX - longest] = '\0';
    assert(LSBBuildGuestInsertedLibraries(root, both, output, sizeof(output)) == ENAMETOOLONG && !output[0]);
    root[PATH_MAX - longest - 1] = '\0';
    assert(!LSBBuildGuestInsertedLibraries(root, both, output, sizeof(output)));
    assert(strlen(output) < sizeof(output));
    puts("InsertedLibraries: fixed C shim, exact optional session flags, and bounded physical paths passed");
    return 0;
}
