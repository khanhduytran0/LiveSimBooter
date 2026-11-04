// Device acceptance test for a cooperating guest frame and the native fork snapshot broker.
#include "../../../Guest/Shared/Fork/ForkContext.h"
#include "../../../Guest/libLiveSimSpawn/ForkHook.h"
#include "../../../ProcessSupport/SpawnTransport.h"
#include "../../../Guest/Shared/PrivateMachVM.h"
#include <errno.h>
#include <mach/vm_statistics.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern void LSBForkFixturePage(void), LSBForkFixtureMarker(void), LSBForkFixtureContinuation(void);
static const uintptr_t Code = 0x120000000ULL;
static uintptr_t Stack = 0x120010000ULL, Omit = 0x120020000ULL;
enum { Page = 16384, Before = 0x1357, After = 0x369c, SavedRegister = 0x2468, SavedFloat = 0x7bdf, ChildStatus = 37, BashStatus = 42 };

typedef struct { uint64_t result, x19, sp, pc, before, after, d8; uint32_t fpcr, fpsr; } ChildReport;
typedef struct { audit_token_t audit; pid_t logicalPID; } ChildReady;
typedef struct { const char *executable; int socket; pid_t pid; bool execBash; } Fixture;
static Fixture *publicJob;
static LSBForkContext publicFrame;
static volatile sig_atomic_t atforkPrepare, atforkParent;
static void prepareHandler(void) { ++atforkPrepare; }
static void parentHandler(void) { ++atforkParent; }

_Static_assert(sizeof(ChildReport) == 64 && offsetof(ChildReport, sp) == 16 && offsetof(ChildReport, before) == 32, "assembly child report layout");
_Static_assert(offsetof(ChildReport, d8) == 48 && offsetof(ChildReport, fpcr) == 56 && offsetof(ChildReport, fpsr) == 60, "assembly FP report layout");
extern pid_t LSBForkFixtureRun(uintptr_t entry, uintptr_t stackTop, Fixture *job, pid_t (*checkpoint)(Fixture *, const LSBForkContext *), LSBForkContext *frame);
extern void LSBForkFixtureResumeChild(const LSBForkContext *frame, const LSBForkContext *bootstrap) __attribute__((noreturn));

static int transfer(int fd, void *buffer, size_t size, bool writing) {
    char *bytes = buffer;
    while (size) {
        struct pollfd event = { .fd = fd, .events = writing ? POLLOUT : POLLIN };
        int ready = poll(&event, 1, 20000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return ready < 0 ? errno : ETIMEDOUT;
        ssize_t n = writing ? write(fd, bytes, size) : read(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? errno : EPIPE;
        bytes += n;
        size -= (size_t)n;
    }
    return 0;
}

static uintptr_t offsetOf(void (*label)(void)) {
    return (uintptr_t)label - (uintptr_t)LSBForkFixturePage;
}

static kern_return_t mapTextAlias(uintptr_t address) {
    mach_vm_address_t destination = address;
    vm_prot_t current = 0, maximum = 0;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &destination, Page, 0, VM_FLAGS_FIXED, mach_task_self(), (uintptr_t)LSBForkFixturePage,
                                     false, &current, &maximum, VM_INHERIT_SHARE);
    if (kr) return kr;
    return destination == address && (current & VM_PROT_EXECUTE) ? KERN_SUCCESS : KERN_PROTECTION_FAILURE;
}

static kern_return_t allocateRange(uintptr_t address, size_t length, vm_inherit_t inheritance) {
    mach_vm_address_t destination = address;
    kern_return_t kr = mach_vm_map(mach_task_self(), &destination, length, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0, false,
                                   VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, inheritance);
    if (kr) return kr;
    return destination == address ? KERN_SUCCESS : KERN_INVALID_ADDRESS;
}

static bool hasProtection(uintptr_t address, vm_prot_t protection) {
    natural_t depth = UINT32_MAX;
    mach_vm_address_t start = address;
    mach_vm_size_t span = 0;
    vm_region_submap_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &start, &span, &depth, (vm_region_recurse_info_t)&info, &count);
    return !kr && count >= VM_REGION_SUBMAP_INFO_V0_COUNT_64 && !info.is_submap && start <= address &&
           address - start <= span && Page <= span - (address - start) && info.protection == protection;
}

