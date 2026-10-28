#include "FakePTY.h"
#include "ProcessSession.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static LSBFakePTYState *terminal;

int LSBGuestFakePTYConfigure(LSBFakePTYState *state, size_t mappedBytes) {
    int error = LSBFakePTYValidate(state, mappedBytes);
    if (!error) terminal = state;
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
            if (mapping != MAP_FAILED && LSBGuestFakePTYConfigure(mapping, LSB_FAKE_PTY_MAP_SIZE)) munmap(mapping, LSB_FAKE_PTY_MAP_SIZE);
        }
        close(fd);
    }
    errno = saved;
}
#endif

static bool isTerminal(int fd) { return LSBFakePTYMatchesFD(terminal, fd); }
static int finish(int error, int saved) { errno = error ? error : saved; return error ? -1 : 0; }

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
    if (action != TCSANOW) return finish(ENOTSUP, saved);
    return finish(LSBFakePTYSetAttributes(terminal, attributes), saved);
}

int LSBPTYSetAttributes(int fd, int action, const struct termios *attributes) {
    return isTerminal(fd) ? setAttributes(action, attributes) : tcsetattr(fd, action, attributes);
}

static pid_t getForeground(void) {
    int saved = errno;
    pid_t foreground;
    int error = LSBFakePTYGetForeground(terminal, &foreground);
    if (!error && !foreground) {
        // Calls within this interposing dylib do not pass through its getpgrp interposition. Use the guest session explicitly.
        foreground = LSBGuestProcessGroup();
        error = foreground < 0 ? errno : LSBFakePTYSetForeground(terminal, foreground);
    }
    if (error) { errno = error; return -1; }
    errno = saved;
    return foreground;
}

pid_t LSBPTYGetForeground(int fd) { return isTerminal(fd) ? getForeground() : tcgetpgrp(fd); }

static int setForeground(pid_t foreground) {
    int saved = errno;
    return finish(LSBFakePTYSetForeground(terminal, foreground), saved);
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
            error = argument ? LSBFakePTYSetWindow(terminal, argument) : EFAULT;
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

int LSBPTYDrain(int fd) { if (!isTerminal(fd)) return tcdrain(fd); errno = ENOTSUP; return -1; }
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
    {(const void *)&LSBPTYIsATTY, (const void *)&isatty}, {(const void *)&LSBPTYGetAttributes, (const void *)&tcgetattr},
    {(const void *)&LSBPTYSetAttributes, (const void *)&tcsetattr}, {(const void *)&LSBPTYGetForeground, (const void *)&tcgetpgrp},
    {(const void *)&LSBPTYSetForeground, (const void *)&tcsetpgrp}, {(const void *)&LSBPTYIOCtl, (const void *)&ioctl},
    {(const void *)&LSBPTYFlush, (const void *)&tcflush}, {(const void *)&LSBPTYDrain, (const void *)&tcdrain},
    {(const void *)&LSBPTYFlow, (const void *)&tcflow}, {(const void *)&LSBPTYSendBreak, (const void *)&tcsendbreak},
    {(const void *)&LSBPTYNameR, (const void *)&ttyname_r}, {(const void *)&LSBPTYName, (const void *)&ttyname}
};
#endif
