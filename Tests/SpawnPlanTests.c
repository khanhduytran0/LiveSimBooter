#define LSB_SPAWN_SHIM_TESTING 1
#include "../Guest/SpawnShim.c"

#include <assert.h>
#include <stdio.h>
#include <sys/event.h>
#include <sys/wait.h>

static unsigned transportCalls;
static int transportFailure;
static int pathSearch;
static int internalWakeFD = -1;
static int internalTerminalFD = -1;
static int creationMaskError;
static uint32_t capturedCreationMask = 027;
static unsigned execCalls;
static const char *expectedExecvePath;
static mach_port_t expectedRegisteredPort;
static bool inspectPorts;
static uint32_t capturedSpawnFlags;
extern int posix_spawn_file_actions_add_fileportdup2_np(posix_spawn_file_actions_t *, mach_port_t, int);
int LSBGuestChildWakeFD(void) { return internalWakeFD; }
int LSBGuestFakePTYWatchFD(void) { return internalTerminalFD; }
LSBChildStatus *LSBGuestChildStatus(void) { return NULL; }
int LSBTransportSetProcessGroup(pid_t pid, pid_t group) { (void)pid; (void)group; return ENOTSUP; }
// CreationMaskTests exercises the kernel-aware snapshot; this suite verifies serialization and error propagation.
int LSBReadCreationMask(uint32_t *mask) {
    if (creationMaskError) return creationMaskError;
    *mask = capturedCreationMask;
    return 0;
}

int LSBTransportSpawnWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports, pid_t *pid) {
    assert(LSBValidateSpawnPlan(plan) == 0);
    capturedSpawnFlags = plan->spawnFlags;
    assert(plan->creationMask == capturedCreationMask);
    for (uint32_t i = 0; i < plan->fdCount; ++i) assert(fcntl(fds[i], F_GETFD) >= 0);
    assert(ports && ports->registeredPresent && ports->bootstrapPresent);
    if (inspectPorts) assert(ports->count && ports->registered[0] == expectedRegisteredPort && ports->bootstrapPort == MACH_PORT_NULL);
    ++transportCalls;
    if (pathSearch) {
        assert(!strcmp(plan->strings + plan->path, transportCalls == 1 ? "/first/tool" : "/second/tool"));
        if (transportCalls == 1) return ENOENT;
    }
    if (transportFailure) return transportFailure;
    *pid = 4242;
    return 0;
}

int LSBTransportExecWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports) {
    ++execCalls;
    if (expectedExecvePath) {
        assert(!strcmp(plan->strings + plan->path, expectedExecvePath));
        assert(plan->argc == 2 && !strcmp(plan->strings + plan->argv[0], "custom-name") && !strcmp(plan->strings + plan->argv[1], "two words"));
        assert(plan->envc == 0 && plan->spawnFlags == 0 && plan->actionCount == 0);
    }
    pid_t ignored;
    return LSBTransportSpawnWithPorts(plan, fds, ports, &ignored);
}

static LSBSpawnPlan *newPlan(void) {
    LSBSpawnPlan *plan = calloc(1, sizeof(*plan));
    assert(plan);
    plan->version = LSB_PLAN_VERSION;
    plan->argc = 1;
    assert(LSBPlanAddString(plan, "/bin/test", &plan->path) == 0);
    plan->argv[0] = plan->path;
    assert(LSBPlanAddString(plan, "/", &plan->cwd) == 0);
    return plan;
}

static void emptyFDs(int fds[LSB_MAX_FDS]) { for (unsigned i = 0; i < LSB_MAX_FDS; ++i) fds[i] = -1; }
static void importFD(LSBSpawnPlan *plan, int fds[LSB_MAX_FDS], int source, int destination, int flags) {
    assert(plan->fdCount < LSB_MAX_FDS);
    fds[plan->fdCount] = dup(source);
    assert(fds[plan->fdCount] >= 0);
    plan->fds[plan->fdCount++] = (LSBInheritedFD){destination, (uint32_t)flags};
}

#define CHECK(condition) do { if (!(condition)) _exit((__LINE__ % 200) + 1); } while (0)

static void runChild(void (*test)(void), const char *name) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) { test(); _exit(0); }
    int status;
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "%s failed: status=%d\n", name, status);
        abort();
    }
}

static void validation(void) {
    LSBSpawnPlan *plan = newPlan();
    assert(LSBValidateSpawnPlan(plan) == 0);
    plan->argc = LSB_MAX_VECTOR + 1;
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->argc = 1;
    plan->argv[0] = plan->stringBytes;
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->argv[0] = plan->path + 1;
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->argv[0] = plan->path;
    plan->strings[plan->stringBytes - 1] = 'x';
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->strings[plan->stringBytes - 1] = 0;
    plan->spawnFlags = POSIX_SPAWN_SETEXEC;
    assert(LSBValidateSpawnPlan(plan) == ENOTSUP);
    plan->spawnFlags = 0;
    plan->actionCount = 1;
    plan->actions[0] = (LSBFileAction){.kind = 100, .fd = 0};
    assert(LSBValidateSpawnPlan(plan) == ENOTSUP);
    plan->actionCount = 0;
    plan->fdCount = 2;
    plan->fds[0].number = plan->fds[1].number = 10;
    assert(LSBValidateSpawnPlan(plan) == EINVAL);
    plan->fdCount = 0;
    uint32_t unused;
    char *tooLong = malloc(LSB_STRING_BYTES + 1);
    assert(tooLong);
    memset(tooLong, 'x', LSB_STRING_BYTES);
    tooLong[LSB_STRING_BYTES] = 0;
    assert(LSBPlanAddString(plan, tooLong, &unused) == E2BIG);
    free(tooLong);
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    received[0] = open("/dev/null", O_RDONLY);
    assert(received[0] >= 0);
    int copied = received[0];
    plan->fdCount = 1;
    plan->version = 0;
    assert(LSBApplySpawnPlan(plan, received) == EINVAL);
    assert(received[0] == -1 && fcntl(copied, F_GETFD) == -1 && errno == EBADF);
    free(plan);
}

