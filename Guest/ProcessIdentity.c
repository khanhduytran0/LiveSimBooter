#include "ProcessIdentity.h"
#ifdef LSB_GUEST_RUNTIME
#include "PeerProcessCompatibility.h"
#endif
#include "ChildWait.h"
#include <limits.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

static _Atomic(pid_t) guestPID, parentPID;
static _Atomic(bool) parentKnown;
static char nativeNotifydPath[PATH_MAX];
static char nativeBaseBoardPath[PATH_MAX];
static char nativeRunningBoardServicesPath[PATH_MAX];
static char nativeFrontBoardPath[PATH_MAX];
static char nativeUIKitCorePath[PATH_MAX];
static char nativeFrontBoardServicesPath[PATH_MAX];
static char nativeBackBoardServicesPath[PATH_MAX];
static char nativeExtensionFoundationPath[PATH_MAX];
static char nativeCoreServicesPath[PATH_MAX];
static char nativeCoreFoundationPath[PATH_MAX];
static char nativeNetworkPath[PATH_MAX];
static const char baseBoardInstallPath[] = "/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoard";
static const char runningBoardServicesInstallPath[] =
    "/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices";
static const char frontBoardInstallPath[] = "/System/Library/PrivateFrameworks/FrontBoard.framework/FrontBoard";
static const char uiKitCoreInstallPath[] = "/System/Library/PrivateFrameworks/UIKitCore.framework/UIKitCore";
static const char frontBoardServicesInstallPath[] = "/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices";
static const char backBoardServicesInstallPath[] = "/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices";
static const char extensionFoundationInstallPath[] = "/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation";
static const char coreServicesInstallPath[] = "/System/Library/Frameworks/CoreServices.framework/CoreServices";
static const char coreFoundationInstallPath[] = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
static const char networkInstallPath[] = "/System/Library/Frameworks/Network.framework/Network";

// 23A343 CoreServices compares getpid with a genuine audit-token PID in these two bundle-record lookup paths.
// Reverse-engineered from +[LSBundleRecord _bundleRecordForAuditToken:checkNSBundleMainBundle:error:] and
// _LSCopyBundleURLForAuditToken. Keep all other CoreServices callers on guest identity; see docs/springboard-services.md.
enum { CoreServicesRecordSelfReturn = 0x6f294, CoreServicesBundleSelfReturn = 0xd0524 };
static const unsigned coreServicesSelfReturns[] = {CoreServicesRecordSelfReturn, CoreServicesBundleSelfReturn};
static const unsigned char coreServicesUUID[16] = {0x73, 0xf4, 0x11, 0xd9, 0x84, 0xa9, 0x33, 0xfe,
                                                 0x84, 0xc9, 0x8c, 0xd5, 0x7c, 0xa6, 0x33, 0xdc};
static _Atomic(uintptr_t) coreServicesSelfImage, coreFoundationPreferencesImage;
static _Atomic(uintptr_t) networkRouteImage;
enum { CoreFoundationPreferencesReturn = 0x1971a4 };
enum { NetworkRoutePIDReturn = 0xac7984 };
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "Early identity caller checks must not require runtime initialization");

