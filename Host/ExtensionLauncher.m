#import "ExtensionLauncher.h"
#import "Private/FoundationPrivate.h"
#include "../ProcessSupport/PrivateXPC.h"

#include <dispatch/dispatch.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

@interface LSBExtensionSession : NSObject
@property (nonatomic, strong) NSExtension *extension;
@property (nonatomic, strong) PKHostPlugIn *plugin;
@property (nonatomic, strong) PKProcessAssertionProxy *assertion;
@property (nonatomic, strong) NSUUID *instanceIdentifier;
@property (nonatomic, copy) NSString *bundleIdentifier;
@property (nonatomic) uint64_t requestID;
@property (nonatomic) uint32_t launchOptions;
@property (nonatomic) LSBExtensionEvent event;
@property (nonatomic) void *context;
@property (nonatomic) pid_t pid;
@property (nonatomic) BOOL active;
@property (nonatomic) BOOL completed;
@property (nonatomic) BOOL failed;
@property (nonatomic) BOOL assertionInvalidated;
@property (nonatomic, copy) int (^cancelWarmStart)(void);
@end

@implementation LSBExtensionSession
@end

static NSMutableDictionary<NSNumber *, LSBExtensionSession *> *gSessions;
static BOOL gTraceHandoff;

static int LSBExtensionError(NSError *error, int fallback) {
    // Preserve actual errno values without mistaking Foundation/PlugInKit domain-specific codes for errno.
    for (unsigned depth = 0; error && depth < 8; ++depth) {
        if ([error.domain isEqualToString:NSPOSIXErrorDomain] && error.code > 0 && error.code <= INT_MAX) return (int)error.code;
        id underlying = error.userInfo[NSUnderlyingErrorKey];
        error = [underlying isKindOfClass:NSError.class] ? underlying : nil;
    }
    return fallback;
}

static void LSBOnMainThread(dispatch_block_t block) {
    // Defer even synchronous replies so immediate submission failure cannot also deliver a callback.
    dispatch_async(dispatch_get_main_queue(), block);
}

static void LSBExtensionReport(LSBExtensionSession *session, pid_t pid, int error) {
    if (!session.active || session.failed) return;
    if (error) session.failed = YES;
    LSBExtensionEvent event = session.event;
    if (event) event(session.requestID, pid, error, session.context);
}

pid_t LSBExtensionProcessIdentifier(uint64_t requestID) {
    if (!pthread_main_np()) return 0;
    LSBExtensionSession *session = gSessions[@(requestID)];
    return session.active && !session.failed ? session.pid : 0;
}

static int LSBSignalInstance(NSString *bundleIdentifier, NSUUID *instanceIdentifier, int signal) {
    uuid_t instance;
    [instanceIdentifier getUUIDBytes:instance];
    xpc_connection_t connection = xpc_connection_create(bundleIdentifier.UTF8String, NULL);
    if (!connection) return ENOMEM;
    // PlugInKit uses the extension bundle identifier plus this unique instance UUID for its service connection.
    // Signaling only needs the launchd job name, and the child has no NSXPC Hub implementation.
    xpc_connection_set_oneshot_instance(connection, instance);
    xpc_connection_kill(connection, signal);
    // Releasing an inactive connection traps, even after cancel. Cancel before activation so cleanup cannot initiate a peer handshake.
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) { (void)event; });
    xpc_connection_cancel(connection);
    xpc_connection_activate(connection);
    // ARC releases the canceled native connection here; no state is passed to the guest.
    return 0;
}

int LSBSignalExtension(uint64_t requestID, int signal) {
    if (!pthread_main_np()) return EDEADLK;
    if (!requestID || signal < 0 || signal >= NSIG) return EINVAL;
    LSBExtensionSession *session = gSessions[@(requestID)];
    // Assertion loss ends GPU execution authority, but must not remove our way to terminate that exact instance.
    BOOL invalidatedGPUKill = signal == SIGKILL && (session.launchOptions & LSB_EXTENSION_LAUNCH_GPU) && session.assertionInvalidated;
    if (!session.active || (session.failed && !invalidatedGPUKill) || !session.completed || session.pid <= 0 ||
        !session.instanceIdentifier || !session.bundleIdentifier.length) return ESRCH;
    return LSBSignalInstance(session.bundleIdentifier, session.instanceIdentifier, signal);
}

void LSBReleaseExtension(uint64_t requestID) {
    if (!pthread_main_np()) {
        NSLog(@"[LiveProcess] release request %llu must run on the main thread", (unsigned long long)requestID);
        return;
    }
    LSBExtensionSession *session = gSessions[@(requestID)];
    if (!session) return;
    session.active = NO;
    session.event = NULL;
    session.context = NULL;
    session.assertion.invalidationHandler = nil;
    [session.assertion invalidate];
    session.assertion = nil;
    [gSessions removeObjectForKey:@(requestID)];
}

