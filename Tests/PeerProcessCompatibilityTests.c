#include <assert.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int testPIDPath(int pid, void *buffer, uint32_t size);
static int testTerminate(pid_t pid, int *signalNumber);
static unsigned policyErrnoReads;
static int *testErrnoLocation(void);
int LSBTestSandboxOriginal(pid_t pid, const char *operation, int type, ...);
#define proc_pidpath testPIDPath
#define proc_terminate testTerminate
#define LSB_PEER_PROCESS_COMPATIBILITY_TESTING
#define LSB_PEER_SANDBOX_ORIGINAL "_LSBTestSandboxOriginal"
#undef errno
#define errno (*testErrnoLocation())
#include "../Guest/PeerProcessCompatibility.c"
#undef proc_pidpath
#undef proc_terminate
#undef errno
#define errno (*__error())

static int *testErrnoLocation(void) { ++policyErrnoReads; return __error(); }

static bool inspectionHandled;
static unsigned inspectionCalls;
bool LSBInspectionPIDPath(int pid, void *buffer, uint32_t size, int *result) {
    if (!inspectionHandled) return false;
    ++inspectionCalls;
    const char path[] = "/private/Group/RuntimeRoot/Applications/CocoaTop.app/CocoaTop";
    assert(pid == 404 && size >= sizeof(path));
    memcpy(buffer, path, sizeof(path));
    *result = sizeof(path) - 1;
    return true;
}

const int SANDBOX_CHECK_NO_REPORT = 0x100000;
pid_t LSBNativePID(void) { return 101; }
pid_t LSBGuestPID(void) { return 99; }
static unsigned queries, nativePathCalls, originalSandboxCalls;
static unsigned sandboxShmCalls;
static bool sandboxShmAllowed;
static const char *sandboxShmName, *sandboxShmOperation;
static int queryError, pathResult = 9, pathErrno = EPERM;
static pid_t replyPID, killedPID;
static unsigned kills, nativeTerminations;
static int killError, killedSignal;
static bool badAudit, badSnapshot, badPath;
static mach_port_t namePort;
static const char guestPath[] = "/System/Library/CoreServices/SpringBoard.app/SpringBoard";
static const char physicalGuestPath[] = "/private/Group/RuntimeRoot/System/Library/CoreServices/SpringBoard.app/SpringBoard";
static const char *replyPath = guestPath;
__attribute__((used, aligned(16))) static uint64_t sandboxEntry[30];
__attribute__((used)) static uintptr_t expectedStack, expectedLink;
__attribute__((used)) static const char captureOperation[] = "capture-registers";
__attribute__((used)) static const char workspaceShmOperation[] = "ipc-posix-shm-read-data";
__attribute__((used)) static const char workspaceShmName[] = "/FBW1,2:com.apple.SpringBoard";

bool LSBNotificationShmAllowsSandbox(const char *name, const char *operation) {
    ++sandboxShmCalls;
    sandboxShmName = name;
    sandboxShmOperation = operation;
    errno = EIO; // The wrapper must preserve caller errno even if policy inspection changes it.
    return sandboxShmAllowed;
}

