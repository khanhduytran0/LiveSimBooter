#include "LaunchdJobIdentity.h"
#include "SpawnDiagnostics.h"
#include "../../ProcessSupport/PrivateXPC.h"
#include "../../ProcessSupport/SpawnTransport.h"

#include <errno.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// iOS 26 23A343 arm64 launchd_sim, UUID C3EC5C45-8C85-3D11-B59C-62C4248F5EF9.
// Reverse-engineered from its OSLaunchdJob state serializer and routine 1008/712 handlers; see docs/launchd.md.
// Offsets are return addresses after xpc_dictionary_set/get_int64, relative to its mapped Mach-O header.
enum { JobPIDSetterReturn = 0x33fec, CopyJobPIDGetterReturn = 0x33090, ServicePIDGetterReturn = 0x1bcb4 };
static const unsigned char launchdUUID[16] = {0xc3, 0xec, 0x5c, 0x45, 0x8c, 0x85, 0x3d, 0x11,
                                             0xb5, 0x9c, 0x62, 0xc4, 0x24, 0x8f, 0x5e, 0xf9};
static _Atomic(uintptr_t) launchdBase;
static char launchdExecutablePath[PATH_MAX];
static bool jobIdentityScoped;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "Early XPC caller checks must not require runtime initialization");

static const char *pinnedLaunchdImageError(const struct mach_header_64 *header) {
    if (!header || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64 || header->filetype != MH_EXECUTE ||
        !header->ncmds || header->ncmds > 4096 || header->sizeofcmds > 1048576) return "header";
    const unsigned char *command = (const unsigned char *)(header + 1);
    size_t remaining = header->sizeofcmds;
    bool uuid = false, text = false;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (remaining < sizeof(struct load_command)) return "truncated load commands";
        const struct load_command *load = (const struct load_command *)command;
        if (load->cmdsize < sizeof(*load) || load->cmdsize > remaining) return "load command size";
        if (load->cmd == LC_UUID) {
            if (uuid || load->cmdsize != sizeof(struct uuid_command) ||
                memcmp(((const struct uuid_command *)load)->uuid, launchdUUID, sizeof(launchdUUID))) return "UUID";
            uuid = true;
        } else if (load->cmd == LC_SEGMENT_64) {
            if (load->cmdsize < sizeof(struct segment_command_64)) return "segment command size";
            const struct segment_command_64 *segment = (const struct segment_command_64 *)load;
            if (!strncmp(segment->segname, "__TEXT", sizeof(segment->segname))) {
                if (text || segment->vmaddr != UINT64_C(0x100000000) || segment->fileoff ||
                    segment->vmsize <= JobPIDSetterReturn || segment->filesize <= JobPIDSetterReturn ||
                    (segment->initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE)) return "text geometry/protection";
                text = true;
            }
        }
        command += load->cmdsize;
        remaining -= load->cmdsize;
    }
    if (!uuid || !text || remaining) return "missing text/UUID or trailing commands";
    const unsigned offsets[] = {JobPIDSetterReturn, CopyJobPIDGetterReturn, ServicePIDGetterReturn};
    const uint32_t instructions[] = {0x94002b69, 0x94002f25, 0x94008c1c}; // BL to pinned set/get_int64 stubs, respectively.
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
        uint32_t instruction;
        memcpy(&instruction, (const unsigned char *)header + offsets[i] - 4, sizeof(instruction));
        if (instruction != instructions[i]) return i == 0 ? "setter BL" : i == 1 ? "copy-job getter BL" : "service getter BL";
    }
    return NULL;
}

static void configureJobScope(const char *enabled, const char *root, const char *executable) {
    atomic_store_explicit(&launchdBase, 0, memory_order_release);
    jobIdentityScoped = false;
    launchdExecutablePath[0] = '\0';
    if (!enabled || strcmp(enabled, "1") || !root || root[0] != '/' || !root[1] || !executable) return;
    size_t length = strnlen(root, PATH_MAX);
    if (length == PATH_MAX || root[length - 1] == '/' || strncmp(executable, root, length) ||
        strcmp(executable + length, "/sbin/launchd_sim") || strnlen(executable, PATH_MAX) == PATH_MAX) return;
    strcpy(launchdExecutablePath, executable);
    jobIdentityScoped = true;
}