static void cyclesAndOrdering(void) {
    int a[2], b[2];
    CHECK(!pipe(a) && !pipe(b));
    CHECK(write(a[1], "a", 1) == 1 && write(b[1], "b", 1) == 1);
    CHECK(dup2(a[0], 61) == 61 && dup2(b[0], 60) == 60);
    LSBSpawnPlan *plan = newPlan();
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    // Imported descriptors themselves occupy each other's destination numbers.
    received[0] = 61;
    received[1] = 60;
    plan->fdCount = 2;
    plan->fds[0] = (LSBInheritedFD){60, 0};
    plan->fds[1] = (LSBInheritedFD){61, 0};
    plan->actionCount = 4;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 60, .target = 62};
    plan->actions[1] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 61, .target = 60};
    plan->actions[2] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 62, .target = 61};
    plan->actions[3] = (LSBFileAction){.kind = LSB_ACTION_CLOSE, .fd = 62};
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    char first, second;
    CHECK(read(60, &first, 1) == 1 && first == 'b');
    CHECK(read(61, &second, 1) == 1 && second == 'a');
    CHECK(fcntl(62, F_GETFD) == -1 && errno == EBADF);
    CHECK(received[0] == -1 && received[1] == -1);
}

static void closeOnExecVariant(bool defaults) {
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = defaults ? POSIX_SPAWN_CLOEXEC_DEFAULT : 0;
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    int source = open("/dev/null", O_RDWR);
    CHECK(source >= 0);
    importFD(plan, received, source, 60, FD_CLOEXEC);
    importFD(plan, received, source, 62, 0);
    importFD(plan, received, source, 63, FD_CLOEXEC);
    importFD(plan, received, source, 64, FD_CLOEXEC);
    plan->actionCount = 3;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 60, .target = 61};
    plan->actions[1] = (LSBFileAction){.kind = LSB_ACTION_INHERIT, .fd = 63};
    plan->actions[2] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 64, .target = 64};
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    CHECK(fcntl(60, F_GETFD) == -1);
    CHECK(fcntl(61, F_GETFD) == 0);
    CHECK(defaults ? fcntl(62, F_GETFD) == -1 : fcntl(62, F_GETFD) == 0);
    CHECK(fcntl(63, F_GETFD) == 0);
    CHECK(fcntl(64, F_GETFD) == -1);
}

static void normalCloseOnExec(void) { closeOnExecVariant(false); }
static void defaultCloseOnExec(void) { closeOnExecVariant(true); }

static int snapshotIndex(const LSBSpawnPlan *plan, int descriptor) {
    for (uint32_t i = 0; i < plan->fdCount; ++i) if (plan->fds[i].number == descriptor) return (int)i;
    return -1;
}

static void snapshotDropsKqueueVariant(bool defaults) {
    int queue = kqueue(), ordinary = open("/dev/null", O_RDONLY);
    CHECK(queue >= 0 && ordinary >= 0);
    CHECK(!fcntl(queue, F_SETFD, defaults ? 0 : FD_CLOEXEC));
    mach_port_t unsupported = MACH_PORT_NULL;
    CHECK(fileport_makeport(queue, &unsupported) != 0);
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = defaults ? POSIX_SPAWN_CLOEXEC_DEFAULT : 0;
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    CHECK(snapshotFDs(plan, received) == 0);
    CHECK(snapshotIndex(plan, queue) == -1);
    CHECK(defaults ? snapshotIndex(plan, ordinary) == -1 : snapshotIndex(plan, ordinary) >= 0);
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    CHECK(fcntl(queue, F_GETFD) == -1 && errno == EBADF);
    CHECK(defaults ? fcntl(ordinary, F_GETFD) == -1 : fcntl(ordinary, F_GETFD) == 0);
}

static void snapshotDropsCloseOnExecKqueue(void) { snapshotDropsKqueueVariant(false); }
static void snapshotDropsDefaultKqueue(void) { snapshotDropsKqueueVariant(true); }

static void snapshotKeepsActionInputs(void) {
    int channel[2], inherited = open("/dev/null", O_RDONLY), directory = open("/private", O_RDONLY | O_DIRECTORY);
    CHECK(!pipe(channel) && inherited >= 0 && directory >= 0);
    CHECK(write(channel[1], "s", 1) == 1);
    CHECK(!fcntl(channel[0], F_SETFD, FD_CLOEXEC) && !fcntl(inherited, F_SETFD, FD_CLOEXEC) && !fcntl(directory, F_SETFD, FD_CLOEXEC));
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    plan->actionCount = 4;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = channel[0], .target = 60};
    plan->actions[1] = (LSBFileAction){.kind = LSB_ACTION_INHERIT, .fd = inherited};
    plan->actions[2] = (LSBFileAction){.kind = LSB_ACTION_FCHDIR, .fd = directory};
    plan->actions[3] = (LSBFileAction){.kind = LSB_ACTION_CLOSE, .fd = directory};
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    CHECK(snapshotFDs(plan, received) == 0 && plan->actionCount == 4);
    const int inputs[] = {channel[0], inherited, directory};
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        int index = snapshotIndex(plan, inputs[i]);
        CHECK(index >= 0 && plan->fds[index].flags == FD_CLOEXEC);
        mach_port_t port;
        CHECK(!fileport_makeport(received[index], &port));
        CHECK(mach_port_deallocate(mach_task_self(), port) == KERN_SUCCESS);
    }
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    char byte, cwd[PATH_MAX];
    CHECK(read(60, &byte, 1) == 1 && byte == 's');
    CHECK(fcntl(channel[0], F_GETFD) == -1 && fcntl(directory, F_GETFD) == -1);
    CHECK(fcntl(inherited, F_GETFD) == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/private"));
}

