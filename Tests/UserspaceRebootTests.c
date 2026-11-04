#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <xpc/xpc.h>

static xpc_object_t originalCreateReply(xpc_object_t request);
__attribute__((noreturn)) static void originalExit(int status);
#define LSB_USERSPACE_REBOOT_TESTING
#define xpc_dictionary_create_reply originalCreateReply
#define exit originalExit
#include "../Guest/libLiveSimSpawn/UserspaceReboot.c"
#undef exit
#undef xpc_dictionary_create_reply

enum { ImageBytes = 0x54000 };
static struct mach_header_64 *image;
static unsigned notifications, forwardedReplies, forwardedExits;
static int notificationError, exitStatus;
static xpc_object_t forwardedRequest, originalReply;
static jmp_buf exitJump;

int LSBTransportRebootUserspace(void) {
    ++notifications;
    errno = EIO;
    return notificationError;
}

static xpc_object_t originalCreateReply(xpc_object_t request) {
    ++forwardedReplies;
    forwardedRequest = request;
    errno = EDOM;
    return originalReply;
}

__attribute__((noreturn)) static void originalExit(int status) {
    ++forwardedExits;
    exitStatus = status;
    longjmp(exitJump, 1);
}

static struct segment_command_64 *textSegment(void) { return (struct segment_command_64 *)((struct uuid_command *)(image + 1) + 1); }
static struct section_64 *textSection(void) { return (struct section_64 *)(textSegment() + 1); }

static void makeImage(void) {
    memset(image, 0, ImageBytes);
    *image = (struct mach_header_64){.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .cpusubtype = CPU_SUBTYPE_ARM64_ALL,
        .filetype = MH_EXECUTE, .ncmds = 2,
        .sizeofcmds = sizeof(struct uuid_command) + sizeof(struct segment_command_64) + sizeof(struct section_64)};
    struct uuid_command *uuid = (struct uuid_command *)(image + 1);
    uuid->cmd = LC_UUID;
    uuid->cmdsize = sizeof(*uuid);
    memcpy(uuid->uuid, launchdUUID, sizeof(launchdUUID));
    struct segment_command_64 *segment = textSegment();
    segment->cmd = LC_SEGMENT_64;
    segment->cmdsize = sizeof(*segment) + sizeof(struct section_64);
    strcpy(segment->segname, "__TEXT");
    segment->vmaddr = UINT64_C(0x100000000);
    segment->vmsize = segment->filesize = ImageBytes;
    segment->maxprot = segment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    segment->nsects = 1;
    struct section_64 *section = textSection();
    strcpy(section->sectname, "__text");
    strcpy(section->segname, "__TEXT");
    section->offset = 0x1000;
    section->addr = segment->vmaddr + section->offset;
    section->size = ImageBytes - section->offset;
    section->flags = S_ATTR_PURE_INSTRUCTIONS;
    memcpy((unsigned char *)image + AcceptedContextOffset, acceptedContext, sizeof(acceptedContext));
    memcpy((unsigned char *)image + ExitContextOffset, exitContext, sizeof(exitContext));
}

static void enable(void) {
    configureRebootScope("1", "/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot/sbin/launchd_sim");
    rememberRebootImage((const struct mach_header *)image, "/private/Group/RuntimeRoot/sbin/launchd_sim");
    assert(atomic_load(&launchdBase) == (uintptr_t)image);
}

static xpc_object_t makeRequest(uint64_t flags) {
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(request, "routine", 821);
    xpc_dictionary_set_uint64(request, "type", 1);
    xpc_dictionary_set_uint64(request, "handle", 0);
    xpc_dictionary_set_uint64(request, "flags", flags);
    return request;
}