static int ownToken(audit_token_t *audit) {
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    return task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)audit, &count) == KERN_SUCCESS && count == TASK_AUDIT_TOKEN_COUNT ? 0 : EIO;
}

static bool matches(const LSBForkContext *frame) {
    return frame->sp == Stack + Page - 96 && frame->pc == Code + offsetOf(LSBForkFixtureContinuation) && frame->x19ToX28[0] == SavedRegister &&
           frame->d8ToD15[0] == SavedFloat && (frame->fpcr & 0xc00000) == 0x400000 && (frame->fpsr & 0x8000000);
}

static int child(const char *mode, bool publicMode) {
    kern_return_t kr = mapTextAlias(Omit);
    if (kr) { fprintf(stderr, "child marker alias kr=%d\n", kr); return 10; }
    unsigned (*marker)(void) = (unsigned (*)(void))(Omit + offsetOf(LSBForkFixtureMarker));
    if (marker() != 0x77) return 11;
    ChildReady ready = {.logicalPID = getpid()};
    if (ownToken(&ready.audit) || transfer(STDIN_FILENO, &ready, sizeof(ready), true)) return 12;
    audit_token_t parent;
    if (transfer(STDIN_FILENO, &parent, sizeof(parent), false)) return 13;
    LSBForkContext frame;
    if (publicMode) {
        uint8_t checkpoint = 0;
        if (transfer(STDIN_FILENO, &frame, sizeof(frame), false) || transfer(STDIN_FILENO, &checkpoint, sizeof(checkpoint), false)) return 13;
        if (checkpoint != LSB_FORK_CHECKPOINT || !matches(&frame)) return 15;
    }
    int32_t applied = LSBTransportAcceptFork(&parent);
    if (!applied && (!hasProtection(Code, VM_PROT_READ | VM_PROT_EXECUTE) || !hasProtection(Stack, VM_PROT_READ | VM_PROT_WRITE) || marker() != 0x77))
        applied = EFAULT;
    if (transfer(STDIN_FILENO, &applied, sizeof(applied), true)) return 14;
    if (!strcmp(mode, "reject")) {
        // Reserve the earlier candidate holes again to prove the broker made no partial snapshot.
        return applied == EADDRINUSE && marker() == 0x77 && !allocateRange(Code, Page, VM_INHERIT_NONE) && !allocateRange(Stack, Page, VM_INHERIT_NONE) ? 0 : 16;
    }
    if (applied) return 14;
    if (!publicMode && (transfer(STDIN_FILENO, &frame, sizeof(frame), false) || !matches(&frame))) return 15;
    LSBForkContext bootstrap;
    bool execBash = !strcmp(mode, "public-exec-child");
    if (execBash && LSBCaptureForkContext(&bootstrap) == 0) {
        // Back on this child's original stack, with its own pthread/TSD, broker connection and descriptor table.
        char *args[] = {"bash", "--noprofile", "--norc", "-c",
            "printf 'BASH_EXEC_PID=%s\\n' \"$$\"; printf '%s\\n' \"$$\" >&0 || exit 71; exit 42", NULL};
        execve("/usr/local/bin/bash", args, environ);
        perror("fork child execve bash");
        _exit(72);
    }
    LSBForkFixtureResumeChild(&frame, execBash ? &bootstrap : NULL);
}

static int startChild(Fixture *job, const char *mode) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return errno;
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = posix_spawn_file_actions_addclose(&actions, sockets[0]);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        if (!error) error = posix_spawn_file_actions_addclose(&actions, sockets[1]);
        char *args[] = {(char *)job->executable, (char *)mode, NULL};
        if (!error) error = posix_spawn(&job->pid, job->executable, &actions, NULL, args, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(sockets[1]);
    if (error) close(sockets[0]);
    else job->socket = sockets[0];
    return error;
}

