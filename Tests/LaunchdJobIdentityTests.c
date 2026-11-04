#include <assert.h>
#include <errno.h>
#include <stdio.h>

#define LSB_LAUNCHD_JOB_IDENTITY_TESTING
#include "../Guest/libLiveSimSpawn/LaunchdJobIdentity.c"

static struct mach_header_64 *image;
static unsigned outboundQueries, inboundQueries;
static int queryError;
static pid_t nativeAnswer = 7001, guestAnswer = 6001, queriedPID;
static bool badAudit, badSnapshot, missingRight;
static mach_port_t nameRight;

static void configureJobIdentity(const char *enabled, const char *root, const char *executable, const struct mach_header_64 *header) {
    configureJobScope(enabled, root, executable);
    rememberJobImage((const struct mach_header *)header, executable);
}

static mach_port_urefs_t references(void) {
    mach_port_urefs_t count;
    assert(mach_port_get_refs(mach_task_self(), nameRight, MACH_PORT_RIGHT_SEND, &count) == KERN_SUCCESS);
    return count;
}

int LSBTransportCopyChildIdentity(pid_t guestPID, LSBPeerProcessIdentity *identity) {
    ++outboundQueries;
    queriedPID = guestPID;
    memset(identity, 0, sizeof(*identity));
    errno = EIO;
    uint32_t native = (uint32_t)nativeAnswer, snapshot = badSnapshot ? native + 1 : native;
    identity->audit.val[5] = badAudit ? native + 1 : native;
    memcpy(identity->shortInfo, &snapshot, sizeof(snapshot));
    strcpy(identity->executablePath, "/Applications/Contacts.app/Contacts");
    if (!missingRight) {
        assert(mach_port_mod_refs(mach_task_self(), nameRight, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS);
        identity->taskName = nameRight;
    }
    return queryError;
}

int LSBTransportResolveNativeChildPID(pid_t nativePID, pid_t *guestPID) {
    ++inboundQueries;
    queriedPID = nativePID;
    errno = EIO;
    *guestPID = queryError ? 0 : guestAnswer;
    return queryError;
}

static void makeImage(void) {
    memset(image, 0, 0x40000);
    image->magic = MH_MAGIC_64;
    image->cputype = CPU_TYPE_ARM64;
    image->filetype = MH_EXECUTE;
    image->ncmds = 2;
    image->sizeofcmds = sizeof(struct uuid_command) + sizeof(struct segment_command_64);
    struct uuid_command *uuid = (struct uuid_command *)(image + 1);
    uuid->cmd = LC_UUID;
    uuid->cmdsize = sizeof(*uuid);
    memcpy(uuid->uuid, launchdUUID, sizeof(launchdUUID));
    struct segment_command_64 *text = (struct segment_command_64 *)(uuid + 1);
    text->cmd = LC_SEGMENT_64;
    text->cmdsize = sizeof(*text);
    strcpy(text->segname, "__TEXT");
    text->vmaddr = UINT64_C(0x100000000);
    text->vmsize = text->filesize = 0x40000;
    text->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    const unsigned sites[] = {JobPIDSetterReturn - 4, CopyJobPIDGetterReturn - 4, ServicePIDGetterReturn - 4};
    const unsigned targets[] = {0x3ed8c, 0x3ed20, 0x3ed20};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t instruction = 0x94000000 | ((targets[i] - sites[i]) >> 2);
        memcpy((unsigned char *)image + sites[i], &instruction, sizeof(instruction));
    }
}

static void enable(void) {
    configureJobIdentity("1", "/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot/sbin/launchd_sim", image);
    assert(atomic_load(&launchdBase) == (uintptr_t)image);
}