static pid_t LSBExtensionReplyPID(NSArray<NSNumber *> *pids, NSArray<NSUUID *> *uuids, NSUUID *pluginUUID) {
    // pkd may omit UUIDs when the registered plugin has not changed. Its PID array still corresponds to our one-plugin request.
    if (pids.count != 1 || ![pids[0] isKindOfClass:NSNumber.class] || pids[0].longLongValue <= 0 || pids[0].longLongValue > INT_MAX ||
        (uuids && (uuids.count != 1 || ![uuids[0] isEqual:pluginUUID]))) return 0;
    return pids[0].intValue;
}

static void LSBExtensionReady(LSBExtensionSession *session, NSError *error, pid_t pid, NSArray<NSNumber *> *pids, NSArray<NSUUID *> *uuids) {
    if (!session.active || session.failed || session.completed) return;
    session.completed = YES;
    if (error) {
        NSLog(@"[LiveProcess] request %llu pkd ready failed: %@", (unsigned long long)session.requestID, error);
        LSBExtensionReport(session, 0, LSBExtensionError(error, EIO));
        return;
    }
    if (!pid) {
        NSLog(@"[LiveProcess] request %llu invalid pkd ready result: pids=%@ uuids=%@", (unsigned long long)session.requestID, pids, uuids);
        LSBExtensionReport(session, 0, EPROTO);
        return;
    }
    // PKHostPlugIn uses flags 7 before its Hub handshake. RunningBoard raises gpuRole to 2 for PreventThrottleDownUI.
    uint32_t flags = LSBProcessAssertionPreventTaskSuspend | LSBProcessAssertionPreventTaskThrottleDown | LSBProcessAssertionAllowIdleSleep;
    if (session.launchOptions & LSB_EXTENSION_LAUNCH_GPU) flags |= LSBProcessAssertionPreventThrottleDownUI;
    session.assertion = [session.plugin.external.runningboard processAssertionWithPID:pid flags:flags reason:9 name:@"com.apple.extension.session"];
    __weak LSBExtensionSession *weakSession = session;
    session.assertion.invalidationHandler = ^{
        LSBOnMainThread(^{
            LSBExtensionSession *current = weakSession;
            if (!current.active || current.failed) return;
            current.assertionInvalidated = YES;
            NSLog(@"[LiveProcess] request %llu PID %d assertion invalidated", (unsigned long long)current.requestID, current.pid);
            LSBExtensionReport(current, 0, ECANCELED);
        });
    };
    BOOL acquired = [session.assertion acquire];
    if (!acquired || gTraceHandoff)
        NSLog(@"[LiveProcess] request %llu PID %d assertion flags=0x%x acquired=%d", (unsigned long long)session.requestID, pid, flags, acquired);
    if (!acquired) {
        LSBExtensionReport(session, 0, EACCES);
        return;
    }
    session.pid = pid;
    if (gTraceHandoff)
        NSLog(@"[LiveProcess] request %llu instance %@ launched PID %d via pkd", (unsigned long long)session.requestID, session.instanceIdentifier, session.pid);
    LSBExtensionReport(session, session.pid, 0);
}

