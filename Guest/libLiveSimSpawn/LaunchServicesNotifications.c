#include "LaunchServicesNotifications.h"
#include "../../ProcessSupport/GuestExecutablePath.h"
#include "../../ProcessSupport/PrivateXPC.h"

#include <dlfcn.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int sandbox_check_by_audit_token(audit_token_t token, const char *operation, int type, ...);
extern uint32_t dyld_get_active_platform(void);

static const char coreServicesPath[] = "/System/Library/Frameworks/CoreServices.framework/CoreServices";
static char physicalCoreServicesPath[PATH_MAX];
static _Atomic(uintptr_t) coreServicesImage;
static _Atomic(bool) notificationPolicyReady;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2, "Early sandbox forwarding must not call runtime helpers");
_Static_assert(sizeof(audit_token_t) == 32, "Darwin arm64 passes this complete audit token indirectly");

// 23A343 CoreServices _LSProcessCanAccessProgressPort asks _LSCheckMachPortAccessForAuditToken about com.apple.lsd.xpc.
// Native sandbox policy denies that name although the simulator's bootstrap namespace owns a live installation-observer port.
// Pin the sole sandbox callsite in the latter function, not every CoreServices sandbox check or arbitrary service name.
enum { CoreServicesSandboxReturn = 0xcfa94, GuestMachLookupFilter = 0x40000002 };
static const unsigned char coreServicesUUID[16] = {0x73, 0xf4, 0x11, 0xd9, 0x84, 0xa9, 0x33, 0xfe,
                                                 0x84, 0xc9, 0x8c, 0xd5, 0x7c, 0xa6, 0x33, 0xdc};
static const struct { unsigned offset; uint32_t instruction; } callsitePins[] = {
    {0xcfa68, 0xad400680}, {0xcfa6c, 0xad0087e0}, {0xcfa7c, 0xf90003e0},
    {0xcfa88, 0x910043e0}, {0xcfa8c, 0x321f02a2}, {0xcfa90, 0x9402c855}, {0xcfa94, 0x7100001f}
};

static bool pinnedCoreServices(const struct mach_header_64 *header, intptr_t slide) {
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64 || header->cpusubtype != CPU_SUBTYPE_ARM64_ALL ||
        header->filetype != MH_DYLIB || !header->ncmds || header->ncmds > 2048 || header->sizeofcmds > 1048576) return false;
    const unsigned char *command = (const unsigned char *)(header + 1);
    size_t remaining = header->sizeofcmds;
    uintptr_t base = (uintptr_t)header;
    bool uuid = false, text = false, instructions = false;
    for (unsigned i = 0; i < header->ncmds; ++i) {
        if (remaining < sizeof(struct load_command)) return false;
        const struct load_command *load = (const struct load_command *)command;
        if (load->cmdsize < sizeof(*load) || load->cmdsize > remaining) return false;
        if (load->cmd == LC_UUID) {
            if (uuid || load->cmdsize != sizeof(struct uuid_command) ||
                memcmp(((const struct uuid_command *)load)->uuid, coreServicesUUID, 16)) return false;
            uuid = true;
        } else if (load->cmd == LC_SEGMENT_64) {
            if (load->cmdsize < sizeof(struct segment_command_64)) return false;
            const struct segment_command_64 *segment = (const struct segment_command_64 *)load;
            if (!strncmp(segment->segname, "__TEXT", sizeof(segment->segname))) {
                uintptr_t magnitude = slide >= 0 ? (uintptr_t)slide : (uintptr_t)(-(slide + 1)) + 1;
                bool validSlide = slide >= 0 ? segment->vmaddr <= UINTPTR_MAX - magnitude : segment->vmaddr >= magnitude;
                uintptr_t begin = slide >= 0 ? segment->vmaddr + magnitude : segment->vmaddr - magnitude;
                if (text || !validSlide || begin != base || segment->vmsize > UINTPTR_MAX - base ||
                    segment->filesize > segment->vmsize || segment->filesize < sizeof(*header) + header->sizeofcmds ||
                    (segment->initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE) ||
                    segment->nsects > (load->cmdsize - sizeof(*segment)) / sizeof(struct section_64)) return false;
                text = true;
                const struct section_64 *sections = (const struct section_64 *)(segment + 1);
                for (unsigned j = 0; j < segment->nsects; ++j) {
                    const struct section_64 *section = &sections[j];
                    if (strncmp(section->sectname, "__text", sizeof(section->sectname))) continue;
                    if (instructions || strncmp(section->segname, "__TEXT", sizeof(section->segname)) ||
                        section->addr < segment->vmaddr) return false;
                    uint64_t offset = section->addr - segment->vmaddr;
                    if (offset > segment->filesize || section->size > segment->filesize - offset ||
                        callsitePins[0].offset < offset || CoreServicesSandboxReturn + 4 > offset + section->size) return false;
                    instructions = true;
                }
            }
        }
        command += load->cmdsize;
        remaining -= load->cmdsize;
    }
    if (!uuid || !text || !instructions || remaining) return false;
    // Exclude ADRP/GOT operands changed by cache relocation; all pinned instructions remain inside the original executable text.
    for (unsigned i = 0; i < sizeof(callsitePins) / sizeof(*callsitePins); ++i) {
        uint32_t instruction;
        memcpy(&instruction, (const unsigned char *)header + callsitePins[i].offset, sizeof(instruction));
        if (instruction != callsitePins[i].instruction) return false;
    }
    return true;
}