static bool pinnedIdentityText(const struct mach_header_64 *header, intptr_t slide, const unsigned char expectedUUID[16],
                               uint64_t firstInstruction, uint64_t endInstruction) {
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64 || header->cpusubtype != CPU_SUBTYPE_ARM64_ALL ||
        header->filetype != MH_DYLIB || !header->ncmds || header->ncmds > 2048 || header->sizeofcmds > 1048576) return false;
    const unsigned char *command = (const unsigned char *)(header + 1);
    size_t remaining = header->sizeofcmds;
    uintptr_t base = (uintptr_t)header;
    bool uuid = false, text = false, instructions = false;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (remaining < sizeof(struct load_command)) return false;
        const struct load_command *load = (const struct load_command *)command;
        if (load->cmdsize < sizeof(*load) || load->cmdsize > remaining) return false;
        if (load->cmd == LC_UUID) {
            if (uuid || load->cmdsize != sizeof(struct uuid_command) ||
                memcmp(((const struct uuid_command *)load)->uuid, expectedUUID, 16)) return false;
            uuid = true;
        } else if (load->cmd == LC_SEGMENT_64) {
            if (load->cmdsize < sizeof(struct segment_command_64)) return false;
            const struct segment_command_64 *segment = (const struct segment_command_64 *)load;
            if (!strncmp(segment->segname, "__TEXT", sizeof(segment->segname))) {
                uintptr_t magnitude = slide >= 0 ? (uintptr_t)slide : (uintptr_t)(-(slide + 1)) + 1;
                bool valid = slide >= 0 ? segment->vmaddr <= UINTPTR_MAX - magnitude : segment->vmaddr >= magnitude;
                uintptr_t begin = slide >= 0 ? segment->vmaddr + magnitude : segment->vmaddr - magnitude;
                if (text || !valid || begin != base || segment->vmsize > UINTPTR_MAX - base ||
                    segment->filesize > segment->vmsize || segment->filesize < sizeof(*header) + header->sizeofcmds ||
                    (segment->initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE) ||
                    segment->nsects > (load->cmdsize - sizeof(*segment)) / sizeof(struct section_64)) return false;
                text = true;
                const struct section_64 *sections = (const struct section_64 *)(segment + 1);
                for (uint32_t j = 0; j < segment->nsects; ++j) {
                    const struct section_64 *section = &sections[j];
                    if (strncmp(section->sectname, "__text", sizeof(section->sectname))) continue;
                    if (instructions || strncmp(section->segname, "__TEXT", sizeof(section->segname)) ||
                        section->addr < segment->vmaddr) return false;
                    uint64_t relative = section->addr - segment->vmaddr;
                    if (relative > segment->filesize || section->size > segment->filesize - relative) return false;
                    if (firstInstruction < relative || endInstruction > relative + section->size) return false;
                    instructions = true;
                }
            }
        }
        command += load->cmdsize;
        remaining -= load->cmdsize;
    }
    return uuid && text && instructions && !remaining;
}

static bool pinnedCoreServicesSelfImage(const struct mach_header_64 *header, intptr_t slide) {
    if (!pinnedIdentityText(header, slide, coreServicesUUID, CoreServicesRecordSelfReturn - 16, CoreServicesBundleSelfReturn + 8)) return false;
    // These windows, including BL getpid, are byte-identical in the stock image and the rebuilt development cache.
    static const uint32_t expected[][6] = {
        {0xaa1303e0, 0x94018282, 0xaa0003f5, 0x94044839, 0x6b0002bf, 0x54000961},
        {0x97ff3e44, 0x940554ca, 0x370002e0, 0x9402c395, 0xaa0003f4, 0xb81dc3bf}
    };
    for (unsigned i = 0; i < sizeof(coreServicesSelfReturns) / sizeof(*coreServicesSelfReturns); ++i)
        if (memcmp((const unsigned char *)header + coreServicesSelfReturns[i] - 16, expected[i], sizeof(expected[i]))) return false;
    return true;
}

// Original 23A343 ____CFPrefsDirectMode_block_invoke calls getpid here solely for sandbox_check's native self preflight.
// A retired logical PID denies the named cfprefsd lookup and incorrectly selects an anonymous per-process preferences daemon.
// Translate identity, not policy: the original sandbox_check still decides whether the current process can use the service.
static bool pinnedCoreFoundationPreferencesImage(const struct mach_header_64 *header, intptr_t slide) {
    static const unsigned char uuid[16] = {0x6e, 0x5c, 0xfb, 0x28, 0xdd, 0x41, 0x37, 0x56,
                                          0x97, 0x86, 0x3e, 0xf0, 0xad, 0xcd, 0x9d, 0x38};
    if (!pinnedIdentityText(header, slide, uuid, 0x1971a0, 0x1971d4)) return false;
    // Relocated ADRP/GOT operands differ in a rebuilt cache. Pin the unchanged getpid/sandbox_check BL targets and argument/control flow.
    static const struct { unsigned offset; uint32_t instruction; } pins[] = {
        {0x1971a0, 0x9400df45}, {0x1971b8, 0xf90003e9}, {0x1971c4, 0x321f0102},
        {0x1971c8, 0x9400e1d5}, {0x1971cc, 0xaa0003f3}, {0x1971d0, 0x340000c0}
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(*pins); ++i) {
        uint32_t instruction;
        memcpy(&instruction, (const unsigned char *)header + pins[i].offset, sizeof(instruction));
        if (instruction != pins[i].instruction) return false;
    }
    return true;
}