static int stageSnapshot(Fixture *job, const char *mode) {
    int error = startChild(job, mode);
    ChildReady ready = {};
    if (!error) error = transfer(job->socket, &ready, sizeof(ready), false);
    if (!error && ready.logicalPID != job->pid) error = ESRCH;
    audit_token_t parent;
    if (!error) error = ownToken(&parent);
    const LSBForkWireRange ranges[] = {{Code, Page}, {Stack, Page}, {Omit, Page}};
    if (!error) error = LSBTransportOfferFork(&ready.audit, ranges, sizeof(ranges) / sizeof(ranges[0]));
    if (!error) error = transfer(job->socket, &parent, sizeof(parent), true);
    if (!error) printf("broker_offer parent=%d child=%d native=%u generation=%u\n", getpid(), job->pid, ready.audit.val[5], ready.audit.val[7]);
    return error;
}

static int offerSnapshot(Fixture *job, bool expectCollision) {
    int error = stageSnapshot(job, expectCollision ? "reject" : "child");
    int applied = EPROTO;
    if (!error) error = transfer(job->socket, &applied, sizeof(applied), false);
    if (!error) error = applied;
    return error;
}

static int preparePublicFork(LSBForkAttempt *attempt, const LSBForkContext *caller) {
    // Only the fixed guest frame can run after this snapshot. Everything it needs is in its own code and stack page.
    if (!publicJob || !matches(caller) || publicJob->pid) return LSB_FORK_NOT_HANDLED;
    publicFrame = *caller;
    int error = stageSnapshot(publicJob, publicJob->execBash ? "public-exec-child" : "public-child");
    attempt->child = publicJob->pid;
    if (!error) error = transfer(publicJob->socket, &publicFrame, sizeof(publicFrame), true);
    // fork owns the checkpoint descriptor. Keep the original to read the child's raw syscall report after fork returns.
    if (!error && (attempt->socket = dup(publicJob->socket)) < 0) error = errno;
    return error;
}

