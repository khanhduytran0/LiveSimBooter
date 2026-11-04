#include "FakePTY.h"
#include "ChildWait.h"
#include "ProcessSession.h"
#include "../../ProcessSupport/SpawnTransport.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <limits.h>
#include <time.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/uio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static LSBFakePTYState *terminal;
static int watchFD = -1;
static dev_t watchDevice;
static ino_t watchInode;

int LSBGuestFakePTYWatchFD(void) {
    if (watchFD < 0) return -1;
    int saved = errno;
    struct stat metadata;
    bool ours = fcntl(watchFD, F_GETFD) == FD_CLOEXEC && !fstat(watchFD, &metadata) && S_ISREG(metadata.st_mode) &&
                metadata.st_dev == watchDevice && metadata.st_ino == watchInode;
    errno = saved;
    return ours ? watchFD : -1;
}

int LSBGuestFakePTYConfigure(LSBFakePTYState *state, size_t mappedBytes) {
    int error = LSBFakePTYValidate(state, mappedBytes);
    if (!error) {
        int old = LSBGuestFakePTYWatchFD();
        if (old >= 0) close(old);
        watchFD = -1;
        terminal = state;
    }
    return error;
}

#ifndef LSB_FAKE_PTY_TESTING
__attribute__((constructor)) static void initializeTerminal(void) {
    const char *path = getenv(LSB_FAKE_PTY_ENV);
    if (!path || path[0] != '/') return;
    int saved = errno;
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        struct stat metadata;
        if (!fstat(fd, &metadata) && S_ISREG(metadata.st_mode) && metadata.st_size == LSB_FAKE_PTY_MAP_SIZE) {
            void *mapping = mmap(NULL, LSB_FAKE_PTY_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (mapping != MAP_FAILED) {
                if (LSBGuestFakePTYConfigure(mapping, LSB_FAKE_PTY_MAP_SIZE)) munmap(mapping, LSB_FAKE_PTY_MAP_SIZE);
                else {
                    // Keep the mapped vnode identity through unlink/rename. A reused guest fd must never become an unrelated event source.
                    watchDevice = metadata.st_dev;
                    watchInode = metadata.st_ino;
                    watchFD = fd;
                    fd = -1;
                }
            }
        }
        if (fd >= 0) close(fd);
    }
    errno = saved;
}
#endif

static bool isTerminal(int fd) { return LSBFakePTYMatchesFD(terminal, fd); }
static int finish(int error, int saved) { errno = error ? error : saved; return error ? -1 : 0; }

// This function is reached only for the slave socket and its aliases, including dup/fileport aliases.
// ttyread, ttywrite and modifying ttioctl in XNU's bsd/kern/tty.c distinguish blocked/ignored TTIN from TTOU.
static int terminalAccess(LSBTerminalAccess access) {
    for (;;) {
        if (!LSBFakePTYRequiresForeground(terminal, LSBGuestProcessGroup(), access == LSB_TERMINAL_WRITE)) return 0;
        int number = access == LSB_TERMINAL_READ ? SIGTTIN : SIGTTOU;
        struct sigaction action;
        sigset_t mask;
        if (sigprocmask(SIG_SETMASK, NULL, &mask) || LSBSigaction(number, NULL, &action)) return errno;
        if (sigismember(&mask, number) || action.sa_handler == SIG_IGN) return access == LSB_TERMINAL_READ ? EIO : 0;

        bool signalCaller = false;
        int error = LSBTransportCheckTerminalIO(access, &signalCaller);
        if (error || !signalCaller) return error;
        // The broker signals the other members. Deliver synchronously on this issuing thread, whose signal mask we checked.
        if (raise(number)) return errno;
        if (action.sa_handler != SIG_DFL) return EINTR;
        // The shim's default tty-stop handler returns after SIGCONT. Only proceed if the shell has given us the terminal.
    }
}

// The guest libc uses the $NOCANCEL exports for stdio. Preserve each original entry for ordinary descriptors.
extern ssize_t readNoCancel(int, void *, size_t) __asm("_read$NOCANCEL");
extern ssize_t writeNoCancel(int, const void *, size_t) __asm("_write$NOCANCEL");
extern ssize_t readvNoCancel(int, const struct iovec *, int) __asm("_readv$NOCANCEL");
extern ssize_t writevNoCancel(int, const struct iovec *, int) __asm("_writev$NOCANCEL");
extern ssize_t recvmsgNoCancel(int, struct msghdr *, int) __asm("_recvmsg$NOCANCEL");
extern ssize_t sendmsgNoCancel(int, const struct msghdr *, int) __asm("_sendmsg$NOCANCEL");
extern int pollNoCancel(struct pollfd *, nfds_t, int) __asm("_poll$NOCANCEL");
extern int closeNoCancel(int) __asm("_close$NOCANCEL");

