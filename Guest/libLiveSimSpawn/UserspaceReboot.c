#include "UserspaceReboot.h"
#include "LaunchdCompatibility.h"
#include "SpawnDiagnostics.h"
#include "../../ProcessSupport/SpawnTransport.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Original 23A343 arm64 launchd_sim, UUID C3EC5C45-8C85-3D11-B59C-62C4248F5EF9. Routine 821 checks the
// caller and initiates shutdown before creating this reply. Dry runs also create a reply and must be excluded.
// Both reboot and ordinary shutdown eventually call C exit(0), so terminal status alone cannot identify intent.
enum { AcceptedReplyReturn = 0x2aee4, TerminalExitReturn = 0x352f0, AcceptedContextOffset = 0x2aeb4, ExitContextOffset = 0x352e0 };
static const unsigned char launchdUUID[16] = {0xc3, 0xec, 0x5c, 0x45, 0x8c, 0x85, 0x3d, 0x11,
                                             0xb5, 0x9c, 0x62, 0xc4, 0x24, 0x8f, 0x5e, 0xf9};
static const uint32_t acceptedContext[] = {
    0x92800048, 0xf2e23fe8, 0x8a0802c4, 0xaa1503e0, 0xaa1803e1, 0xaa1703e2, 0xd2800003,
    0x97ffe8ea, 0xaa0003f9, 0x35fff620, 0xaa1403e0, 0x94004f78, 0x52800019, 0xf9000260, 0x17ffffac
};
static const uint32_t exitContext[] = {0xa9bf7bfd, 0x910003fd, 0x52800000, 0x9400238a};
static _Atomic(uintptr_t) launchdBase;
static _Atomic(bool) rebootAccepted;
static bool rebootScoped;
static char launchdExecutablePath[PATH_MAX];
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2, "Early interposers must not acquire runtime locks");

static const char *pinnedRebootImageError(const struct mach_header_64 *header) {
    if (!header || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64 ||
        (header->cpusubtype & ~CPU_SUBTYPE_MASK) != CPU_SUBTYPE_ARM64_ALL || header->filetype != MH_EXECUTE ||
        !header->ncmds || header->ncmds > 4096 || header->sizeofcmds > 1048576) return "header";
    const unsigned char *command = (const unsigned char *)(header + 1);
    size_t remaining = header->sizeofcmds;
    bool uuid = false, text = false, instructions = false;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (remaining < sizeof(struct load_command)) return "truncated commands";
        const struct load_command *load = (const struct load_command *)command;
        if (load->cmdsize < sizeof(*load) || load->cmdsize > remaining || load->cmdsize % 8) return "command size";
        if (load->cmd == LC_UUID) {
            if (uuid || load->cmdsize != sizeof(struct uuid_command) ||
                memcmp(((const struct uuid_command *)load)->uuid, launchdUUID, sizeof(launchdUUID))) return "UUID";
            uuid = true;
        } else if (load->cmd == LC_SEGMENT_64) {
            if (load->cmdsize < sizeof(struct segment_command_64)) return "segment size";
            const struct segment_command_64 *segment = (const struct segment_command_64 *)load;
            if (segment->nsects > (load->cmdsize - sizeof(*segment)) / sizeof(struct section_64) ||
                load->cmdsize != sizeof(*segment) + segment->nsects * sizeof(struct section_64)) return "section table";
            if (!strncmp(segment->segname, "__TEXT", sizeof(segment->segname))) {
                if (text || segment->vmaddr != UINT64_C(0x100000000) || segment->fileoff ||
                    segment->filesize > segment->vmsize || segment->filesize < TerminalExitReturn ||
                    segment->initprot != (VM_PROT_READ | VM_PROT_EXECUTE)) return "text geometry/protection";
                text = true;
                const struct section_64 *section = (const struct section_64 *)(segment + 1);
                for (uint32_t j = 0; j < segment->nsects; ++j, ++section) {
                    if (strncmp(section->sectname, "__text", sizeof(section->sectname))) continue;
                    if (instructions || strncmp(section->segname, "__TEXT", sizeof(section->segname)) ||
                        section->addr < segment->vmaddr || section->addr - segment->vmaddr != section->offset ||
                        section->offset > AcceptedContextOffset || section->size > segment->filesize - section->offset ||
                        section->size < TerminalExitReturn - section->offset ||
                        !(section->flags & S_ATTR_PURE_INSTRUCTIONS)) return "instruction extent";
                    instructions = true;
                }
            }
        }
        command += load->cmdsize;
        remaining -= load->cmdsize;
    }
    if (!uuid || !text || !instructions || remaining) return "missing image metadata";
    const unsigned char *bytes = (const unsigned char *)header;
    if (memcmp(bytes + AcceptedContextOffset, acceptedContext, sizeof(acceptedContext))) return "accepted reply context";
    if (memcmp(bytes + ExitContextOffset, exitContext, sizeof(exitContext))) return "terminal exit context";
    return NULL;
}