static void snapshotOrderedReplacements(void) {
    int channel[2], overwritten = kqueue(), closed = kqueue();
    CHECK(!pipe(channel) && overwritten >= 0 && closed >= 0 && overwritten != 60 && closed != 60);
    CHECK(write(channel[1], "r", 1) == 1);
    CHECK(!fcntl(channel[0], F_SETFD, FD_CLOEXEC) && !fcntl(overwritten, F_SETFD, FD_CLOEXEC) && !fcntl(closed, F_SETFD, FD_CLOEXEC));
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    uint32_t nullPath;
    CHECK(LSBPlanAddString(plan, "/dev/null", &nullPath) == 0);
    plan->actionCount = 5;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = channel[0], .target = overwritten};
    plan->actions[1] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = overwritten, .target = 60};
    plan->actions[2] = (LSBFileAction){.kind = LSB_ACTION_CLOSE, .fd = closed};
    plan->actions[3] = (LSBFileAction){.kind = LSB_ACTION_OPEN, .fd = closed, .flags = O_RDONLY, .path = nullPath};
    plan->actions[4] = (LSBFileAction){.kind = LSB_ACTION_INHERIT, .fd = closed};
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    CHECK(snapshotFDs(plan, received) == 0 && plan->actionCount == 4);
    CHECK(snapshotIndex(plan, channel[0]) >= 0 && snapshotIndex(plan, overwritten) == -1 && snapshotIndex(plan, closed) == -1);
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    char byte;
    CHECK(read(60, &byte, 1) == 1 && byte == 'r');
    CHECK(fcntl(overwritten, F_GETFD) == 0 && fcntl(closed, F_GETFD) == 0 && read(closed, &byte, 1) == 0);
}

static void snapshotPreservesActionErrors(void) {
    const unsigned afterClose[] = {LSB_ACTION_CLOSE, LSB_ACTION_DUP2, LSB_ACTION_INHERIT, LSB_ACTION_FCHDIR};
    for (unsigned i = 0; i < sizeof(afterClose) / sizeof(afterClose[0]); ++i) {
        int queue = kqueue();
        CHECK(queue >= 0 && !fcntl(queue, F_SETFD, FD_CLOEXEC));
        LSBSpawnPlan *plan = newPlan();
        plan->actionCount = 2;
        plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_CLOSE, .fd = queue};
        plan->actions[1] = (LSBFileAction){.kind = afterClose[i], .fd = queue, .target = 60};
        int received[LSB_MAX_FDS];
        emptyFDs(received);
        CHECK(snapshotFDs(plan, received) == 0 && plan->actionCount == 1);
        CHECK(snapshotIndex(plan, queue) == -1);
        CHECK(LSBApplySpawnPlan(plan, received) == EBADF);
        free(plan);
    }
}

static void preservedInternalDescriptor(void) {
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    int source = open("/dev/null", O_RDWR);
    CHECK(source >= 0);
    internalWakeFD = fcntl(source, F_DUPFD_CLOEXEC, 90);
    CHECK(internalWakeFD >= 90);
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    importFD(plan, received, source, 60, 0);
    CHECK(LSBApplySpawnPlanPreservingFD(plan, received, internalWakeFD) == 0);
    CHECK(fcntl(internalWakeFD, F_GETFD) == FD_CLOEXEC);
    CHECK(fcntl(60, F_GETFD) == -1 && errno == EBADF);
    LSBSpawnPlan *snapshot = newPlan();
    internalTerminalFD = fcntl(internalWakeFD, F_DUPFD_CLOEXEC, 91);
    CHECK(internalTerminalFD >= 91);
    snapshot->actions[0] = (LSBFileAction){ .kind = LSB_ACTION_INHERIT, .fd = internalTerminalFD };
    snapshot->actionCount = 1;
    emptyFDs(received);
    CHECK(snapshotFDs(snapshot, received) == 0 && snapshot->fdCount == 0);
    CHECK(close(internalWakeFD) == 0);
    CHECK(close(internalTerminalFD) == 0);
    internalWakeFD = internalTerminalFD = -1;
}

