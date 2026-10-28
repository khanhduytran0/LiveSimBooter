#import <Foundation/Foundation.h>
#import "../Host/TerminalSession.h"
#include "../ProcessSupport/FakePTY.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void pumpUntil(BOOL (^finished)(void)) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!finished() && deadline.timeIntervalSinceNow > 0) {
        [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    }
    assert(finished());
}

static void consoleMirroring(void) {
    char directory[] = "/tmp/LiveSimBooter-terminal-console.XXXXXX";
    assert(mkdtemp(directory));
    const char *existing = getenv("LSB_TERMINAL_CONSOLE_OUTPUT");
    char *original = existing ? strdup(existing) : NULL;
    const char *settings[] = {NULL, "0", "1", "true"};
    for (unsigned i = 0; i < sizeof(settings) / sizeof(*settings); ++i) {
        if (settings[i]) assert(!setenv("LSB_TERMINAL_CONSOLE_OUTPUT", settings[i], 1));
        else assert(!unsetenv("LSB_TERMINAL_CONSOLE_OUTPUT"));
        FILE *capture = tmpfile();
        assert(capture);
        fflush(stdout);
        int savedOutput = dup(STDOUT_FILENO);
        assert(savedOutput >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
        int error = 0;
        LSBTerminalSession *session = [[LSBTerminalSession alloc] initWithDirectory:[NSString stringWithUTF8String:directory] error:&error];
        assert(session && !error);
        LSBReceivedMessage message = {0};
        assert(![session attachToMessage:&message]);
        NSMutableData *output = [NSMutableData new];
        session.outputHandler = ^(NSData *data) { [output appendData:data]; };
        [session start];
        assert(write(message.fds[1], "console\n", 8) == 8);
        pumpUntil(^BOOL { return output.length == 9; });
        for (unsigned fd = 0; fd < message.plan.fdCount; ++fd) close(message.fds[fd]);
        pumpUntil(^BOOL { return session.closed; });
        fflush(stdout);
        assert(dup2(savedOutput, STDOUT_FILENO) >= 0);
        close(savedOutput);
        rewind(capture);
        char bytes[16];
        size_t size = fread(bytes, 1, sizeof(bytes), capture);
        assert(i == 2 ? size == 9 && !memcmp(bytes, "console\r\n", 9) : size == 0);
        assert([output isEqualToData:[@"console\r\n" dataUsingEncoding:NSUTF8StringEncoding]]);
        fclose(capture);
    }
    if (original) { assert(!setenv("LSB_TERMINAL_CONSOLE_OUTPUT", original, 1)); free(original); }
    else assert(!unsetenv("LSB_TERMINAL_CONSOLE_OUTPUT"));
    assert(!rmdir(directory));
}

static void replacingStandardIO(void) {
    char directory[] = "/tmp/LiveSimBooter-terminal-replace.XXXXXX";
    assert(mkdtemp(directory));
    int error = 0;
    LSBTerminalSession *session = [[LSBTerminalSession alloc] initWithDirectory:[NSString stringWithUTF8String:directory] error:&error];
    assert(session && !error && !session.closed);
    LSBReceivedMessage *message = calloc(1, sizeof(*message));
    assert(message);
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) message->fds[i] = -1;
    LSBSpawnPlan *plan = &message->plan;
    plan->version = LSB_PLAN_VERSION;
    plan->spawnFlags = POSIX_SPAWN_CLOEXEC_DEFAULT;
    assert(!LSBPlanAddString(plan, "/bin/fish", &plan->path));
    plan->argv[plan->argc++] = plan->path;
    assert(!LSBPlanAddString(plan, directory, &plan->cwd));
    assert(!LSBPlanAddString(plan, LSB_FAKE_PTY_ENV "=/wrong/first", &plan->envp[plan->envc++]));
    assert(!LSBPlanAddString(plan, "UNCHANGED=value", &plan->envp[plan->envc++]));
    assert(!LSBPlanAddString(plan, LSB_FAKE_PTY_ENV "=/wrong/second", &plan->envp[plan->envc++]));
    int originalDirectory = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(originalDirectory >= 0);
    plan->fds[plan->fdCount++] = (LSBInheritedFD){7, FD_CLOEXEC};
    message->fds[0] = originalDirectory;
    plan->actions[plan->actionCount++] = (LSBFileAction){.kind = LSB_ACTION_FCHDIR, .fd = 7};
    plan->actions[plan->actionCount++] = (LSBFileAction){.kind = LSB_ACTION_INHERIT, .fd = 7};
    uint32_t nullPath;
    assert(!LSBPlanAddString(plan, "/dev/null", &nullPath));
    plan->actions[plan->actionCount++] = (LSBFileAction){.kind = LSB_ACTION_OPEN, .fd = 1, .flags = O_WRONLY, .path = nullPath};
    plan->actions[plan->actionCount++] = (LSBFileAction){.kind = LSB_ACTION_DUP2, .fd = 1, .target = 2};
    assert(!LSBValidateSpawnPlan(plan));
    assert(![session attachToMessageReplacingStandardIO:message]);
    assert(plan->fdCount == 2 && plan->actionCount == 8 && plan->envc == 2 && message->fds[0] == originalDirectory);
    assert(!strcmp(plan->strings + plan->envp[0], "UNCHANGED=value"));
    NSString *expected = [@LSB_FAKE_PTY_ENV "=" stringByAppendingString:session.statePath];
    assert(!strcmp(plan->strings + plan->envp[1], expected.UTF8String));
    assert(plan->actions[4].kind == LSB_ACTION_DUP2 && plan->actions[4].fd == 8 && plan->actions[7].kind == LSB_ACTION_CLOSE);
    assert([session attachToMessageReplacingStandardIO:message] == EINVAL);
    int stateFD = open(session.statePath.fileSystemRepresentation, O_RDWR);
    assert(stateFD >= 0);
    LSBFakePTYState *state = mmap(NULL, LSB_FAKE_PTY_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, stateFD, 0);
    assert(state != MAP_FAILED);
    close(stateFD);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        if (LSBApplySpawnPlan(plan, message->fds)) _exit(81);
        for (int fd = 0; fd < 3; ++fd) if (!LSBFakePTYMatchesFD(state, fd)) _exit(82);
        if (fcntl(7, F_GETFD) < 0 || fcntl(8, F_GETFD) >= 0) _exit(83);
        if (write(1, "attached\n", 9) != 9 || write(2, "stderr\n", 7) != 7) _exit(84);
        const char failure[] = "terminal-fish: Fish replacement failed: Exec format error\n";
        if (write(2, failure, sizeof(failure) - 1) != (ssize_t)sizeof(failure) - 1) _exit(85);
        _exit(127);
    }
    for (unsigned i = 0; i < plan->fdCount; ++i) close(message->fds[i]);
    __block NSMutableData *output = [NSMutableData new];
    __block unsigned closedNotifications = 0;
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBTerminalSessionDidCloseNotification object:session queue:nil
        usingBlock:^(NSNotification *notification) { (void)notification; ++closedNotifications; }];
    [session start];
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 127);
    pumpUntil(^BOOL { return session.closed; });
    // A late/hidden UI still receives startup and final output even after the shell's endpoint reached EOF.
    assert(!output.length && closedNotifications == 1);
    session.outputHandler = ^(NSData *bytes) { [output appendData:bytes]; };
    assert([output isEqualToData:[@"attached\r\nstderr\r\nterminal-fish: Fish replacement failed: Exec format error\r\n"
        dataUsingEncoding:NSUTF8StringEncoding]]);
    session.outputHandler = nil;
    session.outputHandler = ^(NSData *bytes) { (void)bytes; assert(false && "Backlog must be delivered exactly once"); };
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    munmap(state, LSB_FAKE_PTY_MAP_SIZE);
    free(message);
    assert(!rmdir(directory));
}