int LSBTransportCopyProcessIdentity(pid_t pid, LSBPeerProcessIdentity *identity) {
    ++queries;
    memset(identity, 0, sizeof(*identity));
    errno = EIO; // Helpers must preserve the native call's errno rather than transport scratch state.
    if (queryError) return queryError;
    for (unsigned i = 0; i < 8; ++i) identity->audit.val[i] = 0x100 + i;
    uint32_t nativePID = replyPID ? (uint32_t)replyPID : (uint32_t)pid;
    identity->audit.val[5] = badAudit ? nativePID + 1 : nativePID;
    memset(identity->shortInfo, 0xa5, sizeof(identity->shortInfo));
    uint32_t snapshotPID = badSnapshot ? nativePID + 1 : nativePID;
    memcpy(identity->shortInfo, &snapshotPID, sizeof(snapshotPID));
    if (badPath) memset(identity->executablePath, 'x', sizeof(identity->executablePath));
    else {
        assert(strlen(replyPath) < sizeof(identity->executablePath));
        memcpy(identity->executablePath, replyPath, strlen(replyPath) + 1);
    }
    assert(mach_port_mod_refs(mach_task_self(), namePort, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS);
    identity->taskName = namePort;
    return 0;
}

int LSBTransportCopyChildIdentity(pid_t pid, LSBPeerProcessIdentity *identity) {
    return LSBTransportCopyProcessIdentity(pid, identity);
}

int LSBTransportKill(pid_t pid, int signalNumber) {
    ++kills;
    killedPID = pid; killedSignal = signalNumber;
    errno = EIO;
    return killError;
}

static int testTerminate(pid_t pid, int *signalNumber) {
    (void)pid; (void)signalNumber;
    ++nativeTerminations;
    errno = EBUSY;
    return ENOTSUP;
}

static mach_port_urefs_t sendReferences(void) {
    mach_port_urefs_t references;
    assert(mach_port_get_refs(mach_task_self(), namePort, MACH_PORT_RIGHT_SEND, &references) == KERN_SUCCESS);
    return references;
}

static int testPIDPath(int pid, void *buffer, uint32_t size) {
    (void)pid;
    ++nativePathCalls;
    errno = pathErrno;
    if (buffer && size >= 10 && pathResult) memcpy(buffer, "/host/app", 10);
    return pathResult;
}

__attribute__((used)) static int sandboxOriginalBody(pid_t pid, const char *operation, int type, ...) {
    ++originalSandboxCalls;
    assert(pid == 101 || pid == 42);
    if (operation && !strcmp(operation, "forward-variadic")) {
        assert(type == 0x76543210);
        va_list args;
        va_start(args, type);
        for (uint64_t i = 0; i < 12; ++i) assert(va_arg(args, uint64_t) == UINT64_C(0xfedcba9800000000) + i);
        assert(va_arg(args, double) == 3.25);
        assert(!strcmp(va_arg(args, const char *), "tail-stack-argument"));
        va_end(args);
    }
    if (operation == workspaceShmOperation) {
        assert(type == (SANDBOX_CHECK_NO_REPORT | 17));
        va_list args;
        va_start(args, type);
        assert(va_arg(args, const char *) == workspaceShmName);
        va_end(args);
    }
    errno = EBUSY;
    return 117;
}

// Capture the actual native forwarding boundary before a C prologue can repurpose its caller-saved registers.
__attribute__((naked)) int LSBTestSandboxOriginal(pid_t pid, const char *operation, int type, ...) {
    __asm__("adrp x9, _sandboxEntry@PAGE\n add x9, x9, _sandboxEntry@PAGEOFF\n"
            "stp x0, x1, [x9, #0]\n stp x2, x3, [x9, #16]\n stp x4, x5, [x9, #32]\n stp x6, x7, [x9, #48]\n str x8, [x9, #64]\n"
            "stp q0, q1, [x9, #80]\n stp q2, q3, [x9, #112]\n stp q4, q5, [x9, #144]\n stp q6, q7, [x9, #176]\n"
            "mrs x10, nzcv\n str x10, [x9, #208]\n mov x10, sp\n str x10, [x9, #216]\n str x30, [x9, #224]\n"
            "b _sandboxOriginalBody\n");
}

__attribute__((naked, noinline)) static int callWithRegisters(void) {
    __asm__("stp x29, x30, [sp, #-16]!\n mov x29, sp\n"
            "adrp x9, _expectedStack@PAGE\n add x9, x9, _expectedStack@PAGEOFF\n mov x10, sp\n str x10, [x9]\n"
            "adrp x9, _expectedLink@PAGE\n add x9, x9, _expectedLink@PAGEOFF\n adr x10, 1f\n str x10, [x9]\n"
            "mov x0, #42\n adrp x1, _captureOperation@PAGE\n add x1, x1, _captureOperation@PAGEOFF\n"
            "mov x2, #0x1234\n mov x3, #0x3333\n mov x4, #0x4444\n mov x5, #0x5555\n mov x6, #0x6666\n"
            "mov x7, #0x7777\n mov x8, #0x8888\n"
            "movi v0.16b, #0x10\n movi v1.16b, #0x11\n movi v2.16b, #0x12\n movi v3.16b, #0x13\n"
            "movi v4.16b, #0x14\n movi v5.16b, #0x15\n movi v6.16b, #0x16\n movi v7.16b, #0x17\n"
            "cmp xzr, xzr\n bl _LSBPeerSandboxCheck\n1: ldp x29, x30, [sp], #16\n ret\n");
}

__attribute__((naked, noinline)) static int callWithWorkspaceShmRegisters(void) {
    __asm__("sub sp, sp, #32\n stp x29, x30, [sp, #16]\n add x29, sp, #16\n"
            "adrp x9, _workspaceShmName@PAGE\n add x9, x9, _workspaceShmName@PAGEOFF\n str x9, [sp]\n"
            "adrp x9, _expectedStack@PAGE\n add x9, x9, _expectedStack@PAGEOFF\n mov x10, sp\n str x10, [x9]\n"
            "adrp x9, _expectedLink@PAGE\n add x9, x9, _expectedLink@PAGEOFF\n adr x10, 1f\n str x10, [x9]\n"
            "mov x0, #101\n adrp x1, _workspaceShmOperation@PAGE\n add x1, x1, _workspaceShmOperation@PAGEOFF\n"
            "mov x2, #17\n movk x2, #0x10, lsl #16\n mov x3, #0x3333\n mov x4, #0x4444\n mov x5, #0x5555\n mov x6, #0x6666\n"
            "mov x7, #0x7777\n mov x8, #0x8888\n"
            "movi v0.16b, #0x10\n movi v1.16b, #0x11\n movi v2.16b, #0x12\n movi v3.16b, #0x13\n"
            "movi v4.16b, #0x14\n movi v5.16b, #0x15\n movi v6.16b, #0x16\n movi v7.16b, #0x17\n"
            "cmp xzr, xzr\n bl _LSBPeerSandboxCheck\n1: ldp x29, x30, [sp, #16]\n add sp, sp, #32\n ret\n");
}

static void setCaller(uintptr_t begin, uintptr_t end) {
    for (unsigned i = 0; i < 8; ++i) atomic_store(&peerCallerRanges[i].image, 0);
    atomic_store(&peerCallerRanges[0].begin, begin);
    atomic_store(&peerCallerRanges[0].end, end);
    atomic_store(&peerCallerRanges[0].image, 1);
}

static void testScope(void) {
    const char *root = "/private/Group/RuntimeRoot", *exe = "/private/Group/RuntimeRoot/usr/libexec/runningboardd";
    const char *disabled[] = {NULL, "", "0", "true", "01"};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) {
        configurePeerScope(disabled[i], NULL, root, exe);
        assert(!peerInspectionEnabled);
    }
    const char *wrongExe[] = {"/usr/libexec/runningboardd", "/private/Group/RuntimeRoot-other/usr/libexec/runningboardd",
        "/private/Group/RuntimeRoot/usr/sbin/runningboardd", "/private/Group/RuntimeRoot/usr/libexec/runningboardd-helper",
        "/private/Group/RuntimeRoot/usr/libexec/../libexec/runningboardd", "/private/Group/RuntimeRoot/bin/df", "/x"};
    for (unsigned i = 0; i < sizeof(wrongExe) / sizeof(*wrongExe); ++i) {
        configurePeerScope("1", NULL, root, wrongExe[i]);
        assert(!peerInspectionEnabled);
    }
    const char *wrongRoot[] = {NULL, "", "/", "relative", "/private/Group/RuntimeRoot/", "/private//Group", "/A:/B", "/A/./B", "/A/../B"};
    for (unsigned i = 0; i < sizeof(wrongRoot) / sizeof(*wrongRoot); ++i) {
        configurePeerScope("1", NULL, wrongRoot[i], exe);
        assert(!peerInspectionEnabled);
    }
    configurePeerScope("1", NULL, root, exe);
    assert(!strcmp(peerRuntimeRoot, root));
    assert(peerInspectionEnabled && !strcmp(servicesPhysicalPath,
        "/private/Group/RuntimeRoot/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices"));
    struct { struct mach_header_64 header; struct segment_command_64 text, data; } image = {0};
    image.header = (struct mach_header_64){.magic = MH_MAGIC_64, .ncmds = 2, .sizeofcmds = sizeof(image.text) + sizeof(image.data)};
    image.text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image.text), .vmaddr = 0x1000,
                                           .vmsize = 0x100, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    image.data = image.text;
    image.data.vmaddr = 0x2000;
    image.data.initprot = VM_PROT_READ | VM_PROT_WRITE;
    const struct mach_header *header = (const struct mach_header *)&image;
    const char *paths[] = {servicesInstallPath, servicesPhysicalPath};
    for (unsigned i = 0; i < 2; ++i) {
        rememberPeerCaller(header, 0x4000, paths[i]);
        assert(!peerCallerAllowed(0x4fff) && peerCallerAllowed(0x5000) && peerCallerAllowed(0x50ff));
        assert(!peerCallerAllowed(0x5100) && !peerCallerAllowed(0x6000));
        forgetPeerCaller(header);
        assert(!peerCallerAllowed(0x5000));
    }
    const char *wrongPaths[] = {"/Plugins/RunningBoardServices", "/System/Library/Frameworks/RunningBoardServices.framework/RunningBoardServices",
        "/OtherRoot/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices",
        "/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServicesHelper"};
    for (unsigned i = 0; i < sizeof(wrongPaths) / sizeof(*wrongPaths); ++i) {
        rememberPeerCaller(header, 0, wrongPaths[i]);
        assert(!peerCallerAllowed(0x1000));
    }
    setCaller(0x1000, 0x1100);
}

