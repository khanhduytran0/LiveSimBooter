#include "../Guest/FakePTY.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned descriptorChecks;
static int countedFstat(int fd, struct stat *metadata) {
    ++descriptorChecks;
    return fstat(fd, metadata);
}

// Count actual descriptor validation while retaining real socket identities and native ioctl forwarding.
#define fstat countedFstat
#include "../ProcessSupport/FakePTY.c"
#undef fstat
static int waitAck = -1;
static int detectBlockingWait(struct pollfd *ready, nfds_t count, int timeout) {
    if (waitAck != -1) { assert(write(waitAck, "W", 1) == 1); waitAck = -1; }
    return poll(ready, count, timeout);
}
#define poll(...) detectBlockingWait(__VA_ARGS__)
#define LSB_FAKE_PTY_TESTING
#include "../Guest/FakePTY.c"
#undef poll

static pid_t testGroup;
static int groupError;
static int signalRequests;
static volatile sig_atomic_t caught;
static void caughtSignal(int sig) { caught = sig; }
static void ttyDefault(int sig) { (void)sig; kill(getpid(), SIGSTOP); }

pid_t LSBGuestProcessGroup(void) { return testGroup ?: atomic_load(&terminal->foreground); }
int LSBSigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    int result = sigaction(sig, act, old);
    if (!result && old && old->sa_handler == ttyDefault) old->sa_handler = SIG_DFL;
    return result;
}
int LSBTransportCheckTerminalIO(LSBTerminalAccess access, bool *signalCaller) {
    assert(access >= LSB_TERMINAL_READ && access <= LSB_TERMINAL_CONTROL);
    ++signalRequests;
    *signalCaller = !groupError;
    return groupError;
}


int LSBTransportSetForegroundGroup(pid_t group) { return LSBFakePTYSetForeground(terminal, group); }
int LSBTransportDrainTerminal(void) { return 0; }

#define CHECK(call, result, error) do { \
    descriptorChecks = 0; \
    errno = EDOM; \
    assert((call) == (result)); \
    assert(errno == (error)); \
    assert(descriptorChecks == 1); \
} while (0)

static void testAccess(int master, int slave) {
    testGroup = 123;
    assert(!LSBFakePTYSetForeground(terminal, 456));
    assert(write(master, "abc", 3) == 3);
    char data[4] = {};
    struct iovec v[] = {{data, 2}, {data + 2, 1}};
    struct sigaction catch = {.sa_handler = caughtSignal, .sa_flags = SA_RESTART}, oldRead, oldWrite;
    assert(!sigaction(SIGTTIN, &catch, &oldRead) && !sigaction(SIGTTOU, &catch, &oldWrite));
    assert(LSBPTYReadV(slave, v, 2) == -1 && errno == EINTR && caught == SIGTTIN && signalRequests == 1);
    assert(signal(SIGTTIN, SIG_IGN) == caughtSignal);
    assert(LSBPTYReadNoCancel(slave, data, 3) == -1 && errno == EIO && signalRequests == 1);
    assert(signal(SIGTTIN, caughtSignal) == SIG_IGN);
    sigset_t blocked = 0, previous;
    sigaddset(&blocked, SIGTTIN);
    sigaddset(&blocked, SIGTTOU);
    assert(!sigprocmask(SIG_BLOCK, &blocked, &previous));
    assert(LSBPTYRead(slave, data, 3) == -1 && errno == EIO && signalRequests == 1);
    struct termios attrs;
    assert(!LSBPTYGetAttributes(slave, &attrs));
    assert(LSBPTYWrite(slave, "A", 1) == 1 && signalRequests == 1); // TOSTOP clear
    attrs.c_lflag |= TOSTOP;
    assert(!LSBPTYSetAttributes(slave, TCSANOW, &attrs)); // blocked control is permitted
    assert(LSBPTYWriteNoCancel(slave, "B", 1) == 1 && signalRequests == 1);
    assert(!sigprocmask(SIG_SETMASK, &previous, NULL));
    assert(LSBPTYWriteVNoCancel(slave, v, 2) == -1 && errno == EINTR && caught == SIGTTOU && signalRequests == 2);
    assert(signal(SIGTTOU, SIG_IGN) == caughtSignal);
    assert(LSBPTYWrite(slave, "C", 1) == 1 && signalRequests == 2);
    assert(signal(SIGTTOU, caughtSignal) == SIG_IGN);
    groupError = EIO; // orphaned process group
    assert(LSBPTYRead(slave, data, 3) == -1 && errno == EIO && signalRequests == 3);
    assert(LSBPTYWrite(slave, data, 3) == -1 && errno == EIO && signalRequests == 4);
    groupError = 0;
    assert(LSBPTYSetAttributes(slave, TCSANOW, &attrs) == -1 && errno == EINTR && signalRequests == 5);
    assert(!LSBFakePTYSetForeground(terminal, testGroup));
    assert(LSBPTYReadVNoCancel(slave, v, 2) == 3 && !strcmp(data, "abc")); // no background attempt consumed input
    assert(read(master, data, 3) == 3 && !strcmp(data, "ABC"));
    assert(fcntl(slave, F_SETFL, O_NONBLOCK) == 0);
    assert(LSBPTYRead(slave, data, 1) == -1 && errno == EAGAIN);
    assert(!fcntl(slave, F_SETFL, 0));
    assert(!sigaction(SIGTTIN, &oldRead, NULL) && !sigaction(SIGTTOU, &oldWrite, NULL));
    testGroup = 0;
}

