#include "../Guest/Shared/Fork/ForkDescriptorState.h"
#include "../ProcessSupport/ForkDescriptorSlots.h"
#define LSB_FORK_SNAPSHOT_TRIAL 1
#include "../Guest/libLiveSimSpawn/TerminalStopWitness.c"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>

static void *receiveMessage(void *context) {
    mach_port_t port = *(mach_port_t *)context;
    struct { mach_msg_header_t header; mach_msg_max_trailer_t trailer; } message = {};
    mach_msg_return_t kr;
    do { kr = mach_msg(&message.header, MACH_RCV_MSG, 0, sizeof(message), port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL); }
    while (kr == MACH_RCV_INTERRUPTED);
    assert(kr == MACH_RCV_PORT_DIED || kr == MACH_RCV_INVALID_NAME || kr == MACH_RCV_PORT_CHANGED);
    return NULL;
}

static void *runUserCode(void *context) {
    _Atomic int *stage = context;
    atomic_store(stage, 1);
    while (atomic_load(stage) == 1) atomic_signal_fence(memory_order_seq_cst);
    return NULL;
}

static void testReservedSlots(void) {
    int unrelated[2], anchor;
    struct stat expected;
    assert(!pipe(unrelated) && fcntl(unrelated[0], F_SETFD, FD_CLOEXEC) == 0);
    assert(!LSBForkReserveDescriptorSlots(&anchor, &expected));
    assert(anchor >= LSB_MAX_FDS && expected.st_ino && S_ISFIFO(expected.st_mode));
    int alias = -1;
    for (int fd = 3; fd < LSB_MAX_FDS; ++fd) {
        struct stat current;
        assert(!fstat(fd, &current)); // Every previously empty low slot is held.
        if (current.st_dev == expected.st_dev && current.st_ino == expected.st_ino && S_ISFIFO(current.st_mode)) alias = fd;
    }
    assert(alias >= 3 && dup2(unrelated[0], alias) == alias && fcntl(alias, F_SETFD, FD_CLOEXEC) == 0);
    assert(!LSBForkReleaseDescriptorSlots(anchor, expected.st_dev, expected.st_ino));
    assert(fcntl(alias, F_GETFD) == FD_CLOEXEC && fcntl(unrelated[0], F_GETFD) == FD_CLOEXEC);
    assert(fcntl(anchor, F_GETFD) < 0 && errno == EBADF);
    close(alias);

    assert(!LSBForkReserveDescriptorSlots(&anchor, &expected));
    int saved = fcntl(anchor, F_DUPFD_CLOEXEC, anchor + 1);
    assert(saved > anchor && dup2(unrelated[0], anchor) == anchor && !fcntl(anchor, F_SETFD, FD_CLOEXEC));
    assert(LSBForkReleaseDescriptorSlots(anchor, expected.st_dev, expected.st_ino) != 0);
    assert(fcntl(unrelated[0], F_GETFD) == FD_CLOEXEC && fcntl(anchor, F_GETFD) == FD_CLOEXEC);
    assert(!LSBForkReleaseDescriptorSlots(saved, expected.st_dev, expected.st_ino));
    assert(fcntl(unrelated[0], F_GETFD) == FD_CLOEXEC && fcntl(anchor, F_GETFD) == FD_CLOEXEC);
    close(anchor);
    close(unrelated[0]);
    close(unrelated[1]);
}

static void testPeerHolds(const ForkDescriptorTraps *traps) {
    pthread_t worker;
    mach_port_t port = MACH_PORT_NULL;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!pthread_create(&worker, NULL, receiveMessage, &port));
    unsigned attempt = 0;
    int error;
    do {
        usleep(2000);
        ForkDescriptorPeers peers;
        error = forkDescriptorPark(traps, &peers);
        if (!error) assert(peers.count >= 1 && !forkDescriptorSameThreads(&peers));
        assert(!forkDescriptorUnpark(&peers));
    } while (error == EAGAIN && ++attempt < 50);
    assert(!error);
    assert(!mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!pthread_join(worker, NULL));

    _Atomic int stage = 0;
    assert(!pthread_create(&worker, NULL, runUserCode, &stage));
    while (!atomic_load(&stage)) usleep(1000);
    ForkDescriptorPeers peers;
    assert(forkDescriptorPark(traps, &peers) == EAGAIN);
    assert(!forkDescriptorUnpark(&peers));
    atomic_store(&stage, 2);
    assert(!pthread_join(worker, NULL));
}