static int LSBSubmitExtension(const char *identifier, uint64_t requestID, uint32_t options, BOOL warm, LSBExtensionEvent event, void *context) {
    if (!pthread_main_np()) return EDEADLK;
    if (!identifier || !identifier[0] || (warm && !requestID) || !event || (options & ~LSB_EXTENSION_LAUNCH_GPU)) return EINVAL;
    NSString *bundleIdentifier = [NSString stringWithUTF8String:identifier];
    if (!bundleIdentifier) return EINVAL;
    if (!gSessions) {
        gSessions = [NSMutableDictionary dictionary];
        const char *trace = getenv("LSB_TRACE_HANDOFF");
        gTraceHandoff = trace && !strcmp(trace, "1");
    }
    if (gSessions[@(requestID)]) return EEXIST;

    @try {
        NSError *error = nil;
        NSExtension *extension = [NSExtension extensionWithIdentifier:bundleIdentifier error:&error];
        if (!extension) {
            NSLog(@"[LiveProcess] lookup %@ failed: %@", bundleIdentifier, error);
            return LSBExtensionError(error, ENOENT);
        }
        NSUUID *instanceIdentifier = [NSUUID UUID];
        PKHostPlugIn *plugin = [[extension _plugIn] createInstanceWithUUID:instanceIdentifier];
        if (!plugin || !plugin.host) return ENOENT;
        LSBExtensionSession *session = [LSBExtensionSession new];
        session.extension = extension;
        session.plugin = plugin;
        session.instanceIdentifier = instanceIdentifier;
        session.bundleIdentifier = bundleIdentifier;
        session.requestID = requestID;
        session.launchOptions = options;
        session.event = event;
        session.context = context;
        session.active = YES;
        gSessions[@(requestID)] = session;
        __weak LSBExtensionSession *weakSession = session;
        // Cancellation can precede pkd's asynchronous reply. Retain only the exact launch identifiers in the pending cleanup.
        __block BOOL cancelled = NO, receivedPID = NO;
        NSUUID *registeredUUID = plugin.uuid;
        if (warm) session.cancelWarmStart = ^int {
            cancelled = YES;
            return receivedPID ? LSBSignalInstance(bundleIdentifier, instanceIdentifier, SIGKILL) : 0;
        };

        xpc_init_services();
        [plugin.host readyPlugIns:@[plugin] synchronously:NO environment:nil languages:@[] persona:nil sandbox:plugin.sandboxProfile
                           ready:^(NSError *error, NSArray<NSNumber *> *pids, NSArray<NSUUID *> *uuids) {
            LSBOnMainThread(^{
                pid_t pid = error ? 0 : LSBExtensionReplyPID(pids, uuids, registeredUUID);
                if (warm && pid && !receivedPID) {
                    receivedPID = YES;
                    if (cancelled) {
                        int result = LSBSignalInstance(bundleIdentifier, instanceIdentifier, SIGKILL);
                        if (result) NSLog(@"[LiveProcess] late warm launch %@ could not be terminated: %d", instanceIdentifier, result);
                        return;
                    }
                }
                LSBExtensionReady(weakSession, error, pid, pids, uuids);
            });
        }];
        if (gTraceHandoff)
            NSLog(@"[LiveProcess] submitted launch-only request %llu instance %@ for %@", (unsigned long long)requestID, instanceIdentifier, bundleIdentifier);
        return 0;
    } @catch (NSException *exception) {
        NSLog(@"[LiveProcess] request %llu raised %@: %@", (unsigned long long)requestID, exception.name, exception.reason);
        if (warm && gSessions[@(requestID)].cancelWarmStart) LSBCancelWarmExtension(requestID);
        else LSBReleaseExtension(requestID);
        return EIO;
    }
}

int LSBLaunchExtension(const char *identifier, uint64_t requestID, LSBExtensionEvent event, void *context) {
    return LSBSubmitExtension(identifier, requestID, 0, NO, event, context);
}

int LSBLaunchExtensionWithOptions(const char *identifier, uint64_t requestID, uint32_t options, LSBExtensionEvent event, void *context) {
    return LSBSubmitExtension(identifier, requestID, options, NO, event, context);
}

int LSBLaunchWarmExtension(const char *identifier, uint64_t poolID, LSBExtensionEvent event, void *context) {
    return LSBSubmitExtension(identifier, poolID, 0, YES, event, context);
}

int LSBClaimWarmExtension(uint64_t poolID, uint64_t requestID, pid_t expectedPID, LSBExtensionEvent event, void *context) {
    if (!pthread_main_np()) return EDEADLK;
    if (!poolID || !requestID || poolID == requestID || expectedPID <= 0 || !event) return EINVAL;
    LSBExtensionSession *session = gSessions[@(poolID)];
    if (!session.active || session.failed) return ESRCH;
    if (!session.cancelWarmStart) return EINVAL;
    if (!session.pid) return EAGAIN;
    if (session.pid != expectedPID) return ESRCH;
    if (gSessions[@(requestID)]) return EEXIST;
    [gSessions removeObjectForKey:@(poolID)];
    session.cancelWarmStart = nil;
    session.requestID = requestID;
    session.event = event;
    session.context = context;
    gSessions[@(requestID)] = session;
    return 0;
}

int LSBCancelWarmExtension(uint64_t poolID) {
    if (!pthread_main_np()) return EDEADLK;
    if (!poolID) return EINVAL;
    LSBExtensionSession *session = gSessions[@(poolID)];
    if (!session.active) return ESRCH;
    if (!session.cancelWarmStart) return EINVAL;
    session.failed = YES; // A failed signal submission can be retried, but the retiring session must never be claimed.
    session.event = NULL;
    session.context = NULL;
    int error = session.cancelWarmStart();
    if (!error) LSBReleaseExtension(poolID);
    return error;
}