static char workingDirectory[LSB_PATH_BYTES];
static void opensAndDirectories(void) {
    LSBSpawnPlan *plan = newPlan();
    CHECK(LSBPlanAddString(plan, workingDirectory, &plan->cwd) == 0);
    plan->creationMask = 0027;
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    int directory = open(workingDirectory, O_RDONLY | O_DIRECTORY);
    CHECK(directory >= 0);
    importFD(plan, received, directory, 62, 0);
    uint32_t nested, made, after;
    CHECK(LSBPlanAddString(plan, "nested", &nested) == 0);
    CHECK(LSBPlanAddString(plan, "made", &made) == 0);
    CHECK(LSBPlanAddString(plan, "after", &after) == 0);
    plan->actionCount = 6;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_CHDIR, .path = nested};
    plan->actions[1] = (LSBFileAction){.kind = LSB_ACTION_OPEN, .fd = 60, .flags = O_CREAT | O_WRONLY, .mode = 0666, .path = made};
    plan->actions[2] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 60, .target = 61};
    plan->actions[3] = (LSBFileAction){.kind = LSB_ACTION_CLOSE, .fd = 60};
    plan->actions[4] = (LSBFileAction){.kind = LSB_ACTION_FCHDIR, .fd = 62};
    plan->actions[5] = (LSBFileAction){.kind = LSB_ACTION_OPEN, .fd = 63, .flags = O_CREAT | O_WRONLY, .mode = 0600, .path = after};
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    CHECK(write(61, "made", 4) == 4 && write(63, "after", 5) == 5);
    struct stat info;
    CHECK(!stat("nested/made", &info) && (info.st_mode & 0777) == 0640 && info.st_size == 4);
    CHECK(!stat("after", &info) && info.st_size == 5);
    char cwd[LSB_PATH_BYTES];
    CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, workingDirectory));
}

static void attributesAndFailure(void) {
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_RESETIDS;
    plan->signalMask = 1u << (SIGUSR1 - 1);
    plan->signalIgnored = (1u << (SIGUSR2 - 1)) | (1u << (SIGPIPE - 1));
    plan->signalDefault = 1u << (SIGUSR2 - 1);
    plan->processGroup = 0;
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    CHECK(getpgrp() == getpid());
    sigset_t mask;
    CHECK(!sigprocmask(SIG_SETMASK, NULL, &mask) && sigismember(&mask, SIGUSR1));
    struct sigaction action;
    CHECK(!sigaction(SIGUSR2, NULL, &action) && action.sa_handler == SIG_DFL);
    CHECK(!sigaction(SIGPIPE, NULL, &action) && action.sa_handler == SIG_IGN);
    plan->actionCount = 1;
    plan->actions[0] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 70, .target = 71};
    CHECK(LSBApplySpawnPlan(plan, received) == EBADF);
}

static void defaultProcessGroup(void) {
    LSBSpawnPlan *plan = newPlan();
    pid_t original = getpgrp();
    // The requested parent's group is metadata unless SETPGROUP is explicitly present.
    plan->processGroup = INT_MAX;
    int received[LSB_MAX_FDS];
    emptyFDs(received);
    CHECK(LSBApplySpawnPlan(plan, received) == 0 && getpgrp() == original);
}

static void schedulerHints(void) {
    LSBSpawnPlan *plan = newPlan();
    LSBAdditionalSpawnAttributes additional;
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    // Stock 23A343 app and WidgetKit launches preserve CLOEXEC_DEFAULT, SETSIGDEF and SETSIGMASK.
    short flags = POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
    assert(flags == 0x400c && !posix_spawnattr_setflags(&attributes, flags));
    sigset_t mask = 0, defaults = 0;
    assert(!sigaddset(&mask, SIGUSR1) && !sigaddset(&defaults, SIGTERM));
    assert(!posix_spawnattr_setsigmask(&attributes, &mask) && !posix_spawnattr_setsigdefault(&attributes, &defaults));
    static const struct { uint32_t type, role; int error; } cases[] = {
        {0, 0, 0}, {0x600, 0, 0}, {0x100, 0, 0}, {0x100, 1, 0}, {0x100, 2, 0}, {0x200, 0, 0}, {0x400, 0, 0},
        {0, 1, ENOTSUP}, {0, 2, ENOTSUP}, {0x600, 1, ENOTSUP}, {0x600, 2, ENOTSUP},
        {0x100, 3, ENOTSUP}, {0x100, 4, ENOTSUP}, {0x100, 5, ENOTSUP}, {0x100, 6, ENOTSUP}, {0x100, 7, ENOTSUP},
        {0x200, 1, ENOTSUP}, {0x200, 2, ENOTSUP}, {0x200, 3, ENOTSUP}, {0x200, 4, ENOTSUP},
        {0x200, 5, ENOTSUP}, {0x200, 6, ENOTSUP}, {0x200, 7, ENOTSUP}, {0x200, UINT32_MAX, ENOTSUP},
        {0x400, 1, ENOTSUP}, {0x400, 2, ENOTSUP}, {0x400, 3, ENOTSUP}, {0x400, 4, ENOTSUP},
        {0x400, 5, ENOTSUP}, {0x400, 6, ENOTSUP}, {0x400, 7, ENOTSUP}, {0x400, UINT32_MAX, ENOTSUP},
        {0x101, 1, ENOTSUP}, {0x201, 0, ENOTSUP}, {0x300, 0, ENOTSUP}, {0x401, 0, ENOTSUP},
        {0x500, 0, ENOTSUP}, {0x700, 0, ENOTSUP}, {UINT32_MAX, 0, ENOTSUP}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memcpy((char *)attributes + 0x24, &cases[i].type, sizeof(cases[i].type));
        memcpy((char *)attributes + 0x58, &cases[i].role, sizeof(cases[i].role));
        assert(readAttributes(plan, &attributes, &additional) == cases[i].error);
        if (!cases[i].error) {
            assert(plan->spawnFlags == (uint16_t)flags && plan->signalMask == mask && plan->signalDefault == defaults);
            assert(additional.processType == cases[i].type && additional.darwinRole == cases[i].role);
        }
    }
    uint32_t type = 0x100, role = 1;
    memcpy((char *)attributes + 0x24, &type, sizeof(type));
    memcpy((char *)attributes + 0x58, &role, sizeof(role));
    assert(!posix_spawnattr_setflags(&attributes, flags | POSIX_SPAWN_START_SUSPENDED));
    assert(readAttributes(plan, &attributes, &additional) == ENOTSUP);
    assert(!posix_spawnattr_destroy(&attributes));
    free(plan);
}

