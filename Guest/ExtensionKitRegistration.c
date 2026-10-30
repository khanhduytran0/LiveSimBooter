#include "ExtensionKitRegistration.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// iOS 26 libxpc imports this bundle into the calling process's PID domain. It provides no success result.
// BoardServices Discovery and Launch resolve this domain directly, without the CFBundle resource path adapter.
extern void xpc_add_bundle(const char *path, uint64_t flags);

static int appendPath(char output[PATH_MAX], const char *root, const char *suffix) {
    int length = snprintf(output, PATH_MAX, "%s/%s", root, suffix);
    return length < 0 || length >= PATH_MAX ? ENAMETOOLONG : 0;
}

static int readableBundleFile(int directory, const char *name) {
    int descriptor = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return errno;
    struct stat metadata;
    int error = fstat(descriptor, &metadata) ? errno : !S_ISREG(metadata.st_mode) || !metadata.st_size ? EINVAL : 0;
    close(descriptor);
    return error;
}

int LSBRegisterExtensionKitForExecutable(const char *enabled, const char *runtimeRoot, const char *executablePath, bool *requested) {
    if (!requested) return EINVAL;
    *requested = false;
    if (!enabled || strcmp(enabled, "1")) return 0;
    if (!runtimeRoot || runtimeRoot[0] != '/' || !executablePath || executablePath[0] != '/') return EINVAL;
    char root[PATH_MAX], executable[PATH_MAX], expected[PATH_MAX], bundle[PATH_MAX];
    if (!realpath(runtimeRoot, root)) return errno;
    size_t rootLength = strlen(root);
    if (rootLength <= 1) return EINVAL;
    struct stat metadata;
    if (stat(root, &metadata)) return errno;
    if (!S_ISDIR(metadata.st_mode)) return ENOTDIR;
    if (!realpath(executablePath, executable)) return errno;
    // An inherited option cannot register a domain for launchd, xpcproxy, the service, or an unrelated executable.
    // 23A343 chronod's widget-provider startup resolves extensionkitservice's Launch endpoint in its own PID domain.
    const char *roles[] = {"System/Library/CoreServices/SpringBoard.app/SpringBoard", "Applications/PosterBoard.app/PosterBoard",
                          "System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod"};
    bool recognized = false;
    int error;
    for (unsigned i = 0; i < sizeof(roles) / sizeof(*roles); ++i) {
        error = appendPath(expected, root, roles[i]);
        if (error) return error;
        if (!strcmp(executable, expected)) { recognized = true; break; }
    }
    if (!recognized) return 0;
    if (stat(executable, &metadata)) return errno;
    if (!S_ISREG(metadata.st_mode)) return EINVAL;
    error = appendPath(expected, root, "System/Library/Frameworks/ExtensionFoundation.framework/XPCServices/extensionkitservice.xpc");
    if (error) return error;
    if (!realpath(expected, bundle)) return errno;
    if (strncmp(bundle, root, rootLength) || bundle[rootLength] != '/') return EPERM;
    int directory = open(bundle, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return errno;
    error = readableBundleFile(directory, "Info.plist");
    if (!error) error = readableBundleFile(directory, "extensionkitservice");
    close(directory);
    if (error) return error;
    fprintf(stderr, "[extensionkit] registration request path=%s flags=2\n", bundle);
    xpc_add_bundle(bundle, 2);
    *requested = true;
    fprintf(stderr, "[extensionkit] registration returned; success unconfirmed (void API)\n");
    return 0;
}

#ifndef LSB_EXTENSION_KIT_REGISTRATION_TESTING
__attribute__((constructor)) static void initializeExtensionKitRegistration(void) {
    const char *enabled = getenv("LSB_REGISTER_EXTENSIONKIT");
    if (!enabled || strcmp(enabled, "1")) return;
    // The inserted dylib's libSystem dependency is initialized first; this runs after the native-cache handoff.
    // Do not call from a dyld image callback, where a synchronous launchd round trip could reenter loader state.
    int savedError = errno;
    char executable[PATH_MAX];
    uint32_t capacity = sizeof(executable);
    bool requested;
    int error = _NSGetExecutablePath(executable, &capacity) ? ENAMETOOLONG :
        LSBRegisterExtensionKitForExecutable(enabled, getenv("DYLD_ROOT_PATH"), executable, &requested);
    if (error) fprintf(stderr, "[extensionkit] registration skipped error=%d (%s)\n", error, strerror(error));
    errno = savedError;
}
#endif