static void testShortInfo(void) {
    unsigned char output[80], original[80];
    memset(output, 0xcc, sizeof(output));
    memcpy(original, output, sizeof(output));
    struct { uintptr_t caller; int pid, flavor; uint64_t argument; int size, result, error; } rejected[] = {
        {0x999, 202, 13, 0, 80, 0, EPERM}, {0x1100, 202, 13, 0, 80, 0, EPERM}, {0x1000, 101, 13, 0, 80, 0, EPERM},
        {0x1000, 99, 13, 0, 80, 0, EPERM}, {0x1000, -1, 13, 0, 80, 0, EPERM}, {0x1000, 202, 3, 0, 80, 0, EPERM},
        {0x1000, 202, 13, 1, 80, 0, EPERM}, {0x1000, 202, 13, 0, 63, 0, EPERM}, {0x1000, 202, 13, 0, -1, 0, EPERM},
        {0x1000, 202, 13, 0, 80, 64, EPERM}, {0x1000, 202, 13, 0, 80, 0, ESRCH}, {0x1000, 202, 13, 0, 80, -1, EPERM}
    };
    unsigned before = queries;
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i) {
        errno = ERANGE;
        assert(LSBPeerProcessPIDInfo(rejected[i].caller, rejected[i].pid, rejected[i].flavor, rejected[i].argument, output,
            rejected[i].size, rejected[i].result, rejected[i].error) == rejected[i].result && errno == ERANGE);
        assert(!memcmp(output, original, sizeof(output)) && queries == before);
    }
    assert(!LSBPeerProcessPIDInfo(0x1000, 202, 13, 0, NULL, 80, 0, EPERM) && queries == before);
    errno = EPERM;
    assert(LSBPeerProcessPIDInfo(0x1000, 202, 13, 0, output, sizeof(output), 0, EPERM) == 64 && errno == EPERM);
    uint32_t pid;
    memcpy(&pid, output, sizeof(pid));
    assert(pid == 202 && output[4] == 0xa5 && output[63] == 0xa5 && output[64] == 0xcc && sendReferences() == 1);
    for (unsigned i = 0; i < 4; ++i) {
        queryError = i == 0 ? ESRCH : 0;
        badAudit = i == 1; badSnapshot = i == 2; badPath = i == 3;
        memcpy(output, original, sizeof(output));
        errno = EPERM;
        assert(!LSBPeerProcessPIDInfo(0x1000, 202, 13, 0, output, sizeof(output), 0, EPERM) && errno == EPERM);
        assert(!memcmp(output, original, sizeof(output)) && sendReferences() == 1);
    }
    queryError = 0; badAudit = badSnapshot = badPath = false;
}