static void testTerminalWitness(ForkDescriptorTraps traps) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        alarm(5);
        struct kevent64_s event;
        assert(!LSBStartTerminalStopWitness(&event));
        while (!(traps.stopWitnessThread = LSBTerminalStopWitnessThreadID())) usleep(1000);
        int error;
        ForkDescriptorPeers peers;
        do {
            error = forkDescriptorPark(&traps, &peers);
            assert(!forkDescriptorUnpark(&peers));
            if (error == EAGAIN) usleep(1000);
        } while (error == EAGAIN);
        assert(!error);
        // Same thread and same syscall with the wrong identity must be rejected.
        ++traps.stopWitnessThread;
        assert(forkDescriptorPark(&traps, &peers) == EAGAIN);
        assert(!forkDescriptorUnpark(&peers));
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--inventory")) {
        ForkDescriptorTraps traps = {};
        ForkDescriptorPeers peers = {};
        ForkDescriptorPacket packet = {};
        int fds[LSB_MAX_FDS], error = forkDescriptorPrepare(&traps);
        audit_token_t audit = {};
        mach_msg_type_number_t words = TASK_AUDIT_TOKEN_COUNT;
        if (!error && task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &words) != KERN_SUCCESS) error = EIO;
        const char *wake = getenv("LSB_CHILD_WAKE_FD");
        if (!error) error = forkDescriptorPark(&traps, &peers);
        if (!error) error = forkDescriptorList(audit.val[5], -1, wake ? atoi(wake) : -1, -1, traps.nullDevice, &packet, fds);
        unsigned parked = peers.count;
        int resumed = forkDescriptorUnpark(&peers);
        printf("fd inventory errno=%d resumed=%d parked=%u mach=%#lx workq=%#lx count=%u\n", error, resumed, parked,
               (unsigned long)traps.machReturn, (unsigned long)traps.workqReturn, packet.count);
        for (unsigned i = 0; i < packet.count; ++i) printf(" fd=%u flags=%u\n", packet.entries[i].number, packet.entries[i].flags);
        return error || resumed;
    }
    ForkDescriptorTraps traps;
    assert(!forkDescriptorPrepare(&traps));
    testTerminalWitness(traps);
    char path[] = "/tmp/lsb-fork-fds.XXXXXX";
    int file = mkstemp(path);
    assert(file > 2 && unlink(path) == 0 && write(file, "abc", 3) == 3 && lseek(file, 0, SEEK_SET) == 0);
    struct rlimit limit;
    assert(!getrlimit(RLIMIT_NOFILE, &limit) && limit.rlim_cur >= 256);
    limit.rlim_cur = 256;
    assert(!setrlimit(RLIMIT_NOFILE, &limit));
    int alias = fcntl(file, F_DUPFD_CLOEXEC, 255); // Bash keeps an open script here even when only a few descriptors are live.
    int channel[2], pipeFDs[2];
    assert(alias == 255 && !socketpair(AF_UNIX, SOCK_STREAM, 0, channel) && !pipe(pipeFDs));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        // Start with only the observer's socket and its explicit boot stdio, as the guest spawn plan does.
        for (int fd = 3; fd < getdtablesize(); ++fd) if (fd != channel[1]) close(fd);
        int error = forkDescriptorReceive(channel[1], -1, -1, 7);
        if (error) { fprintf(stderr, "rights receive errno=%d\n", error); _exit(10); }
        char data[2];
        if (fcntl(alias, F_GETFD) != FD_CLOEXEC || read(alias, data, 1) != 1 || data[0] != 'a' ||
            read(file, data, 1) != 1 || data[0] != 'b' || write(pipeFDs[1], "x", 1) != 1) _exit(11);
        _exit(37);
    }
    close(channel[1]);
    int error = forkDescriptorSend(channel[0], getpid(), -1, -1, &traps);
    close(channel[0]);
    if (error) fprintf(stderr, "rights send errno=%d\n", error);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && !error && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    char data;
    assert(read(file, &data, 1) == 1 && data == 'c' && read(pipeFDs[0], &data, 1) == 1 && data == 'x');
    close(alias); close(file); close(pipeFDs[0]); close(pipeFDs[1]);
    testReservedSlots();
    testPeerHolds(&traps);
    puts("Checkpoint SCM_RIGHTS, descriptor numbers, shared offset, pipe and FD_CLOEXEC passed");
    return 0;
}