static void extensionSchedulerTransportCase(uint32_t type, char *job) {
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    memcpy((char *)attributes + 0x24, &type, sizeof(type));
    assert(!posix_spawnattr_setflags(&attributes, 0x400c));
    unsigned char snapshot[0x100];
    memcpy(snapshot, attributes, sizeof(snapshot));
    pid_t session = 0, group = 0, afterSession = 0, afterGroup = 0;
    int sessionError = LSBProcessSessionSnapshot(&session, &group);
    pid_t nativeSession = getsid(0), nativeGroup = getpgrp();
    char *argv[] = {"xpcproxy_sim", job, NULL}, *environment[] = {NULL};
    const int results[] = {0, EACCES};
    for (unsigned i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        transportFailure = results[i];
        unsigned calls = transportCalls, replacements = execCalls;
        pid_t pid = 123;
        errno = EDOM;
        assert(LSBPosixSpawn(&pid, "/usr/libexec/xpcproxy_sim", NULL, &attributes, argv, environment) == results[i]);
        assert(errno == EDOM && pid == (results[i] ? 123 : 4242));
        assert(transportCalls == calls + 1 && execCalls == replacements && capturedSpawnFlags == 0x400c);
        assert(!memcmp(snapshot, attributes, sizeof(snapshot)));
        assert(LSBProcessSessionSnapshot(&afterSession, &afterGroup) == sessionError);
        assert(session == afterSession && group == afterGroup && getsid(0) == nativeSession && getpgrp() == nativeGroup);
    }
    transportFailure = 0;
    // Recognizing extension scheduler hints must not accept suspension, session creation or visible roles, even on SETEXEC.
    const short unsupported[] = {0x400c | POSIX_SPAWN_START_SUSPENDED, 0x400c | POSIX_SPAWN_SETSID};
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        assert(!posix_spawnattr_setflags(&attributes, unsupported[i]));
        unsigned calls = transportCalls;
        pid_t pid = 123;
        errno = EDOM;
        assert(LSBPosixSpawn(&pid, "/usr/libexec/xpcproxy_sim", NULL, &attributes, argv, environment) == ENOTSUP);
        assert(errno == EDOM && pid == 123 && transportCalls == calls);
    }
    assert(!posix_spawnattr_setflags(&attributes, 0x400c | POSIX_SPAWN_SETEXEC));
    for (uint32_t role = 1; role <= 2; ++role) {
        memcpy((char *)attributes + 0x58, &role, sizeof(role));
        unsigned calls = transportCalls, replacements = execCalls;
        pid_t pid = 123;
        errno = EDOM;
        assert(LSBPosixSpawn(&pid, "/usr/libexec/xpcproxy_sim", NULL, &attributes, argv, environment) == ENOTSUP);
        assert(errno == EDOM && pid == 123 && transportCalls == calls && execCalls == replacements);
    }
    assert(!posix_spawnattr_destroy(&attributes));
}

static void extensionSchedulerTransport(void) {
    extensionSchedulerTransportCase(0x200, "com.apple.mobilecal.CalendarWidgetExtension.test");
    extensionSchedulerTransportCase(0x400, "com.apple.CalendarWidget.IntentsExtension");
}

static void allocatorHintAndInheritedAppRole(void) {
    LSBSpawnPlan *plan = newPlan();
    LSBAdditionalSpawnAttributes additional;
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    assert(POSIX_SPAWN_SETSID == 0x0400 && NativeSpawnNanoAllocator == 0x0200);
    static const struct { short flags; uint32_t type, role; int error; } cases[] = {
        {0x0200, 0, 0, 0}, {0x420c, 0x100, 1, 0}, {0x420c, 0x100, 2, 0}, {0x4240, 0, 1, 0}, {0x4240, 0, 2, 0},
        {0x4200, 0, 1, ENOTSUP}, {0x4200, 0, 2, ENOTSUP}, {0x4240, 0x600, 1, ENOTSUP}, {0x4240, 0x600, 2, ENOTSUP},
        {0x4240, 0, 3, ENOTSUP}, {0x4240, 0, 4, ENOTSUP}, {0x4240, 0, 7, ENOTSUP},
        {0x4440, 0, 1, ENOTSUP}, {0x42c0, 0, 1, ENOTSUP}, {0x4440, 0, 2, ENOTSUP}, {0x42c0, 0, 2, ENOTSUP}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(!posix_spawnattr_setflags(&attributes, cases[i].flags));
        memcpy((char *)attributes + 0x24, &cases[i].type, sizeof(cases[i].type));
        memcpy((char *)attributes + 0x58, &cases[i].role, sizeof(cases[i].role));
        assert(readAttributes(plan, &attributes, &additional) == cases[i].error);
        if (!cases[i].error) assert(plan->spawnFlags == ((uint16_t)cases[i].flags & ~NativeSpawnNanoAllocator));
    }
    // Contacts/WidgetRenderer use UI_FOCAL; stock App LaunchAngels use UI. Both replacements retain only CLOEXEC_DEFAULT.
    assert(!posix_spawnattr_setflags(&attributes, 0x4240));
    uint32_t type = 0, role = 1;
    memcpy((char *)attributes + 0x24, &type, sizeof(type));
    memcpy((char *)attributes + 0x58, &role, sizeof(role));
    uint32_t options = 4, datalessPolicy = 6;
    memcpy((char *)attributes + 0x80, &options, sizeof(options));
    memcpy((char *)attributes + 0x98, &datalessPolicy, sizeof(datalessPolicy));
    pid_t session = 0, group = 0, afterSession = 0, afterGroup = 0;
    int sessionError = LSBProcessSessionSnapshot(&session, &group);
    pid_t nativeSession = getsid(0), nativeGroup = getpgrp();
    char *argv[] = {"tool", NULL}, *environment[] = {NULL};
    transportFailure = EACCES;
    for (role = 1; role <= 2; ++role) {
        memcpy((char *)attributes + 0x58, &role, sizeof(role));
        unsigned char snapshot[0x100];
        memcpy(snapshot, attributes, sizeof(snapshot));
        pid_t pid = 123;
        unsigned calls = execCalls;
        errno = EDOM;
        assert(LSBPosixSpawn(&pid, "/bin/tool", NULL, &attributes, argv, environment) == EACCES && errno == EDOM && pid == 123);
        assert(execCalls == calls + 1 && capturedSpawnFlags == POSIX_SPAWN_CLOEXEC_DEFAULT);
        assert(!memcmp(snapshot, attributes, sizeof(snapshot)));
        assert(LSBProcessSessionSnapshot(&afterSession, &afterGroup) == sessionError);
        assert(session == afterSession && group == afterGroup && getsid(0) == nativeSession && getpgrp() == nativeGroup);
    }
    transportFailure = 0;
    assert(!posix_spawnattr_destroy(&attributes));
    free(plan);
}

