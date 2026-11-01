#include "../LiveProcess/InsertedLibraries.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void testInstallerRole(void) {
    // This is the stock 23A343 Info.plist's CFBundleExecutable, beneath its physical framework XPCServices directory.
    const char *installer = "/RuntimeRoot/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/"
        "com.apple.MobileInstallationHelperService.xpc/com.apple.MobileInstallationHelperService";
    char output[LSB_INSERT_LIBRARIES_BYTES], expected[1024];
    for (unsigned flags = 0; flags < 4; ++flags) {
        const char *environment[] = {flags & 1 ? "LSB_AUDIO_OUTPUT=1" : "LSB_AUDIO_OUTPUT=0",
            flags & 2 ? "LSB_EXPERIMENTAL_HAPTICS=1" : "LSB_EXPERIMENTAL_HAPTICS=0",
            "LSB_INSTALL_HELPER=0", "DYLD_INSERT_LIBRARIES=/untrusted.dylib", NULL};
        snprintf(expected, sizeof(expected), "DYLD_INSERT_LIBRARIES=/RuntimeRoot/usr/lib/libLiveSimSpawn.dylib%s%s%s",
                 flags & 1 ? ":/RuntimeRoot/usr/lib/libLiveSimAudio.dylib" : "",
                 flags & 2 ? ":/RuntimeRoot/usr/lib/libLiveSimHaptics.dylib" : "", ":/RuntimeRoot/usr/lib/libLiveSimInstall.dylib");
        assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", installer, environment, output, sizeof(output)));
        assert(!strcmp(output, expected));
        size_t size = strlen(expected) + 1;
        memset(output, 0x5a, sizeof(output));
        assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", installer, environment, output, size));
        assert(!strcmp(output, expected) && output[size] == 0x5a);
        assert(LSBBuildGuestInsertedLibraries("/RuntimeRoot", installer, environment, output, size - 1) == ENAMETOOLONG && !output[0]);
        assert(output[size] == 0x5a);
    }
    const char *unrelated[] = {"/RuntimeRoot/bin/df", "/RuntimeRoot/usr/libexec/installd",
        "/RuntimeRoot/System/Library/PrivateFrameworks/InstallCoordination.framework/Support/installcoordinationd",
        "/data/Containers/Bundle/Application/container/App.app/com.apple.MobileInstallationHelperService",
        "/RuntimeRoot/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/"
            "com.apple.MobileInstallationHelperService.xpc/Other",
        "/RuntimeRoot/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/"
            "com.apple.MobileInstallationHelperService.xpc/com.apple.MobileInstallationHelperService.extra",
        "/RuntimeRoot/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/"
            "com.apple.MobileInstallationHelperService.xpc/nested/com.apple.MobileInstallationHelperService",
        installer + strlen("/RuntimeRoot")};
    const char *spoofed[] = {"LSB_INSTALL_HELPER=1", "LSB_REGISTER_INSTALLER=1", "LSB_EXECUTABLE_PATH=MobileInstallationHelperService",
        "DYLD_INSERT_LIBRARIES=/RuntimeRoot/usr/lib/libLiveSimInstall.dylib", NULL};
    for (unsigned i = 0; i < sizeof(unrelated) / sizeof(*unrelated); ++i) {
        assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", unrelated[i], spoofed, output, sizeof(output)));
        assert(!strcmp(output, "DYLD_INSERT_LIBRARIES=/RuntimeRoot/usr/lib/libLiveSimSpawn.dylib"));
    }
    assert(!LSBBuildGuestInsertedLibraries("/OtherRoot", installer, spoofed, output, sizeof(output)));
    assert(!strcmp(output, "DYLD_INSERT_LIBRARIES=/OtherRoot/usr/lib/libLiveSimSpawn.dylib"));
    assert(LSBBuildGuestInsertedLibraries("/RuntimeRoot", NULL, spoofed, output, sizeof(output)) == EINVAL && !output[0]);
    assert(LSBBuildGuestInsertedLibraries("/RuntimeRoot", "relative", spoofed, output, sizeof(output)) == EINVAL && !output[0]);
}