static void testResumedReader(int master, int slave, bool groupChangeOnly) {
    LSBFakePTYState *state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    assert(state != MAP_FAILED && !LSBFakePTYInitialize(state, slave, 24, 80));
    LSBFakePTYState *previous = terminal;
    signalRequests = 0;
    testGroup = 123;
    assert(!LSBFakePTYSetForeground(state, testGroup) && !LSBGuestFakePTYConfigure(state, sizeof(*state)));
    int hostWatch = -1;
    if (groupChangeOnly) {
        char path[] = "/tmp/LSBFakePTYWake.XXXXXX";
        hostWatch = mkstemp(path);
        assert(hostWatch >= 0);
        watchFD = open(path, O_RDONLY | O_CLOEXEC);
        struct stat metadata;
        assert(watchFD >= 0 && !fstat(watchFD, &metadata) && !unlink(path));
        watchDevice = metadata.st_dev;
        watchInode = metadata.st_ino;
        assert(LSBGuestFakePTYWatchFD() == watchFD);
    }
    int ack[2];
    assert(!pipe(ack));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(ack[0]);
        waitAck = ack[1];
        struct sigaction action = {.sa_handler = ttyDefault};
        assert(!sigaction(SIGTTIN, &action, NULL));
        char byte;
        _exit(LSBPTYRead(slave, &byte, 1) == 1 && byte == 'R' && signalRequests == 1 ? 37 : 100);
    }
    close(ack[1]);
    char marker;
    assert(read(ack[0], &marker, 1) == 1 && marker == 'W'); // pending foreground read, with no input available
    int status;
    if (groupChangeOnly) {
        // Only the group changes. No input, SIGSTOP or SIGCONT can wake kevent to conceal a missing vnode event.
        assert(!LSBFakePTYSetForeground(state, 456) && !flock(hostWatch, LOCK_UN));
    } else {
        assert(!kill(child, SIGSTOP) && waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
        assert(!LSBFakePTYSetForeground(state, 456) && !kill(child, SIGCONT)); // bg must stop again before any input
    }
    assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
    assert(!LSBFakePTYSetForeground(state, testGroup) && !kill(child, SIGCONT));
    assert(write(master, "R", 1) == 1 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    close(ack[0]);
    if (groupChangeOnly) {
        assert(!close(watchFD) && LSBGuestFakePTYWatchFD() == -1);
        char path[] = "/tmp/LSBFakePTYOther.XXXXXX";
        int other = mkstemp(path);
        assert(other >= 0 && dup2(other, watchFD) == watchFD && !fcntl(watchFD, F_SETFD, FD_CLOEXEC));
        assert(!unlink(path) && LSBGuestFakePTYWatchFD() == -1);
        if (other != watchFD) close(other);
        close(watchFD);
        close(hostWatch);
    }
    assert(!LSBGuestFakePTYConfigure(previous, sizeof(*previous)) && !munmap(state, sizeof(*state)));
    testGroup = 0;
}

static void *readForever(void *context) {
    char byte;
    (void)LSBPTYRead(*(int *)context, &byte, 1);
    return NULL;
}

static void testReadWaitPolicy(int fd) {
    struct timeval timeout = {.tv_usec = 50000};
    assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    char byte;
    assert(LSBPTYRead(fd, &byte, 1) == -1 && errno == EAGAIN);
    timeout = (struct timeval){};
    assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    int ack[2];
    assert(!pipe(ack));
    waitAck = ack[1];
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, readForever, &fd));
    assert(read(ack[0], &byte, 1) == 1 && byte == 'W');
    assert(!pthread_cancel(thread));
    void *result;
    assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
    close(ack[0]);
    close(ack[1]);
}