static void testTaskNames(void) {
    mach_port_t result = 0x1234;
    unsigned before = queries;
    errno = EDOM;
    assert(LSBPeerProcessTaskName(0x999, mach_task_self(), 202, &result, KERN_FAILURE) == KERN_FAILURE);
    assert(LSBPeerProcessTaskName(0x1000, MACH_PORT_NULL, 202, &result, KERN_FAILURE) == KERN_FAILURE);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 101, &result, KERN_FAILURE) == KERN_FAILURE);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, NULL, KERN_FAILURE) == KERN_FAILURE);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_INVALID_ARGUMENT) == KERN_INVALID_ARGUMENT);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_SUCCESS) == KERN_SUCCESS);
    assert(result == 0x1234 && queries == before && errno == EDOM);
    queryError = ESRCH;
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_FAILURE) == KERN_FAILURE && result == 0x1234);
    queryError = 0;
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_FAILURE) == KERN_SUCCESS);
    assert(result == namePort && sendReferences() == 2 && errno == EDOM);
    assert(mach_port_deallocate(mach_task_self(), result) == KERN_SUCCESS && sendReferences() == 1);
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_PROTECTION_FAILURE) == KERN_SUCCESS);
    assert(mach_port_deallocate(mach_task_self(), result) == KERN_SUCCESS && sendReferences() == 1);
    replyPID = 303; // RunningBoard's API still uses native PIDs; it must reject mismatched replies.
    result = 0x1234;
    assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, &result, KERN_FAILURE) == KERN_FAILURE && result == 0x1234);
    assert(sendReferences() == 1);
    replyPID = 0;
}

