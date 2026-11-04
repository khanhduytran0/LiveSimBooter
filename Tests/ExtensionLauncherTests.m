#import <Foundation/Foundation.h>
#include <xpc/xpc.h>

static void TestNativeConnectionCleanup(void) {
    @autoreleasepool {
        xpc_connection_t connection = xpc_connection_create("com.kdt.LiveSimBooter.SignalLifecycleProbe", NULL);
        xpc_connection_set_event_handler(connection, ^(xpc_object_t event) { (void)event; });
        xpc_connection_cancel(connection);
        xpc_connection_activate(connection);
    }
}

static xpc_connection_t LSBMockXPCConnectionCreate(const char *name, dispatch_queue_t queue);
static void LSBMockXPCConnectionSetEventHandler(xpc_connection_t connection, xpc_handler_t handler);
static void LSBMockXPCConnectionCancel(xpc_connection_t connection);
static void LSBMockXPCConnectionActivate(xpc_connection_t connection);
#define xpc_connection_create LSBMockXPCConnectionCreate
#define xpc_connection_set_oneshot_instance LSBMockXPCConnectionSetInstance
#define xpc_connection_kill LSBMockXPCConnectionKill
#define xpc_connection_set_event_handler LSBMockXPCConnectionSetEventHandler
#define xpc_connection_cancel LSBMockXPCConnectionCancel
#define xpc_connection_activate LSBMockXPCConnectionActivate
#define NSExtension LSBMockExtension
#define PKHost LSBMockHost
#define PKHostPlugIn LSBMockHostPlugIn
#define PKExternalProviders LSBMockExternalProviders
#define PKRunningBoardProvider LSBMockRunningBoardProvider
#define PKProcessAssertionProxy LSBMockProcessAssertionProxy
#define xpc_init_services LSBMockXPCInitServices
#import "../Host/ExtensionLauncher.m"
#include <assert.h>

static void (^testReady)(NSError *, NSArray<NSNumber *> *, NSArray<NSUUID *> *);
static unsigned testMode;
static NSUUID *testPluginUUID;
static NSUUID *lastInstanceUUID;
static PKHostPlugIn *testPlugin;
static PKProcessAssertionProxy *testAssertion;
static void (^testInvalidation)(void);
static unsigned assertionReleaseCount;
static uint32_t expectedAssertionFlags = 7;
static unsigned assertionAcquireCount;
static BOOL signalAllocationFails;
static NSString *signaledService;
static NSUUID *signaledInstance;
static int signaledSignal;
static unsigned signalCount;
static unsigned signalConnectionStep;
static __weak id signalConnection;

static xpc_connection_t LSBMockXPCConnectionCreate(const char *name, dispatch_queue_t queue) {
    assert(name && !queue);
    signaledService = [NSString stringWithUTF8String:name];
    if (signalAllocationFails) return nil;
    xpc_connection_t connection = (xpc_connection_t)(id)[NSObject new];
    signalConnection = connection;
    signalConnectionStep = 1;
    return connection;
}
void LSBMockXPCConnectionSetInstance(xpc_connection_t connection, const uuid_t instance) {
    assert(connection && connection == signalConnection && signalConnectionStep++ == 1);
    signaledInstance = [[NSUUID alloc] initWithUUIDBytes:instance];
}
void LSBMockXPCConnectionKill(xpc_connection_t connection, int signal) {
    assert(connection && connection == signalConnection && signalConnectionStep++ == 2);
    ++signalCount;
    signaledSignal = signal;
}
static void LSBMockXPCConnectionSetEventHandler(xpc_connection_t connection, xpc_handler_t handler) {
    assert(connection && connection == signalConnection && signalConnectionStep++ == 3 && handler);
}
static void LSBMockXPCConnectionCancel(xpc_connection_t connection) {
    assert(connection && connection == signalConnection && signalConnectionStep++ == 4);
}
static void LSBMockXPCConnectionActivate(xpc_connection_t connection) {
    assert(connection && connection == signalConnection && signalConnectionStep++ == 5);
}

