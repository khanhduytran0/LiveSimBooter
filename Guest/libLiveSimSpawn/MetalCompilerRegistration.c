#include "MetalCompilerRegistration.h"
#include "../../ProcessSupport/GuestExecutablePath.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// iOS 26 libxpc imports this bundle into the calling process's PID domain. It provides no success result.
extern void xpc_add_bundle(const char *path, uint64_t flags);
extern uint32_t dyld_get_active_platform(void);

static const char compilerService[] = "com.apple.MTLCompilerService";
static const char compilerSuffix[] = "System/Library/Frameworks/Metal.framework/XPCServices/MTLCompilerService.xpc";
static char compilerRuntimeRoot[PATH_MAX];
static atomic_bool compilerPolicyReady;
static pthread_once_t compilerRegistrationOnce = PTHREAD_ONCE_INIT;
static _Thread_local bool registeringCompiler;
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "Pre-constructor forwarding must not invoke runtime helpers");

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

static int configureCompilerRegistration(const char *enabled, const char *runtimeRoot, const char *simulatorRoot,
                                         const char *executablePath, uint32_t platform) {
    if (!enabled || strcmp(enabled, "1") || platform != 7) return 0;
    if (!runtimeRoot || !simulatorRoot || !executablePath || !LSBGuestPathIsCanonical(runtimeRoot) ||
        strcmp(runtimeRoot, simulatorRoot) || executablePath[0] != '/') return EINVAL;
    char root[PATH_MAX], executable[PATH_MAX];
    if (!realpath(runtimeRoot, root) || !realpath(executablePath, executable)) return errno;
    struct stat metadata;
    if (stat(root, &metadata)) return errno;
    if (!S_ISDIR(metadata.st_mode)) return ENOTDIR;
    // Any guest executable may need Metal, including nested app extensions and daemons. Native helper roles never load this shim.
    if (!LSBGuestExecutableIsContained(root, executable)) return EPERM;
    if (stat(executable, &metadata)) return errno;
    if (!S_ISREG(metadata.st_mode)) return EINVAL;
    memcpy(compilerRuntimeRoot, root, strlen(root) + 1);
    atomic_store_explicit(&compilerPolicyReady, true, memory_order_release);
    return 0;
}

static int registerCompilerBundle(void) {
    char expected[PATH_MAX], bundle[PATH_MAX];
    int error = appendPath(expected, compilerRuntimeRoot, compilerSuffix);
    if (error) return error;
    if (!realpath(expected, bundle)) return errno;
    if (!LSBGuestPathInsideRoot(bundle, compilerRuntimeRoot)) return EPERM;
    int directory = open(bundle, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return errno;
    error = readableBundleFile(directory, "Info.plist");
    if (!error) error = readableBundleFile(directory, "MTLCompilerService");
    close(directory);
    if (error) return error;
    xpc_add_bundle(bundle, 2);
    return 0;
}

static void registerCompilerOnce(void) {
    int error = registerCompilerBundle();
    if (error) fprintf(stderr, "[metal-compiler] registration skipped error=%d (%s)\n", error, strerror(error));
    // A returned void import is not proof that launchd accepted it; the subsequent unmodified connection reports any failure.
}

xpc_connection_t LSBMetalCompilerConnectionCreate(const char *name, dispatch_queue_t targetQueue) {
    // Interposition is active before guest TLS/constructors. Do not read errno, TLS, or environment until this acquire succeeds.
    if (atomic_load_explicit(&compilerPolicyReady, memory_order_acquire) && name && !strcmp(name, compilerService)) {
        int savedError = errno;
        if (!registeringCompiler) {
            registeringCompiler = true;
            // Serialize concurrent first requests; a same-thread reentrant XPC request forwards rather than waiting on itself.
            pthread_once(&compilerRegistrationOnce, registerCompilerOnce);
            registeringCompiler = false;
        }
        errno = savedError;
    }
    // 23A343 Metal (UUID 4C510B20-400D-397A-AE23-DAFB35D7FA0A) uses this API from both compiler implementations,
    // at offsets 0x9e88 and 0x127008. Device creation APIs alone do not cover MTLCopyAllDevices or private device paths.
    // Calls from this interposing image bind the original symbol. Return its +1 connection untouched; own no XPC object here.
    return xpc_connection_create(name, targetQueue);
}

#ifndef LSB_METAL_COMPILER_REGISTRATION_TESTING
__attribute__((constructor)) static void initializeMetalCompilerRegistration(void) {
    // This constructor only snapshots guest identity after libSystem initialization; it does no synchronous launchd work.
    const char *enabled = getenv("LSB_REGISTER_METAL_COMPILER");
    if (!enabled || strcmp(enabled, "1")) return;
    int savedError = errno;
    char executable[PATH_MAX];
    uint32_t capacity = sizeof(executable);
    int error = _NSGetExecutablePath(executable, &capacity) ? ENAMETOOLONG :
        configureCompilerRegistration(enabled, getenv("DYLD_ROOT_PATH"), getenv("SIMULATOR_ROOT"), executable, dyld_get_active_platform());
    if (error) fprintf(stderr, "[metal-compiler] policy disabled error=%d (%s)\n", error, strerror(error));
    errno = savedError;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interposition = {
    (const void *)&LSBMetalCompilerConnectionCreate, (const void *)&xpc_connection_create
};
#endif
