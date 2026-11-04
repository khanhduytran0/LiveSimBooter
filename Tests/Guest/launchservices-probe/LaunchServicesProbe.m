#import "../../../Guest/Shared/LaunchServicesReadProtocol.h"

#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !TARGET_OS_SIMULATOR
#error This probe must be built only for the iOS simulator guest.
#endif

static atomic_int outcome;
extern uint32_t dyld_get_active_platform(void);

static void timedOut(int signalNumber) {
    (void)signalNumber;
    static const char message[] = "[ls-ipc] hard timeout after 20 seconds\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static BOOL completeOnce(int value) {
    int expected = 0;
    return atomic_compare_exchange_strong(&outcome, &expected, value);
}

int main(void) {
    struct sigaction action = {0};
    action.sa_handler = timedOut;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0) return 2;
    alarm(20);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *root = getenv("DYLD_ROOT_PATH");
    const char *simulatorRoot = getenv("SIMULATOR_ROOT");
    // Refuse an accidental native invocation before contacting any LaunchServices endpoint.
    if (dyld_get_active_platform() != 7 || !root || root[0] != '/' || !simulatorRoot || strcmp(root, simulatorRoot)) {
        fprintf(stderr, "[ls-ipc] requires the simulator runtime with matching DYLD_ROOT_PATH and SIMULATOR_ROOT\n");
        return 2;
    }

    @autoreleasepool {
        @try {
            dispatch_semaphore_t finished = dispatch_semaphore_create(0);
            id<LSBMachServiceConnectionInitializer> allocated = (id)[NSXPCConnection alloc];
            NSXPCConnection *connection = (id)[allocated initWithMachServiceName:@"com.apple.lsd.mapdb" options:0];
            if (!connection) return 2;
            connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(LSBLaunchServicesReadProtocol)];
            connection.interruptionHandler = ^{
                if (completeOnce(2)) {
                    fprintf(stderr, "[ls-ipc] connection interrupted before reply\n");
                    dispatch_semaphore_signal(finished);
                }
            };
            connection.invalidationHandler = ^{
                if (completeOnce(3)) {
                    fprintf(stderr, "[ls-ipc] connection invalidated before reply\n");
                    dispatch_semaphore_signal(finished);
                }
            };
            [connection resume];
            id<LSBLaunchServicesReadProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
                if (completeOnce(4)) {
                    fprintf(stderr, "[ls-ipc] XPC error domain=%s code=%ld description=%s\n",
                            error.domain.UTF8String, (long)error.code, error.localizedDescription.UTF8String);
                    dispatch_semaphore_signal(finished);
                }
            }];
            printf("[ls-ipc] request com.apple.lsd.mapdb getServerStatusWithCompletionHandler: guest-pid=%d\n", getpid());
            [proxy getServerStatusWithCompletionHandler:^(uint32_t status) {
                if (completeOnce(1)) {
                    printf("[ls-ipc] daemon reply status=0x%08x; zero is a valid status, not a missing reply\n", status);
                    dispatch_semaphore_signal(finished);
                }
            }];
            long waitResult = dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 18 * NSEC_PER_SEC));
            if (waitResult && completeOnce(5)) fprintf(stderr, "[ls-ipc] no reply within 18 seconds\n");
            int result = atomic_load(&outcome);
            if (result == 1) printf("[ls-ipc] reply peer-pid=%d\n", connection.processIdentifier);
            [connection invalidate];
            alarm(0);
            return result == 1 ? 0 : (result == 5 ? 124 : 1);
        } @catch (NSException *exception) {
            fprintf(stderr, "[ls-ipc] exception %s: %s\n", exception.name.UTF8String, exception.reason.UTF8String);
            return 1;
        }
    }
}
