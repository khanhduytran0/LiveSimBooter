#include "../Guest/FakePTY.h"
#include "../ProcessSupport/SpawnTransport.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/fileport.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static LSBFakePTYState *hostState;
pid_t LSBGuestProcessGroup(void) { return atomic_load(&hostState->foreground); }
int LSBSigaction(int sig, const struct sigaction *act, struct sigaction *old) { return sigaction(sig, act, old); }
int LSBTransportCheckTerminalIO(LSBTerminalAccess access, bool *signalCaller) { (void)access; (void)signalCaller; return EIO; }
int LSBTransportSetForegroundGroup(pid_t group) { return LSBFakePTYSetForeground(hostState, group); }
static int drainError;
int LSBTransportDrainTerminal(void) { return drainError; }

typedef struct {
    unsigned char input[16384], output[16384];
    size_t inputLength, outputLength;
    unsigned signals;
    pid_t foreground;
    int signalNumber, error;
} Capture;

static int captureInput(void *context, const void *bytes, size_t length) {
    Capture *capture = context;
    if (capture->error) return capture->error;
    assert(capture->inputLength + length <= sizeof(capture->input));
    memcpy(capture->input + capture->inputLength, bytes, length);
    capture->inputLength += length;
    return 0;
}

static int captureOutput(void *context, const void *bytes, size_t length) {
    Capture *capture = context;
    if (capture->error) return capture->error;
    assert(capture->outputLength + length <= sizeof(capture->output));
    memcpy(capture->output + capture->outputLength, bytes, length);
    capture->outputLength += length;
    return 0;
}

static int captureSignal(void *context, pid_t foreground, int signalNumber) {
    Capture *capture = context;
    if (capture->error) return capture->error;
    ++capture->signals;
    capture->foreground = foreground;
    capture->signalNumber = signalNumber;
    return 0;
}

static void expectBytes(const unsigned char *bytes, size_t length, const char *expected) {
    assert(length == strlen(expected));
    assert(!memcmp(bytes, expected, length));
}