static void rememberJobImage(const struct mach_header *image, const char *path) {
    if (!jobIdentityScoped || !path) return;
    int saved = errno;
    char physical[PATH_MAX];
    bool matches = !strcmp(path, "/sbin/launchd_sim") || !strcmp(path, launchdExecutablePath);
    if (!matches && realpath(path, physical)) matches = !strcmp(physical, launchdExecutablePath);
    if (matches) {
        const char *error = pinnedLaunchdImageError((const struct mach_header_64 *)image);
        if (!error) atomic_store_explicit(&launchdBase, (uintptr_t)image, memory_order_release);
#ifdef LSB_TRANSPORT_TRACE
        static _Atomic(unsigned) traces;
        if (atomic_fetch_add_explicit(&traces, 1, memory_order_relaxed) < 8)
            LSBSpawnTrace("[launchd-job] image=%p path=%s pin=%s\n", image, path, error ? error : "enabled");
#endif
    }
    errno = saved;
}

static void forgetJobImage(const struct mach_header *image) {
    uintptr_t expected = (uintptr_t)image;
    atomic_compare_exchange_strong_explicit(&launchdBase, &expected, 0, memory_order_acq_rel, memory_order_acquire);
}

static uintptr_t jobCallerOffset(uintptr_t caller) {
    uintptr_t base = atomic_load_explicit(&launchdBase, memory_order_acquire);
    return base && caller >= base ? caller - base : 0;
}

static bool dictionaryPIDKey(xpc_object_t dictionary, const char *key) {
    return dictionary && key && !strcmp(key, "pid") && xpc_get_type(dictionary) == XPC_TYPE_DICTIONARY;
}

static bool unsignedFieldEquals(xpc_object_t dictionary, const char *key, uint64_t expected) {
    xpc_object_t value = xpc_dictionary_get_value(dictionary, key);
    return value && xpc_get_type(value) == XPC_TYPE_UINT64 && xpc_uint64_get_value(value) == expected;
}

static int64_t outgoingJobPID(uintptr_t caller, xpc_object_t dictionary, const char *key, int64_t value) {
    // Before guest constructors/TLS, only the lock-free caller check runs; ordinary XPC calls never touch errno here.
    if (jobCallerOffset(caller) != JobPIDSetterReturn) return value;
    int saved = errno;
    if (value > 0 && value <= INT_MAX && dictionaryPIDKey(dictionary, key) && unsignedFieldEquals(dictionary, "job-state", 1)) {
        LSBPeerProcessIdentity identity = {0};
        int error = LSBTransportCopyChildIdentity((pid_t)value, &identity);
        uint32_t native = LSBPeerProcessIdentityPID(&identity);
        if (!error && (!native || native > INT_MAX || native != identity.audit.val[5] || !MACH_PORT_VALID(identity.taskName))) error = EPROTO;
#ifdef LSB_TRANSPORT_TRACE
        static _Atomic(unsigned) traces;
        if (atomic_fetch_add_explicit(&traces, 1, memory_order_relaxed) < 32)
            LSBSpawnTrace("[launchd-job] state pid logical=%lld native=%u error=%d\n", (long long)value, native, error);
#endif
        // A failed lookup must not advertise the retired logical PID as a live native process.
        value = error ? 0 : native;
        LSBPeerProcessIdentityDispose(&identity);
    }
    errno = saved;
    return value;
}