static void closeIOQueue(void *address) {
    int queue = *(int *)address;
    if (queue >= 0) closeNoCancel(queue);
}

static ssize_t terminalTransfer(int fd, const struct iovec *vectors, int count, LSBTerminalAccess access, bool noCancel) {
    bool writing = access == LSB_TERMINAL_WRITE;
    struct msghdr message = {.msg_iov = (struct iovec *)vectors, .msg_iovlen = count};
    struct kevent events[3];
    int saved = errno, queue = -1, error = 0;
    ssize_t result = -1;
    uint64_t deadline = 0;
    pthread_cleanup_push(closeIOQueue, &queue);
    for (;;) {
        error = terminalAccess(access);
        if (error) break;
        if (writing) result = noCancel ? sendmsgNoCancel(fd, &message, MSG_DONTWAIT) : sendmsg(fd, &message, MSG_DONTWAIT);
        else result = noCancel ? recvmsgNoCancel(fd, &message, MSG_DONTWAIT) : recvmsg(fd, &message, MSG_DONTWAIT);
        if (result >= 0) break;
        error = errno;
        if (error != EWOULDBLOCK) break;
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0) { error = errno; break; }
        if (flags & O_NONBLOCK) break;
        if (queue < 0) {
            // Wait on either socket readiness or continuation, without an idle timer. A foreground read may still be
            // waiting in the kernel when Ctrl-Z stops the process; after bg it must generate TTIN before consuming input.
            queue = kqueue();
            if (queue < 0) { error = errno; break; }
            EV_SET(events, fd, writing ? EVFILT_WRITE : EVFILT_READ, EV_ADD, 0, 0, NULL);
            EV_SET(events + 1, SIGCONT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
            if (kevent(queue, events, 2, NULL, 0, NULL) < 0) { error = errno; break; }
            int vnode = LSBGuestFakePTYWatchFD();
            if (vnode >= 0) {
                // The host unlocks this vnode after tcsetpgrp; an empty foreground reader must recheck even without SIGCONT.
                EV_SET(events + 2, vnode, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_FUNLOCK, 0, NULL);
                (void)kevent(queue, events + 2, 1, NULL, 0, NULL); // Socket and SIGCONT remain usable if vnode events are unavailable.
            }
            struct timeval timeout;
            socklen_t size = sizeof(timeout);
            if (getsockopt(fd, SOL_SOCKET, writing ? SO_SNDTIMEO : SO_RCVTIMEO, &timeout, &size) < 0) { error = errno; break; }
            if (timeout.tv_sec || timeout.tv_usec)
                deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC) + (uint64_t)timeout.tv_sec * 1000000000 + (uint64_t)timeout.tv_usec * 1000;
            continue; // Cover foreground/readiness changes preceding event registration.
        }
        int milliseconds = -1;
        if (deadline) {
            uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
            if (now >= deadline) break; // EWOULDBLOCK from the most recent transfer.
            uint64_t remaining = (deadline - now + 999999) / 1000000;
            milliseconds = remaining > INT_MAX ? INT_MAX : (int)remaining;
        }
        // kevent is not a Darwin cancellation point. Poll the kqueue with the original API's cancellation policy.
        struct pollfd ready = {.fd = queue, .events = POLLIN};
        int polled = noCancel ? pollNoCancel(&ready, 1, milliseconds) : poll(&ready, 1, milliseconds);
        if (!polled) continue;
        if (polled < 0 && errno != EINTR) { error = errno; break; }
        const struct timespec immediate = {};
        int received = kevent(queue, NULL, 0, events, 3, &immediate);
        if (received < 0) { error = errno; break; }
        if (polled < 0) {
            bool resumed = false;
            for (int i = 0; i < received; ++i) if (events[i].filter == EVFILT_SIGNAL && events[i].ident == SIGCONT) resumed = true;
            if (!resumed) { error = EINTR; break; }
        }
    }
    pthread_cleanup_pop(1);
    errno = result < 0 ? error : saved;
    return result;
}

static bool checkTerminalFD(int fd, LSBTerminalAccess access) {
    // In the usual TOSTOP-off session, writes retain the socket's native blocking and cancellation behavior.
    if (!terminal || (access == LSB_TERMINAL_WRITE && !atomic_load_explicit(&terminal->stopBackgroundOutput, memory_order_acquire))) return false;
    return isTerminal(fd);
}

