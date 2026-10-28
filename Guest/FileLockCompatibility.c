#include "FileLockCompatibility.h"
#include "SpawnDiagnostics.h"
#include "../ProcessSupport/RuntimeLayout.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#ifdef LSB_TRANSPORT_TRACE
#include <stdatomic.h>
#endif

extern int lsb_native_fcntl_nocancel(int descriptor, int command, ...) __asm("_fcntl$NOCANCEL");
extern int __fcntl(int descriptor, int command, uintptr_t argument);
extern int __fcntl_nocancel(int descriptor, int command, uintptr_t argument);
// Exported by libSystem, but the simulator SDK omits mach/mach_vm.h.
extern kern_return_t mach_vm_read_overwrite(vm_map_t task, mach_vm_address_t source, mach_vm_size_t size,
                                            mach_vm_address_t destination, mach_vm_size_t *copied);

static char fileLockRoot[PATH_MAX];
static size_t fileLockRootLength;
#ifdef LSB_TRANSPORT_TRACE
static _Atomic(unsigned) fileLockTraceCount;
#endif

static bool cleanAbsolutePath(const char *path) {
    if (!path || path[0] != '/' || !path[1] || strnlen(path, PATH_MAX) == PATH_MAX || strchr(path, ':')) return false;
    for (const char *part = path + 1; *part;) {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') || (size == 2 && part[0] == '.' && part[1] == '.') || (end && !end[1])) return false;
        part += size + (end != NULL);
    }
    return true;
}

