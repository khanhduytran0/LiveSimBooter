#include "../Guest/FakePTY.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
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
#define LSB_FAKE_PTY_TESTING
#include "../Guest/FakePTY.c"

pid_t LSBGuestProcessGroup(void) { return 789123; }

#define CHECK(call, result, error) do { \
    descriptorChecks = 0; \
    errno = EDOM; \
    assert((call) == (result)); \
    assert(errno == (error)); \
    assert(descriptorChecks == 1); \
} while (0)

int main(void) {
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    LSBFakePTYState state;
    assert(!LSBFakePTYInitialize(&state, sockets[1], 24, 80));
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
    CHECK(LSBPTYIOCtl(duplicate, TIOCSETAW, &attributes), -1, ENOTSUP);
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
    close(pipeFDs[0]); close(pipeFDs[1]); close(sockets[0]); close(sockets[1]);
    puts("FakePTY descriptor validation: single ioctl check, direct wrappers, errno and native/reused/closed FD forwarding passed");
    return 0;
}