static void testImagePins(void) {
    assert(pinnedRebootImageError(NULL));
    assert(!rebootCaller((uintptr_t)image + AcceptedReplyReturn, AcceptedReplyReturn));
    makeImage();
    assert(!pinnedRebootImageError(image));
    for (unsigned change = 0; change < 25; ++change) {
        makeImage();
        struct uuid_command *uuid = (struct uuid_command *)(image + 1);
        struct segment_command_64 *segment = textSegment();
        struct section_64 *section = textSection();
        switch (change) {
            case 0: image->magic = MH_CIGAM_64; break;
            case 1: image->cputype = CPU_TYPE_X86_64; break;
            case 2: image->cpusubtype = CPU_SUBTYPE_ARM64E; break;
            case 3: image->filetype = MH_DYLIB; break;
            case 4: image->ncmds = 0; break;
            case 5: image->ncmds = 3; break;
            case 6: image->sizeofcmds -= 4; break;
            case 7: image->sizeofcmds += 8; break;
            case 8: uuid->uuid[15] ^= 1; break;
            case 9: uuid->cmdsize -= 8; break;
            case 10: segment->fileoff = 0x1000; break;
            case 11: segment->vmaddr += 0x1000; break;
            case 12: segment->vmsize = TerminalExitReturn - 1; break;
            case 13: segment->filesize = TerminalExitReturn - 1; break;
            case 14: segment->initprot = VM_PROT_READ; break;
            case 15: segment->initprot |= VM_PROT_WRITE; break;
            case 16: segment->nsects = 2; break;
            case 17: strcpy(section->segname, "__DATA"); break;
            case 18: strcpy(section->sectname, "__cstring"); break;
            case 19: section->addr += 4; break;
            case 20: section->offset = AcceptedContextOffset + 4; section->addr = segment->vmaddr + section->offset; break;
            case 21: section->size = TerminalExitReturn - section->offset - 1; break;
            case 22: section->size = UINT64_MAX; break;
            case 23: section->flags = 0; break;
            case 24: segment->cmdsize -= 8; break;
        }
        assert(pinnedRebootImageError(image));
        configureRebootScope("1", "/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot/sbin/launchd_sim");
        rememberRebootImage((const struct mach_header *)image, "/sbin/launchd_sim");
        assert(!atomic_load(&launchdBase));
    }
    const unsigned offsets[] = {AcceptedContextOffset, ExitContextOffset};
    const size_t sizes[] = {sizeof(acceptedContext), sizeof(exitContext)};
    for (unsigned range = 0; range < 2; ++range) for (size_t offset = 0; offset < sizes[range]; offset += 4) {
        makeImage();
        ((unsigned char *)image)[offsets[range] + offset] ^= 1;
        assert(pinnedRebootImageError(image)); // Validate the surrounding branch/context, not just the BL opcode.
    }
    makeImage(); enable();
    assert(rebootCaller((uintptr_t)image + AcceptedReplyReturn, AcceptedReplyReturn));
    assert(!rebootCaller((uintptr_t)image + AcceptedReplyReturn + 4, AcceptedReplyReturn));
    assert(!rebootCaller((uintptr_t)image - 1, AcceptedReplyReturn));
}

static void testScope(void) {
    const char *markers[] = {NULL, "", "0", "true", "01"};
    for (unsigned i = 0; i < sizeof(markers) / sizeof(*markers); ++i) {
        configureRebootScope(markers[i], "/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot/sbin/launchd_sim");
        assert(!rebootScoped && !atomic_load(&launchdBase));
    }
    const char *executables[] = {NULL, "/sbin/launchd_sim", "/private/Group/RuntimeRoot/bin/df", "/private/Group/RuntimeRoot/sbin/launchd_sim-other",
                                "/private/Group/RuntimeRoot-other/sbin/launchd_sim", "/private/Group/RuntimeRoot/subdir/../sbin/launchd_sim"};
    for (unsigned i = 0; i < sizeof(executables) / sizeof(*executables); ++i) {
        configureRebootScope("1", "/private/Group/RuntimeRoot", executables[i]);
        assert(!rebootScoped && !atomic_load(&launchdBase));
    }
    const char *roots[] = {NULL, "", "/", "RuntimeRoot", "/private/Group/RuntimeRoot/", "/private/./RuntimeRoot", "/private/../RuntimeRoot"};
    for (unsigned i = 0; i < sizeof(roots) / sizeof(*roots); ++i) {
        configureRebootScope("1", roots[i], "/private/Group/RuntimeRoot/sbin/launchd_sim");
        assert(!rebootScoped && !atomic_load(&launchdBase));
    }
    configureRebootScope("1", "/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot/sbin/launchd_sim");
    rememberRebootImage((const struct mach_header *)image, "/private/Group/RuntimeRoot/usr/lib/libLiveSimSpawn.dylib");
    assert(!atomic_load(&launchdBase));
    errno = EBUSY;
    rememberRebootImage((const struct mach_header *)image, "/sbin/launchd_sim");
    assert(atomic_load(&launchdBase) && errno == EBUSY);
    atomic_store(&rebootAccepted, true);
    struct mach_header unrelated = {0};
    forgetRebootImage(&unrelated);
    assert(atomic_load(&launchdBase) && atomic_load(&rebootAccepted));
    forgetRebootImage((const struct mach_header *)image);
    assert(!atomic_load(&launchdBase) && !atomic_load(&rebootAccepted));
    enable();
}