static void testLaunchdIdentityAndTermination(void) {
    const char *root = "/private/Group/RuntimeRoot", *exe = "/private/Group/RuntimeRoot/sbin/launchd_sim";
    const char *flags[] = {NULL, "", "0", "01", "true"};
    for (unsigned i = 0; i < sizeof(flags) / sizeof(*flags); ++i) {
        configurePeerScope("1", flags[i], root, exe);
        assert(!peerInspectionEnabled && !launchdInspectionEnabled);
    }
    const char *wrong[] = {"/sbin/launchd_sim", "/private/Group/RuntimeRoot/sbin/launchd_sim-helper",
        "/private/Group/RuntimeRoot/sbin/../sbin/launchd_sim", "/private/Group/RuntimeRoot-other/sbin/launchd_sim"};
    for (unsigned i = 0; i < sizeof(wrong) / sizeof(*wrong); ++i) {
        configurePeerScope(NULL, "1", root, wrong[i]);
        assert(!peerInspectionEnabled && !launchdInspectionEnabled);
    }
    configurePeerScope(NULL, "1", root, exe);
    assert(peerInspectionEnabled && launchdInspectionEnabled && !strcmp(servicesPhysicalPath, exe));
    struct { struct mach_header_64 header; struct segment_command_64 text; } image = {0};
    image.header = (struct mach_header_64){.magic = MH_MAGIC_64, .ncmds = 1, .sizeofcmds = sizeof(image.text)};
    image.text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image.text), .vmaddr = 0x4000,
                                           .vmsize = 0x100, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    setCaller(0, 0);
    const struct mach_header *header = (const struct mach_header *)&image;
    rememberPeerCaller(header, 0, servicesInstallPath);
    assert(!peerCallerAllowed(0x4000));
    rememberPeerCaller(header, 0, "/sbin/launchd_sim-helper");
    assert(!peerCallerAllowed(0x4000));
    const char *paths[] = {launchdExecutable, exe};
    for (unsigned i = 0; i < 2; ++i) {
        rememberPeerCaller(header, 0, paths[i]);
        assert(peerCallerAllowed(0x4000) && !LSBPeerProcessCallerEnabled(0x4000));
        forgetPeerCaller(header);
        assert(!peerCallerAllowed(0x4000));
    }
    setCaller(0x4000, 0x4100);
    unsigned before = queries;
    kern_return_t result = KERN_ABORTED;
    mach_port_t task = 0x1234;
    errno = EDOM;
    assert(!LSBLaunchdProcessTaskName(0x3fff, mach_task_self(), 202, &task, &result));
    assert(!LSBLaunchdProcessTaskName(0x4000, MACH_PORT_NULL, 202, &task, &result));
    assert(!LSBLaunchdProcessTaskName(0x4000, mach_task_self(), 99, &task, &result));
    assert(!LSBLaunchdProcessTaskName(0x4000, mach_task_self(), 101, &task, &result));
    assert(result == KERN_ABORTED && task == 0x1234 && queries == before && errno == EDOM);
    assert(LSBLaunchdProcessTaskName(0x4000, mach_task_self(), -1, &task, &result) && result == KERN_INVALID_ARGUMENT);
    assert(LSBLaunchdProcessTaskName(0x4000, mach_task_self(), 202, NULL, &result) && result == KERN_INVALID_ARGUMENT && queries == before);
    queryError = ESRCH;
    assert(LSBLaunchdProcessTaskName(0x4000, mach_task_self(), 202, &task, &result) && result == KERN_FAILURE);
    assert(task == 0x1234 && errno == EDOM); // A failed managed lookup must never fall through to a reused native PID.
    queryError = EACCES;
    assert(LSBLaunchdProcessTaskName(0x4000, mach_task_self(), 202, &task, &result) && result == KERN_PROTECTION_FAILURE);
    queryError = 0; replyPID = 303;
    assert(LSBLaunchdProcessTaskName(0x4000, mach_task_self(), 202, &task, &result) && result == KERN_SUCCESS);
    assert(task == namePort && sendReferences() == 2 && errno == EDOM);
    assert(!mach_port_deallocate(mach_task_self(), task) && sendReferences() == 1);
    badSnapshot = true;
    assert(LSBLaunchdProcessTaskName(0x4000, mach_task_self(), 202, &task, &result) && result == KERN_FAILURE && sendReferences() == 1);
    badSnapshot = false;
    assert(LSBLaunchdProcessTaskName(0x4000, mach_task_self(), 202, (mach_port_t *)(uintptr_t)1, &result) && result == KERN_FAILURE);
    assert(sendReferences() == 1 && errno == EDOM);
    replyPID = 0;
    // Other peer fallbacks and sandbox policy remain exclusive to RunningBoardServices.
    before = queries;
    unsigned char info[64] = {0};
    assert(!LSBPeerProcessPIDInfo(0x4000, 202, 13, 0, info, sizeof(info), 0, EPERM));
    assert(!peerSandboxPolicy(101, "mach-task-name", SANDBOX_CHECK_NO_REPORT, 0x4000, NULL) && queries == before);
    int delivered = 55;
    assert(launchdProcessTerminate(0x3fff, 202, &delivered) == ENOTSUP && nativeTerminations == 1 && errno == EBUSY);
    errno = EDOM;
    assert(launchdProcessTerminate(0x4000, 202, NULL) == EINVAL && !kills);
    assert(launchdProcessTerminate(0x4000, -1, &delivered) == EINVAL && !kills);
    assert(launchdProcessTerminate(0x4000, 99, &delivered) == EPERM && !kills);
    assert(launchdProcessTerminate(0x4000, 101, &delivered) == EPERM && !kills);
    killError = ESRCH;
    assert(launchdProcessTerminate(0x4000, 202, &delivered) == ESRCH && delivered == 55 && errno == EDOM);
    killError = 0;
    assert(!launchdProcessTerminate(0x4000, 202, &delivered) && delivered == SIGTERM && killedPID == 202 && killedSignal == SIGTERM);
    assert(errno == EDOM && kills == 2 && nativeTerminations == 1);
    configurePeerScope("1", NULL, root, "/private/Group/RuntimeRoot/usr/libexec/runningboardd");
    setCaller(0x1000, 0x1100);
    assert(launchdProcessTerminate(0x1000, 202, &delivered) == ENOTSUP && nativeTerminations == 2);
}