// 23A343 nw_route_get_ifindex compares this getpid result with the RTM_GET_SILENT reply's kernel-authored rtm_pid.
// XNU route_output always writes proc_selfpid(), so a logical PID discards a valid route and makes NWPath report offline.
// Keep only this callsite native; routing permissions, interface data and every other Network caller remain unchanged.
static bool pinnedNetworkRouteImage(const struct mach_header_64 *header, intptr_t slide) {
    static const unsigned char uuid[16] = {0x37, 0xe5, 0x9d, 0x23, 0x28, 0x0c, 0x36, 0x6b,
                                          0xa7, 0xad, 0x3c, 0x26, 0xd8, 0x94, 0x50, 0x8e};
    if (!pinnedIdentityText(header, slide, uuid, 0xac7980, 0xac7ca4)) return false;
    // Stock and rebuilt-cache instructions match; omit ADRP/GOT operands relocated by the cache builder. See docs/networking.md.
    static const struct { unsigned offset; uint32_t instruction; } pins[] = {
        {0xac7980, 0x940eb4e0}, {0xac7984, 0xaa0003f5}, {0xac79e4, 0x290bdbe0},
        {0xac7c80, 0x39413fe8}, {0xac7c84, 0x7100111f}, {0xac7c88, 0x54fffec1},
        {0xac7c8c, 0xb9405fe8}, {0xac7c90, 0x6b15011f}, {0xac7c94, 0x54fffe61},
        {0xac7c98, 0xb94063e8}, {0xac7c9c, 0x6b16011f}, {0xac7ca0, 0x54fffe01}
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(*pins); ++i) {
        uint32_t instruction;
        memcpy(&instruction, (const unsigned char *)header + pins[i].offset, sizeof(instruction));
        if (instruction != pins[i].instruction) return false;
    }
    return true;
}

static void configureNativeImagePath(char path[PATH_MAX], const char *suffix) {
    path[0] = '\0';
    const char *root = getenv("DYLD_ROOT_PATH");
    if (!root || root[0] != '/') return;
    size_t length = strnlen(root, PATH_MAX), suffixSize = strlen(suffix) + 1;
    if (length < 2 || suffixSize > PATH_MAX || length > PATH_MAX - suffixSize || root[length - 1] == '/' || strchr(root, ':')) return;
    // Launchd configuration supplies a canonical host path. Reject aliases and root lists without doing filesystem work in dyld callbacks.
    for (const char *component = root + 1; *component;) {
        const char *end = strchr(component, '/');
        size_t size = end ? (size_t)(end - component) : strlen(component);
        if (!size || (size == 1 && component[0] == '.') || (size == 2 && component[0] == '.' && component[1] == '.')) return;
        component += size + (end != NULL);
    }
    memcpy(path, root, length);
    memcpy(path + length, suffix, suffixSize);
}

static void configureNativeNotifydIdentity(void) {
    nativeNotifydPath[0] = '\0';
    const char *enabled = getenv("LSB_SIMULATE_NOTIFICATION_SHM");
    if (enabled && !strcmp(enabled, "1")) configureNativeImagePath(nativeNotifydPath, "/usr/sbin/notifyd");
}

typedef struct { _Atomic(uintptr_t) image, begin, end; } NativeIdentityRange;
static NativeIdentityRange nativeIdentityRanges[16];
static _Atomic(unsigned) nativeIdentitySequence;

static void forgetNativeIdentityImage(const struct mach_header *image) {
    uintptr_t expected = (uintptr_t)image;
    atomic_compare_exchange_strong_explicit(&coreServicesSelfImage, &expected, 0, memory_order_acq_rel, memory_order_acquire);
    expected = (uintptr_t)image;
    atomic_compare_exchange_strong_explicit(&coreFoundationPreferencesImage, &expected, 0, memory_order_acq_rel, memory_order_acquire);
    expected = (uintptr_t)image;
    atomic_compare_exchange_strong_explicit(&networkRouteImage, &expected, 0, memory_order_acq_rel, memory_order_acquire);
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_acq_rel);
    for (unsigned i = 0; i < 16; ++i) if (atomic_load_explicit(&nativeIdentityRanges[i].image, memory_order_relaxed) == (uintptr_t)image)
        atomic_store_explicit(&nativeIdentityRanges[i].image, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_release);
}

