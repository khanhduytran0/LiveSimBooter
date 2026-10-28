#import "TerminalSession.h"
#include "FakePTY.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

NSString *const LSBTerminalSessionDidCloseNotification = @"LSBTerminalSessionDidClose";
static const NSUInteger LSBTerminalInputCapacity = 1048576;

@interface LSBTerminalSession () {
    int _master, _slave, _stateFD, _consoleFD;
    LSBFakePTYState *_state;
    LSBFakePTYDiscipline _discipline;
    dispatch_source_t _reader, _writer, _consoleReader;
    dispatch_group_t _masterRegistrations;
    NSMutableData *_pendingInput;
    NSMutableData *_unclaimedOutput;
    NSString *_inputPath;
    BOOL _started, _closed, _mirrorConsole;
    uint32_t _inputFlush;
}
@property (nonatomic, readwrite) NSString *statePath;
- (int)queueInput:(const void *)bytes length:(size_t)length;
- (void)drainInput;
- (void)emitOutput:(const void *)bytes length:(size_t)length;
@end

static int terminalInput(void *context, const void *bytes, size_t length) {
    return [(__bridge LSBTerminalSession *)context queueInput:bytes length:length];
}
static int terminalOutput(void *context, const void *bytes, size_t length) {
    [(__bridge LSBTerminalSession *)context emitOutput:bytes length:length];
    return 0;
}
static int terminalSignal(void *context, pid_t group, int number) {
    LSBTerminalSession *session = (__bridge LSBTerminalSession *)context;
    return session.signalHandler ? session.signalHandler(group, number) : ESRCH;
}

