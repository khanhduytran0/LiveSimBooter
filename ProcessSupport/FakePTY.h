#ifndef LSB_FAKE_PTY_H
#define LSB_FAKE_PTY_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>

#define LSB_FAKE_PTY_ENV "LSB_FAKE_PTY_STATE"
#define LSB_FAKE_PTY_MAP_SIZE 16384
#define LSB_FAKE_PTY_LINE_BYTES 4096

// One shared terminal state belongs to one socketpair. Socket identity follows dup/fileport inheritance without tracking descriptor numbers.
typedef struct {
    uint64_t magic;
    uint32_t version, structureSize;
    int64_t device;
    uint64_t inode;
    _Atomic(uint32_t) lock;
    _Atomic(uint32_t) inputFlush;
    struct termios attributes;
    struct winsize window;
    _Atomic(pid_t) foreground;
    _Atomic(uint32_t) stopBackgroundOutput;
} LSBFakePTYState;

typedef struct {
    unsigned char line[LSB_FAKE_PTY_LINE_BYTES];
    size_t length;
    uint32_t flushGeneration;
    bool literalNext;
} LSBFakePTYDiscipline;

typedef struct {
    void *context;
    int (*input)(void *context, const void *bytes, size_t length);
    int (*output)(void *context, const void *bytes, size_t length);
    int (*signal)(void *context, pid_t foreground, int signalNumber);
} LSBFakePTYCallbacks;

// Functions return errno values. The host initializes a new, zero-filled shared mapping before starting any guest.
int LSBFakePTYInitialize(LSBFakePTYState *state, int slaveFD, unsigned short rows, unsigned short columns);
int LSBFakePTYValidate(const LSBFakePTYState *state, size_t mappedBytes);
bool LSBFakePTYMatchesFD(const LSBFakePTYState *state, int fd);
int LSBFakePTYGetAttributes(LSBFakePTYState *state, struct termios *attributes);
int LSBFakePTYSetAttributes(LSBFakePTYState *state, const struct termios *attributes);
int LSBFakePTYGetWindow(LSBFakePTYState *state, struct winsize *window);
int LSBFakePTYSetWindow(LSBFakePTYState *state, const struct winsize *window);
int LSBFakePTYGetForeground(LSBFakePTYState *state, pid_t *foreground);
int LSBFakePTYSetForeground(LSBFakePTYState *state, pid_t foreground);
bool LSBFakePTYRequiresForeground(const LSBFakePTYState *state, pid_t group, bool checkTOSTOP);
int LSBFakePTYFlush(LSBFakePTYState *state, int selector);

// The host serializes calls per session. Real socket I/O supplies readiness, backpressure and descriptor inheritance.
// Input implements canonical editing/echo, CR conversion and signal characters; output implements OPOST/ONLCR.
// Canonical empty-line EOF, software flow control, VDISCARD, timed/minimum-length reads and guest flush are unsupported.
int LSBFakePTYInput(LSBFakePTYState *state, LSBFakePTYDiscipline *discipline, const void *bytes, size_t length,
                    const LSBFakePTYCallbacks *callbacks);
int LSBFakePTYOutput(LSBFakePTYState *state, const void *bytes, size_t length, const LSBFakePTYCallbacks *callbacks);

#endif