static void replacementFailureIsAtomic(void) {
    char directory[] = "/tmp/LiveSimBooter-terminal-atomic.XXXXXX";
    assert(mkdtemp(directory));
    int error = 0;
    LSBTerminalSession *session = [[LSBTerminalSession alloc] initWithDirectory:[NSString stringWithUTF8String:directory] error:&error];
    assert(session && !error);
    LSBReceivedMessage *message = calloc(1, sizeof(*message)), *before = malloc(sizeof(*before));
    assert(message && before);
    message->plan.version = LSB_PLAN_VERSION;
    assert(!LSBPlanAddString(&message->plan, "/bin/fish", &message->plan.path));
    message->plan.argv[message->plan.argc++] = message->plan.path;
    assert(!LSBPlanAddString(&message->plan, directory, &message->plan.cwd));
    message->plan.actionCount = LSB_MAX_ACTIONS - 3;
    for (unsigned i = 0; i < message->plan.actionCount; ++i) message->plan.actions[i] = (LSBFileAction){.kind = LSB_ACTION_INHERIT, .fd = 0};
    memcpy(before, message, sizeof(*message));
    assert([session attachToMessageReplacingStandardIO:message] == E2BIG && !memcmp(before, message, sizeof(*message)));
    message->plan.actionCount = 0;
    assert(![session attachToMessageReplacingStandardIO:message]); // Failure did not consume the pending slave.
    close(message->fds[0]);
    [session close];
    free(message); free(before);
    assert(!rmdir(directory));
}