@implementation LSBTerminalSession
- (instancetype)initWithDirectory:(NSString *)directory error:(int *)error {
    if (!(self = [super init])) { *error = ENOMEM; return nil; }
    _master = _slave = _stateFD = _consoleFD = -1;
    const char *consoleOutput = getenv("LSB_TERMINAL_CONSOLE_OUTPUT");
    _mirrorConsole = consoleOutput && !strcmp(consoleOutput, "1");
    _pendingInput = [NSMutableData new];
    _unclaimedOutput = [NSMutableData new];
    _masterRegistrations = dispatch_group_create();
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/.terminal.XXXXXX", directory.fileSystemRepresentation);
    *error = length < 0 || (size_t)length >= sizeof(path) ? ENAMETOOLONG : 0;
    if (!*error && (_stateFD = mkstemp(path)) < 0) *error = errno;
    if (!*error) self.statePath = [NSString stringWithUTF8String:path];
    if (!*error && (fcntl(_stateFD, F_SETFD, FD_CLOEXEC) || ftruncate(_stateFD, LSB_FAKE_PTY_MAP_SIZE))) *error = errno;
    if (!*error) {
        void *mapping = mmap(NULL, LSB_FAKE_PTY_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, _stateFD, 0);
        if (mapping == MAP_FAILED) *error = errno;
        else _state = mapping;
    }
    int sockets[2];
    if (!*error && socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) *error = errno;
    else if (!*error) {
        _master = sockets[0]; _slave = sockets[1];
        int enabled = 1;
        for (unsigned i = 0; i < 2 && !*error; ++i) {
            if (fcntl(sockets[i], F_SETFD, FD_CLOEXEC) || setsockopt(sockets[i], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
                *error = errno;
        }
        if (!*error && fcntl(_master, F_SETFL, O_NONBLOCK)) *error = errno;
    }
    if (!*error) *error = LSBFakePTYInitialize(_state, _slave, 24, 80);
    if (*error) { [self close]; return nil; }
    return self;
}

- (int)attachToMessage:(LSBReceivedMessage *)message {
    if (_slave < 0 || message->plan.fdCount) return EINVAL;
    char environment[PATH_MAX + 32];
    int length = snprintf(environment, sizeof(environment), LSB_FAKE_PTY_ENV "=%s", self.statePath.fileSystemRepresentation);
    if (length < 0 || (size_t)length >= sizeof(environment)) return ENAMETOOLONG;
    LSBSpawnPlan *plan = &message->plan;
    if (plan->envc == LSB_MAX_VECTOR) return E2BIG;
    int error = LSBPlanAddString(plan, environment, &plan->envp[plan->envc]);
    if (error) return error;
    ++plan->envc;
    for (int descriptor = 0; descriptor < 3; ++descriptor) {
        int duplicate = fcntl(_slave, F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0) return errno;
        message->fds[plan->fdCount] = duplicate;
        plan->fds[plan->fdCount++] = (LSBInheritedFD){descriptor, 0};
    }
    close(_slave); _slave = -1;
    return 0;
}

- (BOOL)isClosed { return _closed; }

- (int)attachToMessageReplacingStandardIO:(LSBReceivedMessage *)message {
    if (_closed || _slave < 0 || !message) return EINVAL;
    int error = LSBValidateSpawnPlan(&message->plan);
    if (error) return error;
    if (message->plan.fdCount == LSB_MAX_FDS || message->plan.actionCount > LSB_MAX_ACTIONS - 4) return E2BIG;
    // No existing descriptor/action may name the staging slot. All original operations run before the final stdio override.
    int highest = STDERR_FILENO;
    for (unsigned i = 0; i < message->plan.fdCount; ++i) highest = MAX(highest, message->plan.fds[i].number);
    for (unsigned i = 0; i < message->plan.actionCount; ++i) {
        const LSBFileAction *action = &message->plan.actions[i];
        if (action->kind != LSB_ACTION_CHDIR) highest = MAX(highest, action->fd);
        if (action->kind == LSB_ACTION_DUP2) highest = MAX(highest, action->target);
    }
    int staging = highest + 1;
    if (staging >= getdtablesize() - (int)message->plan.fdCount - 1) return EMFILE;
    LSBSpawnPlan *plan = malloc(sizeof(*plan));
    if (!plan) return ENOMEM;
    memcpy(plan, &message->plan, sizeof(*plan));
    // Replace every inherited spelling; the job cannot select another terminal state file through its environment.
    unsigned count = 0;
    for (unsigned i = 0; i < plan->envc; ++i)
        if (strncmp(plan->strings + plan->envp[i], LSB_FAKE_PTY_ENV "=", sizeof(LSB_FAKE_PTY_ENV))) plan->envp[count++] = plan->envp[i];
    plan->envc = count;
    char environment[PATH_MAX + 32];
    int length = snprintf(environment, sizeof(environment), LSB_FAKE_PTY_ENV "=%s", self.statePath.fileSystemRepresentation);
    if (length < 0 || (size_t)length >= sizeof(environment)) error = ENAMETOOLONG;
    if (!error && plan->envc == LSB_MAX_VECTOR) error = E2BIG;
    if (!error) error = LSBPlanAddString(plan, environment, &plan->envp[plan->envc]);
    if (!error) {
        ++plan->envc;
        plan->fds[plan->fdCount++] = (LSBInheritedFD){staging, FD_CLOEXEC};
        for (int target = STDIN_FILENO; target <= STDERR_FILENO; ++target)
            plan->actions[plan->actionCount++] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = staging, .target = target};
        plan->actions[plan->actionCount++] = (LSBFileAction){.kind = LSB_ACTION_CLOSE, .fd = staging};
        error = LSBValidateSpawnPlan(plan);
    }
    int duplicate = error ? -1 : fcntl(_slave, F_DUPFD_CLOEXEC, 3);
    if (!error && duplicate < 0) error = errno;
    if (!error) {
        message->fds[message->plan.fdCount] = duplicate;
        memcpy(&message->plan, plan, sizeof(*plan));
        close(_slave);
        _slave = -1;
    }
    free(plan);
    return error;
}

- (void)start {
    if (_started || _closed) return;
    _started = YES;
    __weak LSBTerminalSession *weakSelf = self;
    _reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, _master, 0, dispatch_get_main_queue());
    dispatch_group_t registrations = _masterRegistrations;
    dispatch_group_enter(registrations);
    dispatch_source_set_cancel_handler(_reader, ^{ dispatch_group_leave(registrations); });
    dispatch_source_set_event_handler(_reader, ^{
        LSBTerminalSession *session = weakSelf;
        if (!session || session->_closed) return;
        unsigned char buffer[8192];
        // A continuously writing guest must yield to keyboard, signal and broker events on this same queue.
        for (unsigned reads = 0; reads < 8; ++reads) {
            ssize_t count = read(session->_master, buffer, sizeof(buffer));
            if (count > 0) {
                LSBFakePTYCallbacks callbacks = {(__bridge void *)session, terminalInput, terminalOutput, terminalSignal};
                int error = LSBFakePTYOutput(session->_state, buffer, (size_t)count, &callbacks);
                if (error) fprintf(stderr, "[terminal] output: %s\n", strerror(error));
            } else if (count < 0 && errno == EINTR) continue;
            else {
                if (!count || (errno != EAGAIN && errno != EWOULDBLOCK)) [session close];
                break;
            }
        }
    });
    dispatch_resume(_reader);
    // devicectl --console can supply a pipe or terminal on stdin. A regular file or /dev/null is not an input event source.
    struct stat metadata;
    const char *debugInput = getenv("LSB_TERMINAL_INPUT_FIFO");
    if (debugInput && !strcmp(debugInput, "1")) {
        // Opt-in device regression input. devicectl's console does not consistently forward stdin on iOS.
        _inputPath = [self.statePath stringByAppendingString:@".input"];
        if (!mkfifo(_inputPath.fileSystemRepresentation, 0600)) {
            _consoleFD = open(_inputPath.fileSystemRepresentation, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (_consoleFD >= 0) fprintf(stderr, "[terminal] input FIFO=%s\n", _inputPath.fileSystemRepresentation);
            else fprintf(stderr, "[terminal] open input FIFO %s: %s\n", _inputPath.fileSystemRepresentation, strerror(errno));
        } else fprintf(stderr, "[terminal] create input FIFO %s: %s\n", _inputPath.fileSystemRepresentation, strerror(errno));
    } else if (!fstat(STDIN_FILENO, &metadata) && (S_ISFIFO(metadata.st_mode) || isatty(STDIN_FILENO))) {
        _consoleFD = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    }
    if (_consoleFD >= 0) {
            _consoleReader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, _consoleFD, 0, dispatch_get_main_queue());
            int console = _consoleFD;
            dispatch_source_set_cancel_handler(_consoleReader, ^{ close(console); });
            dispatch_source_set_event_handler(_consoleReader, ^{
                LSBTerminalSession *session = weakSelf;
                if (!session || session->_closed) return;
                char bytes[4096];
                ssize_t count = read(session->_consoleFD, bytes, sizeof(bytes));
                if (count > 0) {
                    int error = [session sendInput:[NSData dataWithBytes:bytes length:(NSUInteger)count]];
                    if (error) fprintf(stderr, "[terminal] console input: %s\n", strerror(error));
                }
                else if (!count || errno != EINTR) {
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
                    dispatch_source_cancel(session->_consoleReader);
                    session->_consoleReader = nil;
                    session->_consoleFD = -1;
                    // EOF on the console does not hang up the on-screen terminal.
                }
            });
            dispatch_resume(_consoleReader);
    }
}