static void testImageAndScope(void) {
    assert(!jobCallerOffset((uintptr_t)image + JobPIDSetterReturn));
    assert(pinnedLaunchdImageError(NULL));
    makeImage(); enable();
    assert(jobCallerOffset((uintptr_t)image + JobPIDSetterReturn) == JobPIDSetterReturn);
    assert(!jobCallerOffset((uintptr_t)image - 1));
    const char *disabled[] = {NULL, "", "0", "true", "01"};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) {
        configureJobIdentity(disabled[i], "/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot/sbin/launchd_sim", image);
        assert(!atomic_load(&launchdBase));
    }
    const char *wrong[] = {"/sbin/launchd_sim", "/private/Group/RuntimeRoot/bin/df", "/private/Group/RuntimeRoot-other/sbin/launchd_sim",
                           "/private/Group/RuntimeRoot/sbin/launchd_sim-other", "/x"};
    for (unsigned i = 0; i < sizeof(wrong) / sizeof(*wrong); ++i) {
        configureJobIdentity("1", "/private/Group/RuntimeRoot", wrong[i], image);
        assert(!atomic_load(&launchdBase));
    }
    configureJobIdentity("1", "/", "/sbin/launchd_sim", image);
    assert(!atomic_load(&launchdBase));
    for (unsigned condition = 0; condition < 13; ++condition) {
        makeImage();
        struct uuid_command *uuid = (struct uuid_command *)(image + 1);
        struct segment_command_64 *text = (struct segment_command_64 *)(uuid + 1);
        switch (condition) {
            case 0: image->magic = MH_CIGAM_64; break;
            case 1: image->cputype = CPU_TYPE_X86_64; break;
            case 2: image->filetype = MH_DYLIB; break;
            case 3: uuid->uuid[15] ^= 1; break;
            case 4: uuid->cmdsize = sizeof(*uuid) - 1; break;
            case 5: image->ncmds = 3; break;
            case 6: image->sizeofcmds += 4; break;
            case 7: text->initprot = VM_PROT_READ; break;
            case 8: text->vmaddr += 0x1000; break;
            case 9: text->filesize = JobPIDSetterReturn - 1; break;
            case 10: case 11: case 12: {
                unsigned returns[] = {JobPIDSetterReturn, CopyJobPIDGetterReturn, ServicePIDGetterReturn};
                // A different BL still fails even when UUID and all other headers remain unchanged.
                ((unsigned char *)image)[returns[condition - 10] - 4] ^= 1;
                break;
            }
        }
        assert(pinnedLaunchdImageError(image));
        configureJobIdentity("1", "/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot/sbin/launchd_sim", image);
        assert(!atomic_load(&launchdBase));
    }
    makeImage(); enable();
}

static void testImageSelection(void) {
    struct mach_header_64 inserted = {.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = MH_DYLIB};
    configureJobScope("1", "/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot/sbin/launchd_sim");
    assert(jobIdentityScoped && !atomic_load(&launchdBase));
    // An injected dylib is enumerated first. Selecting launchd by path does not depend on dyld's image index ordering.
    rememberJobImage((const struct mach_header *)&inserted, "/private/Group/RuntimeRoot/usr/lib/libLiveSimSpawn.dylib");
    assert(!atomic_load(&launchdBase));
    errno = ERANGE;
    rememberJobImage((const struct mach_header *)image, "/private/Group/RuntimeRoot/sbin/launchd_sim");
    assert(atomic_load(&launchdBase) == (uintptr_t)image && errno == ERANGE);
    forgetJobImage((const struct mach_header *)&inserted);
    assert(atomic_load(&launchdBase) == (uintptr_t)image);
    forgetJobImage((const struct mach_header *)image);
    assert(!atomic_load(&launchdBase));
    rememberJobImage((const struct mach_header *)image, "/sbin/launchd_sim");
    assert(atomic_load(&launchdBase) == (uintptr_t)image);
    configureJobScope("1", "/private/Group/RuntimeRoot", "/private/Group/RuntimeRoot/bin/df");
    rememberJobImage((const struct mach_header *)image, "/sbin/launchd_sim");
    assert(!atomic_load(&launchdBase));
    enable();
}

