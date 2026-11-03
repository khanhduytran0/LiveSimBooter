#include "FakePTY.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LSB_FAKE_PTY_MAGIC UINT64_C(0x4c53425054593031)
_Static_assert(sizeof(LSBFakePTYState) <= LSB_FAKE_PTY_MAP_SIZE, "Terminal state exceeds its mapping");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Shared terminal counters must be lock-free");

static int acquire(LSBFakePTYState *state) {
    // Critical sections only copy fixed records. Bound retries in case a guest dies while modifying its terminal.
    for (unsigned attempt = 0; attempt < 4096; ++attempt) {
        uint32_t expected = 0;
        if (atomic_compare_exchange_weak_explicit(&state->lock, &expected, 1, memory_order_acquire, memory_order_relaxed)) return 0;
        if (!(attempt & 63)) sched_yield();
    }
    return EBUSY;
}

static void release(LSBFakePTYState *state) { atomic_store_explicit(&state->lock, 0, memory_order_release); }

int LSBFakePTYInitialize(LSBFakePTYState *state, int slaveFD, unsigned short rows, unsigned short columns) {
    if (!state || !rows || !columns) return EINVAL;
    struct stat metadata;
    if (fstat(slaveFD, &metadata)) return errno;
    if (!S_ISSOCK(metadata.st_mode)) return ENOTSOCK;
    memset(state, 0, sizeof(*state));
    state->magic = LSB_FAKE_PTY_MAGIC;
    state->version = 2;
    state->structureSize = sizeof(*state);
    state->device = metadata.st_dev;
    state->inode = metadata.st_ino;
    atomic_init(&state->lock, 0);
    atomic_init(&state->inputFlush, 0);
    atomic_init(&state->foreground, 0);
    atomic_init(&state->stopBackgroundOutput, 0);
    struct termios *attributes = &state->attributes;
    attributes->c_iflag = ICRNL;
    attributes->c_oflag = OPOST | ONLCR;
    attributes->c_cflag = CREAD | CS8 | CLOCAL;
    attributes->c_lflag = ICANON | ISIG | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN;
    memset(attributes->c_cc, _POSIX_VDISABLE, sizeof(attributes->c_cc));
    attributes->c_cc[VINTR] = 3;
    attributes->c_cc[VQUIT] = 28;
    attributes->c_cc[VERASE] = 127;
    attributes->c_cc[VKILL] = 21;
    attributes->c_cc[VEOF] = 4;
    attributes->c_cc[VSUSP] = 26;
    attributes->c_cc[VWERASE] = 23;
    attributes->c_cc[VLNEXT] = 22;
    attributes->c_cc[VREPRINT] = 18;
    attributes->c_cc[VDISCARD] = 15;
    attributes->c_cc[VMIN] = 1;
    attributes->c_cc[VTIME] = 0;
    cfsetispeed(attributes, B38400);
    cfsetospeed(attributes, B38400);
    state->window.ws_row = rows;
    state->window.ws_col = columns;
    return 0;
}

int LSBFakePTYValidate(const LSBFakePTYState *state, size_t mappedBytes) {
    if (!state || mappedBytes < sizeof(*state)) return EINVAL;
    if (state->magic != LSB_FAKE_PTY_MAGIC || state->version != 2 || state->structureSize != sizeof(*state)) return EPROTO;
    return 0;
}

bool LSBFakePTYMatchesFD(const LSBFakePTYState *state, int fd) {
    if (!state) return false;
    int saved = errno;
    struct stat metadata;
    bool matches = !fstat(fd, &metadata) && S_ISSOCK(metadata.st_mode) && metadata.st_dev == state->device && metadata.st_ino == state->inode;
    errno = saved;
    return matches;
}

int LSBFakePTYGetAttributes(LSBFakePTYState *state, struct termios *attributes) {
    if (!state || !attributes) return EINVAL;
    int error = acquire(state);
    if (error) return error;
    *attributes = state->attributes;
    release(state);
    return 0;
}

static int validateAttributes(const struct termios *attributes) {
    tcflag_t input = IGNBRK | BRKINT | IGNPAR | ICRNL | INLCR | IGNCR;
    tcflag_t output = OPOST | ONLCR;
    tcflag_t control = CREAD | CS8 | CLOCAL | HUPCL;
    tcflag_t local = ICANON | ISIG | ECHO | ECHOE | ECHOK | ECHONL | ECHOCTL | IEXTEN | NOFLSH | TOSTOP;
    if ((attributes->c_iflag & ~input) || (attributes->c_oflag & ~output) || (attributes->c_cflag & ~control) ||
        (attributes->c_lflag & ~local) || (attributes->c_cflag & CSIZE) != CS8) return ENOTSUP;
    if (!(attributes->c_lflag & ICANON) && (attributes->c_cc[VMIN] != 1 || attributes->c_cc[VTIME])) return ENOTSUP;
    return 0;
}