static bool hasDotComponent(const char *path) {
    for (const char *part = path; *part;) {
        while (*part == '/') ++part;
        const char *end = part;
        while (*end && *end != '/') ++end;
        size_t size = (size_t)(end - part);
        if ((size == 1 && *part == '.') || (size == 2 && part[0] == '.' && part[1] == '.')) return true;
        part = end;
    }
    return false;
}

static void configureRebootScope(const char *enabled, const char *root, const char *executable) {
    atomic_store_explicit(&launchdBase, 0, memory_order_release);
    atomic_store_explicit(&rebootAccepted, false, memory_order_release);
    rebootScoped = false;
    launchdExecutablePath[0] = '\0';
    if (!enabled || strcmp(enabled, "1") || !root || root[0] != '/' || !root[1] || !executable) return;
    size_t size = strnlen(root, PATH_MAX);
    if (size == PATH_MAX || root[size - 1] == '/' || hasDotComponent(root) || strncmp(executable, root, size) ||
        strcmp(executable + size, "/sbin/launchd_sim") || strnlen(executable, PATH_MAX) == PATH_MAX) return;
    strcpy(launchdExecutablePath, executable);
    rebootScoped = true;
}

static void rememberRebootImage(const struct mach_header *image, const char *path) {
    if (!rebootScoped || !path) return;
    int saved = errno;
    char physical[PATH_MAX];
    bool match = !strcmp(path, "/sbin/launchd_sim") || !strcmp(path, launchdExecutablePath);
    if (!match && realpath(path, physical)) match = !strcmp(physical, launchdExecutablePath);
    if (match && !pinnedRebootImageError((const struct mach_header_64 *)image))
        atomic_store_explicit(&launchdBase, (uintptr_t)image, memory_order_release);
    errno = saved;
}

static void forgetRebootImage(const struct mach_header *image) {
    uintptr_t expected = (uintptr_t)image;
    if (atomic_compare_exchange_strong_explicit(&launchdBase, &expected, 0, memory_order_acq_rel, memory_order_acquire))
        atomic_store_explicit(&rebootAccepted, false, memory_order_release);
}

static bool rebootCaller(uintptr_t caller, uintptr_t offset) {
    uintptr_t base = atomic_load_explicit(&launchdBase, memory_order_acquire);
    return base && caller >= base && caller - base == offset;
}

static bool unsignedValue(xpc_object_t dictionary, const char *key, uint64_t *value) {
    xpc_object_t object = xpc_dictionary_get_value(dictionary, key);
    if (!object || xpc_get_type(object) != XPC_TYPE_UINT64) return false;
    *value = xpc_uint64_get_value(object);
    return true;
}