- (void)drainInput {
    if (_closed) return;
    uint32_t generation = atomic_load_explicit(&_state->inputFlush, memory_order_acquire);
    if (generation != _inputFlush) {
        _inputFlush = generation;
        [_pendingInput setLength:0];
    }
    size_t budget = 65536;
    unsigned attempts = 8;
    while (_pendingInput.length && !_closed && budget && attempts--) {
        ssize_t count = write(_master, _pendingInput.bytes, MIN(_pendingInput.length, budget));
        if (count > 0) {
            budget -= (size_t)count;
            [_pendingInput replaceBytesInRange:NSMakeRange(0, (NSUInteger)count) withBytes:NULL length:0];
        }
        else if (count < 0 && errno == EINTR) continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { [self close]; return; }
    }
    if (!_pendingInput.length && _writer) { dispatch_source_cancel(_writer); _writer = nil; }
}
- (int)queueInput:(const void *)bytes length:(size_t)length {
    if (_closed) return EIO;
    [self drainInput];
    if (_closed) return EIO;
    if (length > LSBTerminalInputCapacity - _pendingInput.length) return ENOBUFS;
    [_pendingInput appendBytes:bytes length:length];
    [self drainInput];
    if (_closed) return EIO;
    if (_pendingInput.length && !_writer) {
        __weak LSBTerminalSession *weakSelf = self;
        _writer = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, _master, 0, dispatch_get_main_queue());
        dispatch_group_t registrations = _masterRegistrations;
        dispatch_group_enter(registrations);
        dispatch_source_set_cancel_handler(_writer, ^{ dispatch_group_leave(registrations); });
        dispatch_source_set_event_handler(_writer, ^{ [weakSelf drainInput]; });
        dispatch_resume(_writer);
    }
    return 0;
}
- (void)emitOutput:(const void *)bytes length:(size_t)length {
    if (_outputHandler) _outputHandler([NSData dataWithBytes:bytes length:length]);
    else {
        // Preserve startup/EOF output until the UI binds, with a fixed backlog cap for a disconnected or recreated scene.
        const NSUInteger capacity = 256 * 1024;
        if (length >= capacity) [_unclaimedOutput setData:[NSData dataWithBytes:(const char *)bytes + length - capacity length:capacity]];
        else {
            NSUInteger excess = _unclaimedOutput.length + length > capacity ? _unclaimedOutput.length + length - capacity : 0;
            if (excess) [_unclaimedOutput replaceBytesInRange:NSMakeRange(0, excess) withBytes:NULL length:0];
            [_unclaimedOutput appendBytes:bytes length:length];
        }
    }
    // Console backpressure can block UIKit. Mirror only when explicitly requested for device diagnostics.
    if (_mirrorConsole && length) { fwrite(bytes, 1, length, stdout); fflush(stdout); }
}
- (void)setOutputHandler:(void (^)(NSData *data))outputHandler {
    _outputHandler = [outputHandler copy];
    if (_outputHandler && _unclaimedOutput.length) {
        NSData *pending = [_unclaimedOutput copy];
        [_unclaimedOutput setLength:0];
        _outputHandler(pending);
    }
}
- (int)sendInput:(NSData *)data {
    if (_closed) return EIO;
    [self drainInput];
    if (_closed) return EIO;
    // Reserve space for the complete batch before the discipline emits any bytes, including bracketed-paste delimiters.
    // A canonical line may also be emitted after switching to raw mode. Flushed line bytes no longer consume this budget.
    NSUInteger lineBytes = _discipline.flushGeneration == _inputFlush ? _discipline.length : 0;
    NSUInteger available = LSBTerminalInputCapacity - _pendingInput.length;
    // Individual keys must still reach ISIG/line-editing handling when the queue is full (notably Ctrl-C's input flush).
    if (data.length > 1 && (lineBytes > available || data.length > available - lineBytes)) return ENOBUFS;
    LSBFakePTYCallbacks callbacks = {(__bridge void *)self, terminalInput, terminalOutput, terminalSignal};
    return LSBFakePTYInput(_state, &_discipline, data.bytes, data.length, &callbacks);
}
- (void)resizeRows:(unsigned short)rows columns:(unsigned short)columns {
    if (_closed || !rows || !columns) return;
    struct winsize previous, size = {.ws_row = rows, .ws_col = columns};
    if (LSBFakePTYGetWindow(_state, &previous) || !memcmp(&previous, &size, sizeof(size))) return;
    if (!LSBFakePTYSetWindow(_state, &size) && self.signalHandler) {
        pid_t group = 0;
        if (!LSBFakePTYGetForeground(_state, &group)) self.signalHandler(group, SIGWINCH);
    }
}
- (void)close {
    if (_closed) return;
    _closed = YES;
    // The kevent registrations must be cancelled before closing a watched descriptor, even on the same serial queue.
    if (_reader) dispatch_source_cancel(_reader);
    if (_writer) dispatch_source_cancel(_writer);
    if (_consoleReader) dispatch_source_cancel(_consoleReader);
    _reader = _writer = _consoleReader = nil;
    int master = _master;
    dispatch_group_notify(_masterRegistrations, dispatch_get_main_queue(), ^{
        if (master >= 0) close(master);
    });
    if (_slave >= 0) close(_slave);
    if (_stateFD >= 0) close(_stateFD);
    _master = _slave = _stateFD = _consoleFD = -1;
    if (_state) munmap(_state, LSB_FAKE_PTY_MAP_SIZE);
    _state = NULL;
    if (self.statePath) unlink(self.statePath.fileSystemRepresentation);
    if (_inputPath) unlink(_inputPath.fileSystemRepresentation);
    [NSNotificationCenter.defaultCenter postNotificationName:LSBTerminalSessionDidCloseNotification object:self];
}
- (void)dealloc { [self close]; }
@end