int LSBFakePTYSetAttributes(LSBFakePTYState *state, const struct termios *attributes) {
    if (!state || !attributes) return EINVAL;
    int error = validateAttributes(attributes);
    if (!error) error = acquire(state);
    if (error) return error;
    state->attributes = *attributes;
    atomic_store_explicit(&state->stopBackgroundOutput, !!(attributes->c_lflag & TOSTOP), memory_order_release);
    release(state);
    return 0;
}

int LSBFakePTYGetWindow(LSBFakePTYState *state, struct winsize *window) {
    if (!state || !window) return EINVAL;
    int error = acquire(state);
    if (error) return error;
    *window = state->window;
    release(state);
    return 0;
}

int LSBFakePTYSetWindow(LSBFakePTYState *state, const struct winsize *window) {
    if (!state || !window || !window->ws_row || !window->ws_col) return EINVAL;
    int error = acquire(state);
    if (error) return error;
    state->window = *window;
    release(state);
    return 0;
}

int LSBFakePTYGetForeground(LSBFakePTYState *state, pid_t *foreground) {
    if (!state || !foreground) return EINVAL;
    *foreground = atomic_load_explicit(&state->foreground, memory_order_acquire);
    return 0;
}

int LSBFakePTYSetForeground(LSBFakePTYState *state, pid_t foreground) {
    if (!state || foreground <= 0) return EINVAL;
    atomic_store_explicit(&state->foreground, foreground, memory_order_release);
    return 0;
}

bool LSBFakePTYRequiresForeground(const LSBFakePTYState *state, pid_t group, bool checkTOSTOP) {
    if (!state || group <= 0 || (checkTOSTOP && !atomic_load_explicit(&state->stopBackgroundOutput, memory_order_acquire))) return false;
    pid_t foreground = atomic_load_explicit(&state->foreground, memory_order_acquire);
    return foreground > 0 && foreground != group;
}

int LSBFakePTYFlush(LSBFakePTYState *state, int selector) {
    if (!state || (selector != TCIFLUSH && selector != TCOFLUSH && selector != TCIOFLUSH)) return EINVAL;
    if (selector != TCIFLUSH) return ENOTSUP;
    atomic_fetch_add_explicit(&state->inputFlush, 1, memory_order_release);
    return 0;
}

static int output(const struct termios *attributes, const void *bytes, size_t length, const LSBFakePTYCallbacks *callbacks) {
    if (!length) return 0;
    if (!callbacks->output) return EINVAL;
    if ((attributes->c_oflag & (OPOST | ONLCR)) != (OPOST | ONLCR)) return callbacks->output(callbacks->context, bytes, length);
    const unsigned char *input = bytes;
    size_t begin = 0;
    for (size_t i = 0; i < length; ++i) {
        if (input[i] != '\n') continue;
        int error = i > begin ? callbacks->output(callbacks->context, input + begin, i - begin) : 0;
        if (!error) error = callbacks->output(callbacks->context, "\r\n", 2);
        if (error) return error;
        begin = i + 1;
    }
    return begin < length ? callbacks->output(callbacks->context, input + begin, length - begin) : 0;
}

int LSBFakePTYOutput(LSBFakePTYState *state, const void *bytes, size_t length, const LSBFakePTYCallbacks *callbacks) {
    if ((!bytes && length) || !callbacks) return EINVAL;
    struct termios attributes;
    int error = LSBFakePTYGetAttributes(state, &attributes);
    return error ? error : output(&attributes, bytes, length, callbacks);
}

static bool controlCharacter(const struct termios *attributes, unsigned index, unsigned char value) {
    return attributes->c_cc[index] != _POSIX_VDISABLE && attributes->c_cc[index] == value;
}

static int echo(const struct termios *attributes, unsigned char value, const LSBFakePTYCallbacks *callbacks) {
    if (!(attributes->c_lflag & ECHO) && !(value == '\n' && (attributes->c_lflag & ECHONL))) return 0;
    if ((attributes->c_lflag & ECHOCTL) && ((value < 32 && value != '\n' && value != '\t') || value == 127)) {
        unsigned char caret[2] = {'^', (unsigned char)(value == 127 ? '?' : value + 64)};
        return output(attributes, caret, sizeof(caret), callbacks);
    }
    return output(attributes, &value, 1, callbacks);
}

static int eraseLast(LSBFakePTYDiscipline *discipline, const struct termios *attributes, const LSBFakePTYCallbacks *callbacks) {
    if (!discipline->length) return 0;
    size_t offset = discipline->length - 1;
    while (offset && (discipline->line[offset] & 0xc0) == 0x80) --offset;
    unsigned char value = discipline->line[offset];
    discipline->length = offset;
    if (!(attributes->c_lflag & ECHO)) return 0;
    if (!(attributes->c_lflag & ECHOE)) return echo(attributes, attributes->c_cc[VERASE], callbacks);
    int error = output(attributes, "\b \b", 3, callbacks);
    if (!error && (attributes->c_lflag & ECHOCTL) && (value < 32 || value == 127) && value != '\t')
        error = output(attributes, "\b \b", 3, callbacks);
    return error;
}