int main(void) {
    alarm(15);
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    LSBFakePTYState state;
    assert(!LSBFakePTYInitialize(&state, sockets[1], 24, 80));
    assert(!LSBFakePTYSetForeground(&state, 789123));
    assert(!LSBGuestFakePTYConfigure(&state, sizeof(state)));
    int duplicate = dup(sockets[1]);
    assert(duplicate >= 0);

    // All formerly nested terminal operations now perform exactly one real fstat, through the arm64 ioctl entry.
    struct termios attributes;
    CHECK(LSBPTYIOCtl(duplicate, TIOCGETA, &attributes), 0, EDOM);
    assert(attributes.c_lflag & ICANON);
    attributes.c_lflag &= ~ECHO;
    CHECK(LSBPTYIOCtl(duplicate, TIOCSETA, &attributes), 0, EDOM);
    CHECK(LSBPTYIOCtl(duplicate, TIOCGETA, &attributes), 0, EDOM);
    assert(!(attributes.c_lflag & ECHO));
    CHECK(LSBPTYIOCtl(duplicate, TIOCSETAW, &attributes), 0, EDOM);
    CHECK(LSBPTYIOCtl(duplicate, TIOCSETAF, &attributes), -1, ENOTSUP);
    CHECK(LSBPTYIOCtl(duplicate, TIOCGETA, NULL), -1, EFAULT);
    CHECK(LSBPTYIOCtl(duplicate, TIOCSETA, NULL), -1, EFAULT);
    pid_t group = 0;
    CHECK(LSBPTYIOCtl(duplicate, TIOCGPGRP, &group), 0, EDOM);
    assert(group == 789123);
    group = 5678;
    CHECK(LSBPTYIOCtl(duplicate, TIOCSPGRP, &group), 0, EDOM);
    group = 0;
    CHECK(LSBPTYIOCtl(duplicate, TIOCGPGRP, &group), 0, EDOM);
    assert(group == 5678);
    CHECK(LSBPTYIOCtl(duplicate, TIOCGPGRP, NULL), -1, EFAULT);
    CHECK(LSBPTYIOCtl(duplicate, TIOCSPGRP, NULL), -1, EFAULT);
    group = -1;
    CHECK(LSBPTYIOCtl(duplicate, TIOCSPGRP, &group), -1, EINVAL);

    // Direct public termios operations still validate each call; matching is never cached across calls.
    CHECK(LSBPTYGetAttributes(duplicate, &attributes), 0, EDOM);
    CHECK(LSBPTYSetAttributes(duplicate, TCSANOW, &attributes), 0, EDOM);
    CHECK(LSBPTYGetForeground(duplicate), 5678, EDOM);
    CHECK(LSBPTYSetForeground(duplicate, 1234), 0, EDOM);

    // Nonterminal controls on the fake terminal and terminal controls on an unrelated socket remain native.
    assert(write(sockets[0], "abc", 3) == 3);
    int available = 0;
    CHECK(LSBPTYIOCtl(duplicate, FIONREAD, &available), 0, EDOM);
    assert(available == 3);
    assert(ioctl(sockets[0], TIOCGETA, &attributes) == -1);
    int nativeError = errno;
    CHECK(LSBPTYIOCtl(sockets[0], TIOCGETA, &attributes), -1, nativeError);
    assert(tcgetattr(sockets[0], &attributes) == -1);
    nativeError = errno;
    CHECK(LSBPTYGetAttributes(sockets[0], &attributes), -1, nativeError);

    int pipeFDs[2];
    assert(!pipe(pipeFDs));
    assert(dup2(pipeFDs[0], duplicate) == duplicate);
    assert(write(pipeFDs[1], "other", 5) == 5);
    CHECK(LSBPTYIOCtl(duplicate, FIONREAD, &available), 0, EDOM);
    assert(available == 5);
    assert(ioctl(duplicate, TIOCGETA, &attributes) == -1);
    nativeError = errno;
    CHECK(LSBPTYIOCtl(duplicate, TIOCGETA, &attributes), -1, nativeError);
    CHECK(LSBPTYGetAttributes(duplicate, &attributes), -1, nativeError);
    assert(!close(duplicate));
    CHECK(LSBPTYIOCtl(duplicate, TIOCGETA, &attributes), -1, EBADF);
    CHECK(LSBPTYGetAttributes(duplicate, &attributes), -1, EBADF);
    char pending[3];
    assert(read(sockets[1], pending, sizeof(pending)) == 3);
    testAccess(sockets[0], sockets[1]);
    testResumedReader(sockets[0], sockets[1], false);
    testResumedReader(sockets[0], sockets[1], true);
    testReadWaitPolicy(sockets[1]);
    close(pipeFDs[0]); close(pipeFDs[1]); close(sockets[0]); close(sockets[1]);
    puts("FakePTY descriptor validation: ioctl forwarding, background IO, signal masks, TOSTOP and blocked reader stop/resume passed");
    return 0;
}