@implementation LSBMockExtension
+ (instancetype)extensionWithIdentifier:(NSString *)identifier error:(NSError **)error {
    if ([identifier isEqualToString:@"missing"]) {
        *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOENT userInfo:nil];
        return nil;
    }
    return [self new];
}
- (PKHostPlugIn *)_plugIn { return [PKHostPlugIn new]; }
@end
@implementation PKHostPlugIn
- (PKHostPlugIn *)createInstanceWithUUID:(NSUUID *)uuid {
    assert(uuid && ![uuid isEqual:lastInstanceUUID]);
    lastInstanceUUID = uuid;
    testPlugin = [PKHostPlugIn new];
    return testPlugin;
}
- (PKHost *)host { return [PKHost new]; }
- (PKExternalProviders *)external { return [PKExternalProviders new]; }
- (NSString *)sandboxProfile { return nil; }
- (NSUUID *)uuid { return testPluginUUID; }
@end
@implementation PKExternalProviders
- (PKRunningBoardProvider *)runningboard { return [PKRunningBoardProvider new]; }
@end
@implementation PKRunningBoardProvider
- (PKProcessAssertionProxy *)processAssertionWithPID:(pid_t)pid flags:(uint32_t)flags reason:(uint32_t)reason name:(NSString *)name {
    assert(pid == 4242 && flags == expectedAssertionFlags && reason == 9 && [name isEqualToString:@"com.apple.extension.session"]);
    testAssertion = [PKProcessAssertionProxy new];
    return testAssertion;
}
@end
@implementation PKProcessAssertionProxy
- (void (^)(void))invalidationHandler { return testInvalidation; }
- (void)setInvalidationHandler:(void (^)(void))handler { testInvalidation = handler; }
- (BOOL)acquire { ++assertionAcquireCount; return testMode != 2; }
- (void)invalidate { ++assertionReleaseCount; if (testInvalidation) testInvalidation(); }
@end
@implementation PKHost
- (void)readyPlugIns:(NSArray<PKHostPlugIn *> *)plugins synchronously:(BOOL)synchronously environment:(NSDictionary *)environment
          languages:(NSArray<NSString *> *)languages persona:(id)persona sandbox:(NSString *)sandbox
              ready:(void (^)(NSError *, NSArray<NSNumber *> *, NSArray<NSUUID *> *))ready {
    assert(plugins.count == 1 && plugins[0] == testPlugin);
    assert(!synchronously && !environment && !languages.count && !persona && !sandbox);
    testReady = [ready copy];
    if (testMode == 1) {
        ready(nil, @[@4242], @[testPluginUUID]);
        @throw [NSException exceptionWithName:@"test" reason:@"submission failed" userInfo:nil];
    }
}
@end
void xpc_init_services(void) {}

static unsigned count;
static int lastError;
static pid_t lastPID;
static uint64_t lastRequest;
static int expectedContext;
static void Record(uint64_t requestID, pid_t pid, int error, void *context) {
    assert(pthread_main_np() && context == &expectedContext);
    ++count;
    lastError = error;
    lastPID = pid;
    lastRequest = requestID;
}
static void Drain(void) { [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]]; }
static void Ready(void) { testReady(nil, @[@4242], @[testPluginUUID]); }

static int jobContext;
static void RecordClaimed(uint64_t requestID, pid_t pid, int error, void *context) {
    assert(pthread_main_np() && context == &jobContext);
    Record(requestID, pid, error, &expectedContext);
}