int main(void) {
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    LSBFakePTYState *state = mmap(NULL, LSB_FAKE_PTY_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(state != MAP_FAILED);
    assert(LSBFakePTYValidate(state, LSB_FAKE_PTY_MAP_SIZE) == EPROTO);
    assert(!LSBFakePTYInitialize(state, sockets[1], 24, 80));
    hostState = state;
    assert(LSBFakePTYValidate(state, sizeof(*state) - 1) == EINVAL);
    assert(!LSBFakePTYValidate(state, LSB_FAKE_PTY_MAP_SIZE));
    assert(!LSBGuestFakePTYConfigure(state, LSB_FAKE_PTY_MAP_SIZE));
    assert(LSBPTYIsATTY(sockets[1]) == 1);
    assert(!LSBPTYIsATTY(sockets[0]));
    int duplicate = dup(sockets[1]);
    assert(duplicate >= 0 && LSBPTYIsATTY(duplicate) == 1);
    fileport_t port;
    assert(!fileport_makeport(duplicate, &port));
    int imported = fileport_makefd(port);
    assert(imported >= 0 && LSBPTYIsATTY(imported) == 1);
    mach_port_deallocate(mach_task_self(), port);
    close(imported);
    assert(!LSBFakePTYMatchesFD(state, -1));
    errno = E2BIG;
    assert(LSBFakePTYMatchesFD(state, duplicate) && errno == E2BIG);
    assert(!LSBPTYIsATTY(-1) && errno == EBADF);

    struct termios attributes, initial;
    errno = E2BIG;
    assert(!LSBPTYGetAttributes(duplicate, &initial) && errno == E2BIG);
    assert(initial.c_lflag & ICANON);
    assert(LSBPTYGetAttributes(duplicate, NULL) == -1 && errno == EFAULT);
    assert(tcgetattr(sockets[0], &attributes) == -1);
    int nativeError = errno;
    assert(LSBPTYGetAttributes(sockets[0], &attributes) == -1 && errno == nativeError);
    char name[32];
    assert(!LSBPTYNameR(duplicate, name, sizeof(name)) && !strcmp(name, "/dev/lsbtty"));
    assert(LSBPTYNameR(duplicate, name, 2) == ERANGE);
    assert(!strcmp(LSBPTYName(duplicate), name));
    assert(LSBPTYGetForeground(duplicate) == -1 && errno == ENOTTY);
    assert(!LSBFakePTYSetForeground(hostState, 789123));
    errno = E2BIG;
    assert(LSBPTYGetForeground(duplicate) == 789123 && errno == E2BIG);
    assert(!LSBPTYSetForeground(duplicate, 1234));
    assert(LSBPTYGetForeground(duplicate) == 1234);
    assert(LSBPTYSetForeground(duplicate, -1) == -1 && errno == EINVAL);
    pid_t group = 0;
    assert(!LSBPTYIOCtl(duplicate, TIOCGPGRP, &group) && group == 1234);
    group = 5678;
    assert(!LSBPTYIOCtl(duplicate, TIOCSPGRP, &group));
    struct winsize window = {0};
    assert(!LSBPTYIOCtl(duplicate, TIOCGWINSZ, &window) && window.ws_row == 24 && window.ws_col == 80);
    window.ws_row = 40;
    window.ws_col = 120;
    assert(!LSBPTYIOCtl(duplicate, TIOCSWINSZ, &window));
    assert(!LSBFakePTYGetWindow(state, &window) && window.ws_row == 40 && window.ws_col == 120);
    assert(LSBPTYIOCtl(duplicate, TIOCGWINSZ, NULL) == -1 && errno == EFAULT);
    assert(LSBPTYIOCtl(duplicate, TIOCSBRK) == -1 && errno == ENOTSUP);
    assert(!LSBPTYDrain(duplicate));
    assert(LSBPTYFlush(duplicate, TCOFLUSH) == -1 && errno == ENOTSUP);
    assert(LSBPTYFlow(duplicate, TCOOFF) == -1 && errno == ENOTSUP);
    assert(LSBPTYSendBreak(duplicate, 0) == -1 && errno == ENOTSUP);

    assert(write(sockets[0], "pending", 7) == 7);
    int available = 0;
    assert(!LSBPTYIOCtl(duplicate, FIONREAD, &available) && available == 7);
    assert(LSBPTYFlush(duplicate, TCIFLUSH) == -1 && errno == ENOTSUP);
    assert(!LSBPTYIOCtl(duplicate, FIONREAD, &available) && available == 7);
    char pending[7];
    assert(read(duplicate, pending, sizeof(pending)) == sizeof(pending));

    LSBFakePTYDiscipline discipline = {0};
    Capture capture = {0};
    LSBFakePTYCallbacks callbacks = {&capture, captureInput, captureOutput, captureSignal};
    assert(!LSBFakePTYInput(state, &discipline, "one", 3, &callbacks));
    assert(!capture.inputLength && discipline.length == 3);
    assert(!LSBFakePTYInput(state, &discipline, "x\177\r", 3, &callbacks));
    expectBytes(capture.input, capture.inputLength, "one\n");
    expectBytes(capture.output, capture.outputLength, "onex\b \b\r\n");
    assert(!discipline.length);

    memset(&capture, 0, sizeof(capture));
    assert(!LSBFakePTYInput(state, &discipline, "abc def\027!\n", 10, &callbacks));
    expectBytes(capture.input, capture.inputLength, "abc !\n");
    assert(!discipline.length);
    memset(&capture, 0, sizeof(capture));
    assert(!LSBFakePTYInput(state, &discipline, "wrong\025right\n", 12, &callbacks));
    expectBytes(capture.input, capture.inputLength, "right\n");
    memset(&capture, 0, sizeof(capture));
    assert(!LSBFakePTYInput(state, &discipline, "a\026\003\n", 4, &callbacks));
    expectBytes(capture.input, capture.inputLength, "a\003\n");
    assert(!capture.signals);

    memset(&capture, 0, sizeof(capture));
    assert(!LSBFakePTYInput(state, &discipline, "discard\003", 8, &callbacks));
    assert(capture.signals == 1 && capture.foreground == 5678 && capture.signalNumber == SIGINT);
    assert(!capture.inputLength && !discipline.length);
    assert(LSBFakePTYInput(state, &discipline, "\004", 1, &callbacks) == ENOTSUP);
    assert(!LSBFakePTYInput(state, &discipline, "stale\032", 6, &callbacks));
    assert(capture.signals == 2 && capture.foreground == 5678 && capture.signalNumber == SIGTSTP && !discipline.length);
    assert(!LSBFakePTYInput(state, &discipline, "abc\004", 4, &callbacks));
    expectBytes(capture.input, capture.inputLength, "abc");
    memset(&capture, 0, sizeof(capture));
    assert(!LSBFakePTYInput(state, &discipline, "drop", 4, &callbacks));
    assert(!LSBFakePTYFlush(state, TCIFLUSH));
    assert(!LSBFakePTYInput(state, &discipline, "new\n", 4, &callbacks));
    expectBytes(capture.input, capture.inputLength, "new\n");

    // Match Fish's editor termios: no canonical mode or echo, byte reads, no CR translation, suspended/quit controls disabled.
    attributes = initial;
    attributes.c_lflag &= ~(ICANON | ECHO | IEXTEN);
    attributes.c_iflag &= ~(ICRNL | INLCR);
    attributes.c_cc[VSUSP] = attributes.c_cc[VQUIT] = _POSIX_VDISABLE;
    assert(!LSBPTYSetAttributes(duplicate, TCSANOW, &attributes));
    memset(&capture, 0, sizeof(capture));
    const char editor[] = "echo abc\033[D\177z\r\004";
    assert(!LSBFakePTYInput(state, &discipline, editor, sizeof(editor) - 1, &callbacks));
    expectBytes(capture.input, capture.inputLength, editor);
    assert(!capture.outputLength);
    assert(!LSBFakePTYOutput(state, "one\ntwo\r\n", 9, &callbacks));
    expectBytes(capture.output, capture.outputLength, "one\r\ntwo\r\r\n");
    attributes.c_oflag = 0;
    assert(!LSBPTYSetAttributes(duplicate, TCSANOW, &attributes));
    memset(&capture, 0, sizeof(capture));
    assert(!LSBFakePTYOutput(state, "one\n", 4, &callbacks));
    expectBytes(capture.output, capture.outputLength, "one\n");

    attributes.c_cc[VMIN] = 0;
    assert(LSBPTYSetAttributes(duplicate, TCSANOW, &attributes) == -1 && errno == ENOTSUP);
    attributes.c_cc[VMIN] = 1;
    attributes.c_iflag |= IXON;
    assert(LSBPTYSetAttributes(duplicate, TCSANOW, &attributes) == -1 && errno == ENOTSUP);
    attributes = initial;
    attributes.c_cflag = (attributes.c_cflag & ~CSIZE) | CS7;
    assert(LSBPTYSetAttributes(duplicate, TCSANOW, &attributes) == -1 && errno == ENOTSUP);
    attributes = initial;
    drainError = EPIPE;
    assert(LSBPTYSetAttributes(duplicate, TCSADRAIN, &attributes) == -1 && errno == EPIPE);
    drainError = 0;
    assert(!LSBPTYSetAttributes(duplicate, TCSADRAIN, &attributes));
    assert(LSBPTYSetAttributes(duplicate, -1, &attributes) == -1 && errno == EINVAL);
    assert(LSBPTYIOCtl(duplicate, TIOCSETAF, &attributes) == -1 && errno == ENOTSUP);
    assert(!LSBPTYIOCtl(duplicate, TIOCSETA, &attributes));
    assert(!LSBPTYIOCtl(duplicate, TIOCGETA, &attributes) && (attributes.c_lflag & ICANON));
    atomic_store(&state->lock, 1);
    assert(LSBPTYGetAttributes(duplicate, &attributes) == -1 && errno == EBUSY);
    atomic_store(&state->lock, 0);

    // Shared terminal changes and descriptor identity survive a real process boundary.
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(LSBPTYIsATTY(duplicate) == 1);
        assert(!LSBPTYSetForeground(duplicate, getpid()));
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(LSBPTYGetForeground(duplicate) == child);

    int other[2];
    assert(!pipe(other));
    assert(dup2(other[0], duplicate) == duplicate);
    assert(!LSBFakePTYMatchesFD(state, duplicate));
    assert(!LSBPTYIsATTY(duplicate) && errno == ENOTTY);
    close(other[0]); close(other[1]); close(duplicate); close(sockets[0]); close(sockets[1]);
    munmap(state, LSB_FAKE_PTY_MAP_SIZE);
    puts("FakePTY tests passed");
    return 0;
}
