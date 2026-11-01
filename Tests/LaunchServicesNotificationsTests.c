#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static kern_return_t testTaskInfo(task_name_t task, task_flavor_t flavor, task_info_t output, mach_msg_type_number_t *count);
static kern_return_t testLookup(mach_port_t bootstrap, const char *name, mach_port_t *port);
static kern_return_t testDeallocate(ipc_space_t task, mach_port_name_t port);
static unsigned errnoReads;
static int *testErrnoLocation(void);
#define task_info testTaskInfo
#define bootstrap_look_up testLookup
#define mach_port_deallocate testDeallocate
#define LSB_LAUNCH_SERVICES_NOTIFICATIONS_TESTING
#define LSB_NOTIFICATION_SANDBOX_ORIGINAL "_LSBTestNotificationSandboxOriginal"
#undef errno
#define errno (*testErrnoLocation())
#include "../Guest/LaunchServicesNotifications.c"
#undef task_info
#undef bootstrap_look_up
#undef mach_port_deallocate
#undef errno
#define errno (*__error())

static int *testErrnoLocation(void) { ++errnoReads; return __error(); }
__attribute__((used)) static audit_token_t nativeToken = {{501, 502, 503, 504, 505, 400, 507, 508}};
static unsigned taskCalls, lookups, releases, originalCalls;
static kern_return_t taskResult, lookupResult, releaseResult;
static bool shortToken, missingPID, invalidPort;
__attribute__((used, aligned(16))) static uint64_t originalEntry[30];
__attribute__((used)) static uintptr_t expectedStack, expectedLink;
__attribute__((used)) static const char captureOperation[] = "capture-registers";
extern void notificationAllowedReturn(void);

static kern_return_t testTaskInfo(task_name_t task, task_flavor_t flavor, task_info_t output, mach_msg_type_number_t *count) {
    ++taskCalls;
    assert(task == mach_task_self() && flavor == TASK_AUDIT_TOKEN && *count == TASK_AUDIT_TOKEN_COUNT);
    memcpy(output, &nativeToken, sizeof(nativeToken));
    if (missingPID) ((audit_token_t *)output)->val[5] = 0;
    if (shortToken) --*count;
    errno = EIO;
    return taskResult;
}

static kern_return_t testLookup(mach_port_t bootstrap, const char *name, mach_port_t *port) {
    ++lookups;
    assert(bootstrap == bootstrap_port && !strcmp(name, "com.apple.lsd.xpc"));
    *port = invalidPort ? MACH_PORT_NULL : 0x123;
    errno = EPIPE;
    return lookupResult;
}

static kern_return_t testDeallocate(ipc_space_t task, mach_port_name_t port) {
    ++releases;
    assert(task == mach_task_self() && port == 0x123);
    errno = ERANGE;
    return releaseResult;
}

__attribute__((used)) static int originalBody(audit_token_t token, const char *operation, int type, ...) {
    ++originalCalls;
    assert(!memcmp(&token, &nativeToken, sizeof(token)));
    if (operation && !strcmp(operation, "forward-variadic")) {
        assert(type == 0x76543210);
        va_list args;
        va_start(args, type);
        for (uint64_t i = 0; i < 12; ++i) assert(va_arg(args, uint64_t) == UINT64_C(0xfedcba9800000000) + i);
        assert(va_arg(args, double) == 3.25);
        assert(!strcmp(va_arg(args, const char *), "tail-stack-argument"));
        va_end(args);
    }
    errno = EBUSY;
    return 117;
}

__attribute__((naked)) int LSBTestNotificationSandboxOriginal(audit_token_t token, const char *operation, int type, ...) {
    __asm__("adrp x9, _originalEntry@PAGE\n add x9, x9, _originalEntry@PAGEOFF\n"
            "stp x0, x1, [x9, #0]\n stp x2, x3, [x9, #16]\n stp x4, x5, [x9, #32]\n stp x6, x7, [x9, #48]\n str x8, [x9, #64]\n"
            "stp q0, q1, [x9, #80]\n stp q2, q3, [x9, #112]\n stp q4, q5, [x9, #144]\n stp q6, q7, [x9, #176]\n"
            "mrs x10, nzcv\n str x10, [x9, #208]\n mov x10, sp\n str x10, [x9, #216]\n str x30, [x9, #224]\n"
            "b _originalBody\n");
}