int LSBFakePTYInput(LSBFakePTYState *state, LSBFakePTYDiscipline *discipline, const void *bytes, size_t length,
                    const LSBFakePTYCallbacks *callbacks) {
    if (!state || !discipline || (!bytes && length) || !callbacks || !callbacks->input) return EINVAL;
    struct termios attributes;
    int error = LSBFakePTYGetAttributes(state, &attributes);
    if (error) return error;
    uint32_t flushGeneration = atomic_load_explicit(&state->inputFlush, memory_order_acquire);
    if (flushGeneration != discipline->flushGeneration) {
        discipline->length = 0;
        discipline->literalNext = false;
        discipline->flushGeneration = flushGeneration;
    }
    const unsigned char *input = bytes;
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = input[i];
        if (value == '\r') {
            if (attributes.c_iflag & IGNCR) continue;
            if (attributes.c_iflag & ICRNL) value = '\n';
        } else if (value == '\n' && (attributes.c_iflag & INLCR)) value = '\r';
        bool literal = discipline->literalNext;
        discipline->literalNext = false;
        if (!literal && (attributes.c_lflag & ISIG)) {
            int number = 0;
            if (controlCharacter(&attributes, VINTR, value)) number = SIGINT;
            else if (controlCharacter(&attributes, VQUIT, value)) number = SIGQUIT;
            else if (controlCharacter(&attributes, VSUSP, value)) number = SIGTSTP;
            if (number) {
                pid_t foreground;
                error = LSBFakePTYGetForeground(state, &foreground);
                if (!error) error = echo(&attributes, value, callbacks);
                if (!error) error = callbacks->signal ? callbacks->signal(callbacks->context, foreground, number) : ENOTSUP;
                if (error) return error;
                if (!(attributes.c_lflag & NOFLSH)) {
                    discipline->length = 0;
                    LSBFakePTYFlush(state, TCIFLUSH);
                    discipline->flushGeneration = atomic_load_explicit(&state->inputFlush, memory_order_acquire);
                }
                continue;
            }
        }
        if (attributes.c_lflag & ICANON) {
            if (!literal && (attributes.c_lflag & IEXTEN)) {
                if (controlCharacter(&attributes, VLNEXT, value)) { discipline->literalNext = true; continue; }
                if (controlCharacter(&attributes, VDISCARD, value)) return ENOTSUP;
                if (controlCharacter(&attributes, VREPRINT, value)) {
                    error = echo(&attributes, value, callbacks);
                    if (!error) error = output(&attributes, "\n", 1, callbacks);
                    if (!error) error = output(&attributes, discipline->line, discipline->length, callbacks);
                    if (error) return error;
                    continue;
                }
                if (controlCharacter(&attributes, VWERASE, value)) {
                    while (discipline->length && (discipline->line[discipline->length - 1] == ' ' || discipline->line[discipline->length - 1] == '\t'))
                        if ((error = eraseLast(discipline, &attributes, callbacks))) return error;
                    while (discipline->length && discipline->line[discipline->length - 1] != ' ' && discipline->line[discipline->length - 1] != '\t')
                        if ((error = eraseLast(discipline, &attributes, callbacks))) return error;
                    continue;
                }
            }
            if (!literal && controlCharacter(&attributes, VERASE, value)) {
                if ((error = eraseLast(discipline, &attributes, callbacks))) return error;
                continue;
            }
            if (!literal && controlCharacter(&attributes, VKILL, value)) {
                discipline->length = 0;
                error = echo(&attributes, value, callbacks);
                if (!error && (attributes.c_lflag & (ECHO | ECHOK)) == (ECHO | ECHOK)) error = output(&attributes, "\n", 1, callbacks);
                if (error) return error;
                continue;
            }
            if (!literal && controlCharacter(&attributes, VEOF, value)) {
                // A byte stream cannot encode one zero-length read without ending the session. Do not turn Ctrl-D into permanent EOF.
                if (!discipline->length) return ENOTSUP;
                error = callbacks->input(callbacks->context, discipline->line, discipline->length);
                if (error) return error;
                discipline->length = 0;
                continue;
            }
            if (discipline->length == sizeof(discipline->line)) return ENOBUFS;
            discipline->line[discipline->length++] = value;
            if ((error = echo(&attributes, value, callbacks))) return error;
            if (!literal && (value == '\n' || controlCharacter(&attributes, VEOL, value) || controlCharacter(&attributes, VEOL2, value))) {
                error = callbacks->input(callbacks->context, discipline->line, discipline->length);
                if (error) return error;
                discipline->length = 0;
            }
        } else {
            if (discipline->length) {
                error = callbacks->input(callbacks->context, discipline->line, discipline->length);
                if (error) return error;
                discipline->length = 0;
            }
            if ((error = echo(&attributes, value, callbacks))) return error;
            if ((error = callbacks->input(callbacks->context, &value, 1))) return error;
        }
    }
    return 0;
}