static int64_t incomingJobPID(uintptr_t caller, xpc_object_t dictionary, const char *key, int64_t value) {
    uintptr_t offset = jobCallerOffset(caller);
    if (offset != CopyJobPIDGetterReturn && offset != ServicePIDGetterReturn) return value;
    int saved = errno;
    uint64_t routine = offset == CopyJobPIDGetterReturn ? 1008 : 712;
    if (value > 0 && value <= INT_MAX && dictionaryPIDKey(dictionary, key) && unsignedFieldEquals(dictionary, "routine", routine)) {
        xpc_object_t pidObject = xpc_dictionary_get_value(dictionary, key);
        if (pidObject && xpc_get_type(pidObject) == XPC_TYPE_INT64 && xpc_int64_get_value(pidObject) == value) {
            pid_t guest = 0;
            int error = LSBTransportResolveNativeChildPID((pid_t)value, &guest);
            if (!error && guest <= 0) error = EPROTO;
#ifdef LSB_TRANSPORT_TRACE
            static _Atomic(unsigned) traces;
            if (atomic_fetch_add_explicit(&traces, 1, memory_order_relaxed) < 32) {
                audit_token_t sender = {0};
                xpc_dictionary_get_audit_token(dictionary, &sender);
                LSBSpawnTrace("[launchd-job] routine=%llu sender=%u/%u pid native=%lld logical=%d error=%d\n",
                              (unsigned long long)routine, sender.val[5], sender.val[7], (long long)value, guest, error);
            }
#endif
            // Passing an unresolved native PID through could select an unrelated job with that old logical PID.
            value = error ? 0 : guest;
        }
    }
    errno = saved;
    return value;
}

__attribute__((noinline)) void LSBLaunchdJobSetInt64(xpc_object_t dictionary, const char *key, int64_t value) {
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    xpc_dictionary_set_int64(dictionary, key, outgoingJobPID(caller, dictionary, key, value));
}

__attribute__((noinline)) int64_t LSBLaunchdJobGetInt64(xpc_object_t dictionary, const char *key) {
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    int64_t value = xpc_dictionary_get_int64(dictionary, key);
    return incomingJobPID(caller, dictionary, key, value);
}

#ifndef LSB_LAUNCHD_JOB_IDENTITY_TESTING
static void jobImageAdded(const struct mach_header *image, intptr_t slide) {
    (void)slide;
    int saved = errno;
    Dl_info info;
    if (dladdr(image, &info)) rememberJobImage(image, info.dli_fname);
    errno = saved;
}

static void jobImageRemoved(const struct mach_header *image, intptr_t slide) { (void)slide; forgetJobImage(image); }

__attribute__((constructor)) static void initializeJobIdentity(void) {
    int saved = errno;
    const char *enabled = getenv("LSB_SIMULATOR_LAUNCHD"), *root = getenv("DYLD_ROOT_PATH");
    if (!enabled || strcmp(enabled, "1")) { errno = saved; return; }
    char path[PATH_MAX], physicalRoot[PATH_MAX], executable[PATH_MAX];
    uint32_t length = sizeof(path);
    const char *error = NULL;
    if (!root || !realpath(root, physicalRoot)) error = "runtime root resolution";
    else if (_NSGetExecutablePath(path, &length)) error = "executable path query";
    else if (!realpath(path, executable)) error = "executable path resolution";
    else {
        configureJobScope(enabled, physicalRoot, executable);
        if (jobIdentityScoped) {
            // Inserted images can precede the executable in dyld_sim's image array. The add callback visits every loaded image.
            _dyld_register_func_for_add_image(jobImageAdded);
            _dyld_register_func_for_remove_image(jobImageRemoved);
            if (!atomic_load_explicit(&launchdBase, memory_order_acquire)) error = "no matching pinned launchd image";
        }
    }
#ifdef LSB_TRANSPORT_TRACE
    if (error) LSBSpawnTrace("[launchd-job] initialization disabled: %s errno=%d\n", error, errno);
#else
    (void)error;
#endif
    errno = saved;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBLaunchdJobSetInt64, (const void *)&xpc_dictionary_set_int64},
    {(const void *)&LSBLaunchdJobGetInt64, (const void *)&xpc_dictionary_get_int64}
};
#endif