static void __attribute__((unused)) rememberNativeIdentityImage(const struct mach_header *image, intptr_t slide, const char *path) {
    const char *leaf = path ? strrchr(path, '/') : NULL;
    if (!image || image->magic != MH_MAGIC_64 || !leaf) return;
    if (!strcmp(path, coreServicesInstallPath) || (nativeCoreServicesPath[0] && !strcmp(path, nativeCoreServicesPath))) {
        forgetNativeIdentityImage(image);
        if (pinnedCoreServicesSelfImage((const struct mach_header_64 *)image, slide))
            atomic_store_explicit(&coreServicesSelfImage, (uintptr_t)image, memory_order_release);
        return;
    }
    if (!strcmp(path, coreFoundationInstallPath) || (nativeCoreFoundationPath[0] && !strcmp(path, nativeCoreFoundationPath))) {
        forgetNativeIdentityImage(image);
        if (pinnedCoreFoundationPreferencesImage((const struct mach_header_64 *)image, slide))
            atomic_store_explicit(&coreFoundationPreferencesImage, (uintptr_t)image, memory_order_release);
        return;
    }
    if (!strcmp(path, networkInstallPath) || (nativeNetworkPath[0] && !strcmp(path, nativeNetworkPath))) {
        forgetNativeIdentityImage(image);
        if (pinnedNetworkRouteImage((const struct mach_header_64 *)image, slide))
            atomic_store_explicit(&networkRouteImage, (uintptr_t)image, memory_order_release);
        return;
    }
    bool notifyd = nativeNotifydPath[0] && !strcmp(path, nativeNotifydPath);
    // BaseBoard's versioned PID, sandbox and task-handle APIs compare getpid() with genuine kernel audit identities.
    bool baseBoard = !strcmp(path, baseBoardInstallPath) || (nativeBaseBoardPath[0] && !strcmp(path, nativeBaseBoardPath));
    // RunningBoardServices queries the caller's native sandbox before connecting and exchanges kernel process identities.
    bool runningBoard = !strcmp(path, runningBoardServicesInstallPath) ||
        (nativeRunningBoardServicesPath[0] && !strcmp(path, nativeRunningBoardServicesPath));
    // FrontBoard keys its workspace/process dictionaries with RBS native handles and permits repeated registration only for native self.
    bool frontBoard = !strcmp(path, frontBoardInstallPath) || (nativeFrontBoardPath[0] && !strcmp(path, nativeFrontBoardPath));
    // iOS 26 23A343 UIKitCore +0x115ea68 feeds getpid directly into an RBSTarget visibility assertion; its background assertions do likewise.
    bool uiKitCore = !strcmp(path, uiKitCoreInstallPath) || (nativeUIKitCorePath[0] && !strcmp(path, nativeUIKitCorePath));
    // Matching FrontBoardServices +0x54d74 compares native audit PID with getpid; +0x71900 publishes the scene host PID.
    // See docs/springboard-services.md and the pinned disassembly analysis in tmp/app-launch-fix/routine712-runningboard-pid-source.md.
    bool frontBoardServices = !strcmp(path, frontBoardServicesInstallPath) ||
        (nativeFrontBoardServicesPath[0] && !strcmp(path, nativeFrontBoardServicesPath));
    // iOS 26 23A343 BackBoardServices +0x673b4 registers the HID client's PID; +0xcc58 and +0x15bbc construct HID dispatch targets.
    // Those identities, and the delivery-policy self comparison at +0x183f0, must match the kernel audit PID used by the event client.
    bool backBoardServices = !strcmp(path, backBoardServicesInstallPath) ||
        (nativeBackBoardServicesPath[0] && !strcmp(path, nativeBackBoardServicesPath));
    // 23A343 ExtensionFoundation +0x5c658 feeds getpid into the RBS host identifier for _EXHostConfiguration.rbsProcessIdentity.
    // Its extension identity must name the same native process that RunningBoard resolves from audit tokens and task handles.
    bool extensionFoundation = !strcmp(path, extensionFoundationInstallPath) ||
        (nativeExtensionFoundationPath[0] && !strcmp(path, nativeExtensionFoundationPath));
    if (strcmp(leaf + 1, "libxpc.dylib") && !notifyd && !baseBoard && !runningBoard && !frontBoard &&
        !uiKitCore && !frontBoardServices && !backBoardServices && !extensionFoundation) return;
    const struct mach_header_64 *header = (const struct mach_header_64 *)image;
    const unsigned char *command = (const unsigned char *)(header + 1), *end = command + header->sizeofcmds;
    forgetNativeIdentityImage(image);
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_acq_rel);
    for (unsigned i = 0; i < header->ncmds && (size_t)(end - command) >= sizeof(struct load_command); ++i) {
        const struct load_command *load = (const struct load_command *)command;
        if (load->cmdsize < sizeof(*load) || load->cmdsize > (size_t)(end - command)) break;
        if (load->cmd == LC_SEGMENT_64 && load->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
            uintptr_t magnitude = slide >= 0 ? (uintptr_t)slide : (uintptr_t)(-(slide + 1)) + 1;
            bool validSlide = slide >= 0 ? segment->vmaddr <= UINTPTR_MAX - magnitude : segment->vmaddr >= magnitude;
            uintptr_t begin = slide >= 0 ? segment->vmaddr + magnitude : segment->vmaddr - magnitude;
            if (validSlide && segment->vmsize && segment->vmsize <= UINTPTR_MAX - begin && (segment->initprot & VM_PROT_EXECUTE)) {
                for (unsigned slot = 0; slot < 16; ++slot) {
                    NativeIdentityRange *range = &nativeIdentityRanges[slot];
                    if (atomic_load_explicit(&range->image, memory_order_relaxed)) continue;
                    atomic_store_explicit(&range->begin, begin, memory_order_relaxed);
                    atomic_store_explicit(&range->end, begin + segment->vmsize, memory_order_relaxed);
                    atomic_store_explicit(&range->image, (uintptr_t)image, memory_order_relaxed);
                    break;
                }
            }
        }
        command += load->cmdsize;
    }
    atomic_fetch_add_explicit(&nativeIdentitySequence, 1, memory_order_release);
}