static void shimSerialization(void) {
    LSBSpawnPlan *plan = newPlan();
    LSBAdditionalSpawnAttributes additional;
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    assert(readAttributes(plan, &attributes, &additional) == 0);
    assert(!posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETEXEC));
    assert(readAttributes(plan, &attributes, &additional) == 0 && (plan->spawnFlags & POSIX_SPAWN_SETEXEC));
    assert(!posix_spawnattr_setflags(&attributes, 0));
    cpu_type_t architecture = CPU_TYPE_ARM64;
    size_t count;
    assert(!posix_spawnattr_setbinpref_np(&attributes, 1, &architecture, &count));
    assert(readAttributes(plan, &attributes, &additional) == 0);
    uint32_t processType = 0x600;
    memcpy((char *)attributes + 0x24, &processType, sizeof(processType));
    assert(readAttributes(plan, &attributes, &additional) == 0);
    processType = 0x300;
    memcpy((char *)attributes + 0x24, &processType, sizeof(processType));
    assert(readAttributes(plan, &attributes, &additional) == ENOTSUP);
    assert(!posix_spawnattr_destroy(&attributes));
    free(plan);

    posix_spawn_file_actions_t actions;
    assert(!posix_spawn_file_actions_init(&actions));
    assert(!posix_spawn_file_actions_addopen(&actions, 60, "output", O_CREAT | O_WRONLY, 0640));
    assert(!posix_spawn_file_actions_adddup2(&actions, 60, 61));
    assert(!posix_spawn_file_actions_addclose(&actions, 60));
    plan = newPlan();
    int temporaryFDs[LSB_MAX_ACTIONS];
    for (unsigned i = 0; i < LSB_MAX_ACTIONS; ++i) temporaryFDs[i] = -1;
    assert(readActions(plan, &actions, temporaryFDs) == 0 && plan->actionCount == 3);
    assert(plan->actions[0].kind == LSB_ACTION_OPEN && plan->actions[0].mode == 0640);
    assert(!strcmp(plan->strings + plan->actions[0].path, "output"));
    assert(plan->actions[1].kind == LSB_ACTION_DUP2 && plan->actions[1].fd == 60 && plan->actions[1].target == 61);
    assert(plan->actions[2].kind == LSB_ACTION_CLOSE && plan->actions[2].fd == 60);
    assert(!posix_spawn_file_actions_destroy(&actions));
    free(plan);

    char *argv[] = {"tool", "one argument", NULL}, *environment[] = {"KEY=value", NULL};
    pid_t pid = 123;
    errno = EDOM;
    assert(LSBPosixSpawn(&pid, "/bin/tool", NULL, NULL, argv, environment) == 0 && pid == 4242 && errno == EDOM);
    unsigned beforeCalls = transportCalls;
    creationMaskError = EBUSY;
    pid = 123;
    assert(LSBPosixSpawn(&pid, "/bin/tool", NULL, NULL, argv, environment) == EBUSY && pid == 123 && errno == EDOM);
    assert(transportCalls == beforeCalls);
    creationMaskError = 0;
    transportFailure = EACCES;
    pid = 123;
    assert(LSBPosixSpawn(&pid, "/bin/tool", NULL, NULL, argv, environment) == EACCES && pid == 123 && errno == EDOM);
    transportFailure = 0;
    pathSearch = 1;
    transportCalls = 0;
    assert(!setenv("PATH", "/first:/second", 1));
    assert(LSBPosixSpawnP(&pid, "tool", NULL, NULL, argv, environment) == 0 && transportCalls == 2 && pid == 4242 && errno == EDOM);
}