int main(void) {
    char output[LSB_INSERT_LIBRARIES_BYTES];
    const char *base = "DYLD_INSERT_LIBRARIES=/RuntimeRoot/usr/lib/libLiveSimSpawn.dylib";
    assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", "/RuntimeRoot/bin/df", NULL, output, sizeof(output)) && !strcmp(output, base));
    for (unsigned flags = 0; flags < 4; ++flags) {
        const char *environment[] = {flags & 1 ? "LSB_AUDIO_OUTPUT=1" : "LSB_AUDIO_OUTPUT=0",
            flags & 2 ? "LSB_EXPERIMENTAL_HAPTICS=1" : "LSB_EXPERIMENTAL_HAPTICS=0", "DYLD_INSERT_LIBRARIES=/untrusted.dylib", NULL};
        assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", "/RuntimeRoot/bin/df", environment, output, sizeof(output)));
        char expected[512];
        snprintf(expected, sizeof(expected), "%s%s%s", base, flags & 1 ? ":/RuntimeRoot/usr/lib/libLiveSimAudio.dylib" : "",
                 flags & 2 ? ":/RuntimeRoot/usr/lib/libLiveSimHaptics.dylib" : "");
        assert(!strcmp(output, expected));
        size_t size = strlen(expected) + 1;
        memset(output, 0x5a, sizeof(output));
        assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", "/RuntimeRoot/bin/df", environment, output, size));
        assert(!strcmp(output, expected) && output[size] == 0x5a);
        assert(LSBBuildGuestInsertedLibraries("/RuntimeRoot", "/RuntimeRoot/bin/df", environment, output, size - 1) == ENAMETOOLONG && !output[0]);
        assert(output[size] == 0x5a);
    }
    const char *invalid[] = {"LSB_AUDIO_OUTPUT_SUFFIX=1", "LSB_AUDIO_OUTPUT=true", "LSB_AUDIO_OUTPUT=1",
                            "LSB_EXPERIMENTAL_HAPTICS=01", "LSB_EXPERIMENTAL_HAPTICS=1", NULL};
    assert(!LSBBuildGuestInsertedLibraries("/RuntimeRoot", "/RuntimeRoot/bin/df", invalid, output, sizeof(output)) && !strcmp(output, base));
    assert(LSBBuildGuestInsertedLibraries(NULL, "/RuntimeRoot/bin/df", NULL, output, sizeof(output)) == EINVAL && !output[0]);
    assert(LSBBuildGuestInsertedLibraries("relative", "/RuntimeRoot/bin/df", NULL, output, sizeof(output)) == EINVAL && !output[0]);
    assert(LSBBuildGuestInsertedLibraries("/root:/another", "/RuntimeRoot/bin/df", NULL, output, sizeof(output)) == EINVAL && !output[0]);
    assert(LSBBuildGuestInsertedLibraries("/RuntimeRoot", "/RuntimeRoot/bin/df", NULL, NULL, sizeof(output)) == EINVAL);
    assert(LSBBuildGuestInsertedLibraries("/RuntimeRoot", "/RuntimeRoot/bin/df", NULL, output, 0) == EINVAL);
    char root[PATH_MAX + 1];
    memset(root, 'a', sizeof(root));
    root[0] = '/'; root[PATH_MAX] = '\0';
    assert(LSBBuildGuestInsertedLibraries(root, "/RuntimeRoot/bin/df", NULL, output, sizeof(output)) == ENAMETOOLONG && !output[0]);
    const char *both[] = {"LSB_AUDIO_OUTPUT=1", "LSB_EXPERIMENTAL_HAPTICS=1", NULL};
    size_t longest = strlen("/usr/lib/libLiveSimHaptics.dylib");
    root[PATH_MAX - longest] = '\0';
    assert(LSBBuildGuestInsertedLibraries(root, "/RuntimeRoot/bin/df", both, output, sizeof(output)) == ENAMETOOLONG && !output[0]);
    root[PATH_MAX - longest - 1] = '\0';
    assert(!LSBBuildGuestInsertedLibraries(root, "/RuntimeRoot/bin/df", both, output, sizeof(output)));
    assert(strlen(output) < sizeof(output));
    testInstallerRole();
    puts("InsertedLibraries: fixed C shim, exact optional flags, stock installer role, and bounded physical paths passed");
    return 0;
}