static void TestWarmClaim(void) {
    unsigned events = count, acquisitions = assertionAcquireCount, releases = assertionReleaseCount;
    dispatch_semaphore_t checked = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        assert(LSBLaunchWarmExtension("test", 21, Record, &expectedContext) == EDEADLK);
        assert(LSBClaimWarmExtension(21, 22, 4242, RecordClaimed, &jobContext) == EDEADLK);
        assert(LSBCancelWarmExtension(21) == EDEADLK);
        dispatch_semaphore_signal(checked);
    });
    assert(!dispatch_semaphore_wait(checked, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)));
    assert(!LSBLaunchWarmExtension("test", 21, Record, &expectedContext));
    assert(LSBClaimWarmExtension(21, 22, 4242, RecordClaimed, &jobContext) == EAGAIN);
    Ready();
    Drain();
    NSUUID *instance = lastInstanceUUID;
    assert(count == events + 1 && lastRequest == 21 && lastPID == 4242 && LSBExtensionProcessIdentifier(21) == 4242);
    assert(LSBClaimWarmExtension(21, 22, 1234, RecordClaimed, &jobContext) == ESRCH);
    assert(!LSBLaunchExtension("test", 22, Record, &expectedContext));
    assert(LSBClaimWarmExtension(21, 22, 4242, RecordClaimed, &jobContext) == EEXIST);
    assert(LSBCancelWarmExtension(22) == EINVAL);
    LSBReleaseExtension(22);
    assert(!LSBClaimWarmExtension(21, 22, 4242, RecordClaimed, &jobContext));
    assert(!LSBExtensionProcessIdentifier(21) && LSBExtensionProcessIdentifier(22) == 4242);
    assert(LSBSignalExtension(21, SIGTERM) == ESRCH && LSBCancelWarmExtension(21) == ESRCH && LSBCancelWarmExtension(22) == EINVAL);
    assert(LSBClaimWarmExtension(22, 23, 4242, Record, &expectedContext) == EINVAL);
    LSBReleaseExtension(21);
    assert(count == events + 1 && assertionAcquireCount == acquisitions + 1 && assertionReleaseCount == releases);
    assert(!LSBSignalExtension(22, SIGTERM) && [signaledInstance isEqual:instance] && signaledSignal == SIGTERM);
    testInvalidation();
    Drain();
    assert(count == events + 2 && lastRequest == 22 && lastError == ECANCELED);
    LSBReleaseExtension(22);
    assert(assertionReleaseCount == releases + 1);
}

static void TestWarmCancellation(void) {
    unsigned events = count, signals = signalCount;
    assert(!LSBLaunchWarmExtension("test", 31, Record, &expectedContext));
    NSUUID *cancelled = lastInstanceUUID;
    void (^oldReady)(NSError *, NSArray *, NSArray *) = testReady;
    assert(!LSBCancelWarmExtension(31) && LSBCancelWarmExtension(31) == ESRCH && signalCount == signals);
    assert(!LSBLaunchExtension("test", 31, Record, &expectedContext)); // Reuse the ID while pkd still has the old completion.
    oldReady(nil, @[@4242], @[[NSUUID UUID]]);
    oldReady(nil, @[@0], @[testPluginUUID]);
    oldReady([NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil], @[@4242], nil);
    Drain();
    assert(count == events && signalCount == signals);
    oldReady(nil, @[@4242], nil);
    oldReady(nil, @[@4242], nil);
    Ready();
    Drain();
    assert(count == events + 1 && lastRequest == 31 && !lastError);
    assert(signalCount == ++signals && signaledSignal == SIGKILL && [signaledInstance isEqual:cancelled]);
    assert(LSBExtensionProcessIdentifier(31) == 4242);
    LSBReleaseExtension(31);

    testMode = 2;
    assert(!LSBLaunchWarmExtension("test", 32, Record, &expectedContext));
    Ready();
    Drain();
    assert(lastError == EACCES && LSBClaimWarmExtension(32, 33, 4242, RecordClaimed, &jobContext) == ESRCH);
    assert(!LSBCancelWarmExtension(32) && signalCount == ++signals && [signaledInstance isEqual:lastInstanceUUID]);
    testMode = 0;
    assert(!LSBLaunchWarmExtension("test", 33, Record, &expectedContext));
    Ready();
    Drain();
    signals = signalCount;
    signalAllocationFails = YES;
    assert(LSBCancelWarmExtension(33) == ENOMEM && signalCount == signals);
    assert(LSBClaimWarmExtension(33, 34, 4242, RecordClaimed, &jobContext) == ESRCH);
    signalAllocationFails = NO;
    assert(!LSBCancelWarmExtension(33));
    assert(signalCount == ++signals && [signaledInstance isEqual:lastInstanceUUID]);
    testMode = 1; // Synchronous pkd completion followed by an exception during submission; no callback or stranded process.
    events = count;
    assert(LSBLaunchWarmExtension("test", 34, Record, &expectedContext) == EIO);
    Drain();
    assert(count == events && signalCount == signals + 1 && [signaledInstance isEqual:lastInstanceUUID]);
    testMode = 0;
    assert(!LSBLaunchWarmExtension("test", 35, Record, &expectedContext));
    oldReady = testReady;
    oldReady([NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil], nil, nil);
    Drain();
    signals = signalCount;
    assert(lastError == EIO && !LSBCancelWarmExtension(35) && signalCount == signals);
    oldReady(nil, @[@4242], @[testPluginUUID]);
    Drain();
    assert(signalCount == signals + 1 && signaledSignal == SIGKILL && [signaledInstance isEqual:lastInstanceUUID]);
}