__attribute__((naked, noinline)) static int callAllowed(audit_token_t token, const char *operation, int type, const char *name) {
    __asm__("sub sp, sp, #32\n stp x29, x30, [sp, #16]\n add x29, sp, #16\n str x3, [sp]\n"
            "bl _LSBLaunchServicesSandboxCheck\n .globl _notificationAllowedReturn\n_notificationAllowedReturn:\n"
            "ldp x29, x30, [sp, #16]\n add sp, sp, #32\n ret\n");
}

__attribute__((naked, noinline)) static int callWithRegisters(void) {
    __asm__("stp x29, x30, [sp, #-16]!\n mov x29, sp\n"
            "adrp x9, _expectedStack@PAGE\n add x9, x9, _expectedStack@PAGEOFF\n mov x10, sp\n str x10, [x9]\n"
            "adrp x9, _expectedLink@PAGE\n add x9, x9, _expectedLink@PAGEOFF\n adr x10, 1f\n str x10, [x9]\n"
            "adrp x0, _nativeToken@PAGE\n add x0, x0, _nativeToken@PAGEOFF\n"
            "adrp x1, _captureOperation@PAGE\n add x1, x1, _captureOperation@PAGEOFF\n"
            "mov x2, #0x1234\n mov x3, #0x3333\n mov x4, #0x4444\n mov x5, #0x5555\n mov x6, #0x6666\n"
            "mov x7, #0x7777\n mov x8, #0x8888\n"
            "movi v0.16b, #0x10\n movi v1.16b, #0x11\n movi v2.16b, #0x12\n movi v3.16b, #0x13\n"
            "movi v4.16b, #0x14\n movi v5.16b, #0x15\n movi v6.16b, #0x16\n movi v7.16b, #0x17\n"
            "cmp xzr, xzr\n bl _LSBLaunchServicesSandboxCheck\n1: ldp x29, x30, [sp], #16\n ret\n");
}