static void testOutgoing(void) {
    xpc_object_t reply = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(reply, "job-state", 1);
    xpc_dictionary_set_string(reply, "program", "/Applications/Contacts.app/Contacts");
    xpc_dictionary_set_int64(reply, "wait4-status", 23 << 8);
    uintptr_t caller = (uintptr_t)image + JobPIDSetterReturn;
    unsigned before = outboundQueries;
    errno = ERANGE;
    assert(outgoingJobPID(caller, reply, "pid", 6001) == 7001 && queriedPID == 6001 && errno == ERANGE);
    assert(outboundQueries == before + 1 && references() == 1);
    nativeAnswer = 7002; // A later SETEXEC must not use a cached native PID.
    assert(outgoingJobPID(caller, reply, "pid", 6001) == 7002 && references() == 1);
    assert(xpc_dictionary_get_int64(reply, "wait4-status") == (23 << 8));
    assert(!strcmp(xpc_dictionary_get_string(reply, "program"), "/Applications/Contacts.app/Contacts"));
    before = outboundQueries;
    const int64_t invalid[] = {-1, 0, (int64_t)INT_MAX + 1, INT64_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
        assert(outgoingJobPID(caller, reply, "pid", invalid[i]) == invalid[i]);
    assert(outgoingJobPID(caller + 4, reply, "pid", 6001) == 6001);
    assert(outgoingJobPID(caller, reply, "wait4-status", 6001) == 6001);
    assert(outgoingJobPID(caller, reply, NULL, 6001) == 6001);
    assert(outgoingJobPID(caller, NULL, "pid", 6001) == 6001);
    for (uint64_t state = 0; state <= 4; ++state) if (state != 1) {
        xpc_dictionary_set_uint64(reply, "job-state", state);
        assert(outgoingJobPID(caller, reply, "pid", 6001) == 6001);
    }
    xpc_dictionary_set_int64(reply, "job-state", 1);
    assert(outgoingJobPID(caller, reply, "pid", 6001) == 6001);
    xpc_dictionary_set_value(reply, "job-state", NULL);
    assert(outgoingJobPID(caller, reply, "pid", 6001) == 6001 && outboundQueries == before);
    xpc_dictionary_set_uint64(reply, "job-state", 1);
    for (unsigned condition = 0; condition < 6; ++condition) {
        nativeAnswer = 7001; queryError = 0;
        if (condition == 0) queryError = ESRCH;
        if (condition == 1) badAudit = true;
        if (condition == 2) badSnapshot = true;
        if (condition == 3) missingRight = true;
        if (condition == 4) nativeAnswer = 0;
        if (condition == 5) nativeAnswer = -1;
        errno = EDOM;
        assert(outgoingJobPID(caller, reply, "pid", 6001) == 0 && errno == EDOM && references() == 1);
        badAudit = badSnapshot = missingRight = false;
    }
    queryError = 0; nativeAnswer = 7001;
    // Public wrappers called from this test's image do not trigger the pinned launchd callsites.
    LSBLaunchdJobSetInt64(reply, "pid", 6001);
    assert(LSBLaunchdJobGetInt64(reply, "pid") == 6001);
    xpc_release(reply);
}

static void testIncoming(void) {
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    const unsigned returns[] = {CopyJobPIDGetterReturn, ServicePIDGetterReturn};
    const uint64_t routines[] = {1008, 712};
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t caller = (uintptr_t)image + returns[i];
        xpc_dictionary_set_uint64(request, "routine", routines[i]);
        xpc_dictionary_set_int64(request, "pid", 7001);
        unsigned before = inboundQueries;
        errno = EBUSY;
        assert(incomingJobPID(caller, request, "pid", 7001) == 6001 && queriedPID == 7001 && errno == EBUSY);
        assert(inboundQueries == before + 1 && xpc_dictionary_get_int64(request, "pid") == 7001);
        guestAnswer = 6002;
        assert(incomingJobPID(caller, request, "pid", 7001) == 6002); // No cached mapping after reuse/replacement.
        guestAnswer = 6001;
        before = inboundQueries;
        assert(incomingJobPID(caller + 4, request, "pid", 7001) == 7001);
        assert(incomingJobPID(caller, request, "other", 7001) == 7001);
        assert(incomingJobPID(caller, NULL, "pid", 7001) == 7001);
        assert(incomingJobPID(caller, request, "pid", 7002) == 7002);
        assert(incomingJobPID(caller, request, "pid", 0) == 0);
        assert(incomingJobPID(caller, request, "pid", -1) == -1);
        assert(incomingJobPID(caller, request, "pid", INT64_MAX) == INT64_MAX);
        xpc_dictionary_set_uint64(request, "routine", routines[1 - i]);
        assert(incomingJobPID(caller, request, "pid", 7001) == 7001);
        xpc_dictionary_set_int64(request, "routine", (int64_t)routines[i]);
        assert(incomingJobPID(caller, request, "pid", 7001) == 7001);
        xpc_dictionary_set_value(request, "routine", NULL);
        assert(incomingJobPID(caller, request, "pid", 7001) == 7001);
        xpc_dictionary_set_uint64(request, "routine", routines[i]);
        xpc_dictionary_set_uint64(request, "pid", 7001);
        assert(incomingJobPID(caller, request, "pid", 7001) == 7001);
        xpc_dictionary_set_value(request, "pid", NULL);
        assert(incomingJobPID(caller, request, "pid", 7001) == 7001 && inboundQueries == before);
        xpc_dictionary_set_int64(request, "pid", 7001);
        queryError = ESRCH;
        errno = ENOSPC;
        // Native 7001 might also be another job's retired logical ID; failure must never fall back to it.
        assert(incomingJobPID(caller, request, "pid", 7001) == 0 && errno == ENOSPC);
        queryError = 0; guestAnswer = 0;
        assert(incomingJobPID(caller, request, "pid", 7001) == 0);
        guestAnswer = 6001;
    }
    xpc_release(request);
}

int main(void) {
    image = calloc(1, 0x40000);
    assert(image);
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &nameRight) == KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(), nameRight, nameRight, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    testImageAndScope();
    testImageSelection();
    testOutgoing();
    testIncoming();
    assert(references() == 1);
    assert(mach_port_deallocate(mach_task_self(), nameRight) == KERN_SUCCESS);
    assert(mach_port_mod_refs(mach_task_self(), nameRight, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS);
    free(image);
    puts("LaunchdJobIdentity: pinned image/instructions, exact state/request callsites, malformed pass-through, live mapping, errno and right disposal passed");
    return 0;
}