static void testPaths(void) {
    char output[256];
    inspectionHandled = true;
    unsigned nativeBefore = nativePathCalls, queriesBefore = queries;
    errno = EDOM;
    assert(peerPIDPath(0x999, 404, output, sizeof(output)) == (int)strlen(output));
    assert(!strcmp(output, "/private/Group/RuntimeRoot/Applications/CocoaTop.app/CocoaTop") && inspectionCalls == 1 && errno == EDOM);
    assert(nativePathCalls == nativeBefore && queries == queriesBefore); // Authorized inspector paths bypass RBS scope without widening it.
    inspectionHandled = false;
    assert(peerPIDPath(0x999, 202, output, sizeof(output)) == 9 && !strcmp(output, "/host/app"));
    unsigned before = queries;
    assert(peerPIDPath(0x1000, 101, output, sizeof(output)) == 9 && queries == before);
    assert(peerPIDPath(0x1000, 202, output, sizeof(output)) == (int)strlen(physicalGuestPath) && !strcmp(output, physicalGuestPath));
    assert(errno == EPERM && sendReferences() == 1);
    queryError = ESRCH;
    assert(peerPIDPath(0x1000, 202, output, sizeof(output)) == 9 && !strcmp(output, "/host/app") && errno == EPERM);
    queryError = 0;
    memset(output, 0xcc, sizeof(output));
    assert(!peerPIDPath(0x1000, 202, output, 4) && errno == ENOSPC && (unsigned char)output[4] == 0xcc && sendReferences() == 1);
    assert(nativePathCalls == 5);
    memset(output, 0xcc, sizeof(output));
    pathResult = 0;
    assert(!peerPIDPath(0x1000, 202, output, (uint32_t)strlen(physicalGuestPath)) && errno == ENOSPC);
    assert((unsigned char)output[0] == 0xcc); // Space for the complete path including NUL is required.
    assert(peerPIDPath(0x1000, 202, output, sizeof(physicalGuestPath)) == (int)strlen(physicalGuestPath));
    assert(!memcmp(output, physicalGuestPath, sizeof(physicalGuestPath)) && (unsigned char)output[sizeof(physicalGuestPath)] == 0xcc);
    const char *malformed[] = {"relative", "/", "/System//SpringBoard", "/System/../SpringBoard", "/System/./SpringBoard", "/System/SpringBoard/"};
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(*malformed); ++i) {
        replyPath = malformed[i];
        memset(output, 0xcc, sizeof(output));
        assert(!peerPIDPath(0x1000, 202, output, sizeof(output)) && errno == EPERM);
        assert((unsigned char)output[0] == 0xcc && sendReferences() == 1);
    }
    char longPath[LSB_PATH_BYTES];
    memset(longPath, 'a', sizeof(longPath));
    longPath[0] = '/'; longPath[sizeof(longPath) - 1] = '\0';
    replyPath = longPath;
    assert(!peerPIDPath(0x1000, 202, output, sizeof(output)) && errno == ENAMETOOLONG && sendReferences() == 1);
    replyPath = guestPath; pathResult = 9;
}

static void testInvalidOutputs(void) {
    size_t page = (size_t)vm_page_size;
    unsigned char *mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(mapping != MAP_FAILED);
    memset(mapping, 0xcc, page * 2);
    assert(!mprotect(mapping, page, PROT_READ) && !mprotect(mapping + page, page, PROT_NONE));
    void *invalid[] = {(void *)(uintptr_t)1, mapping, mapping + page - 2, mapping + page};
    pathResult = 0; // A native EPERM need not have inspected or touched the supplied output.
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        errno = EPERM;
        assert(!LSBPeerProcessPIDInfo(0x1000, 202, 13, 0, invalid[i], 64, 0, EPERM) && errno == EFAULT);
        assert(sendReferences() == 1);
        errno = EDOM;
        assert(LSBPeerProcessTaskName(0x1000, mach_task_self(), 202, invalid[i], KERN_FAILURE) == KERN_FAILURE && errno == EDOM);
        assert(sendReferences() == 1); // The caller received no right, so the acquired send reference must be released.
        assert(!peerPIDPath(0x1000, 202, invalid[i], 256) && errno == EFAULT && sendReferences() == 1);
    }
    assert(mapping[0] == 0xcc && mapping[page - 1] == 0xcc);
    pathErrno = EFAULT;
    unsigned before = queries;
    assert(!peerPIDPath(0x1000, 202, (void *)(uintptr_t)1, 256) && errno == EFAULT && queries == before);
    pathResult = 9; pathErrno = EPERM;
    assert(!munmap(mapping, page * 2));
}