int main(void) {
    @autoreleasepool {
        consoleMirroring();
        replacingStandardIO();
        replacementFailureIsAtomic();
        char directory[] = "/tmp/LiveSimBooter-terminal-session.XXXXXX";
        assert(mkdtemp(directory));
        int error = 0;
        LSBTerminalSession *session = [[LSBTerminalSession alloc] initWithDirectory:[NSString stringWithUTF8String:directory] error:&error];
        assert(session && !error);
        NSString *statePath = session.statePath;
        LSBReceivedMessage message = {0};
        for (unsigned index = 0; index < LSB_MAX_FDS; ++index) message.fds[index] = -1;
        assert(![session attachToMessage:&message] && message.plan.fdCount == 3);
        int stateFD = open(statePath.fileSystemRepresentation, O_RDWR);
        assert(stateFD >= 0);
        LSBFakePTYState *state = mmap(NULL, LSB_FAKE_PTY_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, stateFD, 0);
        assert(state != MAP_FAILED && !LSBFakePTYValidate(state, LSB_FAKE_PTY_MAP_SIZE));
        close(stateFD);
        assert(LSBFakePTYMatchesFD(state, message.fds[0]) && LSBFakePTYMatchesFD(state, message.fds[2]));
        __block NSMutableData *output = [NSMutableData new];
        __block int signalNumber;
        session.outputHandler = ^(NSData *data) { [output appendData:data]; };
        session.signalHandler = ^int(pid_t group, int number) { (void)group; signalNumber = number; return 0; };
        [session start];
        assert(write(message.fds[1], "session\n", 8) == 8);
        pumpUntil(^BOOL { return output.length >= 9; });
        assert([output isEqualToData:[@"session\r\n" dataUsingEncoding:NSUTF8StringEncoding]]);
        struct termios attributes;
        assert(!LSBFakePTYGetAttributes(state, &attributes));
        cfmakeraw(&attributes);
        assert(!LSBFakePTYSetAttributes(state, &attributes));
        NSData *input = [@"abc\033[D\r" dataUsingEncoding:NSUTF8StringEncoding];
        assert(![session sendInput:input]);
        char bytes[64];
        ssize_t count = read(message.fds[0], bytes, sizeof(bytes));
        assert(count == (ssize_t)input.length && !memcmp(bytes, input.bytes, input.length));
        [session resizeRows:37 columns:101];
        struct winsize size;
        assert(!LSBFakePTYGetWindow(state, &size) && size.ws_row == 37 && size.ws_col == 101 && signalNumber == SIGWINCH);
        attributes.c_lflag |= ISIG;
        assert(!LSBFakePTYSetAttributes(state, &attributes));
        assert(![session sendInput:[NSData dataWithBytes:"\003" length:1]] && signalNumber == SIGINT);

        // Keep the socket readable from each output callback. Main-queue work must run before this stream ends.
        int savedOutput = dup(STDOUT_FILENO), nullOutput = open("/dev/null", O_WRONLY);
        assert(savedOutput >= 0 && nullOutput >= 0 && dup2(nullOutput, STDOUT_FILENO) >= 0);
        close(nullOutput);
        __block unsigned callbacks = 0, callbacksBeforeYield = 0;
        __block BOOL yielded = NO;
        NSData *continuousOutput = [NSMutableData dataWithLength:4096];
        session.outputHandler = ^(NSData *data) {
            assert(data.length == continuousOutput.length);
            if (!callbacks++) dispatch_async(dispatch_get_main_queue(), ^{ callbacksBeforeYield = callbacks; yielded = YES; });
            if (callbacks < 128) assert(write(message.fds[1], continuousOutput.bytes, continuousOutput.length) == (ssize_t)continuousOutput.length);
        };
        assert(write(message.fds[1], continuousOutput.bytes, continuousOutput.length) == (ssize_t)continuousOutput.length);
        pumpUntil(^BOOL { return yielded; });
        assert(callbacksBeforeYield > 0 && callbacksBeforeYield <= 8);
        session.outputHandler = nil;
        for (unsigned index = 0; index < message.plan.fdCount; ++index) close(message.fds[index]);
        pumpUntil(^BOOL { return access(statePath.fileSystemRepresentation, F_OK) < 0; });
        fflush(stdout);
        assert(dup2(savedOutput, STDOUT_FILENO) >= 0);
        close(savedOutput);
        assert([session sendInput:input] == EIO);
        [session close];
        munmap(state, LSB_FAKE_PTY_MAP_SIZE);
        assert(!rmdir(directory));
        puts("TerminalSession: inherited endpoint, fair dispatch I/O, raw input, output translation, resize/signals and EOF cleanup passed");
    }
    return 0;
}