static void testClassification(void) {
    const uint64_t system = UINT64_C(1) << 63, userspace = UINT64_C(1) << 61, purpose = UINT64_C(1) << 56;
    const uint64_t supported[] = {system, userspace, userspace | purpose, system | purpose};
    for (unsigned i = 0; i < sizeof(supported) / sizeof(*supported); ++i) {
        xpc_object_t request = makeRequest(supported[i]);
        xpc_dictionary_set_uint64(request, "userreboot_purpose", 1);
        assert(rebootRequest(request));
        xpc_release(request);
    }
    xpc_object_t request = makeRequest(system);
    for (unsigned bit = 0; bit < 64; ++bit) {
        if (bit == 63) continue;
        xpc_dictionary_set_uint64(request, "flags", system | (UINT64_C(1) << bit));
        // No purpose field: all extra bits reject, including purpose, userspace, dry-run, HALT and recovery.
        assert(!rebootRequest(request));
    }
    xpc_dictionary_set_uint64(request, "flags", 0);
    assert(!rebootRequest(request));
    xpc_dictionary_set_uint64(request, "flags", userspace | purpose);
    const uint64_t purposes[] = {0, 1, 100, 101, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(purposes) / sizeof(*purposes); ++i) {
        xpc_dictionary_set_uint64(request, "userreboot_purpose", purposes[i]);
        assert(rebootRequest(request) == (purposes[i] >= 1 && purposes[i] <= 100));
    }
    xpc_dictionary_set_int64(request, "userreboot_purpose", 1);
    assert(!rebootRequest(request));
    xpc_release(request);
    const char *fields[] = {"routine", "type", "handle", "flags"};
    const uint64_t values[] = {821, 1, 0, system};
    for (unsigned i = 0; i < 4; ++i) {
        request = makeRequest(system);
        xpc_dictionary_set_uint64(request, fields[i], values[i] + 1);
        assert(!rebootRequest(request));
        xpc_dictionary_set_int64(request, fields[i], (int64_t)values[i]);
        assert(!rebootRequest(request));
        xpc_dictionary_set_string(request, fields[i], "1");
        assert(!rebootRequest(request));
        xpc_dictionary_set_value(request, fields[i], NULL);
        assert(!rebootRequest(request));
        xpc_release(request);
    }
    xpc_object_t wrong = xpc_string_create("not a request");
    assert(!rebootRequest(NULL) && !rebootRequest(wrong));
    xpc_release(wrong);
}

static void testLifecycle(void) {
    xpc_object_t request = makeRequest(UINT64_C(1) << 63), reply = xpc_dictionary_create(NULL, NULL, 0), wrong = xpc_bool_create(true);
    uintptr_t accepted = (uintptr_t)image + AcceptedReplyReturn, terminal = (uintptr_t)image + TerminalExitReturn;
    enable();
    errno = ERANGE;
    recordAcceptedReply(accepted, request, NULL);
    recordAcceptedReply(accepted, request, wrong);
    recordAcceptedReply(accepted + 4, request, reply);
    recordAcceptedReply(accepted, NULL, reply);
    assert(!atomic_load(&rebootAccepted) && errno == ERANGE && notifications == 0);
    recordAcceptedReply(accepted, request, reply);
    assert(atomic_load(&rebootAccepted) && errno == ERANGE && notifications == 0);
    notifyTerminalReboot(terminal + 4, 0);
    notifyTerminalReboot(terminal, 1);
    assert(notifications == 0 && atomic_load(&rebootAccepted));
    notifyTerminalReboot(terminal, 0);
    assert(notifications == 1 && !atomic_load(&rebootAccepted) && errno == ERANGE);
    notifyTerminalReboot(terminal, 0);
    assert(notifications == 1);
    recordAcceptedReply(accepted, request, reply);
    notificationError = EACCES;
    errno = ENOSPC;
    notifyTerminalReboot(terminal, 0);
    assert(notifications == 2 && !atomic_load(&rebootAccepted) && errno == ENOSPC);
    notifyTerminalReboot(terminal, 0); // Failed notification is never retried during teardown.
    assert(notifications == 2);
    recordAcceptedReply(accepted, request, reply);
    configureRebootScope(NULL, NULL, NULL);
    recordAcceptedReply(accepted, request, reply);
    notifyTerminalReboot(terminal, 0);
    assert(notifications == 2 && !atomic_load(&rebootAccepted));
    enable();
    // Actual wrappers called from an ordinary image must pass through even if a prior accepted reboot was recorded.
    recordAcceptedReply(accepted, request, reply);
    originalReply = reply;
    assert(LSBUserspaceRebootCreateReply(request) == reply && forwardedReplies == 1 && forwardedRequest == request && errno == EDOM);
    originalReply = NULL;
    assert(!LSBUserspaceRebootCreateReply(request) && forwardedReplies == 2 && errno == EDOM);
    errno = EBUSY;
    if (!setjmp(exitJump)) LSBUserspaceRebootExit(0);
    assert(forwardedExits == 1 && exitStatus == 0 && errno == EBUSY && notifications == 2);
    if (!setjmp(exitJump)) LSBUserspaceRebootExit(37);
    assert(forwardedExits == 2 && exitStatus == 37 && notifications == 2);
    forgetRebootImage((const struct mach_header *)image);
    notifyTerminalReboot(terminal, 0);
    assert(notifications == 2);
    xpc_release(wrong);
    xpc_release(reply);
    xpc_release(request);
}

int main(void) {
    image = calloc(1, ImageBytes);
    assert(image);
    testImagePins();
    testScope();
    testClassification();
    testLifecycle();
    free(image);
    puts("UserspaceReboot: exact image/context/caller pins, accepted typed requests, clean-exit-only one-shot notification and forwarding passed");
    return 0;
}