__attribute__((noinline)) static void testSandbox(void) {
    setCaller((uintptr_t)&testSandbox, (uintptr_t)&testSandbox + 4096);
    errno = EDOM;
    assert(!LSBPeerSandboxCheck(101, "process-info-pidinfo", SANDBOX_CHECK_NO_REPORT) && errno == EDOM);
    assert(!LSBPeerSandboxCheck(101, "mach-task-name", SANDBOX_CHECK_NO_REPORT) && errno == EDOM && !originalSandboxCalls);
    assert(LSBPeerSandboxCheck(42, "process-info-pidinfo", SANDBOX_CHECK_NO_REPORT) == 117 && errno == EBUSY);
    assert(LSBPeerSandboxCheck(101, "mach-task-name", SANDBOX_CHECK_NO_REPORT | 1, "filter") == 117);
    assert(LSBPeerSandboxCheck(101, "other-operation", 0) == 117); // No variadic arguments: no va_arg is permitted on forwarding.
    assert(LSBPeerSandboxCheck(42, "forward-variadic", 0x76543210,
        UINT64_C(0xfedcba9800000000), UINT64_C(0xfedcba9800000001), UINT64_C(0xfedcba9800000002), UINT64_C(0xfedcba9800000003),
        UINT64_C(0xfedcba9800000004), UINT64_C(0xfedcba9800000005), UINT64_C(0xfedcba9800000006), UINT64_C(0xfedcba9800000007),
        UINT64_C(0xfedcba9800000008), UINT64_C(0xfedcba9800000009), UINT64_C(0xfedcba980000000a), UINT64_C(0xfedcba980000000b),
        3.25, "tail-stack-argument") == 117 && errno == EBUSY);
    assert(originalSandboxCalls == 4);
    assert(callWithRegisters() == 117 && errno == EBUSY);
    assert(sandboxEntry[0] == 42 && sandboxEntry[1] == (uintptr_t)captureOperation && sandboxEntry[2] == 0x1234);
    for (unsigned i = 3; i <= 8; ++i) assert(sandboxEntry[i] == i * 0x1111);
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t pattern = UINT64_C(0x0101010101010101) * (0x10 + i);
        assert(sandboxEntry[10 + 2 * i] == pattern && sandboxEntry[11 + 2 * i] == pattern);
    }
    assert(sandboxEntry[26] == 0x60000000 && sandboxEntry[27] == expectedStack && sandboxEntry[28] == expectedLink);
    setCaller(0x1000, 0x1100);
    assert(LSBPeerSandboxCheck(101, "mach-task-name", SANDBOX_CHECK_NO_REPORT) == 117 && originalSandboxCalls == 6);
    peerInspectionEnabled = false;
    assert(!peerSandboxPolicy(101, "mach-task-name", SANDBOX_CHECK_NO_REPORT, 0x1000, NULL));
}