static void configureFileLockScope(const char *enabled, const char *root) {
    fileLockRootLength = 0;
    fileLockRoot[0] = '\0';
    if (!enabled || strcmp(enabled, "1") || !cleanAbsolutePath(root)) return;
    char canonical[PATH_MAX];
    if (!realpath(root, canonical)) return;
    const char *leaf = strrchr(canonical, '/');
    if (!leaf || strcmp(leaf, "/" LSB_DATA_DIRECTORY)) return;
    struct stat status;
    if (stat(canonical, &status) || !S_ISDIR(status.st_mode) || status.st_uid != geteuid()) return;
    char sibling[PATH_MAX];
    int size = snprintf(sibling, sizeof(sibling), "%.*s/" LSB_RUNTIME_DIRECTORY, (int)(leaf - canonical), canonical);
    if (size <= 0 || (size_t)size >= sizeof(sibling)) return;
    int runtime = open(sibling, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (runtime < 0) return;
    bool valid = !fstat(runtime, &status) && S_ISDIR(status.st_mode) && status.st_uid == geteuid();
    close(runtime);
    if (!valid) return;
    size_t length = strlen(canonical);
    memcpy(fileLockRoot, canonical, length + 1);
    fileLockRootLength = length;
}

#ifndef LSB_FILE_LOCK_TESTING
__attribute__((constructor)) static void initializeFileLockScope(void) {
    int saved = errno;
    configureFileLockScope(getenv(LSB_FILE_LOCK_ENABLE_ENV), getenv(LSB_FILE_LOCK_ROOT_ENV));
    errno = saved;
}
#endif

void LSBFileLockPrepare(int descriptor) {
    if (!fileLockRootLength) return; // Early libSystem calls must not consult TLS/environment before the shim constructor.
    int saved = errno;
    struct stat opened, named;
    char path[PATH_MAX], canonical[PATH_MAX];
    if (fstat(descriptor, &opened) || !S_ISREG(opened.st_mode) || opened.st_uid != geteuid() || opened.st_nlink != 1) goto done;
    if (fcntl(descriptor, F_GETPATH, path)) goto done;
    path[sizeof(path) - 1] = '\0';
    if (!cleanAbsolutePath(path) || !realpath(path, canonical)) goto done;
    if (strncmp(canonical, fileLockRoot, fileLockRootLength) || canonical[fileLockRootLength] != '/') goto done;
    // Resolve /var versus /private/var and symlinks, then verify that the path still names the descriptor being annotated.
    if (stat(canonical, &named) || named.st_dev != opened.st_dev || named.st_ino != opened.st_ino) goto done;
    unsigned char value = 0;
    ssize_t length = fgetxattr(descriptor, LSB_FILE_LOCK_ATTRIBUTE, &value, sizeof(value), 0, 0);
    if (length >= 0 || errno != ENOATTR) goto done; // Preserve an explicit existing policy, including zero/malformed values.
    // WebKit's SQLiteFileSystem uses one byte 0xff. Native 23B50 RunningBoard accepts exactly one nonzero byte before checking locks.
    // https://github.com/WebKit/WebKit/blob/main/Source/WebCore/platform/sql/SQLiteFileSystem.cpp
    value = 0xff;
    int result = fsetxattr(descriptor, LSB_FILE_LOCK_ATTRIBUTE, &value, sizeof(value), 0, XATTR_CREATE);
#ifdef LSB_TRANSPORT_TRACE
    if (atomic_fetch_add_explicit(&fileLockTraceCount, 1, memory_order_relaxed) < 32)
        LSBSpawnTrace("[file-lock] fd=%d mark-error=%d path=%s\n", descriptor, result ? errno : 0, canonical);
#else
    (void)result;
#endif
done:
    errno = saved;
}

static void prepareRecordLock(int descriptor, int command, uintptr_t argument) {
    if (!fileLockRootLength) return;
    switch (command) {
        case F_SETLK: case F_SETLKW: case F_SETLKWTIMEOUT:
        case F_OFD_SETLK: case F_OFD_SETLKW: case F_OFD_SETLKWTIMEOUT:
            break;
        default: return;
    }
    int saved = errno;
    struct flock lock;
    mach_vm_size_t copied = 0;
    // Timeout variants start with struct flock. A kernel copy preserves EFAULT handling for invalid caller pointers.
    kern_return_t result = mach_vm_read_overwrite(mach_task_self(), argument, sizeof(lock), (mach_vm_address_t)(uintptr_t)&lock, &copied);
    if (result == KERN_SUCCESS && copied == sizeof(lock) && (lock.l_type == F_RDLCK || lock.l_type == F_WRLCK)) LSBFileLockPrepare(descriptor);
    errno = saved;
}

int LSBFileLockFlock(int descriptor, int operation) {
    int kind = operation & ~LOCK_NB;
    if (kind == LOCK_SH || kind == LOCK_EX) LSBFileLockPrepare(descriptor);
    return flock(descriptor, operation);
}

__attribute__((used)) static int fileLockFCntlArgument(int descriptor, int command, uintptr_t argument) {
    prepareRecordLock(descriptor, command, argument);
    return fcntl(descriptor, command, argument);
}

__attribute__((used)) static int fileLockFCntlNoCancelArgument(int descriptor, int command, uintptr_t argument) {
    prepareRecordLock(descriptor, command, argument);
    return lsb_native_fcntl_nocancel(descriptor, command, argument);
}

// Darwin arm64 varargs are stack arguments. Keep the exact third slot without reading a nonexistent C va_arg on two-argument commands.
// Forward through the corresponding original fcntl entry so it retains command-specific integer/pointer conversion.
__attribute__((naked)) int LSBFileLockFCntl(int descriptor, int command, ...) {
    __asm__("ldr x2, [sp]\n"
            "b _fileLockFCntlArgument");
}

__attribute__((naked)) int LSBFileLockFCntlNoCancel(int descriptor, int command, ...) {
    __asm__("ldr x2, [sp]\n"
            "b _fileLockFCntlNoCancelArgument");
}

int LSBFileLockFCntlRaw(int descriptor, int command, uintptr_t argument) {
    prepareRecordLock(descriptor, command, argument);
    return __fcntl(descriptor, command, argument);
}

int LSBFileLockFCntlRawNoCancel(int descriptor, int command, uintptr_t argument) {
    prepareRecordLock(descriptor, command, argument);
    return __fcntl_nocancel(descriptor, command, argument);
}

#ifndef LSB_FILE_LOCK_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBFileLockFlock, (const void *)&flock}, {(const void *)&LSBFileLockFCntl, (const void *)&fcntl},
    {(const void *)&LSBFileLockFCntlNoCancel, (const void *)&lsb_native_fcntl_nocancel},
    {(const void *)&LSBFileLockFCntlRaw, (const void *)&__fcntl}, {(const void *)&LSBFileLockFCntlRawNoCancel, (const void *)&__fcntl_nocancel}
};
#endif