int main(void) {
    @autoreleasepool {
        TestNativeConnectionCleanup();
        testPluginUUID = [NSUUID UUID];
        assert(LSBLaunchExtension(NULL, 1, Record, &expectedContext) == EINVAL);
        assert(LSBLaunchExtension("test", 1, NULL, &expectedContext) == EINVAL);
        assert(LSBLaunchExtension("missing", 1, Record, &expectedContext) == ENOENT);
        assert(LSBSignalExtension(0, SIGCHLD) == EINVAL);
        assert(LSBSignalExtension(1, -1) == EINVAL);
        assert(LSBSignalExtension(1, NSIG) == EINVAL);
        assert(LSBSignalExtension(1, SIGCHLD) == ESRCH);
        assert(LSBLaunchExtension("test", 1, Record, &expectedContext) == 0);
        assert(LSBLaunchExtension("test", 1, Record, &expectedContext) == EEXIST);
        assert(LSBExtensionProcessIdentifier(1) == 0);
        assert(LSBSignalExtension(1, SIGCHLD) == ESRCH);
        Ready();
        assert(count == 0);
        Drain();
        assert(count == 1 && lastRequest == 1 && lastPID == 4242 && lastError == 0);
        assert(LSBExtensionProcessIdentifier(1) == 4242);
        @autoreleasepool {
            assert(LSBSignalExtension(1, SIGCHLD) == 0);
            assert(signalCount == 1 && signaledSignal == SIGCHLD);
            assert([signaledService isEqualToString:@"test"] && [signaledInstance isEqual:lastInstanceUUID]);
        }
        assert(!signalConnection && signalConnectionStep == 6);
        signalAllocationFails = YES;
        assert(LSBSignalExtension(1, SIGCHLD) == ENOMEM && signalCount == 1);
        signalAllocationFails = NO;
        Ready();
        Drain();
        assert(count == 1);
        testInvalidation();
        testInvalidation();
        Drain();
        assert(count == 2 && lastError == ECANCELED && lastPID == 0);
        assert(LSBSignalExtension(1, SIGCHLD) == ESRCH);
        LSBReleaseExtension(1);
        assert(LSBSignalExtension(1, SIGCHLD) == ESRCH);
        assert(assertionReleaseCount == 1 && LSBExtensionProcessIdentifier(1) == 0);
        assert(LSBLaunchExtension("test", 2, Record, &expectedContext) == 0);
        Ready();
        LSBReleaseExtension(2);
        Drain();
        assert(count == 2);
        testMode = 1;
        assert(LSBLaunchExtension("test", 3, Record, &expectedContext) == EIO);
        Drain();
        assert(count == 2);
        testMode = 2;
        assert(LSBLaunchExtension("test", 4, Record, &expectedContext) == 0);
        Ready();
        Drain();
        assert(count == 3 && lastError == EACCES && LSBExtensionProcessIdentifier(4) == 0);
        assert(LSBSignalExtension(4, SIGCHLD) == ESRCH);
        LSBReleaseExtension(4);
        testMode = 0;
        assert(LSBLaunchExtension("test", 5, Record, &expectedContext) == 0);
        testReady(nil, nil, nil);
        Drain();
        assert(count == 4 && lastError == EPROTO && lastRequest == 5);
        LSBReleaseExtension(5);
        assert(LSBLaunchExtension("test", 6, Record, &expectedContext) == 0);
        testReady([NSError errorWithDomain:@"test" code:100 userInfo:@{
            NSUnderlyingErrorKey: [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOEXEC userInfo:nil]
        }], nil, nil);
        Drain();
        assert(count == 5 && lastError == ENOEXEC);
        LSBReleaseExtension(6);
        assert(LSBLaunchExtension("test", 7, Record, &expectedContext) == 0);
        testReady(nil, @[@4242], @[[NSUUID UUID]]);
        Drain();
        assert(count == 6 && lastError == EPROTO);
        LSBReleaseExtension(7);
        assert(LSBLaunchExtension("test", 8, Record, &expectedContext) == 0);
        testReady(nil, @[@4242], nil);
        Drain();
        assert(count == 7 && lastError == 0 && lastPID == 4242 && LSBExtensionProcessIdentifier(8) == 4242);
        NSUUID *previousSignaledInstance = signaledInstance;
        assert(LSBSignalExtension(8, SIGCHLD) == 0 && signalCount == 2);
        assert([signaledInstance isEqual:lastInstanceUUID] && ![signaledInstance isEqual:previousSignaledInstance]);
        LSBReleaseExtension(8);
        assert(LSBLaunchExtension("test", 9, Record, &expectedContext) == 0);
        testReady(nil, @[@4242], @[]);
        Drain();
        assert(count == 8 && lastError == EPROTO);
        LSBReleaseExtension(9);
        assert(LSBLaunchExtensionWithOptions("test", 10, 2, Record, &expectedContext) == EINVAL);
        expectedAssertionFlags = 39;
        for (unsigned failed = 0; failed < 2; ++failed) {
            testMode = failed ? 2 : 0;
            unsigned previousAcquires = assertionAcquireCount, previousEvents = count;
            uint64_t identifier = 10 + failed;
            assert(!LSBLaunchExtensionWithOptions("test", identifier, LSB_EXTENSION_LAUNCH_GPU, Record, &expectedContext));
            Ready();
            Drain();
            assert(assertionAcquireCount == previousAcquires + 1 && count == previousEvents + 1 && lastRequest == identifier);
            assert(lastError == (failed ? EACCES : 0) && LSBExtensionProcessIdentifier(identifier) == (failed ? 0 : 4242));
            if (!failed) {
                NSUUID *launchedInstance = lastInstanceUUID;
                unsigned previousSignals = signalCount;
                testInvalidation();
                Drain();
                assert(count == previousEvents + 2 && lastError == ECANCELED && !LSBExtensionProcessIdentifier(identifier));
                assert(LSBSignalExtension(identifier, 0) == ESRCH && LSBSignalExtension(identifier, SIGTERM) == ESRCH);
                assert(!LSBSignalExtension(identifier, SIGKILL) && signalCount == previousSignals + 1 && signaledSignal == SIGKILL);
                assert([signaledInstance isEqual:launchedInstance] && [signaledService isEqualToString:@"test"]);
                testInvalidation();
                Drain();
                assert(count == previousEvents + 2); // Duplicate invalidation cannot re-notify or create a different target.
            } else assert(LSBSignalExtension(identifier, SIGKILL) == ESRCH); // Failed acquisition never grants this cleanup path.
            LSBReleaseExtension(identifier);
            assert(LSBSignalExtension(identifier, SIGKILL) == ESRCH);
        }
        // A GPU launch cannot change the policy of a later ordinary guest launch.
        testMode = 0;
        expectedAssertionFlags = 7;
        assert(!LSBLaunchExtension("test", 12, Record, &expectedContext));
        Ready();
        Drain();
        assert(lastRequest == 12 && !lastError && lastPID == 4242);
        LSBReleaseExtension(12);
        TestWarmClaim();
        TestWarmCancellation();
        puts("launcher lifecycle tests passed");
    }
    return 0;
}