static void testWorkspaceShmSandbox(void) {
    // This preflight belongs to the shared-memory adapter, independent of the RunningBoard peer-inspection opt-in.
    peerInspectionEnabled = false;
    const char *operations[] = {"ipc-posix-shm-read-data", "ipc-posix-shm-write-create", "ipc-posix-shm-write-unlink"};
    size_t page = (size_t)vm_page_size;
    void *inaccessible = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(inaccessible != MAP_FAILED);
    atomic_store(&workspaceShmPolicyReady, false);
    unsigned before = sandboxShmCalls;
    for (unsigned i = 0; i < sizeof(operations) / sizeof(*operations); ++i) {
        errno = ERANGE;
        policyErrnoReads = 0;
        assert(!peerSandboxPolicy(101, operations[i], SANDBOX_CHECK_NO_REPORT | 17, 0x1000, inaccessible));
        assert(!policyErrnoReads && errno == ERANGE && sandboxShmCalls == before);
    }
    // The full assembly path must also forward an exact signature before constructors without reading a variadic argument.
    policyErrnoReads = 0;
    assert(LSBPeerSandboxCheck(101, operations[1], SANDBOX_CHECK_NO_REPORT | 17) == 117 && errno == EBUSY);
    assert(!policyErrnoReads && sandboxShmCalls == before);
    atomic_store(&workspaceShmPolicyReady, true);
    unsigned originalBefore = originalSandboxCalls;
    sandboxShmAllowed = true;
    for (unsigned i = 0; i < sizeof(operations) / sizeof(*operations); ++i) {
        errno = EDOM;
        unsigned before = sandboxShmCalls;
        assert(!LSBPeerSandboxCheck(101, operations[i], SANDBOX_CHECK_NO_REPORT | 17, workspaceShmName));
        assert(sandboxShmCalls == before + 1 && sandboxShmName == workspaceShmName && sandboxShmOperation == operations[i]);
        assert(errno == EDOM && originalSandboxCalls == originalBefore);
    }
    // An unmatched signature must not dereference a nonexistent variadic argument, even with an inaccessible stack slot.
    const struct { pid_t pid; const char *operation; int type; } unmatched[] = {
        {99, "ipc-posix-shm-read-data", 17 | SANDBOX_CHECK_NO_REPORT},
        {42, "ipc-posix-shm-read-data", 17 | SANDBOX_CHECK_NO_REPORT},
        {0, "ipc-posix-shm-read-data", 17 | SANDBOX_CHECK_NO_REPORT},
        {-1, "ipc-posix-shm-read-data", 17 | SANDBOX_CHECK_NO_REPORT},
        {101, NULL, 17 | SANDBOX_CHECK_NO_REPORT}, {101, "other-operation", 17 | SANDBOX_CHECK_NO_REPORT},
        {101, "ipc-posix-shm-read-data-extra", 17 | SANDBOX_CHECK_NO_REPORT},
        {101, "ipc-posix-shm-read", 17 | SANDBOX_CHECK_NO_REPORT},
        {101, "ipc-posix-shm-read-data", 17}, {101, "ipc-posix-shm-read-data", 16 | SANDBOX_CHECK_NO_REPORT},
        {101, "ipc-posix-shm-read-data", 18 | SANDBOX_CHECK_NO_REPORT},
        {101, "ipc-posix-shm-read-data", 17 | SANDBOX_CHECK_NO_REPORT | 0x100}
    };
    before = sandboxShmCalls;
    for (unsigned i = 0; i < sizeof(unmatched) / sizeof(*unmatched); ++i) {
        errno = ERANGE;
        assert(!peerSandboxPolicy(unmatched[i].pid, unmatched[i].operation, unmatched[i].type, 0x1000, inaccessible));
        assert(errno == ERANGE && sandboxShmCalls == before);
    }
    assert(!peerSandboxPolicy(101, operations[0], SANDBOX_CHECK_NO_REPORT | 17, 0x1000, NULL) && sandboxShmCalls == before);
    assert(!munmap(inaccessible, page));
    assert(LSBPeerSandboxCheck(101, "other-operation", SANDBOX_CHECK_NO_REPORT | 17) == 117); // No variadic argument.
    assert(LSBPeerSandboxCheck(42, operations[0], SANDBOX_CHECK_NO_REPORT | 17, workspaceShmName) == 117 && sandboxShmCalls == before);
    sandboxShmAllowed = false; // A disabled adapter, unrelated name or unsafe backing directory must retain the original denial.
    errno = EDOM;
    assert(LSBPeerSandboxCheck(101, operations[0], SANDBOX_CHECK_NO_REPORT | 17, "/unmanaged") == 117 && errno == EBUSY);
    assert(sandboxShmCalls == before + 1 && !strcmp(sandboxShmName, "/unmanaged"));
    const char *missingName = NULL;
    errno = EDOM;
    assert(!peerSandboxPolicy(101, operations[0], SANDBOX_CHECK_NO_REPORT | 17, 0x1000, &missingName));
    assert(!sandboxShmName && errno == EDOM);
    assert(callWithWorkspaceShmRegisters() == 117 && errno == EBUSY);
    assert(sandboxShmName == workspaceShmName && sandboxShmOperation == workspaceShmOperation);
    assert(sandboxEntry[0] == 101 && sandboxEntry[1] == (uintptr_t)workspaceShmOperation && sandboxEntry[2] == (SANDBOX_CHECK_NO_REPORT | 17));
    for (unsigned i = 3; i <= 8; ++i) assert(sandboxEntry[i] == i * 0x1111);
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t pattern = UINT64_C(0x0101010101010101) * (0x10 + i);
        assert(sandboxEntry[10 + 2 * i] == pattern && sandboxEntry[11 + 2 * i] == pattern);
    }
    assert(sandboxEntry[26] == 0x60000000 && sandboxEntry[27] == expectedStack && sandboxEntry[28] == expectedLink);
}

int main(void) {
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &namePort) == KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(), namePort, namePort, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    testScope(); testShortInfo(); testTaskNames(); testPaths(); testInvalidOutputs(); testLaunchdIdentityAndTermination(); testSandbox();
    testWorkspaceShmSandbox();
    assert(sendReferences() == 1);
    assert(mach_port_deallocate(mach_task_self(), namePort) == KERN_SUCCESS);
    assert(mach_port_mod_refs(mach_task_self(), namePort, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS);
    puts("PeerProcessCompatibilityTests passed");
    return 0;
}