#define PTY_SCALAR_IO(name, original, type, access, noCancel) \
    ssize_t name(int fd, type *bytes, size_t length) { \
        if (!length || !checkTerminalFD(fd, access)) return original(fd, bytes, length); \
        struct iovec vector = {(void *)bytes, length}; \
        return terminalTransfer(fd, &vector, 1, access, noCancel); \
    }
#define PTY_VECTOR_IO(name, original, access, noCancel) \
    ssize_t name(int fd, const struct iovec *vectors, int count) { \
        if (count <= 0 || !checkTerminalFD(fd, access)) return original(fd, vectors, count); \
        return terminalTransfer(fd, vectors, count, access, noCancel); \
    }

PTY_SCALAR_IO(LSBPTYRead, read, void, LSB_TERMINAL_READ, false)
PTY_SCALAR_IO(LSBPTYWrite, write, const void, LSB_TERMINAL_WRITE, false)
PTY_VECTOR_IO(LSBPTYReadV, readv, LSB_TERMINAL_READ, false)
PTY_VECTOR_IO(LSBPTYWriteV, writev, LSB_TERMINAL_WRITE, false)
PTY_SCALAR_IO(LSBPTYReadNoCancel, readNoCancel, void, LSB_TERMINAL_READ, true)
PTY_SCALAR_IO(LSBPTYWriteNoCancel, writeNoCancel, const void, LSB_TERMINAL_WRITE, true)
PTY_VECTOR_IO(LSBPTYReadVNoCancel, readvNoCancel, LSB_TERMINAL_READ, true)
PTY_VECTOR_IO(LSBPTYWriteVNoCancel, writevNoCancel, LSB_TERMINAL_WRITE, true)
#undef PTY_SCALAR_IO
#undef PTY_VECTOR_IO

int LSBPTYIsATTY(int fd) { return isTerminal(fd) ? 1 : isatty(fd); }

static int getAttributes(struct termios *attributes) {
    int saved = errno;
    return finish(attributes ? LSBFakePTYGetAttributes(terminal, attributes) : EFAULT, saved);
}

int LSBPTYGetAttributes(int fd, struct termios *attributes) {
    return isTerminal(fd) ? getAttributes(attributes) : tcgetattr(fd, attributes);
}

int LSBPTYFlush(int fd, int selector) {
    if (!isTerminal(fd)) return tcflush(fd, selector);
    if (selector != TCIFLUSH && selector != TCOFLUSH && selector != TCIOFLUSH) { errno = EINVAL; return -1; }
    // A stream can have bytes pending in both the host and kernel. Exact flush semantics need an acknowledged control channel.
    errno = ENOTSUP;
    return -1;
}

static int setAttributes(int action, const struct termios *attributes) {
    int saved = errno;
    if (!attributes) return finish(EFAULT, saved);
    if (action != TCSANOW && action != TCSADRAIN && action != TCSAFLUSH) return finish(EINVAL, saved);
    if (action == TCSAFLUSH) return finish(ENOTSUP, saved);
    int access = terminalAccess(LSB_TERMINAL_CONTROL);
    if (access) return finish(access, saved);
    if (action == TCSADRAIN) {
        int error = LSBTransportDrainTerminal();
        if (error) return finish(error, saved);
    }
    return finish(LSBFakePTYSetAttributes(terminal, attributes), saved);
}

int LSBPTYSetAttributes(int fd, int action, const struct termios *attributes) {
    return isTerminal(fd) ? setAttributes(action, attributes) : tcsetattr(fd, action, attributes);
}

static pid_t getForeground(void) {
    int saved = errno;
    pid_t foreground;
    int error = LSBFakePTYGetForeground(terminal, &foreground);
    if (!error && !foreground) error = ENOTTY;
    if (error) { errno = error; return -1; }
    errno = saved;
    return foreground;
}

pid_t LSBPTYGetForeground(int fd) { return isTerminal(fd) ? getForeground() : tcgetpgrp(fd); }

static int setForeground(pid_t foreground) {
    int saved = errno;
    int error = terminalAccess(LSB_TERMINAL_CONTROL);
    return finish(error ? error : LSBTransportSetForegroundGroup(foreground), saved);
}

int LSBPTYSetForeground(int fd, pid_t foreground) { return isTerminal(fd) ? setForeground(foreground) : tcsetpgrp(fd, foreground); }