static bool rebootRequest(xpc_object_t request) {
    if (!request || xpc_get_type(request) != XPC_TYPE_DICTIONARY) return false;
    uint64_t routine, type, handle, flags, purpose;
    if (!unsignedValue(request, "routine", &routine) || routine != 821 || !unsignedValue(request, "type", &type) || type != 1 ||
        !unsignedValue(request, "handle", &handle) || handle || !unsignedValue(request, "flags", &flags)) return false;
    const uint64_t system = UINT64_C(1) << 63, userspace = UINT64_C(1) << 61, withPurpose = UINT64_C(1) << 56;
    // Stock simulator launchctl recognizes system/halt/devicerecovery; its unknown "userspace" argument falls back to SYSTEM.
    // Both supported reboot forms restart only this simulator session. Halt, dry-run, recovery and unknown flags stay ordinary.
    if ((flags & ~(system | userspace | withPurpose)) || ((flags & (system | userspace)) != system &&
        (flags & (system | userspace)) != userspace)) return false;
    return !(flags & withPurpose) || (unsignedValue(request, "userreboot_purpose", &purpose) && purpose >= 1 && purpose <= 100);
}

static void recordAcceptedReply(uintptr_t caller, xpc_object_t request, xpc_object_t reply) {
    // No TLS, environment, XPC inspection, or errno access until the constructor has pinned this exact late callsite.
    if (!rebootCaller(caller, AcceptedReplyReturn)) return;
    int saved = errno;
    if (reply && xpc_get_type(reply) == XPC_TYPE_DICTIONARY && rebootRequest(request)) {
        atomic_store_explicit(&rebootAccepted, true, memory_order_release);
#ifdef LSB_TRANSPORT_TRACE
        LSBSpawnTrace("[userspace-reboot] accepted simulator reboot request\n");
#endif
    }
    errno = saved;
}

static void notifyTerminalReboot(uintptr_t caller, int status) {
    if (status || !rebootCaller(caller, TerminalExitReturn) ||
        !atomic_exchange_explicit(&rebootAccepted, false, memory_order_acq_rel)) return;
    int saved = errno;
    // Only intent crosses IPC. The broker authenticates the current root launchd and waits for its actual clean exit.
    int error = LSBTransportRebootUserspace();
#ifdef LSB_TRANSPORT_TRACE
    LSBSpawnTrace("[userspace-reboot] clean terminal exit notification error=%d\n", error);
#else
    (void)error;
#endif
    errno = saved;
}

__attribute__((noinline)) xpc_object_t LSBUserspaceRebootCreateReply(xpc_object_t request) {
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    xpc_object_t reply = xpc_dictionary_create_reply(request);
    recordAcceptedReply(caller, request, reply);
    return reply;
}

__attribute__((noinline, noreturn)) void LSBUserspaceRebootExit(int status) {
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    notifyTerminalReboot(caller, status);
    exit(status); // Mach-O's imported _exit is C exit; the syscall-facing C _exit would be named __exit.
}

#ifndef LSB_USERSPACE_REBOOT_TESTING
static void rebootImageAdded(const struct mach_header *image, intptr_t slide) {
    (void)slide;
    int saved = errno;
    Dl_info info;
    if (dladdr(image, &info)) rememberRebootImage(image, info.dli_fname);
    errno = saved;
}

static void rebootImageRemoved(const struct mach_header *image, intptr_t slide) { (void)slide; forgetRebootImage(image); }

__attribute__((constructor)) static void initializeUserspaceReboot(void) {
    int saved = errno;
    const char *enabled = getenv(LSB_SIMULATOR_LAUNCHD_ENV), *root = getenv("DYLD_ROOT_PATH");
    if (!enabled || strcmp(enabled, "1")) { errno = saved; return; }
    char path[PATH_MAX], physicalRoot[PATH_MAX], executable[PATH_MAX];
    uint32_t size = sizeof(path);
    if (root && realpath(root, physicalRoot) && !_NSGetExecutablePath(path, &size) && realpath(path, executable)) {
        configureRebootScope(enabled, physicalRoot, executable);
        if (rebootScoped) {
            _dyld_register_func_for_add_image(rebootImageAdded);
            _dyld_register_func_for_remove_image(rebootImageRemoved);
        }
    }
    errno = saved;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBUserspaceRebootCreateReply, (const void *)&xpc_dictionary_create_reply},
    {(const void *)&LSBUserspaceRebootExit, (const void *)&exit}
};
#endif