static void portInheritanceAndExecFailure(void) {
    mach_port_t port;
    CHECK(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS);
    CHECK(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    CHECK(mach_ports_register(mach_task_self(), &port, 1) == KERN_SUCCESS);
    CHECK(task_set_bootstrap_port(mach_task_self(), MACH_PORT_NULL) == KERN_SUCCESS);
    expectedRegisteredPort = port;
    inspectPorts = true;
    char *argv[] = {"tool", NULL}, *environment[] = {NULL};
    posix_spawnattr_t attributes;
    CHECK(!posix_spawnattr_init(&attributes));
    CHECK(!posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETEXEC));
    transportFailure = ENOENT;
    pid_t pid = 123;
    errno = EDOM;
    CHECK(LSBPosixSpawn(&pid, "/bin/missing", NULL, &attributes, argv, environment) == ENOENT && errno == EDOM && pid == 123);
    CHECK(execCalls == 1);
    mach_port_urefs_t refs;
    CHECK(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 1);
    LSBSpawnPorts requested = {.registeredPresent = true, .count = 1, .registered = {port}, .bootstrapPresent = true, .bootstrapPort = port};
    LSBSpawnPorts copied;
    CHECK(snapshotPorts(&requested, &copied) == 0 && copied.bootstrapPort == port && copied.registered[0] == port);
    CHECK(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 3);
    LSBSpawnPortsDispose(&copied);
    CHECK(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 1);
    CHECK(!posix_spawnattr_destroy(&attributes));
    CHECK(mach_port_deallocate(mach_task_self(), port) == KERN_SUCCESS);
    CHECK(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS);
}

static void execNeverReturns(void) {
    posix_spawnattr_t attributes;
    CHECK(!posix_spawnattr_init(&attributes));
    CHECK(!posix_spawnattr_setflags(&attributes, 0x4240));
    uint32_t role = 1;
    memcpy((char *)attributes + 0x58, &role, sizeof(role));
    char *argv[] = {"tool", NULL}, *environment[] = {NULL};
    (void)LSBPosixSpawn(NULL, "/bin/tool", NULL, &attributes, argv, environment);
    _exit(99);
}

static void directExecve(void) {
    CHECK(!setenv("DYLD_ROOT_PATH", workingDirectory, 1) && !chdir(workingDirectory));
    char *argv[] = {"custom-name", "two words", NULL};
    CHECK(LSBExecve("", argv, NULL) == -1 && errno == ENOENT);
    CHECK(LSBExecve("/missing", argv, NULL) == -1 && errno == ENOENT);
    CHECK(!chmod("tool", 0600));
    CHECK(LSBExecve("./tool", argv, NULL) == -1 && (errno == EACCES || errno == EPERM));
    CHECK(execCalls == 0 && !chmod("tool", 0700));
    char path[PATH_MAX];
    CHECK(realpath("tool", path));
    expectedExecvePath = path;
    transportFailure = ENOEXEC;
    CHECK(LSBExecve("./tool", argv, NULL) == -1 && errno == ENOEXEC && execCalls == 1);
    transportFailure = 0;
    (void)LSBExecve("/tool", argv, NULL);
    _exit(99);
}