static bool callerNeedsNativeIdentity(uintptr_t caller) {
    uintptr_t network = atomic_load_explicit(&networkRouteImage, memory_order_acquire);
    if (network && caller >= network && caller - network == NetworkRoutePIDReturn) return true;
    uintptr_t preferences = atomic_load_explicit(&coreFoundationPreferencesImage, memory_order_acquire);
    if (preferences && caller >= preferences && caller - preferences == CoreFoundationPreferencesReturn) return true;
    uintptr_t coreServices = atomic_load_explicit(&coreServicesSelfImage, memory_order_acquire);
    if (coreServices && caller >= coreServices &&
        (caller - coreServices == CoreServicesRecordSelfReturn || caller - coreServices == CoreServicesBundleSelfReturn)) return true;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        unsigned sequence = atomic_load_explicit(&nativeIdentitySequence, memory_order_acquire);
        if (sequence & 1) continue;
        bool found = false;
        for (unsigned i = 0; i < 16; ++i) {
            NativeIdentityRange *range = &nativeIdentityRanges[i];
            if (!atomic_load_explicit(&range->image, memory_order_relaxed)) continue;
            uintptr_t begin = atomic_load_explicit(&range->begin, memory_order_relaxed), end = atomic_load_explicit(&range->end, memory_order_relaxed);
            if (caller >= begin && caller < end) { found = true; break; }
        }
        if (sequence == atomic_load_explicit(&nativeIdentitySequence, memory_order_acquire)) return found;
    }
    return false;
}

#ifndef LSB_PROCESS_IDENTITY_TESTING
static void identityImageAdded(const struct mach_header *image, intptr_t slide) {
    Dl_info info;
    if (dladdr(image, &info)) rememberNativeIdentityImage(image, slide, info.dli_fname);
}
static void identityImageRemoved(const struct mach_header *image, intptr_t slide) {
    (void)slide;
    forgetNativeIdentityImage(image);
}
#endif

