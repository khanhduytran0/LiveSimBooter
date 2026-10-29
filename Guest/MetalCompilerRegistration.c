#include "MetalCompilerRegistration.h"

#include <CoreFoundation/CoreFoundation.h>
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

static int applicationMainExecutable(const char *root, const char *executable, bool *allowed) {
    *allowed = false;
    size_t rootLength = strlen(root);
    if (strncmp(executable, root, rootLength) || executable[rootLength] != '/') return 0;
    const char *relative = executable + rootLength + 1, *app = NULL;
    const char *locations[] = {"Applications/", "System/Applications/", "System/Library/CoreServices/"};
    for (unsigned i = 0; i < sizeof(locations) / sizeof(*locations); ++i) {
        size_t length = strlen(locations[i]);
        if (!strncmp(relative, locations[i], length)) { app = relative + length; break; }
    }
    if (!app) return 0;
    const char *separator = strchr(app, '/');
    if (!separator || separator - app <= 4 || memcmp(separator - 4, ".app", 4)) return 0;
    const char *name = separator + 1;
    if (!*name || strchr(name, '/')) return 0;

    char appPath[PATH_MAX];
    size_t length = (size_t)(separator - executable);
    memcpy(appPath, executable, length);
    appPath[length] = 0;
    int directory = open(appPath, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return errno;
    int descriptor = openat(directory, "Info.plist", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    int error = descriptor < 0 ? errno : 0;
    close(directory);
    if (error) return error;
    struct stat metadata;
    if (fstat(descriptor, &metadata)) error = errno;
    else if (!S_ISREG(metadata.st_mode) || metadata.st_size <= 0 || metadata.st_size > 1024 * 1024) error = EINVAL;
    size_t size = error ? 0 : (size_t)metadata.st_size;
    uint8_t *bytes = error ? NULL : malloc(size);
    if (!error && !bytes) error = ENOMEM;
    size_t offset = 0;
    while (!error && offset < size) {
        ssize_t count = read(descriptor, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) error = count < 0 ? errno : EINVAL;
        else offset += (size_t)count;
    }
    close(descriptor);
    if (error) { free(bytes); return error; }
    CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes, (CFIndex)size, kCFAllocatorNull);
    CFPropertyListRef plist = data ? CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL) : NULL;
    if (!data) error = ENOMEM;
    else if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) error = EINVAL;
    else {
        CFTypeRef main = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("CFBundleExecutable"));
        CFTypeRef type = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("CFBundlePackageType"));
        char declaredName[PATH_MAX];
        if (!main || CFGetTypeID(main) != CFStringGetTypeID() || !type || !CFEqual(type, CFSTR("APPL")) ||
            !CFStringGetCString((CFStringRef)main, declaredName, sizeof(declaredName), kCFStringEncodingUTF8) ||
            !*declaredName || strchr(declaredName, '/') || !strcmp(declaredName, ".") || !strcmp(declaredName, "..")) error = EINVAL;
        else {
            CFStringRef actual = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
            if (!actual) error = EINVAL;
            else { *allowed = CFEqual(main, actual); CFRelease(actual); }
        }
    }
    if (plist) CFRelease(plist);
    if (data) CFRelease(data);
    free(bytes);
    return error;
}

int LSBRegisterMetalCompilerForExecutable(const char *enabled, const char *runtimeRoot, const char *executablePath, bool *requested) {
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
    // WebKit's GPU extension specializes QuartzCore shaders in its own PID domain, independently of its UI application's import.
    const char *roles[] = {"usr/libexec/backboardd", "System/Library/CoreServices/SpringBoard.app/SpringBoard",
                           "System/Library/CoreServices/iconservicesagent",
                           "System/Library/ExtensionKit/Extensions/GPUExtension.appex/com.apple.WebKit.GPU"};
    bool allowed = false;
    int error;
    for (unsigned i = 0; i < sizeof(roles) / sizeof(*roles); ++i) {
        error = appendPath(expected, root, roles[i]);
        if (error) return error;
        if (!strcmp(executable, expected)) { allowed = true; break; }
    }
    // Cached Metal's virtual image path cannot import its XPC bundle into a physical app's PID domain. Register only
    // validated app main executables beyond the exact roles above; inherited options exclude launchd, xpcproxy and unrelated nested services.
    if (!allowed) {
        error = applicationMainExecutable(root, executable, &allowed);
        if (error) return error;
    }
    if (!allowed) return 0;
    if (stat(executable, &metadata)) return errno;
    if (!S_ISREG(metadata.st_mode)) return EINVAL;
    error = appendPath(expected, root, "System/Library/Frameworks/Metal.framework/XPCServices/MTLCompilerService.xpc");
    if (error) return error;
    if (!realpath(expected, bundle)) return errno;
    if (strncmp(bundle, root, rootLength) || bundle[rootLength] != '/') return EPERM;
    int directory = open(bundle, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return errno;
    error = readableBundleFile(directory, "Info.plist");
    if (!error) error = readableBundleFile(directory, "MTLCompilerService");
    close(directory);
    if (error) return error;
    fprintf(stderr, "[metal-compiler] registration request path=%s flags=2\n", bundle);
    xpc_add_bundle(bundle, 2);
    *requested = true;
    fprintf(stderr, "[metal-compiler] registration returned; success unconfirmed (void API)\n");
    return 0;
}

#ifndef LSB_METAL_COMPILER_REGISTRATION_TESTING
__attribute__((constructor)) static void initializeMetalCompilerRegistration(void) {
    const char *enabled = getenv("LSB_REGISTER_METAL_COMPILER");
    if (!enabled || strcmp(enabled, "1")) return;
    // The inserted dylib's libSystem dependency is initialized first; this runs after the native-cache handoff.
    // Do not call from a dyld image callback, where a synchronous launchd round trip could reenter loader state.
    int savedError = errno;
    char executable[PATH_MAX];
    uint32_t capacity = sizeof(executable);
    bool requested;
    int error = _NSGetExecutablePath(executable, &capacity) ? ENAMETOOLONG :
        LSBRegisterMetalCompilerForExecutable(enabled, getenv("DYLD_ROOT_PATH"), executable, &requested);
    if (error) fprintf(stderr, "[metal-compiler] registration skipped error=%d (%s)\n", error, strerror(error));
    errno = savedError;
}
#endif