static void fileportActionOrdering(void) {
    int a[2], b[2];
    CHECK(!pipe(a) && !pipe(b));
    CHECK(write(a[1], "a", 1) == 1 && write(b[1], "b", 1) == 1);
    mach_port_t first, second;
    CHECK(!fileport_makeport(a[0], &first) && !fileport_makeport(b[0], &second));
    posix_spawn_file_actions_t actions;
    CHECK(!posix_spawn_file_actions_init(&actions));
    CHECK(!posix_spawn_file_actions_add_fileportdup2_np(&actions, first, 60));
    CHECK(!posix_spawn_file_actions_adddup2(&actions, 60, 61));
    CHECK(!posix_spawn_file_actions_add_fileportdup2_np(&actions, second, 60));
    LSBSpawnPlan *plan = newPlan();
    plan->spawnFlags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    int temporaryFDs[LSB_MAX_ACTIONS], received[LSB_MAX_FDS];
    for (unsigned i = 0; i < LSB_MAX_ACTIONS; ++i) temporaryFDs[i] = -1;
    emptyFDs(received);
    CHECK(readActions(plan, &actions, temporaryFDs) == 0);
    CHECK(temporaryFDs[0] > 61 && temporaryFDs[2] > 61 && temporaryFDs[0] != temporaryFDs[2]);
    CHECK(snapshotFDs(plan, received) == 0);
    CHECK(!posix_spawn_file_actions_destroy(&actions));
    CHECK(LSBApplySpawnPlan(plan, received) == 0);
    char left, right;
    CHECK(read(60, &left, 1) == 1 && left == 'b');
    CHECK(read(61, &right, 1) == 1 && right == 'a');
    CHECK(fcntl(temporaryFDs[0], F_GETFD) == -1 && fcntl(temporaryFDs[2], F_GETFD) == -1);
    mach_port_urefs_t refs;
    CHECK(mach_port_get_refs(mach_task_self(), first, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 1);
    CHECK(mach_port_get_refs(mach_task_self(), second, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 1);
}

static int countOpenFDs(void) {
    int count = 0;
    for (int fd = 0, limit = getdtablesize(); fd < limit; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

static void fileportFailureCleanup(void) {
    int fd = open("/dev/null", O_RDONLY);
    CHECK(fd >= 0);
    mach_port_t port;
    CHECK(!fileport_makeport(fd, &port));
    posix_spawn_file_actions_t actions;
    CHECK(!posix_spawn_file_actions_init(&actions));
    CHECK(!posix_spawn_file_actions_add_fileportdup2_np(&actions, port, 60));
    CHECK(!posix_spawn_file_actions_add_fileportdup2_np(&actions, port, 61));
    NativeActions *native = (NativeActions *)actions;
    native->actions[1].fd = MACH_PORT_NULL;
    char *argv[] = {"tool", NULL}, *environment[] = {NULL};
    int before = countOpenFDs();
    unsigned calls = transportCalls;
    pid_t pid = 123;
    CHECK(LSBPosixSpawn(&pid, "/bin/tool", &actions, NULL, argv, environment) != 0 && pid == 123);
    CHECK(transportCalls == calls && countOpenFDs() == before);
    native->actions[1].fd = (int32_t)port;
    transportFailure = EACCES;
    CHECK(LSBPosixSpawn(&pid, "/bin/tool", &actions, NULL, argv, environment) == EACCES && pid == 123);
    CHECK(countOpenFDs() == before);
    mach_port_urefs_t refs;
    CHECK(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 1);
    CHECK(fcntl(fd, F_GETFD) >= 0);
    CHECK(!posix_spawn_file_actions_destroy(&actions));
}

static void simulatedSessionRejectsProcessGroup(void) {
    pid_t session, group;
    assert(LSBProcessSessionSnapshot(&session, &group) == 0);
    pid_t nativeGroup = getpgrp();
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    char *argv[] = {"tool", NULL}, *environment[] = {NULL};
    const short flags[] = {POSIX_SPAWN_SETPGROUP, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETEXEC};
    const pid_t groups[] = {0, group};
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        assert(!posix_spawnattr_setflags(&attributes, flags[i]));
        for (unsigned j = 0; j < sizeof(groups) / sizeof(groups[0]); ++j) {
            assert(!posix_spawnattr_setpgroup(&attributes, groups[j]));
            pid_t pid = 123;
            errno = EDOM;
            assert(LSBPosixSpawn(&pid, "/bin/tool", NULL, &attributes, argv, environment) == ENOTSUP);
            assert(pid == 123 && errno == EDOM && !transportCalls && !execCalls);
        }
    }
    assert(!posix_spawnattr_destroy(&attributes));
    pid_t afterSession, afterGroup;
    assert(LSBProcessSessionSnapshot(&afterSession, &afterGroup) == 0 && afterSession == session && afterGroup == group);
    assert(getpgrp() == nativeGroup);
    puts("Spawn plan: simulated sessions reject native group changes before transport");
}

int main(void) {
    if (getenv("LSB_TEST_SESSION_GROUP")) {
        simulatedSessionRejectsProcessGroup();
        runChild(extensionSchedulerTransport, "widget and Intents extension hints preserve logical sessions");
        runChild(allocatorHintAndInheritedAppRole, "allocator hint and inherited app role preserve logical sessions");
        return 0;
    }
    validation();
    runChild(cyclesAndOrdering, "descriptor cycles and ordered actions");
    runChild(normalCloseOnExec, "close-on-exec");
    runChild(defaultCloseOnExec, "close-on-exec default");
    runChild(snapshotDropsCloseOnExecKqueue, "CLOEXEC kqueue is omitted before fileport transfer");
    runChild(snapshotDropsDefaultKqueue, "CLOEXEC_DEFAULT omits unreferenced kqueue and ordinary descriptors");
    runChild(snapshotKeepsActionInputs, "CLOEXEC dup2, inherit and fchdir inputs survive ordered replay");
    runChild(snapshotOrderedReplacements, "replacement and close actions do not export obsolete kqueue values");
    runChild(snapshotPreservesActionErrors, "elided initial close preserves later invalid descriptor errors");
    runChild(preservedInternalDescriptor, "internal wake descriptor survives and is not inherited");
    runChild(attributesAndFailure, "signal attributes and action errors");
    runChild(defaultProcessGroup, "default extension process group");
    runChild(schedulerHints, "recognized app scheduler hints preserve signal attributes and reject unsupported policies");
    runChild(extensionSchedulerTransport, "extension hints reach spawn transport without changing sessions or broadening flags");
    runChild(allocatorHintAndInheritedAppRole, "allocator hint and inherited app role reach exec transport without changing sessions");
    strcpy(workingDirectory, "/tmp/LiveSimBooter-spawn-tests.XXXXXX");
    assert(mkdtemp(workingDirectory));
    char resolved[LSB_PATH_BYTES];
    assert(realpath(workingDirectory, resolved));
    strcpy(workingDirectory, resolved);
    char childPath[LSB_PATH_BYTES + 32];
    snprintf(childPath, sizeof(childPath), "%s/nested", workingDirectory);
    assert(!mkdir(childPath, 0700));
    runChild(opensAndDirectories, "open, chdir and fchdir ordering");
    snprintf(childPath, sizeof(childPath), "%s/tool", workingDirectory);
    int tool = open(childPath, O_CREAT | O_EXCL | O_WRONLY, 0700);
    assert(tool >= 0 && !close(tool));
    runChild(directExecve, "execve validates paths, preserves arguments and reports replacement errors through errno");
    assert(!unlink(childPath));
    snprintf(childPath, sizeof(childPath), "%s/nested/made", workingDirectory);
    assert(!unlink(childPath));
    snprintf(childPath, sizeof(childPath), "%s/nested", workingDirectory);
    assert(!rmdir(childPath));
    snprintf(childPath, sizeof(childPath), "%s/after", workingDirectory);
    assert(!unlink(childPath) && !rmdir(workingDirectory));
    runChild(shimSerialization, "guest attribute and file-action serialization");
    runChild(portInheritanceAndExecFailure, "registered/bootstrap port inheritance and failed SETEXEC cleanup");
    runChild(execNeverReturns, "successful SETEXEC does not return");
    runChild(fileportActionOrdering, "fileportdup2 ownership and ordered descriptor replacement");
    runChild(fileportFailureCleanup, "fileport conversion and RPC failure close all temporary descriptors");
    puts("Spawn plan tests passed");
    return 0;
}