static bool configureNotificationScope(const char *root, const char *simulatorRoot, uint32_t platform) {
    physicalCoreServicesPath[0] = '\0';
    if (platform != 7 || !LSBGuestPathIsCanonical(root) || !simulatorRoot || strcmp(root, simulatorRoot)) return false;
    size_t length = strlen(root);
    if (length > sizeof(physicalCoreServicesPath) - sizeof(coreServicesPath)) return false;
    memcpy(physicalCoreServicesPath, root, length);
    memcpy(physicalCoreServicesPath + length, coreServicesPath, sizeof(coreServicesPath));
    return true;
}

static void rememberNotificationImage(const struct mach_header *image, intptr_t slide, const char *path) {
    if (!image || !path || (strcmp(path, coreServicesPath) &&
        (!physicalCoreServicesPath[0] || strcmp(path, physicalCoreServicesPath)))) return;
    if (pinnedCoreServices((const struct mach_header_64 *)image, slide))
        atomic_store_explicit(&coreServicesImage, (uintptr_t)image, memory_order_release);
}

static void forgetNotificationImage(const struct mach_header *image) {
    uintptr_t expected = (uintptr_t)image;
    atomic_compare_exchange_strong_explicit(&coreServicesImage, &expected, 0, memory_order_acq_rel, memory_order_acquire);
}

__attribute__((used, noinline)) static int notificationSandboxPolicy(const audit_token_t *token, const char *operation, int type,
                                                                    uintptr_t caller, const void *arguments) {
    // Interposition precedes guest TLS/constructors. Unconfigured and unrelated callers must not touch errno or inspect variadic arguments.
    if (!atomic_load_explicit(&notificationPolicyReady, memory_order_acquire)) return 0;
    uintptr_t image = atomic_load_explicit(&coreServicesImage, memory_order_acquire);
    if (!image || caller < image || caller - image != CoreServicesSandboxReturn || type != GuestMachLookupFilter ||
        !operation || strcmp(operation, "mach-lookup") || !token || !arguments) return 0;
    const char *name = *(const char *const *)arguments;
    if (!name || strcmp(name, "com.apple.lsd.xpc")) return 0;
    int saved = errno;
    audit_token_t self = {{0}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    bool handled = false;
    if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&self, &count) == KERN_SUCCESS &&
        count == TASK_AUDIT_TOKEN_COUNT && self.val[5] && self.val[5] <= INT_MAX && !memcmp(token, &self, sizeof(self))) {
        mach_port_t port = MACH_PORT_NULL;
        kern_return_t result = bootstrap_look_up(bootstrap_port, name, &port);
        handled = result == KERN_SUCCESS && MACH_PORT_VALID(port);
        if (MACH_PORT_VALID(port) && mach_port_deallocate(mach_task_self(), port) != KERN_SUCCESS) handled = false;
    }
    errno = saved;
    return handled;
}

#if defined(__arm64__)
#ifndef LSB_NOTIFICATION_SANDBOX_ORIGINAL
#define LSB_NOTIFICATION_SANDBOX_ORIGINAL "_sandbox_check_by_audit_token"
#endif
// The by-value token is passed indirectly in x0; Darwin variadic arguments remain on the original stack.
// On fallback preserve every argument register, NZCV, SP and LR, including calls without any variadic arguments.
__attribute__((naked)) int LSBLaunchServicesSandboxCheck(audit_token_t token, const char *operation, int type, ...) {
    __asm__("sub sp, sp, #240\n"
            "stp x0, x1, [sp, #0]\n stp x2, x3, [sp, #16]\n stp x4, x5, [sp, #32]\n stp x6, x7, [sp, #48]\n"
            "str x8, [sp, #64]\n stp x29, x30, [sp, #80]\n"
            "stp q0, q1, [sp, #96]\n stp q2, q3, [sp, #128]\n stp q4, q5, [sp, #160]\n stp q6, q7, [sp, #192]\n"
            "mrs x9, nzcv\n str x9, [sp, #224]\n mov x3, x30\n add x4, sp, #240\n bl _notificationSandboxPolicy\n mov w16, w0\n"
            "ldr x9, [sp, #224]\n msr nzcv, x9\n"
            "ldp q0, q1, [sp, #96]\n ldp q2, q3, [sp, #128]\n ldp q4, q5, [sp, #160]\n ldp q6, q7, [sp, #192]\n"
            "ldp x0, x1, [sp, #0]\n ldp x2, x3, [sp, #16]\n ldp x4, x5, [sp, #32]\n ldp x6, x7, [sp, #48]\n"
            "ldr x8, [sp, #64]\n ldp x29, x30, [sp, #80]\n add sp, sp, #240\n"
            "cbnz w16, 1f\n b " LSB_NOTIFICATION_SANDBOX_ORIGINAL "\n1: mov w0, #0\n ret\n");
}
#else
#error LaunchServices sandbox forwarding requires the Darwin arm64 ABI.
#endif

#ifndef LSB_LAUNCH_SERVICES_NOTIFICATIONS_TESTING
static void notificationImageAdded(const struct mach_header *image, intptr_t slide) {
    Dl_info info;
    if (dladdr(image, &info)) rememberNotificationImage(image, slide, info.dli_fname);
}
static void notificationImageRemoved(const struct mach_header *image, intptr_t slide) { (void)slide; forgetNotificationImage(image); }

__attribute__((constructor)) static void initializeLaunchServicesNotifications(void) {
    int saved = errno;
    if (configureNotificationScope(getenv("DYLD_ROOT_PATH"), getenv("SIMULATOR_ROOT"), dyld_get_active_platform())) {
        _dyld_register_func_for_add_image(notificationImageAdded);
        _dyld_register_func_for_remove_image(notificationImageRemoved);
        atomic_store_explicit(&notificationPolicyReady, true, memory_order_release);
    }
    errno = saved;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBLaunchServicesSandboxCheck, (const void *)&sandbox_check_by_audit_token}
};
#endif