static void testImageScope(void) {
    assert(!configureNotificationScope(NULL, NULL, 7));
    assert(!configureNotificationScope("/A/../B", "/A/../B", 7));
    assert(!configureNotificationScope("/private/Group/RuntimeRoot", "/different", 7));
    assert(!configureNotificationScope("/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot", 2));
    assert(configureNotificationScope("/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot", 7));
    assert(!strcmp(physicalCoreServicesPath, "/private/Group/RuntimeRoot/System/Library/Frameworks/CoreServices.framework/CoreServices"));
    size_t imageSize = 0xd0000;
    struct mach_header_64 *header = calloc(1, imageSize);
    assert(header);
    header->magic = MH_MAGIC_64; header->cputype = CPU_TYPE_ARM64; header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    header->filetype = MH_DYLIB; header->ncmds = 2;
    struct uuid_command *uuid = (struct uuid_command *)(header + 1);
    uuid->cmd = LC_UUID; uuid->cmdsize = sizeof(*uuid); memcpy(uuid->uuid, coreServicesUUID, 16);
    struct segment_command_64 *text = (struct segment_command_64 *)(uuid + 1);
    text->cmd = LC_SEGMENT_64; text->cmdsize = sizeof(*text) + sizeof(struct section_64); strcpy(text->segname, "__TEXT");
    text->vmaddr = (uintptr_t)header; text->vmsize = text->filesize = imageSize; text->initprot = VM_PROT_READ | VM_PROT_EXECUTE; text->nsects = 1;
    struct section_64 *section = (struct section_64 *)(text + 1);
    strcpy(section->sectname, "__text"); strcpy(section->segname, "__TEXT"); section->addr = (uintptr_t)header + 4096; section->size = imageSize - 4096;
    header->sizeofcmds = sizeof(*uuid) + text->cmdsize;
    for (unsigned i = 0; i < sizeof(callsitePins) / sizeof(*callsitePins); ++i)
        memcpy((char *)header + callsitePins[i].offset, &callsitePins[i].instruction, 4);
    assert(pinnedCoreServices(header, 0));
    text->vmaddr -= 0x1000; section->addr -= 0x1000;
    assert(pinnedCoreServices(header, 0x1000));
    text->vmaddr += 0x1000; section->addr += 0x1000;
    const char *validPaths[] = {coreServicesPath, physicalCoreServicesPath};
    for (unsigned i = 0; i < 2; ++i) {
        rememberNotificationImage((const struct mach_header *)header, 0, validPaths[i]);
        assert(atomic_load(&coreServicesImage) == (uintptr_t)header);
        forgetNotificationImage((const struct mach_header *)(uintptr_t)1);
        assert(atomic_load(&coreServicesImage) == (uintptr_t)header);
        forgetNotificationImage((const struct mach_header *)header);
        assert(!atomic_load(&coreServicesImage));
    }
    rememberNotificationImage((const struct mach_header *)header, 0, "/different/CoreServices");
    assert(!atomic_load(&coreServicesImage));
    uuid->uuid[0] ^= 1; assert(!pinnedCoreServices(header, 0)); uuid->uuid[0] ^= 1;
    header->cpusubtype = CPU_SUBTYPE_ARM64E; assert(!pinnedCoreServices(header, 0)); header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    header->ncmds = 2049; assert(!pinnedCoreServices(header, 0)); header->ncmds = 2;
    uuid->cmdsize = 1; assert(!pinnedCoreServices(header, 0)); uuid->cmdsize = sizeof(*uuid);
    ++header->sizeofcmds; assert(!pinnedCoreServices(header, 0)); --header->sizeofcmds;
    text->initprot = VM_PROT_READ; assert(!pinnedCoreServices(header, 0)); text->initprot |= VM_PROT_EXECUTE;
    text->nsects = 2; assert(!pinnedCoreServices(header, 0)); text->nsects = 1;
    section->size = 4; assert(!pinnedCoreServices(header, 0)); section->size = imageSize - 4096;
    text->filesize = imageSize + 1; assert(!pinnedCoreServices(header, 0)); text->filesize = imageSize;
    assert(!pinnedCoreServices(header, 1));
    for (unsigned i = 0; i < sizeof(callsitePins) / sizeof(*callsitePins); ++i) {
        ((unsigned char *)header)[callsitePins[i].offset] ^= 1;
        assert(!pinnedCoreServices(header, 0));
        ((unsigned char *)header)[callsitePins[i].offset] ^= 1;
    }
    free(header);
}