static pid_t checkpoint(Fixture *job, const LSBForkContext *frame) {
    int error = matches(frame) ? offerSnapshot(job, false) : EINVAL;
    if (!error) {
        LSBForkContext message = *frame;
        error = transfer(job->socket, &message, sizeof(message), true);
    }
    if (!error) return job->pid;
    fprintf(stderr, "snapshot errno=%d (%s)\n", error, strerror(error));
    return -1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    const char *mode = argc == 2 ? argv[1] : "";
    bool execBash = !strcmp(mode, "public-exec") || !strcmp(mode, "public-exec-child");
    bool publicMode = execBash || !strcmp(mode, "public") || !strcmp(mode, "public-child");
    // libc's atfork handlers and guest spawn transport use a normal-sized stack. Only the page holding the caller is inherited by this fixture.
    size_t stackBytes = publicMode ? 1024 * 1024 : Page;
    if (publicMode) { Stack = 0x121000000ULL; Omit = Stack + 2 * Page; }
    if (!strcmp(mode, "child") || !strcmp(mode, "reject") || !strcmp(mode, "public-child") || !strcmp(mode, "public-exec-child"))
        return child(mode, publicMode);
    if ((!publicMode && argc != 1) || getpagesize() != Page || ((uintptr_t)LSBForkFixturePage & (Page-1)) ||
        offsetOf(LSBForkFixtureMarker) >= Page || offsetOf(LSBForkFixtureContinuation) >= Page) return 64;
    kern_return_t kr = mapTextAlias(Code);
    if (!kr) kr = allocateRange(Stack + Page - stackBytes, stackBytes, VM_INHERIT_COPY);
    if (!kr) kr = allocateRange(Omit, Page, publicMode ? VM_INHERIT_NONE : VM_INHERIT_COPY);
    if (kr) { fprintf(stderr, "source preflight kr=%d\n", kr); return 2; }
    if (!publicMode) {
        Fixture rejected = {.executable=argv[0], .socket=-1};
        int collision = offerSnapshot(&rejected, true), rejectedStatus = 0;
        if (rejected.socket >= 0) close(rejected.socket);
        if (rejected.pid <= 0) return 4;
        if (collision != EADDRINUSE) kill(rejected.pid, SIGKILL);
        pid_t reaped;
        do { reaped = waitpid(rejected.pid, &rejectedStatus, 0); } while (reaped < 0 && errno == EINTR);
        printf("required_collision=%d discarded_child=%d status=0x%x\n", collision, reaped, rejectedStatus);
        if (collision != EADDRINUSE || reaped != rejected.pid || rejectedStatus != 0 ||
            vm_inherit(mach_task_self(), Omit, Page, VM_INHERIT_NONE) != KERN_SUCCESS) return 5;
    }
    *(volatile uint64_t *)Omit = 0x89;
    LSBForkContext manualFrame = {};
    LSBForkContext *frame = publicMode ? &publicFrame : &manualFrame;
    Fixture job = {.executable=argv[0], .socket=-1, .execBash=execBash};
    if (publicMode) {
        int error = pthread_atfork(prepareHandler, parentHandler, NULL);
        if (!error) error = LSBInstallForkProvider(preparePublicFork);
        if (error) { fprintf(stderr, "install public fork provider: %d\n", error); return 6; }
        publicJob = &job;
    }
    pid_t result = LSBForkFixtureRun(Code, Stack + Page, &job, publicMode ? NULL : checkpoint, frame);
    if (publicMode) {
        int forkError = result < 0 ? errno : 0;
        printf("public_fork=%d errno=%d atfork_prepare=%d atfork_parent=%d\n", result, forkError, atforkPrepare, atforkParent);
        publicJob = NULL;
        // The public hook owns cleanup and reaping of a staged child if the fork fails.
        if (result <= 0) { if (job.socket >= 0) close(job.socket); return 7; }
    }
    ChildReport report = {};
    int error = (result <= 0 || result != job.pid) ? ECHILD : transfer(job.socket, &report, sizeof(report), false);
    if (!error && execBash) {
        char expected[32], received[32] = {};
        int length = snprintf(expected, sizeof(expected), "%d\n", job.pid);
        error = transfer(job.socket, received, (size_t)length, false);
        if (!error && memcmp(expected, received, (size_t)length)) error = EPROTO;
        printf("bash_pid_matches_fork=%d socket_errno=%d\n", !error, error);
    }
    if (job.socket >= 0) close(job.socket);
    if (job.pid <= 0) return 3;
    if (error) kill(job.pid, SIGKILL);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(job.pid, &status, 0); } while (waited < 0 && errno == EINTR);
    uint64_t parentLocal = matches(frame) ? *(volatile uint64_t *)(uintptr_t)frame->sp : 0;
    printf("parent_return=%d child_return=%llu pc=0x%llx sp=0x%llx x19=0x%llx child_local=0x%llx->0x%llx parent_local=0x%llx wait=%d status=0x%x\n",
           result, report.result, report.pc, report.sp, report.x19, report.before, report.after, parentLocal, waited, status);
    printf("d8=0x%llx fpcr=0x%x fpsr=0x%x\n", report.d8, report.fpcr, report.fpsr);
    bool ok = !error && waited == job.pid && WIFEXITED(status) && WEXITSTATUS(status) == (execBash ? BashStatus : ChildStatus) &&
              report.result == 0 && report.pc == frame->pc && report.sp == frame->sp && report.x19 == SavedRegister &&
              report.before == Before && report.after == After && parentLocal == Before &&
              report.d8 == SavedFloat && report.fpcr == frame->fpcr && report.fpsr == frame->fpsr &&
              (!publicMode || (atforkPrepare == 1 && atforkParent == 1));
    printf("FORK_CONTINUATION_COMPLETE ok=%d\n", ok);
    return !ok;
}