__attribute__((used)) static int ioctlArgument(int fd, unsigned long request, void *argument) {
    if (!isTerminal(fd)) return ioctl(fd, request, argument);
    int saved = errno, error = 0;
    // This call already matched the descriptor. Share the operations without repeating their public wrappers' fstat checks.
    switch (request) {
        case TIOCGETA: return getAttributes(argument);
        case TIOCSETA: return setAttributes(TCSANOW, argument);
        case TIOCSETAW: return setAttributes(TCSADRAIN, argument);
        case TIOCSETAF: return setAttributes(TCSAFLUSH, argument);
        case TIOCGPGRP:
            if (!argument) return finish(EFAULT, saved);
            *(pid_t *)argument = getForeground();
            return *(pid_t *)argument < 0 ? -1 : 0;
        case TIOCSPGRP:
            return argument ? setForeground(*(pid_t *)argument) : finish(EFAULT, saved);
        case TIOCGWINSZ:
            error = argument ? LSBFakePTYGetWindow(terminal, argument) : EFAULT;
            break;
        case TIOCSWINSZ:
            // Host UI changes also send SIGWINCH through its audited broker. A guest's direct resize only updates the stored dimensions.
            error = argument ? terminalAccess(LSB_TERMINAL_CONTROL) : EFAULT;
            if (!error) error = LSBFakePTYSetWindow(terminal, argument);
            break;
        case TIOCFLUSH:
            if (!argument) return finish(EFAULT, saved);
            return finish(ENOTSUP, saved);
        case TIOCOUTQ:
            error = ENOTSUP;
            break;
        default:
            // Keep socket readiness/nonblocking requests native, but never pretend unsupported terminal controls succeeded.
            if (IOCGROUP(request) != 't') return ioctl(fd, request, argument);
            error = ENOTSUP;
            break;
    }
    return finish(error, saved);
}

#if defined(__arm64__)
// Darwin arm64 passes variadic arguments on the stack. Preserve the third slot verbatim, including _IO requests with unencoded payloads.
// A C va_arg would be undefined for legitimate two-argument ioctl calls. The native syscall ignores this slot for no-payload controls.
__attribute__((naked)) int LSBPTYIOCtl(int fd, unsigned long request, ...) {
    __asm__("ldr x2, [sp]\n"
            "b _ioctlArgument");
}
#else
#error The terminal ioctl forwarding entry requires the Darwin arm64 ABI.
#endif

int LSBPTYDrain(int fd) {
    if (!isTerminal(fd)) return tcdrain(fd);
    int saved = errno;
    int error = terminalAccess(LSB_TERMINAL_CONTROL);
    return finish(error ? error : LSBTransportDrainTerminal(), saved);
}
int LSBPTYFlow(int fd, int action) { if (!isTerminal(fd)) return tcflow(fd, action); errno = ENOTSUP; return -1; }
int LSBPTYSendBreak(int fd, int duration) { if (!isTerminal(fd)) return tcsendbreak(fd, duration); errno = ENOTSUP; return -1; }

int LSBPTYNameR(int fd, char *buffer, size_t length) {
    if (!isTerminal(fd)) return ttyname_r(fd, buffer, length);
    static const char name[] = "/dev/lsbtty";
    if (!buffer) return EINVAL;
    if (length < sizeof(name)) return ERANGE;
    memcpy(buffer, name, sizeof(name));
    return 0;
}

char *LSBPTYName(int fd) {
    if (!isTerminal(fd)) return ttyname(fd);
    static char name[] = "/dev/lsbtty";
    return name;
}

#ifndef LSB_FAKE_PTY_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {&LSBPTYRead, &read}, {&LSBPTYWrite, &write}, {&LSBPTYReadV, &readv}, {&LSBPTYWriteV, &writev},
    {&LSBPTYReadNoCancel, &readNoCancel}, {&LSBPTYWriteNoCancel, &writeNoCancel}, {&LSBPTYReadVNoCancel, &readvNoCancel}, {&LSBPTYWriteVNoCancel, &writevNoCancel},
    {(const void *)&LSBPTYIsATTY, (const void *)&isatty}, {(const void *)&LSBPTYGetAttributes, (const void *)&tcgetattr},
    {(const void *)&LSBPTYSetAttributes, (const void *)&tcsetattr}, {(const void *)&LSBPTYGetForeground, (const void *)&tcgetpgrp},
    {(const void *)&LSBPTYSetForeground, (const void *)&tcsetpgrp}, {(const void *)&LSBPTYIOCtl, (const void *)&ioctl},
    {(const void *)&LSBPTYFlush, (const void *)&tcflush}, {(const void *)&LSBPTYDrain, (const void *)&tcdrain},
    {(const void *)&LSBPTYFlow, (const void *)&tcflow}, {(const void *)&LSBPTYSendBreak, (const void *)&tcsendbreak},
    {(const void *)&LSBPTYNameR, (const void *)&ttyname_r}, {(const void *)&LSBPTYName, (const void *)&ttyname}
};
#endif