static void testPolicy(void) {
    const char *name = "com.apple.lsd.xpc";
    uintptr_t image = 0x100000, caller = image + CoreServicesSandboxReturn;
    atomic_store(&coreServicesImage, image);
    errno = EDOM;
    unsigned before = errnoReads;
    assert(!notificationSandboxPolicy(NULL, NULL, GuestMachLookupFilter, caller, (void *)1) && errnoReads == before);
    atomic_store(&notificationPolicyReady, true);
    assert(!notificationSandboxPolicy(NULL, (void *)1, GuestMachLookupFilter, caller + 4, (void *)1) && errnoReads == before);
    assert(!notificationSandboxPolicy(NULL, (void *)1, 0, caller, (void *)1) && errnoReads == before);
    assert(!notificationSandboxPolicy(&nativeToken, "other", GuestMachLookupFilter, caller, (void *)1) && errnoReads == before);
    assert(!notificationSandboxPolicy(&nativeToken, "mach-lookup", GuestMachLookupFilter, caller, NULL) && errnoReads == before);
    const char *wrongNames[] = {NULL, "com.apple.lsd.mapdb", "com.apple.lsd.xpc.extra", "com.apple.cfprefsd.daemon", ""};
    for (unsigned i = 0; i < sizeof(wrongNames) / sizeof(*wrongNames); ++i)
        assert(!notificationSandboxPolicy(&nativeToken, "mach-lookup", GuestMachLookupFilter, caller, &wrongNames[i]));
    assert(errnoReads == before && !taskCalls && !lookups && !releases);
    assert(notificationSandboxPolicy(&nativeToken, "mach-lookup", GuestMachLookupFilter, caller, &name));
    assert(errno == EDOM && taskCalls == 1 && lookups == 1 && releases == 1);
    for (unsigned i = 0; i < 8; ++i) {
        audit_token_t other = nativeToken; other.val[i] ^= 1;
        assert(!notificationSandboxPolicy(&other, "mach-lookup", GuestMachLookupFilter, caller, &name));
    }
    assert(lookups == 1 && releases == 1 && errno == EDOM);
    taskResult = KERN_FAILURE;
    assert(!notificationSandboxPolicy(&nativeToken, "mach-lookup", GuestMachLookupFilter, caller, &name));
    taskResult = KERN_SUCCESS; shortToken = true;
    assert(!notificationSandboxPolicy(&nativeToken, "mach-lookup", GuestMachLookupFilter, caller, &name));
    shortToken = false; missingPID = true;
    assert(!notificationSandboxPolicy(&nativeToken, "mach-lookup", GuestMachLookupFilter, caller, &name));
    missingPID = false;
    assert(lookups == 1 && releases == 1);
    lookupResult = KERN_FAILURE;
    assert(!notificationSandboxPolicy(&nativeToken, "mach-lookup", GuestMachLookupFilter, caller, &name) && releases == 2);
    lookupResult = KERN_SUCCESS; invalidPort = true;
    assert(!notificationSandboxPolicy(&nativeToken, "mach-lookup", GuestMachLookupFilter, caller, &name) && releases == 2);
    invalidPort = false; releaseResult = KERN_FAILURE;
    assert(!notificationSandboxPolicy(&nativeToken, "mach-lookup", GuestMachLookupFilter, caller, &name) && releases == 3);
    releaseResult = KERN_SUCCESS;
    assert(errno == EDOM);
    atomic_store(&coreServicesImage, (uintptr_t)&notificationAllowedReturn - CoreServicesSandboxReturn);
    before = originalCalls;
    assert(callAllowed(nativeToken, "mach-lookup", GuestMachLookupFilter, name) == 0 && errno == EDOM && originalCalls == before);
    lookupResult = KERN_FAILURE;
    assert(callAllowed(nativeToken, "mach-lookup", GuestMachLookupFilter, name) == 117 && errno == EBUSY && originalCalls == before + 1);
    lookupResult = KERN_SUCCESS;
}

static void testForwarding(void) {
    unsigned before = errnoReads;
    assert(LSBLaunchServicesSandboxCheck(nativeToken, NULL, 0) == 117 && errno == EBUSY);
    assert(LSBLaunchServicesSandboxCheck(nativeToken, "forward-variadic", 0x76543210,
        UINT64_C(0xfedcba9800000000), UINT64_C(0xfedcba9800000001), UINT64_C(0xfedcba9800000002), UINT64_C(0xfedcba9800000003),
        UINT64_C(0xfedcba9800000004), UINT64_C(0xfedcba9800000005), UINT64_C(0xfedcba9800000006), UINT64_C(0xfedcba9800000007),
        UINT64_C(0xfedcba9800000008), UINT64_C(0xfedcba9800000009), UINT64_C(0xfedcba980000000a), UINT64_C(0xfedcba980000000b),
        3.25, "tail-stack-argument") == 117);
    assert(callWithRegisters() == 117 && errnoReads == before);
    assert(originalEntry[0] == (uintptr_t)&nativeToken && originalEntry[1] == (uintptr_t)captureOperation && originalEntry[2] == 0x1234);
    for (unsigned i = 3; i <= 8; ++i) assert(originalEntry[i] == i * 0x1111u);
    const unsigned char *simd = (const unsigned char *)originalEntry + 80;
    for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 16; ++j) assert(simd[i * 16 + j] == 0x10 + i);
    assert(originalEntry[26] == UINT64_C(0x60000000) && originalEntry[27] == expectedStack && originalEntry[28] == expectedLink);
    atomic_store(&notificationPolicyReady, false);
    assert(callWithRegisters() == 117 && errnoReads == before);
}

int main(void) {
    testImageScope(); testPolicy(); testForwarding();
    puts("LaunchServicesNotificationsTests passed");
    return 0;
}