static pid_t environmentPID(const char *key) {
    const char *text = getenv(key);
    if (!text || !*text) return 0;
    unsigned long value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > (INT_MAX - (unsigned)(*p - '0')) / 10u) return 0;
        value = value * 10 + (unsigned)(*p - '0');
    }
    return (pid_t)value;
}

__attribute__((constructor)) static void initializeIdentity(void) {
    configureNativeNotifydIdentity();
    configureNativeImagePath(nativeBaseBoardPath, baseBoardInstallPath);
    configureNativeImagePath(nativeRunningBoardServicesPath, runningBoardServicesInstallPath);
    configureNativeImagePath(nativeFrontBoardPath, frontBoardInstallPath);
    configureNativeImagePath(nativeUIKitCorePath, uiKitCoreInstallPath);
    configureNativeImagePath(nativeFrontBoardServicesPath, frontBoardServicesInstallPath);
    configureNativeImagePath(nativeBackBoardServicesPath, backBoardServicesInstallPath);
    configureNativeImagePath(nativeExtensionFoundationPath, extensionFoundationInstallPath);
    configureNativeImagePath(nativeCoreServicesPath, coreServicesInstallPath);
    configureNativeImagePath(nativeCoreFoundationPath, coreFoundationInstallPath);
    configureNativeImagePath(nativeNetworkPath, networkInstallPath);
#ifndef LSB_PROCESS_IDENTITY_TESTING
    // Existing images are enumerated synchronously before publishing the guest PID. Early recursive calls retain native identity.
    _dyld_register_func_for_add_image(identityImageAdded);
    _dyld_register_func_for_remove_image(identityImageRemoved);
#endif
    atomic_store_explicit(&guestPID, environmentPID("LSB_GUEST_PID"), memory_order_release);
    pid_t parent = environmentPID("LSB_GUEST_PPID");
    const char *text = getenv("LSB_GUEST_PPID");
    atomic_store_explicit(&parentPID, parent, memory_order_release);
    atomic_store_explicit(&parentKnown, parent > 0 || (text && text[0] == '0' && !text[1]), memory_order_release);
}

pid_t LSBNativePID(void) { return getpid(); }

pid_t LSBGuestPID(void) {
    pid_t pid = atomic_load_explicit(&guestPID, memory_order_acquire);
    return pid ? pid : LSBNativePID();
}

__attribute__((noinline)) pid_t LSBGetPID(void) {
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    return callerNeedsNativeIdentity(caller) ? LSBNativePID() : LSBGuestPID();
}

kern_return_t LSBTaskNameForPID(mach_port_name_t target, int pid, mach_port_name_t *task) {
#ifdef LSB_GUEST_RUNTIME
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    kern_return_t childResult;
    if (LSBLaunchdProcessTaskName(caller, target, pid, task, &childResult)) return childResult;
#endif
    if (target == mach_task_self() && pid > 0 && pid == LSBGuestPID()) pid = LSBNativePID();
    kern_return_t result = task_name_for_pid(target, pid, task);
#ifdef LSB_GUEST_RUNTIME
    return LSBPeerProcessTaskName(caller, target, pid, task, result);
#else
    return result;
#endif
}

pid_t LSBGuestParentPID(void) {
    LSBChildStatus *ledger = LSBGuestChildStatus();
    pid_t current = ledger ? LSBChildStatusGetParentPID(ledger) : -1;
    if (current >= 0) return current;
    pid_t pid = atomic_load_explicit(&parentPID, memory_order_acquire);
    return atomic_load_explicit(&parentKnown, memory_order_acquire) ? pid : getppid();
}

#ifndef LSB_PROCESS_IDENTITY_TESTING
// libxpc and the scoped UI/process frameworks use kernel identities. Opted-in notifyd publishes its native PID to sibling PROC watches.
// Only direct calls from those images retain native identity; other guest code sees the stable logical PID.
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBGetPID, (const void *)&getpid},
    {(const void *)&LSBTaskNameForPID, (const void *)&task_name_for_pid},
    {(const void *)&LSBGuestParentPID, (const void *)&getppid}
};
#endif
