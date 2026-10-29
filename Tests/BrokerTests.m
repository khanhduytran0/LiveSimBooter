#include "../ProcessSupport/RuntimeLayout.h"
#import <Foundation/Foundation.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include "../Host/GuestEntitlementsConversion.h"
#include "../ProcessSupport/TaskPortCapture.h"
#import "../Host/DeviceConfiguration.h"
static LSBGuestDeviceType *testConfiguredDeviceType(void);
static void testSaveDeviceType(LSBGuestDeviceType *);
static int testInstallGuestDeviceCapabilitiesWithNativeHaptics(NSString *, NSDictionary *, BOOL);
static BOOL testConfiguredHaptics(void);
static void testSaveHaptics(BOOL);
static BOOL testConfiguredAudio(void);
static void testSaveAudio(BOOL);
#define LSBExperimentalAudioOutputEnabled testConfiguredAudio
#define LSBSaveConfiguredAudioOutputEnabled testSaveAudio
#define LSBExperimentalHapticsEnabled testConfiguredHaptics
#define LSBSaveConfiguredHapticsEnabled testSaveHaptics
#define LSBConfiguredGuestDeviceType testConfiguredDeviceType
#define LSBSaveConfiguredGuestDeviceType testSaveDeviceType
#define LSBInstallGuestDeviceCapabilitiesWithNativeHaptics testInstallGuestDeviceCapabilitiesWithNativeHaptics
static int testCopyXMLFromDEREntitlementsWithDictionary(const void *, size_t, void **, size_t *, NSDictionary **);
static int testReadMachOCodeDirectoryHash(int, unsigned char [20]);
static kern_return_t testTaskInfo(task_name_t, task_flavor_t, task_info_t, mach_msg_type_number_t *);
static kern_return_t testBootstrapLookup(mach_port_t, const char *, mach_port_t *);
static int testPeerDebuggerQuery(mach_port_t, const audit_token_t *, uint32_t, struct kinfo_proc *);
static int testTaskCaptureCreate(LSBTaskPortCaptureOffer *);
static int testTaskCaptureReceive(const LSBTaskPortCaptureOffer *, const audit_token_t *, mach_port_t *);
#define LSBTaskPortCaptureCreate testTaskCaptureCreate
#define LSBTaskPortCaptureReceive testTaskCaptureReceive
#define LSBPeerDebuggerQuery testPeerDebuggerQuery
#define LSBCopyXMLFromDEREntitlementsWithDictionary testCopyXMLFromDEREntitlementsWithDictionary
#define LSBReadMachOCodeDirectoryHash testReadMachOCodeDirectoryHash
#define LSBLaunchdRegisterEndpointWithTimeout LSBTestRegisterEndpoint
#define LSBLaunchdUnregisterEndpointWithTimeout LSBTestUnregisterEndpoint
#define task_info testTaskInfo
#define bootstrap_look_up testBootstrapLookup
#define handleMessage brokerHandleMessage
#include "../Host/Broker.m"
#undef handleMessage
#undef LSBTaskPortCaptureCreate
#undef LSBTaskPortCaptureReceive
#undef LSBPeerDebuggerQuery
#undef task_info
#undef bootstrap_look_up
#undef LSBLaunchdRegisterEndpointWithTimeout
#undef LSBLaunchdUnregisterEndpointWithTimeout
#undef LSBCopyXMLFromDEREntitlementsWithDictionary
#undef LSBReadMachOCodeDirectoryHash
#undef LSBExperimentalAudioOutputEnabled
#undef LSBSaveConfiguredAudioOutputEnabled
#undef LSBExperimentalHapticsEnabled
#undef LSBSaveConfiguredHapticsEnabled
#undef LSBConfiguredGuestDeviceType
#undef LSBSaveConfiguredGuestDeviceType
#undef LSBInstallGuestDeviceCapabilitiesWithNativeHaptics

static uint32_t testNativeProductionStatus = LSB_NATIVE_DEVELOPMENT;
uint32_t LSBHostNativeProductionStatus(void) { return testNativeProductionStatus; }

// Isolate preference publication from the developer's real NSUserDefaults domain; filesystem validation/install stays real.
static NSString *savedDeviceIdentifier;
static unsigned devicePreferenceWrites, hapticsPreferenceWrites, audioPreferenceWrites;
static BOOL savedHaptics, testNativeHaptics, savedAudio;
static BOOL testConfiguredAudio(void) { return LSBAudioOutputForcedByEnvironment() || savedAudio; }
static void testSaveAudio(BOOL enabled) { savedAudio = enabled; ++audioPreferenceWrites; }
static BOOL testConfiguredHaptics(void) { return LSBHapticsForcedByEnvironment() || savedHaptics; }
static void testSaveHaptics(BOOL enabled) { savedHaptics = enabled; ++hapticsPreferenceWrites; }
static LSBGuestDeviceType *testConfiguredDeviceType(void) {
    return LSBGuestDeviceTypeForIdentifier(savedDeviceIdentifier) ?: LSBDefaultGuestDeviceType();
}
static void testSaveDeviceType(LSBGuestDeviceType *type) {
    assert(type == LSBGuestDeviceTypeForIdentifier(type.identifier));
    savedDeviceIdentifier = [type.identifier copy];
    ++devicePreferenceWrites;
}
static int testInstallGuestDeviceCapabilitiesWithNativeHaptics(NSString *root, NSDictionary *profile, BOOL enabled) {
    if (!enabled) return LSBInstallGuestDeviceCapabilities(root, profile);
    NSDictionary *adapted = nil;
    int error = LSBProjectGuestHapticsCapability(profile, testNativeHaptics ? kCFBooleanTrue : kCFBooleanFalse, &adapted);
    return error ? error : LSBInstallGuestDeviceCapabilities(root, adapted);
}

typedef struct {
    unsigned objectID;
    uint32_t kind;
    int error, sendError;
    pid_t pid;
    uint64_t requestID;
    bool deliveredPlan;
    mach_port_t launchdTank;
    pid_t guestPID, parentPID;
    uint32_t executionRole, nativeProductionStatus;
} RecordedReply;

typedef struct { unsigned identifier; int retainedFD; } TestMessageObject;

// Each launch may record SPAWN, CHECKIN and READY replies while capacity/reuse tests run a full batch.
static RecordedReply replies[4 * BrokerJobCapacity];
static unsigned replyCount, disposedMessages, submittedLaunches, releasedLaunches, objectSequence;
static uint32_t submittedLaunchOptions;
static pid_t reportedPID;
static int submitError, readySendError, rebootSendError, launchdBootstrapSendError, monitorCreateError, identityCheckError, runningBoardError;
static unsigned deliveredSignals, identityChecks, runningBoardStarts, runningBoardCancels;
static int monitorReadError, monitorWaitStatus;
static unsigned monitorReads;
static unsigned submittedExtensionSignals;
static uint64_t signaledRequest;
static int directSignalError, extensionSignalError, lastSignal;
static audit_token_t signaledAudit;
static void (^signalHook)(void);
static LSBExtensionEvent extensionCallback;
static void (^readyReplyHook)(void);
static void (^checkInReplyHook)(void);
static unsigned endpointCalls;
static NSMutableArray<NSNumber *> *publicTestEndpoints;
static int endpointErrors[8];
static char registeredEndpoints[8][128];
static mach_port_t registeredPorts[8];
static audit_token_t registeredAudits[8];
static uint32_t registeredTimeouts[8];
static void (^endpointHook)(unsigned index);
static unsigned nativeConfigdLookupCalls;
static kern_return_t nativeConfigdLookupError;
static mach_port_t nativeConfigdEndpoint;
static unsigned hapticCreates, hapticCloses, audioCreates, audioInvalidates;
static int hapticCreateError, audioCreateError, audioReplySendError;
static BOOL hapticMissingEndpoint, hapticCopyFailure;
static BOOL (^hapticAuthorization)(const audit_token_t *);
static void *audioSharedMemoryReply;
static unsigned unregisterCalls;
static int unregisterError;
static uint32_t unregisterTimeout;
static char unregisteredEndpoint[128];
static audit_token_t unregisterAudit;
static NSData *entitlementsReply;
static int xmlConversionError;
static NSData *codeDirectoryHash;
static int codeDirectoryHashError;
static void (^afterCodeDirectoryRead)(int fd);
static LSBPeerProcessIdentity identityReply;
static audit_token_t testTaskAudit;
static BOOL useRealTaskInfo, preserveCheckInIdentity, mismatchTaskAudit;
static kern_return_t taskInfoError;
static BOOL automaticTaskCapture = YES;
static int taskCaptureCreateError, taskCaptureReceiveError, taskReplySendError;
static unsigned taskCaptureCreates, taskCaptureReceives;
static mach_port_t processTaskReply;
static audit_token_t processTaskReplyAudit;
static int testTaskCaptureCreate(LSBTaskPortCaptureOffer *offer) {
    memset(offer, 0, sizeof(*offer));
    ++taskCaptureCreates;
    if (taskCaptureCreateError) return taskCaptureCreateError;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &offer->port));
    assert(!mach_port_insert_right(mach_task_self(), offer->port, offer->port, MACH_MSG_TYPE_MAKE_SEND));
    memset(offer->nonce, 0xa5, sizeof(offer->nonce));
    return 0;
}
static int testTaskCaptureReceive(const LSBTaskPortCaptureOffer *offer, const audit_token_t *expected, mach_port_t *task) {
    assert(offer && MACH_PORT_VALID(offer->port) && expected->val[5] && expected->val[7]);
    ++taskCaptureReceives;
    *task = MACH_PORT_NULL;
    if (taskCaptureReceiveError) return taskCaptureReceiveError;
    *task = mach_task_self();
    assert(!mach_port_mod_refs(mach_task_self(), *task, MACH_PORT_RIGHT_SEND, 1));
    return 0;
}
static unsigned debuggerQueryCalls;
static int debuggerQueryError;
static void (^debuggerQueryHook)(void);
static struct kinfo_proc debuggerInfoReply;
static int testPeerDebuggerQuery(mach_port_t endpoint, const audit_token_t *expected, uint32_t timeout, struct kinfo_proc *info) {
    assert(!NSThread.isMainThread && MACH_PORT_VALID(endpoint) && expected->val[5] && expected->val[7] && timeout == 500);
    ++debuggerQueryCalls;
    memset(info, 0, sizeof(*info));
    if (!debuggerQueryError) { info->kp_proc.p_pid = expected->val[5]; info->kp_proc.p_flag = P_TRACED; }
    if (debuggerQueryHook) dispatch_sync(dispatch_get_main_queue(), debuggerQueryHook);
    return debuggerQueryError;
}

static int testReadMachOCodeDirectoryHash(int fd, unsigned char output[20]) {
    struct stat info;
    assert(!fstat(fd, &info) && S_ISREG(info.st_mode));
    memset(output, 0, LSB_GUEST_CDHASH_BYTES);
    if (codeDirectoryHashError) return codeDirectoryHashError;
    if (!codeDirectoryHash) return ENOENT;
    assert(codeDirectoryHash.length == LSB_GUEST_CDHASH_BYTES);
    memcpy(output, codeDirectoryHash.bytes, codeDirectoryHash.length);
    if (afterCodeDirectoryRead) afterCodeDirectoryRead(fd);
    return 0;
}

static kern_return_t testTaskInfo(task_name_t port, task_flavor_t flavor, task_info_t output, mach_msg_type_number_t *count) {
    if (useRealTaskInfo) return task_info(port, flavor, output, count);
    assert(flavor == TASK_AUDIT_TOKEN && *count == TASK_AUDIT_TOKEN_COUNT);
    if (taskInfoError) return taskInfoError;
    audit_token_t audit = testTaskAudit;
    if (mismatchTaskAudit) ++audit.val[7];
    memcpy(output, &audit, sizeof(audit));
    *count = TASK_AUDIT_TOKEN_COUNT;
    return KERN_SUCCESS;
}

static void handleMessage(LSBReceivedMessage *message) {
    testTaskAudit = message->audit;
    if (message->kind == LSB_REQUEST_CHECKIN && !preserveCheckInIdentity) {
        message->peerIdentity.audit = message->audit;
        memcpy(message->peerIdentity.shortInfo, &message->audit.val[5], sizeof(uint32_t));
    }
    if (message->kind == LSB_REQUEST_COPY_PROCESS_IDENTITY || message->kind == LSB_REQUEST_RESOLVE_NATIVE_CHILD_PID ||
        message->kind == LSB_REQUEST_COPY_DEBUGGER_INFO || message->kind == LSB_REQUEST_COPY_PROCESS_TASK) {
        LSBJob *sender = nil;
        for (LSBJob *job in jobs) if (job.pid == (pid_t)message->audit.val[5] && job.pidVersion == message->audit.val[7]) { sender = job; break; }
        BOOL logical = message->kind == LSB_REQUEST_COPY_PROCESS_TASK ||
            (message->kind == LSB_REQUEST_COPY_PROCESS_IDENTITY && sender.simulatorLaunchd);
        for (LSBJob *job in jobs) if (logical ? job.guestPID == message->targetPID &&
            (message->kind == LSB_REQUEST_COPY_PROCESS_TASK || job.parentIdentifier == sender.identifier) : job.pid == message->targetPID) {
            testTaskAudit = job.audit; break;
        }
    }
    if (automaticTaskCapture && message->kind == LSB_REQUEST_READY) {
        LSBJob *job = findJob(message->requestID);
        audit_token_t expected = job.audit;
        if (job && job.phase == JobDispatched && job.executionRole == LSB_EXECUTION_SIMULATOR && job.taskCaptureError == ENOENT &&
            MACH_PORT_VALID(job.taskCapture.port) && !memcmp(&expected, &message->audit, sizeof(expected)))
            receiveGuestTaskPort(job.identifier);
    }
    brokerHandleMessage(message);
}

static int testCopyXMLFromDEREntitlementsWithDictionary(const void *der, size_t size, void **xml, size_t *xmlSize, NSDictionary **dictionary) {
    if (xmlConversionError) { *xml = NULL; *xmlSize = 0; *dictionary = nil; return xmlConversionError; }
    return LSBCopyXMLFromDEREntitlementsWithDictionary(der, size, xml, xmlSize, dictionary);
}

static kern_return_t testBootstrapLookup(mach_port_t bootstrap, const char *name, mach_port_t *port) {
    assert(!NSThread.isMainThread && bootstrap == bootstrap_port);
    assert(port);
    assert(!strcmp(name, "com.apple.SystemConfiguration.configd"));
    ++nativeConfigdLookupCalls;
    *port = MACH_PORT_NULL;
    if (nativeConfigdLookupError) return nativeConfigdLookupError;
    if (MACH_PORT_VALID(nativeConfigdEndpoint))
        assert(!mach_port_mod_refs(mach_task_self(), nativeConfigdEndpoint, MACH_PORT_RIGHT_SEND, 1));
    *port = nativeConfigdEndpoint;
    return KERN_SUCCESS;
}

int LSBTestRegisterEndpoint(mach_port_t service, const audit_token_t *audit, const char *name,
                            mach_port_t endpoint, uint32_t timeoutMilliseconds, kern_return_t *status) {
    assert(!NSThread.isMainThread && MACH_PORT_VALID(service) && audit && name && status && endpointCalls < 8);
    unsigned index = endpointCalls++;
    assert(strlen(name) < sizeof(registeredEndpoints[index]));
    strcpy(registeredEndpoints[index], name);
    registeredPorts[index] = endpoint;
    registeredAudits[index] = *audit;
    assert(timeoutMilliseconds && timeoutMilliseconds <= 30000);
    registeredTimeouts[index] = timeoutMilliseconds;
    if (endpointHook) endpointHook(index);
    *status = endpointErrors[index] ? 1100 : KERN_SUCCESS;
    return endpointErrors[index];
}

int LSBTestUnregisterEndpoint(mach_port_t service, const audit_token_t *audit, const char *name,
                              uint32_t timeoutMilliseconds, kern_return_t *status) {
    assert(!NSThread.isMainThread && MACH_PORT_VALID(service) && audit && name && status);
    assert(strlen(name) < sizeof(unregisteredEndpoint) && timeoutMilliseconds && timeoutMilliseconds <= 30000);
    ++unregisterCalls;
    strcpy(unregisteredEndpoint, name);
    unregisterAudit = *audit;
    unregisterTimeout = timeoutMilliseconds;
    *status = unregisterError == ETIMEDOUT ? -1 : unregisterError ? 0xc000 | unregisterError : KERN_SUCCESS;
    return unregisterError;
}

// Keep these tests on broker decisions; IndigoHIDServerTests exercises its Mach authentication and receive lifecycle.
@interface RecordedIndigoServer : LSBIndigoHIDServer
@property unsigned authorizations, revocations;
@property audit_token_t authorizedAudit;
@end
@implementation RecordedIndigoServer
- (void)authorizeClientWithAuditToken:(const audit_token_t *)audit {
    if (audit) { self.authorizedAudit = *audit; ++self.authorizations; }
    else { self.authorizedAudit = (audit_token_t){{0}}; ++self.revocations; }
}
- (pid_t)connectedClientPID { return 0; }
- (mach_port_t)registrationPort { return 111; }
- (void)close {}
@end

@interface RecordedFramebufferServer : LSBFramebufferServer
@property unsigned authorizations, revocations, closes;
@property audit_token_t authorizedAudit;
@end
@implementation RecordedFramebufferServer
- (void)authorizeClientWithAuditToken:(const audit_token_t *)audit {
    if (audit) { self.authorizedAudit = *audit; ++self.authorizations; }
    else { self.authorizedAudit = (audit_token_t){{0}}; ++self.revocations; }
}
- (pid_t)connectedClientPID { return 0; }
- (mach_port_t)registrationPort { return 222; }
- (void)close { ++self.closes; }
@end

@interface RecordedIOSurfaceServer : LSBIOSurfaceServer
@property unsigned closes;
@end
@implementation RecordedIOSurfaceServer
- (pid_t)connectedClientPID { return 0; }
- (mach_port_t)registrationPort { return 333; }
- (void)close { ++self.closes; }
@end

// These tests isolate broker ownership and authorization; the native service tests exercise their real implementations.
@interface LSBAudioOutput ()
@property BOOL testValid;
@end
@implementation LSBAudioOutput
- (instancetype)initWithError:(int *)error {
    assert(NSThread.isMainThread);
    ++audioCreates;
    *error = audioCreateError;
    if (*error) return nil;
    if ((self = [super init])) self.testValid = YES;
    return self;
}
- (void *)sharedMemory { assert(self.testValid); return (__bridge void *)self; }
- (void)invalidate { if (_testValid) { ++audioInvalidates; _testValid = NO; } }
- (void)dealloc { [self invalidate]; }
@end

@interface LSBHapticServer ()
@property BOOL testValid, testHasSend;
@property mach_port_t testPort;
@end
@implementation LSBHapticServer
- (instancetype)initWithAuthorization:(BOOL (^)(const audit_token_t *))authorization error:(int *)error {
    assert(NSThread.isMainThread && authorization);
    ++hapticCreates;
    *error = hapticCreateError;
    if (*error) return nil;
    if ((self = [super init])) {
        self.testValid = YES;
        hapticAuthorization = [authorization copy];
        assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &_testPort));
        if (!hapticCopyFailure) {
            assert(!mach_port_insert_right(mach_task_self(), _testPort, _testPort, MACH_MSG_TYPE_MAKE_SEND));
            _testHasSend = YES;
        }
    }
    return self;
}
- (mach_port_t)registrationPort { return self.testValid && !hapticMissingEndpoint ? _testPort : MACH_PORT_NULL; }
- (void)close {
    if (_testValid) {
        ++hapticCloses; _testValid = NO;
        if (_testHasSend) assert(!mach_port_deallocate(mach_task_self(), _testPort));
        _testHasSend = NO;
    }
}
- (void)dealloc {
    [self close];
    if (MACH_PORT_VALID(_testPort)) assert(!mach_port_mod_refs(mach_task_self(), _testPort, MACH_PORT_RIGHT_RECEIVE, -1));
}
@end

int LSBRunningBoardValidateIdentity(const audit_token_t *audit) {
    ++identityChecks;
    bool found = false;
    for (LSBJob *job in jobs) if (audit->val[5] == (unsigned)job.pid && audit->val[7] == job.pidVersion) found = true;
    assert(found);
    return identityCheckError;
}
@interface LSBRunningBoardMonitor : NSObject
@property (nonatomic, copy) LSBRunningBoardExitHandler completion;
@end
@implementation LSBRunningBoardMonitor
@end
int LSBRunningBoardMonitorStart(const audit_token_t *audit, LSBRunningBoardExitHandler handler, LSBRunningBoardMonitor **monitor) {
    assert(audit && handler && monitor);
    ++runningBoardStarts;
    if (runningBoardError) return runningBoardError;
    *monitor = [LSBRunningBoardMonitor new];
    (*monitor).completion = handler;
    return 0;
}
void LSBRunningBoardMonitorCancel(LSBRunningBoardMonitor *monitor) { monitor.completion = nil; ++runningBoardCancels; }

int LSBChildMonitorCreate(pid_t pid, int *descriptor) {
    assert(pid > 0 && descriptor);
    *descriptor = -1;
    if (monitorCreateError) return monitorCreateError;
    *descriptor = kqueue();
    assert(*descriptor >= 0);
    return 0;
}
int LSBChildMonitorRead(int descriptor, pid_t pid, int *status) {
    assert(descriptor >= 0 && pid > 0 && status);
    ++monitorReads;
    *status = monitorWaitStatus;
    return monitorReadError;
}
int LSBChildSignal(const audit_token_t *audit, int signal) {
    assert(audit && signal > 0 && signal < NSIG);
    if (signalHook) signalHook();
    signaledAudit = *audit;
    lastSignal = signal;
    ++deliveredSignals;
    return directSignalError;
}

static void releaseMessageObject(void *pointer) {
    TestMessageObject *object = pointer;
    if (!object) return;
    if (object->retainedFD >= 0) close(object->retainedFD);
    free(object);
}

int LSBLaunchExtensionWithOptions(const char *identifier, uint64_t requestID, uint32_t options, LSBExtensionEvent event, void *context) {
    assert(!strcmp(identifier, LSB_EXTENSION_IDENTIFIER) && requestID && context == NULL);
    submittedLaunchOptions = options;
    ++submittedLaunches;
    extensionCallback = event;
    int error = submitError;
    submitError = 0;
    return error;
}
pid_t LSBExtensionProcessIdentifier(uint64_t requestID) { assert(requestID); return reportedPID; }
void LSBReleaseExtension(uint64_t requestID) { assert(requestID); ++releasedLaunches; }
int LSBSignalExtension(uint64_t requestID, int signal) {
    assert(requestID && signal > 0 && signal < NSIG);
    signaledRequest = requestID;
    lastSignal = signal;
    ++submittedExtensionSignals;
    return extensionSignalError;
}

int LSBTransportReceive(mach_port_t port, LSBReceivedMessage *message) { (void)port; (void)message; return ENOSYS; }
int LSBTransportReply(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *root,
                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]) {
    assert(message && message->object && replyCount < sizeof(replies) / sizeof(replies[0]));
    int sendError = message->kind == LSB_REQUEST_READY ? readySendError : message->kind == LSB_REQUEST_REBOOT_USERSPACE ? rebootSendError :
        message->kind == LSB_REQUEST_COPY_PROCESS_TASK ? taskReplySendError :
        message->kind == LSB_REQUEST_OPEN_AUDIO_OUTPUT ? audioReplySendError : 0;
    if (plan) {
        assert(root && !strcmp(root, runtimeRoot.fileSystemRepresentation) && fds);
        for (unsigned i = 0; i < plan->fdCount; ++i) assert(fcntl(fds[i], F_GETFD) >= 0);
    }
    unsigned identifier = ((TestMessageObject *)message->object)->identifier;
    replies[replyCount++] = (RecordedReply){identifier, message->kind, error, sendError, pid, requestID, plan != NULL, MACH_PORT_NULL, 0, 0, 0, 0};
    releaseMessageObject(message->object);
    message->object = NULL;
    if (message->kind == LSB_REQUEST_READY && readyReplyHook) readyReplyHook();
    return sendError;
}
int LSBTransportReplyAudioOutput(LSBReceivedMessage *message, int error, void *sharedMemory) {
    assert(message->kind == LSB_REQUEST_OPEN_AUDIO_OUTPUT && (error ? !sharedMemory : sharedMemory != NULL));
    audioSharedMemoryReply = error || audioReplySendError ? NULL : sharedMemory;
    return LSBTransportReply(message, error, 0, 0, NULL, NULL, NULL);
}
int LSBTransportReplyEntitlements(LSBReceivedMessage *message, int error, const void *bytes, size_t size) {
    assert(message->kind == LSB_REQUEST_COPY_ENTITLEMENTS);
    assert(error ? !bytes && !size : bytes && size >= 8);
    if (!error && message->entitlementOperation == 5) assert(size == LSB_GUEST_CDHASH_BYTES);
    else if (!error) assert(((const unsigned char *)bytes)[3] == (message->entitlementOperation == 7 ? 0x71 : 0x72));
    entitlementsReply = error ? nil : [NSData dataWithBytes:bytes length:size];
    return LSBTransportReply(message, error, 0, 0, NULL, NULL, NULL);
}
static NSData *processListReply;
int LSBTransportReplyProcessTask(LSBReceivedMessage *message, int error, mach_port_t task, const audit_token_t *audit) {
    assert(message->kind == LSB_REQUEST_COPY_PROCESS_TASK);
    assert(error ? !audit && !MACH_PORT_VALID(task) : audit && audit->val[5] && audit->val[7] && MACH_PORT_VALID(task));
    if (MACH_PORT_VALID(processTaskReply)) mach_port_deallocate(mach_task_self(), processTaskReply);
    processTaskReply = MACH_PORT_NULL;
    memset(&processTaskReplyAudit, 0, sizeof(processTaskReplyAudit));
    if (!error && !taskReplySendError) {
        processTaskReply = task;
        processTaskReplyAudit = *audit;
        assert(!mach_port_mod_refs(mach_task_self(), task, MACH_PORT_RIGHT_SEND, 1));
    }
    return LSBTransportReply(message, error, error ? 0 : message->targetPID, 0, NULL, NULL, NULL);
}
int LSBTransportReplyDebuggerInfo(LSBReceivedMessage *message, int error, const struct kinfo_proc *info) {
    assert(message->kind == LSB_REQUEST_COPY_DEBUGGER_INFO && (error ? info == NULL : info && info->kp_proc.p_pid == message->targetPID));
    memset(&debuggerInfoReply, 0, sizeof(debuggerInfoReply));
    if (info) debuggerInfoReply = *info;
    return LSBTransportReply(message, error, 0, 0, NULL, NULL, NULL);
}
int LSBTransportReplyProcessList(LSBReceivedMessage *message, int error, const LSBProcessList *list, size_t size) {
    assert(message->kind == LSB_REQUEST_LIST_PROCESSES && (error ? !list && !size : !LSBValidateProcessList(list, size)));
    processListReply = error ? nil : [NSData dataWithBytes:list length:size];
    return LSBTransportReply(message, error, 0, 0, NULL, NULL, NULL);
}
int LSBTransportReplyProcessIdentity(LSBReceivedMessage *message, int error, const LSBPeerProcessIdentity *identity) {
    assert(message->kind == LSB_REQUEST_COPY_PROCESS_IDENTITY && (error ? identity == NULL : identity != NULL));
    LSBPeerProcessIdentityDispose(&identityReply);
    if (!error) {
        identityReply = *identity;
        assert(mach_port_mod_refs(mach_task_self(), identityReply.taskName, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS);
    }
    return LSBTransportReply(message, error, 0, 0, NULL, NULL, NULL);
}
int LSBTransportReplyBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *root,
                               const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD) {
    assert(statusFD >= 0 && wakeFD >= 0 && (fcntl(wakeFD, F_GETFL) & O_NONBLOCK));
    struct stat metadata;
    assert(!fstat(statusFD, &metadata) && metadata.st_size == LSB_CHILD_STATUS_MAPPING_SIZE);
    return LSBTransportReply(message, error, pid, requestID, root, plan, fds);
}
int LSBTransportReplyLaunchdBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *root,
                                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, mach_port_t tank) {
    mach_port_type_t type = 0;
    assert(tank && mach_port_type(mach_task_self(), tank, &type) == KERN_SUCCESS && (type & MACH_PORT_TYPE_SEND));
    LSBJob *job = findJob(requestID);
    assert(job && job.simulatorLaunchd && job.launchdSource && job.launchdHandshake);
    assert(tank == LSBLaunchdHandshakeTankPort(job.launchdHandshake) && !job.launchdHandshake->completed);
    assert(!memcmp(&message->audit, &job.launchdHandshake->expectedAudit, sizeof(message->audit)));
    int result = LSBTransportReplyBootstrap(message, error, pid, requestID, root, plan, fds, statusFD, wakeFD);
    replies[replyCount - 1].launchdTank = tank;
    if (launchdBootstrapSendError) replies[replyCount - 1].sendError = result = launchdBootstrapSendError;
    return result;
}
int LSBTransportReplyBootstrapWithMetadata(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *root,
                                           const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD,
                                           const LSBBootstrapMetadata *metadata) {
    assert(metadata && metadata->guestPID > 0 && metadata->parentPID >= 0 && metadata->ports);
    assert(metadata->nativeProductionStatus == (metadata->executionRole == LSB_EXECUTION_SIMULATOR ? testNativeProductionStatus : LSB_NATIVE_PRODUCTION_UNKNOWN));
    if (metadata->executionRole == LSB_EXECUTION_SIMULATOR) {
        LSBJob *job = findJob(requestID);
        assert(job.taskCaptureSource && metadata->taskCapture.port == job.taskCapture.port && MACH_PORT_VALID(metadata->taskCapture.port));
        LSBTaskPortCaptureOffer offer = job.taskCapture;
        assert(!memcmp(metadata->taskCapture.nonce, offer.nonce, sizeof(offer.nonce)));
    } else {
        const unsigned char zero[LSB_TASK_CAPTURE_NONCE_BYTES] = {0};
        assert(!MACH_PORT_VALID(metadata->taskCapture.port) && !memcmp(metadata->taskCapture.nonce, zero, sizeof(zero)));
    }
    if (checkInReplyHook) checkInReplyHook();
    int result;
    if (metadata->launchdTankPort)
        result = LSBTransportReplyLaunchdBootstrap(message, error, pid, requestID, root, plan, fds, statusFD, wakeFD, metadata->launchdTankPort);
    else result = LSBTransportReplyBootstrap(message, error, pid, requestID, root, plan, fds, statusFD, wakeFD);
    replies[replyCount - 1].guestPID = metadata->guestPID;
    replies[replyCount - 1].parentPID = metadata->parentPID;
    replies[replyCount - 1].executionRole = metadata->executionRole;
    replies[replyCount - 1].nativeProductionStatus = metadata->nativeProductionStatus;
    return result;
}
void LSBTransportDisposeMessage(LSBReceivedMessage *message) {
    assert(message);
    releaseMessageObject(message->object);
    message->object = NULL;
    if (MACH_PORT_VALID(message->nativeServicePort)) mach_port_deallocate(mach_task_self(), message->nativeServicePort);
    message->nativeServicePort = MACH_PORT_NULL;
    if (MACH_PORT_VALID(message->processDebuggerPort)) mach_port_deallocate(mach_task_self(), message->processDebuggerPort);
    message->processDebuggerPort = MACH_PORT_NULL;
    LSBPeerProcessIdentityDispose(&message->peerIdentity);
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) {
        if (message->fds[i] >= 0) close(message->fds[i]);
        message->fds[i] = -1;
    }
    ++disposedMessages;
}

static void resetBroker(void) {
    // Invalidate queued timeout/death callbacks from a prior fixture instead of recycling its generation.
    ++rebootGeneration;
    sessionDraining = rebootDrainFailed = NO;
    rebootArguments = rebootEnvironment = nil;
    sessionDeviceType = pendingDeviceType = nil;
    sessionHapticsEnabled = pendingHapticsEnabled = pendingHapticsPreference = sessionAudioEnabled = NO;
    pendingAudioEnabled = pendingAudioPreference = savedAudio = NO;
    savedHaptics = testNativeHaptics = NO;
    hapticsPreferenceWrites = audioPreferenceWrites = 0;
    pendingDeviceCapabilities = nil;
    deviceChangeCompletion = nil;
    savedDeviceIdentifier = nil;
    devicePreferenceWrites = 0;
    LSBLaunchdConfigurationSetDeviceType(nil);
    rebootPendingDeaths = nil;
    for (LSBRunningBoardMonitor *monitor in rebootDeathMonitors) LSBRunningBoardMonitorCancel(monitor);
    rebootDeathMonitors = nil;
    for (dispatch_source_t source in rebootExitSources) dispatch_source_cancel(source);
    rebootExitSources = nil;
    if (adoptionRetrySource) dispatch_source_cancel(adoptionRetrySource);
    adoptionRetrySource = nil;
    pendingAdoptions = [NSMutableArray new];
    for (LSBJob *job in jobs) {
        stopGuestTaskCapture(job);
        if (job.exitMonitor) dispatch_source_cancel(job.exitMonitor);
        job.exitMonitor = nil;
        if (job.launchdSource) dispatch_source_cancel(job.launchdSource);
        job.launchdSource = nil;
        if (job.runningBoardMonitor) LSBRunningBoardMonitorCancel(job.runningBoardMonitor);
        job.runningBoardMonitor = nil;
        [job.indigo close];
        [job.framebuffer close];
        [job.iosurface close];
        [job.haptics close]; job.haptics = nil;
        [job.audioOutput invalidate]; job.audioOutput = nil;
        if (job.request) disposeMessage(job.request);
        job.request = NULL;
    }
    startingJob = nil;
    jobs = [NSMutableArray new];
    for (NSNumber *endpoint in publicTestEndpoints)
        assert(!mach_port_mod_refs(mach_task_self(), endpoint.unsignedIntValue, MACH_PORT_RIGHT_RECEIVE, -1));
    publicTestEndpoints = [NSMutableArray new];
    runtimeRoot = @"/tmp/RuntimeRoot";
    [hostTerminal close]; hostTerminal = nil;
    initialTerminalPending = NO;
    initialNativeConfigdPending = NO;
    nextIdentifier = 1;
    launchLaneFailed = NO;
    enableIndigo = NO;
    enableSpringBoardMetal = NO;
    retiredIdentityCount = auditResolutionLogs = peerIdentityLogs = guestEntitlementErrorLogs = guestProxySigningErrorLogs = 0;
    retiredIdentitiesFull = NO;
    memset(retiredIdentities, 0, sizeof(retiredIdentities));
    readyReplyHook = nil;
    checkInReplyHook = nil;
    signalHook = nil;
    endpointHook = nil;
    entitlementsReply = nil;
    LSBPeerProcessIdentityDispose(&identityReply);
    useRealTaskInfo = preserveCheckInIdentity = mismatchTaskAudit = NO;
    taskInfoError = KERN_SUCCESS;
    automaticTaskCapture = YES;
    taskCaptureCreateError = taskCaptureReceiveError = taskReplySendError = 0;
    taskCaptureCreates = taskCaptureReceives = 0;
    if (MACH_PORT_VALID(processTaskReply)) mach_port_deallocate(mach_task_self(), processTaskReply);
    processTaskReply = MACH_PORT_NULL;
    memset(&processTaskReplyAudit, 0, sizeof(processTaskReplyAudit));
    debuggerQueryCalls = 0;
    debuggerQueryError = 0;
    debuggerQueryHook = nil;
    xmlConversionError = 0;
    codeDirectoryHash = nil;
    codeDirectoryHashError = 0;
    afterCodeDirectoryRead = nil;
    endpointCalls = 0;
    nativeConfigdLookupCalls = 0;
    nativeConfigdLookupError = KERN_SUCCESS;
    nativeConfigdEndpoint = MACH_PORT_NULL;
    unsetenv("LSB_EXPERIMENTAL_HAPTICS");
    unsetenv("LSB_AUDIO_OUTPUT");
    hapticCreates = hapticCloses = audioCreates = audioInvalidates = 0;
    hapticCreateError = audioCreateError = audioReplySendError = 0;
    hapticMissingEndpoint = hapticCopyFailure = NO;
    hapticAuthorization = nil;
    audioSharedMemoryReply = NULL;
    unregisterCalls = unregisterTimeout = 0;
    unregisterError = 0;
    memset(unregisteredEndpoint, 0, sizeof(unregisteredEndpoint));
    memset(&unregisterAudit, 0, sizeof(unregisterAudit));
    memset(endpointErrors, 0, sizeof(endpointErrors));
    memset(registeredEndpoints, 0, sizeof(registeredEndpoints));
    memset(registeredPorts, 0, sizeof(registeredPorts));
    memset(registeredAudits, 0, sizeof(registeredAudits));
    memset(registeredTimeouts, 0, sizeof(registeredTimeouts));
    reportedPID = getpid();
    submitError = readySendError = rebootSendError = launchdBootstrapSendError = monitorCreateError = identityCheckError = 0;
    runningBoardError = EACCES;
    monitorReadError = EAGAIN;
    monitorWaitStatus = 0;
    monitorReads = 0;
    deliveredSignals = identityChecks = finishingDepth = 0;
    runningBoardStarts = runningBoardCancels = 0;
    submittedExtensionSignals = 0;
    signaledRequest = 0;
    directSignalError = extensionSignalError = lastSignal = 0;
    memset(&signaledAudit, 0, sizeof(signaledAudit));
    replyCount = disposedMessages = submittedLaunches = releasedLaunches = 0;
    memset(replies, 0, sizeof(replies));
}

static LSBReceivedMessage *messageFor(uint32_t kind, pid_t pid, uint32_t generation, uint64_t requestID) {
    LSBReceivedMessage *message = calloc(1, sizeof(*message));
    assert(message);
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) message->fds[i] = -1;
    TestMessageObject *object = malloc(sizeof(*object));
    assert(object);
    *object = (TestMessageObject){++objectSequence, -1};
    message->object = object;
    message->kind = kind;
    message->requestID = requestID;
    message->audit.val[5] = (unsigned)pid;
    message->audit.val[7] = generation;
    if (kind == LSB_REQUEST_READY) {
        LSBJob *job = findJob(requestID);
        if (job && job.pid == pid && job.pidVersion == generation) message->audit = job.audit;
    }
    if (kind == LSB_REQUEST_CHECKIN) {
        assert(task_name_for_pid(mach_task_self(), getpid(), &message->peerIdentity.taskName) == KERN_SUCCESS);
        message->peerIdentity.audit = message->audit;
        memcpy(message->peerIdentity.shortInfo, &message->audit.val[5], sizeof(uint32_t));
    }
    return message;
}

static LSBReceivedMessage *spawnRequest(int *ownedFD, unsigned *objectID) {
    LSBReceivedMessage *message = messageFor(LSB_REQUEST_SPAWN, getpid() + 100, 8, 0);
    message->plan.version = LSB_PLAN_VERSION;
    message->plan.argc = 1;
    assert(!LSBPlanAddString(&message->plan, "/bin/df", &message->plan.path));
    message->plan.argv[0] = message->plan.path;
    assert(!LSBPlanAddString(&message->plan, "/tmp/RuntimeRoot", &message->plan.cwd));
    message->plan.fdCount = 1;
    message->plan.fds[0].number = STDOUT_FILENO;
    message->fds[0] = open("/dev/null", O_RDWR);
    assert(message->fds[0] >= 0);
    *ownedFD = message->fds[0];
    *objectID = ((TestMessageObject *)message->object)->identifier;
    return message;
}

static RecordedReply *replyFor(unsigned objectID) {
    for (unsigned i = 0; i < replyCount; ++i) if (replies[i].objectID == objectID) return &replies[i];
    return NULL;
}
static unsigned planReplies(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < replyCount; ++i) if (replies[i].deliveredPlan) ++count;
    return count;
}
static void assertClosed(int fd) { errno = 0; assert(fcntl(fd, F_GETFD) < 0 && errno == EBADF); }

static void testIdentityAndSuccessfulHandoff(void) {
    resetBroker();
    int fd;
    unsigned parentObject;
    enqueueSpawn(spawnRequest(&fd, &parentObject), nil);
    assert(startingJob && startingJob.phase == JobStarting && submittedLaunches == 1);
    uint64_t identifier = startingJob.identifier;
    reportedPID = 0;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    assert(replies[replyCount - 1].error == EAGAIN && startingJob.phase == JobStarting && !planReplies());
    reportedPID = getpid();
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid() + 1, 3, 0));
    assert(replies[replyCount - 1].error == EACCES && !planReplies() && !startingJob.pid);
    assert(fcntl(fd, F_GETFD) >= 0);
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    assert(planReplies() == 1 && startingJob.phase == JobDispatched && startingJob.pidVersion == 3);
    assertClosed(fd);
    assert(!replyFor(parentObject));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 4, identifier));
    assert(replies[replyCount - 1].error == ESRCH && replies[replyCount - 1].requestID == identifier && !replyFor(parentObject));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 3, identifier));
    assert(!startingJob && jobs.count == 1 && jobs[0].phase == JobRunning && jobs[0].request == NULL);
    RecordedReply *reply = replyFor(parentObject);
    assert(reply && !reply->error && reply->pid == getpid());
    assert(disposedMessages == 6);
}

static void testReadySendFailure(void) {
    resetBroker();
    int fd;
    unsigned parentObject;
    enqueueSpawn(spawnRequest(&fd, &parentObject), nil);
    uint64_t identifier = startingJob.identifier;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    readySendError = EPIPE;
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 3, identifier));
    RecordedReply *reply = replyFor(parentObject);
    assert(reply && reply->error == EPIPE && !reply->pid && !jobs.count && !startingJob && !launchLaneFailed);
    assertClosed(fd);
}

static void testChildReadyFailure(int childError) {
    resetBroker();
    int firstFD, nextFD, pipeFDs[2];
    unsigned parentObject, nextObject;
    LSBReceivedMessage *request = spawnRequest(&firstFD, &parentObject);
    assert(!pipe(pipeFDs));
    close(firstFD);
    request->fds[0] = pipeFDs[1];
    // An XPC FD object retains its own writer even after the broker closes the imported descriptor.
    TestMessageObject *object = request->object;
    object->retainedFD = dup(pipeFDs[1]);
    assert(object->retainedFD >= 0 && !fcntl(pipeFDs[0], F_SETFL, O_NONBLOCK));
    enqueueSpawn(request, nil);
    uint64_t identifier = startingJob.identifier;
    enqueueSpawn(spawnRequest(&nextFD, &nextObject), nil);
    assert(jobs.count == 2 && submittedLaunches == 1);

    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    assert(startingJob.phase == JobDispatched && startingJob.request->fds[0] == -1 && !replyFor(parentObject));
    char byte;
    errno = 0;
    assert(read(pipeFDs[0], &byte, 1) < 0 && errno == EAGAIN);
    LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, getpid(), 3, identifier);
    unsigned readyObject = ((TestMessageObject *)ready->object)->identifier;
    ready->error = childError;
    handleMessage(ready);

    RecordedReply *parentReply = replyFor(parentObject), *childReply = replyFor(readyObject);
    assert(parentReply && parentReply->error == childError && !parentReply->pid && parentReply->requestID == identifier);
    assert(childReply && !childReply->error && !childReply->sendError && childReply->requestID == identifier);
    assert(disposedMessages == 3 && releasedLaunches == 1 && !findJob(identifier) && !launchLaneFailed);
    assert(read(pipeFDs[0], &byte, 1) == 0);
    close(pipeFDs[0]);

    assert(jobs.count == 1 && startingJob && startingJob.phase == JobStarting && startingJob.identifier != identifier);
    assert(submittedLaunches == 2 && !replyFor(nextObject) && fcntl(nextFD, F_GETFD) >= 0);
    uint64_t nextIdentifier = startingJob.identifier;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 4, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 4, nextIdentifier));
    RecordedReply *nextReply = replyFor(nextObject);
    assert(nextReply && !nextReply->error && nextReply->pid == getpid());
    assert(jobs.count == 1 && jobs[0].phase == JobRunning && !startingJob && !launchLaneFailed);
    assertClosed(nextFD);
}

static void testUnboundFailureStopsQueue(void) {
    resetBroker();
    int firstFD, secondFD, rejectedFD;
    unsigned firstObject, secondObject, rejectedObject;
    reportedPID = 0;
    enqueueSpawn(spawnRequest(&firstFD, &firstObject), nil);
    uint64_t identifier = startingJob.identifier;
    enqueueSpawn(spawnRequest(&secondFD, &secondObject), nil);
    assert(jobs.count == 2 && submittedLaunches == 1);
    extensionCallback(identifier, 0, ETIMEDOUT, NULL);
    assert(launchLaneFailed && !startingJob && !jobs.count && submittedLaunches == 1);
    assert(replyFor(firstObject)->error == ETIMEDOUT && replyFor(secondObject)->error == EBUSY);
    assertClosed(firstFD);
    assertClosed(secondFD);
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    assert(replies[replyCount - 1].error == ESRCH && !planReplies());
    enqueueSpawn(spawnRequest(&rejectedFD, &rejectedObject), nil);
    assert(replyFor(rejectedObject)->error == EBUSY && submittedLaunches == 1);
    assertClosed(rejectedFD);
}

static void testSubmissionFailureAndSpawnAuthentication(void) {
    resetBroker();
    int fd;
    unsigned objectID;
    submitError = ENOENT;
    enqueueSpawn(spawnRequest(&fd, &objectID), nil);
    assert(!launchLaneFailed && !jobs.count && replyFor(objectID)->error == ENOENT);
    assertClosed(fd);

    LSBJob *parent = [LSBJob new];
    parent.identifier = 99;
    parent.pid = getpid() + 100;
    parent.pidVersion = 8;
    parent.phase = JobRunning;
    assert(!prepareChildStatus(parent));
    [jobs addObject:parent];
    LSBReceivedMessage *wrongGeneration = spawnRequest(&fd, &objectID);
    wrongGeneration->audit.val[7] = 9;
    handleMessage(wrongGeneration);
    assert(replyFor(objectID)->error == EACCES && jobs.count == 1);
    assertClosed(fd);
    handleMessage(spawnRequest(&fd, &objectID));
    assert(startingJob && jobs.count == 2 && submittedLaunches == 2 && !replyFor(objectID));
    finishJob(startingJob, ECANCELED);
    assert(replyFor(objectID)->error == ECANCELED);
    assertClosed(fd);
}

static void testInitialGuestArguments(void) {
    resetBroker();
    char directory[] = "/tmp/LiveSimBooter-terminal-test.XXXXXX";
    assert(mkdtemp(directory));
    runtimeRoot = [[NSString stringWithUTF8String:directory] stringByAppendingPathComponent:@"RuntimeRoot"];
    const char *environment[] = { "LSB_TEST=preserved", NULL };
    const char *defaultArguments[] = { "LiveSimBooter", "--terminal", NULL };
    const char *customArguments[] = { "LiveSimBooter", "/bin/df", "-h", NULL };
    for (unsigned custom = 0; custom < 2; ++custom) {
        LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
        assert(initial);
        assert(!initialPlan(initial, custom ? 3 : 2, custom ? customArguments : defaultArguments, environment));
        LSBSpawnPlan *plan = &initial->plan;
        assert(plan->argc == (custom ? 2 : 6) && plan->envc == (custom ? 1 : 5));
        assert(!strcmp(plan->strings + plan->path, custom ? "/bin/df" : "/bin/fish"));
        assert(!strcmp(plan->strings + plan->argv[0], plan->strings + plan->path));
        if (custom) assert(!strcmp(plan->strings + plan->argv[1], "-h"));
        else {
            assert(hostTerminal && plan->fdCount == 3);
            struct stat first, other;
            assert(!fstat(initial->fds[0], &first) && S_ISSOCK(first.st_mode));
            for (unsigned i = 1; i < 3; ++i) {
                assert(!fstat(initial->fds[i], &other) && first.st_dev == other.st_dev && first.st_ino == other.st_ino);
            }
            assert(!strcmp(plan->strings + plan->argv[1], "-i"));
            NSString *path = [NSString stringWithFormat:@"PATH=%@/usr/local/bin:%@/bin:%@/usr/bin", runtimeRoot, runtimeRoot, runtimeRoot];
            assert(!strcmp(plan->strings + plan->envp[3], path.UTF8String));
            assert(!strncmp(plan->strings + plan->envp[4], LSB_FAKE_PTY_ENV "=", sizeof(LSB_FAKE_PTY_ENV)));
        }
        assert(!strcmp(plan->strings + plan->envp[0], "LSB_TEST=preserved"));
        disposeMessage(initial);
        [hostTerminal close]; hostTerminal = nil;
    }
    assert([NSFileManager.defaultManager removeItemAtPath:[NSString stringWithUTF8String:directory] error:nil]);
}

static void testNativeProbeRoleIsHostOnly(void) {
    const char *arguments[] = { "LiveSimBooter", "--native-metal-probe", NULL };
    const char *extra[] = { "LiveSimBooter", "--native-metal-probe", "/untrusted/backend.dylib", NULL };
    const char *guestPath[] = { "LiveSimBooter", "/__LiveSimBooterNativeMetalProbe", NULL };
    const char *environment[] = { "LSB_EXECUTION_ROLE=native-metal-probe", "DYLD_INSERT_LIBRARIES=/untrusted/backend.dylib", NULL };
    uint32_t role = UINT32_MAX;
    assert(!initialExecutionRole(2, arguments, &role) && role == LSB_EXECUTION_NATIVE_METAL_PROBE);
    assert(initialExecutionRole(3, extra, &role) == EINVAL && role == LSB_EXECUTION_SIMULATOR);
    assert(!initialExecutionRole(2, guestPath, &role) && role == LSB_EXECUTION_SIMULATOR);
    for (unsigned failed = 0; failed < 2; ++failed) {
        resetBroker();
        LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
        assert(initial && !initialPlan(initial, 2, arguments, environment));
        assert(initial->plan.argc == 1 && !initial->plan.envc && !initial->plan.actionCount);
        assert(!strcmp(initial->plan.strings + initial->plan.path, "/__LiveSimBooterNativeMetalProbe"));
        enqueueJob(initial, nil, LSB_EXECUTION_NATIVE_METAL_PROBE);
        assert(startingJob && startingJob.executionRole == LSB_EXECUTION_NATIVE_METAL_PROBE && !startingJob.simulatorLaunchd);
        assert(submittedLaunchOptions == LSB_EXTENSION_LAUNCH_GPU);
        uint64_t identifier = startingJob.identifier;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid() + 1, 3, 0));
        assert(replies[replyCount - 1].error == EACCES && !planReplies());
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
        assert(replies[replyCount - 1].executionRole == LSB_EXECUTION_NATIVE_METAL_PROBE && !replies[replyCount - 1].launchdTank);
        assert(!taskCaptureCreates && !taskCaptureReceives && !MACH_PORT_VALID(startingJob.taskCapture.port));
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, getpid(), 3, identifier);
        ready->error = failed ? ENOENT : 0;
        handleMessage(ready);
        if (failed) { assert(!jobs.count && !startingJob); continue; }
        assert(jobs.count == 1 && jobs[0].phase == JobRunning && !startingJob);
        for (unsigned kind = LSB_REQUEST_SPAWN; kind <= LSB_REQUEST_EXEC; ++kind) {
            if (kind != LSB_REQUEST_SPAWN && kind != LSB_REQUEST_EXEC && kind != LSB_REQUEST_KILL) continue;
            LSBReceivedMessage *request = messageFor(kind, getpid(), 3, 0);
            handleMessage(request);
            assert(replies[replyCount - 1].error == EACCES && jobs.count == 1 && submittedLaunches == 1);
        }
    }
    resetBroker();
    int fd;
    unsigned object;
    enqueueJob(spawnRequest(&fd, &object), nil, LSB_EXECUTION_NATIVE_METAL_PROBE);
    assert(!jobs.count && replyFor(object)->error == EACCES);
    assertClosed(fd);
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    assert(!LSBPlanAddString(&request->plan, "/__LiveSimBooterNativeMetalProbe", &request->plan.path));
    request->plan.argv[0] = request->plan.path;
    assert(!LSBPlanAddString(&request->plan, environment[0], &request->plan.envp[request->plan.envc++]));
    enqueueSpawn(request, nil);
    assert(startingJob && startingJob.executionRole == LSB_EXECUTION_SIMULATOR);
    assert(!submittedLaunchOptions);
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
    assert(replies[replyCount - 1].executionRole == LSB_EXECUTION_SIMULATOR);
}

static void testProductionStatusIsHostOnly(void) {
    for (uint32_t status = LSB_NATIVE_PRODUCTION_UNKNOWN; status <= LSB_NATIVE_DEVELOPMENT; ++status) {
        resetBroker();
        testNativeProductionStatus = status;
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        const char *spoof[] = {"EffectiveProductionStatusAp=0", "LSB_NATIVE_PRODUCTION_STATUS=2", LSB_RETAIN_HOST_CACHE_ENV "=0"};
        for (unsigned index = 0; index < sizeof(spoof) / sizeof(*spoof); ++index)
            assert(!LSBPlanAddString(&request->plan, spoof[index], &request->plan.envp[request->plan.envc++]));
        enqueueSpawn(request, nil);
        assert(startingJob);
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 3, 0));
        assert(replies[replyCount - 1].nativeProductionStatus == status);
        assert(replies[replyCount - 1].executionRole == LSB_EXECUTION_SIMULATOR);
    }
    resetBroker();
    testNativeProductionStatus = LSB_NATIVE_DEVELOPMENT;
}

static LSBJob *runTestJob(LSBJob *parent, uint32_t generation) {
    int fd;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    enqueueSpawn(request, parent);
    assert(startingJob);
    LSBJob *job = startingJob;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), generation, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), generation, job.identifier));
    assert(job.phase == JobRunning && !startingJob && replyFor(object) && !replyFor(object)->error);
    return job;
}

static LSBJob *auditJob(pid_t nativePID, pid_t guestPID, unsigned generation, BOOL launchd) {
    LSBJob *job = [LSBJob new];
    job.identifier = nextIdentifier++;
    job.pid = nativePID;
    job.guestPID = guestPID;
    job.pidVersion = generation;
    job.audit = (audit_token_t){{1, 2, 3, 4, 5, (unsigned)nativePID, 7, generation}};
    job.phase = JobRunning;
    job.simulatorLaunchd = launchd;
    [jobs addObject:job];
    return job;
}

static RecordedReply *auditReply(audit_token_t caller, audit_token_t target) {
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_RESOLVE_AUDIT_PID, caller.val[5], caller.val[7], 0);
    request->audit = caller;
    request->targetAudit = target;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    RecordedReply *reply = replyFor(object);
    assert(reply && reply->kind == LSB_REQUEST_RESOLVE_AUDIT_PID && !reply->requestID && !reply->deliveredPlan);
    return reply;
}

static void testAuditOwnerAuthenticationAndIdentity(void) {
    resetBroker();
    LSBJob *launchd = auditJob(8100, 8100, 11, YES), *target = auditJob(8200, 8150, 12, NO);
    audit_token_t targetAudit = target.audit, caller = launchd.audit;
    RecordedReply *reply = auditReply(caller, targetAudit);
    assert(!reply->error && reply->pid == target.guestPID);
    assert(auditReply(targetAudit, targetAudit)->error == EACCES);
    for (unsigned index = 0; index < 8; ++index) {
        audit_token_t changed = caller;
        changed.val[index] ^= 1;
        assert(auditReply(changed, targetAudit)->error == EACCES);
        if (index == 5) continue; // An unrelated native PID is not a managed target; trusted launchd supplies its actual audit token.
        changed = targetAudit;
        changed.val[index] ^= 1;
        assert(auditReply(caller, changed)->error == ESRCH);
    }
    launchd.phase = JobDispatched;
    assert(auditReply(caller, targetAudit)->error == EACCES);
    launchd.phase = JobRunning;
    launchd.retiredForExec = YES;
    assert(auditReply(caller, targetAudit)->error == EACCES);
    launchd.retiredForExec = NO;
    target.phase = JobDispatched;
    assert(auditReply(caller, targetAudit)->error == ESRCH);
    target.phase = JobRunning;
    target.retiredForExec = YES;
    assert(auditReply(caller, targetAudit)->error == ESRCH);
    target.retiredForExec = NO;
    audit_token_t unknown = {{8, 7, 6, 5, 4, 8300, 2, 1}};
    reply = auditReply(caller, unknown);
    assert(!reply->error && reply->pid == 8300);
    unknown.val[5] = target.guestPID;
    assert(auditReply(caller, unknown)->error == ESRCH); // Native fallback cannot become a guest job by colliding with its stable PID.
    unknown.val[5] = 0;
    assert(auditReply(caller, unknown)->error == EINVAL);
    unknown.val[5] = UINT32_MAX;
    assert(auditReply(caller, unknown)->error == EINVAL);
    finishJob(target, 0);
    assert(retiredIdentityCount == 1 && !findJob(target.identifier));
    assert(auditReply(caller, targetAudit)->error == ESRCH);
    unknown.val[5] = target.guestPID;
    assert(auditReply(caller, unknown)->error == ESRCH);
    LSBJob *reused = auditJob(target.pid, 8400, 13, NO);
    reply = auditReply(caller, reused.audit);
    assert(!reply->error && reply->pid == 8400); // A newly committed exact identity remains usable even when its native PID was retired.
    assert(auditReply(caller, targetAudit)->error == ESRCH);
}

static void testAuditIdentityHistoryNeverEvicts(void) {
    resetBroker();
    LSBJob *launchd = auditJob(8100, 8100, 11, YES), *current = auditJob(8200, 8150, 12, NO);
    LSBJob *old = [LSBJob new];
    for (unsigned index = 0; index < RetiredIdentityCapacity + 3; ++index) {
        old.pid = 9000 + index;
        old.guestPID = 10000 + index;
        old.audit = (audit_token_t){{1, 2, 3, 4, 5, (unsigned)old.pid, 7, 200 + index}};
        retireAuditIdentity(old);
    }
    assert(retiredIdentitiesFull && retiredIdentityCount == RetiredIdentityCapacity);
    assert(retiredIdentities[0].audit.val[5] == 9000);
    assert(retiredIdentities[RetiredIdentityCapacity - 1].audit.val[5] == 9000 + RetiredIdentityCapacity - 1);
    audit_token_t unknown = {{1, 2, 3, 4, 5, 20000, 7, 30}};
    assert(auditReply(launchd.audit, unknown)->error == ESRCH);
    assert(auditReply(launchd.audit, old.audit)->error == ESRCH); // Even an identity beyond the bound cannot escape through fallback.
    RecordedReply *reply = auditReply(launchd.audit, current.audit);
    assert(!reply->error && reply->pid == current.guestPID);
}

static void testExitStatusesAndRepeatedReaping(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 10);
    parent.audit = (audit_token_t){{1, 2, 3, 4, 5, (unsigned)getpid(), 7, 10}};
    for (unsigned iteration = 0; iteration < LSB_CHILD_STATUS_CAPACITY + 3; ++iteration) {
        LSBJob *child = runTestJob(parent, 11 + iteration);
        pid_t waited = -1;
        int status = -1;
        assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &waited, &status) == EAGAIN);
        int expected = iteration % 2 ? SIGTERM : (42 << 8);
        recordChildExit(child.identifier, expected);
        assert(!findJob(child.identifier) && deliveredSignals == iteration + 1);
        audit_token_t expectedAudit = parent.audit;
        assert(!memcmp(&signaledAudit, &expectedAudit, sizeof(signaledAudit)));
        char hint;
        assert(read(parent.wakeReadFD, &hint, 1) == 1);
        assert(!LSBChildStatusTryWait(parent.childStatus, child.pid, getpgrp(), &waited, &status));
        assert(waited == child.pid && status == expected);
        assert(iteration % 2 ? WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM : WIFEXITED(status) && WEXITSTATUS(status) == 42);
        assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &waited, &status) == ECHILD);
    }
    recordChildExit(parent.identifier, 0);
    assert(!jobs.count);
}

static void testExecReplacement(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 100);
    LSBJob *old = runTestJob(parent, 101);
    LSBJob *grandchild = runTestJob(old, 102);
    pid_t guestPID = old.guestPID;
    LSBChildTicket ticket = old.parentTicket;
    int fd;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    request->kind = LSB_REQUEST_EXEC;
    request->audit = old.audit;
    handleMessage(request);
    LSBJob *replacement = startingJob;
    assert(replacement && replacement != old && replacement.guestPID == guestPID && old.replacementIdentifier == replacement.identifier);
    assert(replacement.childStatus != old.childStatus);
    LSBChildObservation observation;
    assert(!LSBChildStatusObserve(replacement.childStatus, grandchild.guestPID, &observation));
    assert(!observation.execGeneration && !observation.exited);
    reportedPID = getpid() + 200;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 103, 0));
    assert(replies[replyCount - 1].guestPID == guestPID && replies[replyCount - 1].parentPID == parent.guestPID);
    handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 103, replacement.identifier));
    assert(!findJob(old.identifier) && findJob(replacement.identifier) && !startingJob);
    assert(replacement.parentIdentifier == parent.identifier && grandchild.parentIdentifier == replacement.identifier);
    assert(replacement.parentTicket.index == ticket.index && replacement.parentTicket.generation == ticket.generation);
    assert(replyFor(object) && !replyFor(object)->error && replyFor(object)->pid == guestPID);
    assert(lastSignal == SIGKILL && signaledAudit.val[7] == 101);
    assert(!LSBChildStatusObserve(parent.childStatus, guestPID, &observation));
    assert(observation.nativePID == reportedPID && observation.execGeneration == 1 && !observation.exited);
    assert(!signalChild(parent, guestPID, SIGTERM) && signaledAudit.val[5] == (unsigned)reportedPID && signaledAudit.val[7] == 103);
    pid_t waited = -1;
    int status = -1;
    assert(LSBChildStatusTryWait(parent.childStatus, guestPID, getpgrp(), &waited, &status) == EAGAIN);
    recordChildExit(old.identifier, SIGKILL); // A queued notification from the retired native instance is ignored.
    assert(LSBChildStatusTryWait(parent.childStatus, guestPID, getpgrp(), &waited, &status) == EAGAIN);
    recordChildExit(replacement.identifier, 23 << 8);
    assert(!LSBChildStatusTryWait(parent.childStatus, guestPID, getpgrp(), &waited, &status) && waited == guestPID && status == (23 << 8));
    assert(grandchild.parentIdentifier == 0);
}

static void testExecFailurePreservesCaller(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 110);
    LSBJob *old = runTestJob(parent, 111);
    for (unsigned failReply = 0; failReply < 2; ++failReply) {
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        request->kind = LSB_REQUEST_EXEC;
        request->audit = old.audit;
        handleMessage(request);
        LSBJob *replacement = startingJob;
        reportedPID = getpid() + 200;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 112 + failReply, 0));
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, reportedPID, 112 + failReply, replacement.identifier);
        if (failReply) readySendError = EPIPE;
        else ready->error = ENOEXEC;
        handleMessage(ready);
        readySendError = 0;
        assert(findJob(old.identifier) && !old.replacementIdentifier && !findJob(replacement.identifier));
        assert(!deliveredSignals && replyFor(object)->error == (failReply ? EPIPE : ENOEXEC));
        LSBChildObservation observation;
        assert(!LSBChildStatusObserve(parent.childStatus, old.guestPID, &observation));
        assert(!observation.execGeneration && !observation.exited && observation.nativePID == old.pid);
    }
}

static void setRequestProgram(LSBReceivedMessage *request, const char *program) {
    assert(!LSBPlanAddString(&request->plan, program, &request->plan.path));
    request->plan.argv[0] = request->plan.path;
}

static void testGraphicsRegistrationGatesExecOnly(void) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        resetBroker();
        LSBJob *parent = runTestJob(nil, 200);
        parent.simulatorLaunchd = YES;
        RecordedIndigoServer *indigo = [RecordedIndigoServer new];
        RecordedFramebufferServer *framebuffer = [RecordedFramebufferServer new];
        RecordedIOSurfaceServer *iosurface = [RecordedIOSurfaceServer new];
        parent.indigo = indigo;
        parent.framebuffer = framebuffer;
        parent.iosurface = iosurface;
        parent.servicesPending = YES;
        int fd;
        unsigned proxyObject;
        LSBReceivedMessage *request = spawnRequest(&fd, &proxyObject);
        setRequestProgram(request, "/usr/libexec/xpcproxy_sim");
        unsigned launches = submittedLaunches;
        enqueueSpawn(request, parent);
        LSBJob *proxy = startingJob;
        // Launchd cannot answer endpoint-registration requests until its initial spawn returns from READY.
        assert(proxy && proxy.phase == JobStarting && submittedLaunches == launches + 1 && parent.servicesPending);
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 201, 0));
        handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 201, proxy.identifier));
        assert(proxy.phase == JobRunning && !startingJob && replyFor(proxyObject) && !replyFor(proxyObject)->error);
        assert(!indigo.authorizations && !parent.indigoAuthorizedIdentifier);
        assert(!framebuffer.authorizations && !parent.framebufferAuthorizedIdentifier);

        unsigned execObject;
        request = spawnRequest(&fd, &execObject);
        request->kind = LSB_REQUEST_EXEC;
        request->audit = proxy.audit;
        setRequestProgram(request, scenario == 1 ? "/tmp/RuntimeRoot/usr/libexec/backboardd" : "/usr/libexec/backboardd");
        handleMessage(request);
        LSBJob *replacement = findJob(proxy.replacementIdentifier);
        assert(replacement && replacement.phase == JobQueued && !startingJob && submittedLaunches == launches + 1);
        assert(!replyFor(execObject) && fcntl(fd, F_GETFD) >= 0 && !indigo.authorizations);
        startNextJob();
        assert(!startingJob && replacement.phase == JobQueued && !replyFor(execObject));

        parent.servicesPending = NO;
        parent.servicesError = scenario == 2 ? EACCES : 0;
        startNextJob();
        if (parent.servicesError) {
            assert(replyFor(execObject) && replyFor(execObject)->error == EACCES && !replyFor(execObject)->pid);
            assert(!findJob(replacement.identifier) && !proxy.replacementIdentifier && findJob(proxy.identifier));
            assert(submittedLaunches == launches + 1 && !indigo.authorizations && !indigo.revocations && !deliveredSignals);
            assert(!framebuffer.authorizations && !framebuffer.revocations);
            assertClosed(fd);
            continue;
        }
        assert(startingJob == replacement && replacement.phase == JobStarting && submittedLaunches == launches + 2);
        reportedPID = getpid() + 200;
        LSBReceivedMessage *checkIn = messageFor(LSB_REQUEST_CHECKIN, reportedPID, 202, 0);
        checkIn->audit.val[0] = 91;
        checkIn->audit.val[6] = 92;
        audit_token_t identity = checkIn->audit;
        handleMessage(checkIn);
        audit_token_t authorized = indigo.authorizedAudit;
        assert(indigo.authorizations == 1 && !indigo.revocations && !memcmp(&authorized, &identity, sizeof(identity)));
        assert(parent.indigoAuthorizedIdentifier == replacement.identifier && !indigo.connectedClientPID);
        audit_token_t framebufferAudit = framebuffer.authorizedAudit;
        assert(framebuffer.authorizations == 1 && !framebuffer.revocations && !memcmp(&framebufferAudit, &identity, sizeof(identity)));
        assert(parent.framebufferAuthorizedIdentifier == replacement.identifier && !framebuffer.connectedClientPID);
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, reportedPID, 202, replacement.identifier);
        ready->audit = identity;
        ready->error = scenario == 3 ? ENOEXEC : 0;
        handleMessage(ready);
        if (scenario == 3) {
            assert(replyFor(execObject)->error == ENOEXEC && findJob(proxy.identifier) && !proxy.replacementIdentifier);
            assert(!findJob(replacement.identifier) && indigo.revocations == 1 && !parent.indigoAuthorizedIdentifier);
            assert(framebuffer.revocations == 1 && !parent.framebufferAuthorizedIdentifier);
            assert(!deliveredSignals);
            continue;
        }
        assert(!findJob(proxy.identifier) && replacement.phase == JobRunning && !replyFor(execObject)->error);
        // Retiring xpcproxy must not revoke the newly authorized replacement.
        assert(indigo.authorizations == 1 && !indigo.revocations && parent.indigoAuthorizedIdentifier == replacement.identifier);
        assert(framebuffer.authorizations == 1 && !framebuffer.revocations && parent.framebufferAuthorizedIdentifier == replacement.identifier);
        recordChildExit(proxy.identifier, SIGKILL);
        assert(!indigo.revocations);
        assert(!framebuffer.revocations);
        // Death before the first Indigo connection still revokes this exact native identity.
        assert(!indigo.connectedClientPID);
        recordChildExit(replacement.identifier, SIGTERM);
        authorized = indigo.authorizedAudit;
        audit_token_t empty = {{0}};
        assert(indigo.revocations == 1 && !memcmp(&authorized, &empty, sizeof(empty)) && !parent.indigoAuthorizedIdentifier);
        framebufferAudit = framebuffer.authorizedAudit;
        assert(framebuffer.revocations == 1 && !memcmp(&framebufferAudit, &empty, sizeof(empty)) && !parent.framebufferAuthorizedIdentifier);
        recordChildExit(replacement.identifier, SIGTERM);
        assert(indigo.revocations == 1);
        assert(framebuffer.revocations == 1);
    }
}

static void testGraphicsOwnerExecIsRejectedWithoutChangingAuthorization(void) {
  for (unsigned servers = 1; servers <= 3; ++servers) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 210);
    parent.simulatorLaunchd = YES;
    RecordedIndigoServer *indigo = (servers & 1) ? [RecordedIndigoServer new] : nil;
    RecordedFramebufferServer *framebuffer = (servers & 2) ? [RecordedFramebufferServer new] : nil;
    RecordedIOSurfaceServer *iosurface = [RecordedIOSurfaceServer new];
    parent.indigo = indigo;
    parent.framebuffer = framebuffer;
    parent.iosurface = iosurface;
    int fd;
    unsigned firstObject;
    LSBReceivedMessage *request = spawnRequest(&fd, &firstObject);
    setRequestProgram(request, "/usr/libexec/backboardd");
    enqueueSpawn(request, parent);
    LSBJob *old = startingJob;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 211, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 211, old.identifier));
    audit_token_t identity = old.audit;
    assert(old.phase == JobRunning);
    if (indigo) assert(indigo.authorizations == 1 && parent.indigoAuthorizedIdentifier == old.identifier);
    if (framebuffer) assert(framebuffer.authorizations == 1 && parent.framebufferAuthorizedIdentifier == old.identifier);
    unsigned launches = submittedLaunches;
    uint64_t next = nextIdentifier;
    LSBChildStatus *ledger = old.childStatus;
    for (unsigned differentProgram = 0; differentProgram < 2; ++differentProgram) {
        unsigned execObject;
        request = spawnRequest(&fd, &execObject);
        setRequestProgram(request, differentProgram ? "/bin/df" : "/usr/libexec/backboardd");
        request->kind = LSB_REQUEST_EXEC;
        request->audit = identity;
        handleMessage(request);
        assert(replyFor(execObject) && replyFor(execObject)->error == ENOTSUP && !replyFor(execObject)->pid);
        assert(findJob(old.identifier) && old.phase == JobRunning && !old.replacementIdentifier && old.childStatus == ledger);
        assert(jobs.count == 2 && !startingJob && submittedLaunches == launches && nextIdentifier == next && !deliveredSignals);
        if (indigo) {
            audit_token_t authorized = indigo.authorizedAudit;
            assert(parent.indigoAuthorizedIdentifier == old.identifier && !memcmp(&authorized, &identity, sizeof(identity)));
            assert(indigo.authorizations == 1 && !indigo.revocations);
        }
        if (framebuffer) {
            audit_token_t authorized = framebuffer.authorizedAudit;
            assert(parent.framebufferAuthorizedIdentifier == old.identifier && !memcmp(&authorized, &identity, sizeof(identity)));
            assert(framebuffer.authorizations == 1 && !framebuffer.revocations);
        }
        assertClosed(fd);
        LSBChildObservation observation;
        assert(!LSBChildStatusObserve(parent.childStatus, old.guestPID, &observation));
        assert(!observation.execGeneration && !observation.exited && observation.nativePID == old.pid);
    }
    recordChildExit(old.identifier, SIGTERM);
    assert(!parent.indigoAuthorizedIdentifier && !parent.framebufferAuthorizedIdentifier);
    if (indigo) assert(indigo.revocations == 1);
    if (framebuffer) assert(framebuffer.revocations == 1);
  }
}

static void testDirectBackboardWaitsForServices(void) {
    for (unsigned failRegistration = 0; failRegistration < 2; ++failRegistration) {
        resetBroker();
        LSBJob *parent = runTestJob(nil, 220);
        parent.simulatorLaunchd = YES;
        RecordedFramebufferServer *framebuffer = [RecordedFramebufferServer new];
        parent.framebuffer = framebuffer;
        parent.servicesPending = YES;
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        setRequestProgram(request, "/usr/libexec/backboardd");
        unsigned launches = submittedLaunches;
        enqueueSpawn(request, parent);
        assert(!startingJob && submittedLaunches == launches && !replyFor(object));
        parent.servicesPending = NO;
        parent.servicesError = failRegistration ? EACCES : 0;
        startNextJob();
        if (failRegistration) {
            assert(!startingJob && submittedLaunches == launches && replyFor(object)->error == EACCES && !framebuffer.authorizations);
            assertClosed(fd);
        } else {
            LSBJob *child = startingJob;
            assert(child && submittedLaunches == launches + 1);
            handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 221, 0));
            handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 221, child.identifier));
            assert(child.phase == JobRunning && framebuffer.authorizations == 1 && !replyFor(object)->error);
        }
    }
}

static void testMonitorFailureAndEarlyExit(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 20);
    for (unsigned earlyExit = 0; earlyExit < 2; ++earlyExit) {
        int fd;
        unsigned object;
        enqueueSpawn(spawnRequest(&fd, &object), parent);
        uint64_t identifier = startingJob.identifier;
        monitorCreateError = earlyExit ? 0 : EACCES;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 21 + earlyExit, 0));
        if (earlyExit) recordChildExit(identifier, 99 << 8);
        assert(replyFor(object) && replyFor(object)->error == (earlyExit ? ECHILD : EACCES));
        assert(!findJob(identifier) && jobs.count == 1 && !deliveredSignals);
        pid_t waited;
        int status;
        assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &waited, &status) == ECHILD);
    }
}

static void testAuditedGenerationDisappearsBeforeBootstrap(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 25);
    int fd;
    unsigned object;
    enqueueSpawn(spawnRequest(&fd, &object), parent);
    uint64_t identifier = startingJob.identifier;
    unsigned beforePlans = planReplies();
    identityCheckError = ESRCH;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 26, 0));
    assert(identityChecks == 2 && !findJob(identifier) && jobs.count == 1);
    assert(planReplies() == beforePlans && replyFor(object)->error == ESRCH && !deliveredSignals);
    assertClosed(fd);
    pid_t waited;
    int status;
    assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &waited, &status) == ECHILD);
}

static void testRunningBoardFallback(void) {
    resetBroker();
    monitorCreateError = EACCES;
    runningBoardError = 0;
    LSBJob *parent = runTestJob(nil, 27);
    LSBJob *child = runTestJob(parent, 28);
    assert(runningBoardStarts == 2 && !identityChecks && !child.exitMonitor && child.runningBoardMonitor);
    directSignalError = EPERM;
    child.runningBoardMonitor.completion(37 << 8, 0);
    assert(!findJob(child.identifier) && runningBoardCancels == 1 && deliveredSignals == 1);
    assert(submittedExtensionSignals == 1 && signaledRequest == parent.identifier);
    pid_t pid;
    int status;
    assert(!LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &pid, &status));
    assert(pid == child.pid && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    LSBJob *unknown = runTestJob(parent, 29);
    unknown.runningBoardMonitor.completion(0, EPROTO);
    assert(!findJob(unknown.identifier) && deliveredSignals == 1);
    assert(LSBChildStatusTryWait(parent.childStatus, -1, getpgrp(), &pid, &status) == ECHILD);
}

static void testParentDeathDetachesOrphans(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 30);
    LSBJob *running = runTestJob(parent, 31);
    int fd;
    unsigned object;
    enqueueSpawn(spawnRequest(&fd, &object), parent);
    uint64_t pending = startingJob.identifier;
    // Bind the pending process first so cancellation does not poison the unrelated launch lane.
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 32, 0));
    recordChildExit(parent.identifier, SIGKILL);
    assert(!findJob(parent.identifier) && !findJob(pending));
    assert(replyFor(object)->error == ECANCELED && !running.parentIdentifier);
    assert(LSBChildStatusGetParentPID(running.childStatus) == 0);
    assert(jobs.count == 1 && !launchLaneFailed && !startingJob);
    recordChildExit(running.identifier, 7 << 8);
    assert(!jobs.count && !deliveredSignals);
}

// Capacity and adoption cases use distinct logical/native identities without launching real subprocesses.
static LSBJob *managedJob(LSBJob *parent, pid_t identity) {
    LSBJob *job = [LSBJob new];
    job.identifier = nextIdentifier++;
    job.phase = JobRunning;
    job.pid = job.guestPID = identity;
    job.pidVersion = (uint32_t)job.identifier;
    job.audit = (audit_token_t){{0, 0, 0, 0, 0, (unsigned)identity, 0, job.pidVersion}};
    assert(!prepareChildStatus(job));
    assert(!LSBChildStatusSetParentPID(job.childStatus, parent ? parent.guestPID : getpid()));
    if (parent) {
        LSBChildTicket ticket;
        assert(!LSBChildStatusReserve(parent.childStatus, &ticket));
        job.parentTicket = ticket;
        assert(!LSBChildStatusPublishRunning(parent.childStatus, job.parentTicket, identity, identity));
        job.parentIdentifier = parent.identifier;
    }
    [jobs addObject:job];
    return job;
}

static LSBJob *terminalLaunchd(void) {
    LSBJob *launchd = managedJob(nil, 6600);
    launchd.simulatorLaunchd = YES;
    launchd.peerIdentityError = 0;
    LSBPeerProcessIdentity identity = launchd.peerIdentity;
    strlcpy(identity.executablePath, "/sbin/launchd_sim", sizeof(identity.executablePath));
    launchd.peerIdentity = identity;
    return launchd;
}

static void testLaunchdTerminalLineage(void) {
    char temporary[] = "/tmp/LSBLaunchdTerminal.XXXXXX";
    assert(mkdtemp(temporary));
    NSString *directory = [NSString stringWithUTF8String:temporary];
    for (unsigned replacing = 0; replacing < 2; ++replacing) {
        resetBroker();
        enableIndigo = YES;
        runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
        LSBJob *launchd = terminalLaunchd(), *sender = launchd;
        if (replacing) {
            sender = managedJob(launchd, 6601);
            sender.peerIdentityError = 0;
            LSBPeerProcessIdentity identity = sender.peerIdentity;
            strlcpy(identity.executablePath, "/usr/libexec/xpcproxy_sim", sizeof(identity.executablePath));
            sender.peerIdentity = identity;
        }
        __block unsigned notifications = 0;
        id observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostTerminalDidChangeNotification object:nil queue:nil
            usingBlock:^(NSNotification *note) { (void)note; ++notifications; assert(LSBHostTerminal()); }];
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        setRequestProgram(request, replacing ? [[runtimeRoot stringByAppendingString:@"//usr/libexec/terminal-fish"] UTF8String] :
            "/usr/libexec/terminal-fish");
        request->audit = sender.audit;
        if (replacing) { request->kind = LSB_REQUEST_EXEC; enqueueExec(request, sender); }
        else enqueueSpawn(request, sender);
        LSBJob *terminal = startingJob;
        assert(terminal && terminal.terminalSession && !launchd.terminalSession && !sender.terminalSession);
        assert(hostTerminal && notifications == 1 && request->plan.actionCount == 4 && request->plan.fdCount == 2);
        assert(request->plan.actions[0].kind == LSB_ACTION_DUP2 && request->plan.actions[3].kind == LSB_ACTION_CLOSE);
        if (replacing) assert(terminal.guestPID == sender.guestPID && terminal.replacesIdentifier == sender.identifier);
        reportedPID = replacing ? getpid() + 350 : getpid();
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 501, 0));
        handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 501, terminal.identifier));
        assert(terminal.phase == JobRunning && terminal.parentIdentifier == launchd.identifier);
        assertClosed(fd);
        // The helper's replacement retains terminal ownership; launchd's other children never inherit it.
        request = spawnRequest(&fd, &object);
        setRequestProgram(request, "/bin/fish");
        request->kind = LSB_REQUEST_EXEC;
        request->audit = terminal.audit;
        enqueueExec(request, terminal);
        LSBJob *fish = startingJob;
        assert(fish && fish.terminalSession && fish.guestPID == terminal.guestPID && notifications == 1);
        reportedPID = getpid() + 351;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 502, 0));
        handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 502, fish.identifier));
        assert(fish.phase == JobRunning && fish.parentIdentifier == launchd.identifier);
        request = spawnRequest(&fd, &object);
        enqueueSpawn(request, fish);
        LSBJob *descendant = startingJob;
        assert(descendant.terminalSession);
        request = spawnRequest(&fd, &object);
        enqueueSpawn(request, launchd);
        LSBJob *unrelated = jobs.lastObject;
        assert(!unrelated.terminalSession);
        request = spawnRequest(&fd, &object);
        setRequestProgram(request, "/usr/libexec/terminal-fish");
        request->audit = launchd.audit;
        enqueueSpawn(request, launchd);
        assert(replyFor(object)->error == EBUSY && notifications == 1);
        assertClosed(fd);
        [NSNotificationCenter.defaultCenter removeObserver:observer];
    }
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        resetBroker();
        enableIndigo = YES;
        runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
        LSBJob *launchd = terminalLaunchd(), *sender = launchd;
        BOOL replacing = invalid >= 5;
        if (replacing) sender = managedJob(launchd, 6610);
        if (invalid == 0) enableIndigo = NO;
        if (invalid == 1) launchd.peerIdentityError = EACCES;
        if (invalid == 2) launchd.simulatorLaunchd = NO;
        if (invalid == 3) launchd.parentIdentifier = 42;
        if (invalid == 6) {
            sender.peerIdentityError = 0;
            LSBPeerProcessIdentity identity = sender.peerIdentity;
            strlcpy(identity.executablePath, "/usr/libexec/xpcproxy_sim", sizeof(identity.executablePath));
            sender.peerIdentity = identity;
            sender.parentIdentifier = 0;
        }
        if (invalid == 7) sender.retiredForExec = YES;
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        setRequestProgram(request, "/usr/libexec/terminal-fish");
        request->audit = sender.audit;
        if (invalid == 4) request->audit.val[0] ^= 1;
        if (replacing) { request->kind = LSB_REQUEST_EXEC; enqueueExec(request, sender); }
        else enqueueSpawn(request, sender);
        assert(replyFor(object)->error == EACCES && !hostTerminal && !startingJob);
        assertClosed(fd);
    }
    resetBroker();
    assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
}

static void testGlobalJobCapacity(void) {
    assert(BrokerJobCapacity == 2 * LSB_CHILD_STATUS_CAPACITY + 4);
    for (unsigned replacing = 0; replacing < 2; ++replacing) {
        resetBroker();
        for (unsigned index = 0; index < BrokerJobCapacity - 1; ++index) managedJob(nil, 6000 + index);
        LSBJob *old = jobs[0], *other = jobs[1];
        int acceptedFD, rejectedFD;
        unsigned acceptedObject, rejectedObject;
        LSBReceivedMessage *accepted = spawnRequest(&acceptedFD, &acceptedObject);
        if (replacing) { accepted->kind = LSB_REQUEST_EXEC; enqueueExec(accepted, old); }
        else enqueueSpawn(accepted, nil);
        assert(jobs.count == BrokerJobCapacity && startingJob && submittedLaunches == 1);
        assert(!replyFor(acceptedObject) && fcntl(acceptedFD, F_GETFD) >= 0);
        if (replacing) assert(old.replacementIdentifier == startingJob.identifier && startingJob.guestPID == old.guestPID);

        enqueueSpawn(spawnRequest(&rejectedFD, &rejectedObject), nil);
        assert(replyFor(rejectedObject)->error == EAGAIN && jobs.count == BrokerJobCapacity && submittedLaunches == 1);
        assertClosed(rejectedFD);
        LSBReceivedMessage *exec = spawnRequest(&rejectedFD, &rejectedObject);
        exec->kind = LSB_REQUEST_EXEC;
        enqueueExec(exec, other);
        assert(replyFor(rejectedObject)->error == EAGAIN && !other.replacementIdentifier);
        assert(jobs.count == BrokerJobCapacity && submittedLaunches == 1);
        assertClosed(rejectedFD);
        resetBroker();
        assertClosed(acceptedFD);
    }
}

static void testExecDoesNotConsumeAnotherChildSlot(void) {
    resetBroker();
    LSBJob *parent = managedJob(nil, 6500);
    for (unsigned index = 0; index < LSB_CHILD_STATUS_CAPACITY; ++index) managedJob(parent, 6501 + index);
    assert(jobs.count < BrokerJobCapacity);
    LSBJob *old = jobs[1];
    LSBChildTicket ticket = old.parentTicket;
    int fd;
    unsigned object;
    enqueueSpawn(spawnRequest(&fd, &object), parent);
    assert(replyFor(object)->error == EAGAIN && !startingJob && !submittedLaunches);
    assertClosed(fd);

    LSBReceivedMessage *exec = spawnRequest(&fd, &object);
    exec->kind = LSB_REQUEST_EXEC;
    enqueueExec(exec, old);
    LSBJob *replacement = startingJob;
    assert(replacement && replacement.guestPID == old.guestPID && jobs.count == LSB_CHILD_STATUS_CAPACITY + 2);
    reportedPID = getpid() + 200;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 6600, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 6600, replacement.identifier));
    assert(replyFor(object) && !replyFor(object)->error && replyFor(object)->pid == old.guestPID);
    assert(!findJob(old.identifier) && jobs.count == LSB_CHILD_STATUS_CAPACITY + 1);
    assert(replacement.parentTicket.index == ticket.index && replacement.parentTicket.generation == ticket.generation);
    LSBChildObservation observation;
    assert(!LSBChildStatusObserve(parent.childStatus, old.guestPID, &observation));
    assert(observation.nativePID == reportedPID && observation.execGeneration == 1 && !observation.exited);
    LSBChildTicket extra;
    assert(LSBChildStatusReserve(parent.childStatus, &extra) == EAGAIN);
    assertClosed(fd);
}

static void testFullLaunchdBatchFitsExecOverlap(void) {
    resetBroker();
    LSBJob *launchd = managedJob(nil, 6700);
    launchd.simulatorLaunchd = YES;
    managedJob(nil, 6701); // Host-owned native Metal helpers occupy physical jobs, not launchd child-ledger slots.
    managedJob(nil, 6800);
    managedJob(nil, 6801);
    NSMutableArray<LSBJob *> *proxies = [NSMutableArray array];
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) [proxies addObject:managedJob(launchd, 6702 + i)];
    int fds[LSB_CHILD_STATUS_CAPACITY];
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        unsigned object;
        LSBReceivedMessage *exec = spawnRequest(&fds[i], &object);
        exec->kind = LSB_REQUEST_EXEC;
        enqueueExec(exec, proxies[i]);
        LSBJob *replacement = findJob(proxies[i].replacementIdentifier);
        assert(replacement && !replyFor(object) && replacement.guestPID == proxies[i].guestPID);
        assert(replacement.phase == (i ? JobQueued : JobStarting));
    }
    assert(jobs.count == BrokerJobCapacity && submittedLaunches == 1);
    LSBChildTicket extra;
    assert(LSBChildStatusReserve(launchd.childStatus, &extra) == EAGAIN);
    resetBroker();
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) assertClosed(fds[i]);
}

static void testLaunchdAdoptsRunningAndZombieChildren(void) {
    resetBroker();
    LSBJob *init = managedJob(nil, 7001);
    init.simulatorLaunchd = YES;
    LSBJob *parent = managedJob(init, 7002), *child = managedJob(parent, 7003), *grandchild = managedJob(child, 7004);
    LSBJob *zombie = managedJob(parent, 7005);
    recordChildExit(zombie.identifier, 23 << 8);
    assert(!LSBChildStatusPublishExec(parent.childStatus, child.parentTicket, 7103));
    child.pid = 7103;
    child.executed = YES;
    recordChildExit(parent.identifier, 0);
    assert(child.parentIdentifier == init.identifier && grandchild.parentIdentifier == child.identifier);
    assert(LSBChildStatusGetParentPID(child.childStatus) == init.guestPID);
    assert(LSBChildStatusGetParentPID(grandchild.childStatus) == child.guestPID);
    LSBChildObservation observation;
    assert(!LSBChildStatusObserve(init.childStatus, child.guestPID, &observation));
    assert(observation.nativePID == 7103 && observation.execGeneration == 1 && !observation.exited);
    assert(!LSBChildStatusObserve(init.childStatus, zombie.guestPID, &observation) && observation.exited && observation.status == (23 << 8));
    assert(!pendingAdoptions.count);
    recordChildExit(child.identifier, 37 << 8);
    assert(grandchild.parentIdentifier == init.identifier && LSBChildStatusGetParentPID(grandchild.childStatus) == init.guestPID);
    pid_t waited;
    int status;
    assert(!LSBChildStatusTryWait(init.childStatus, child.guestPID, 0, &waited, &status) && status == (37 << 8));
    assert(!LSBChildStatusTryWait(init.childStatus, zombie.guestPID, 0, &waited, &status) && status == (23 << 8));
    assert(!LSBChildStatusTryWait(init.childStatus, parent.guestPID, 0, &waited, &status) && status == 0);
}

static void fillExitedLedger(LSBJob *parent) {
    for (unsigned index = 0; index < LSB_CHILD_STATUS_CAPACITY; ++index) {
        LSBChildTicket ticket;
        assert(!LSBChildStatusReserve(parent.childStatus, &ticket));
        assert(!LSBChildStatusPublishRunning(parent.childStatus, ticket, 10000 + index, 10000 + index));
        assert(!LSBChildStatusPublishExit(parent.childStatus, ticket, 0));
    }
}

static void testAdoptionCapacityPreservesExitAndExec(void) {
    resetBroker();
    LSBJob *init = managedJob(nil, 7201);
    init.simulatorLaunchd = YES;
    fillExitedLedger(init);
    LSBJob *parent = managedJob(nil, 7202), *child = managedJob(parent, 7203), *zombie = managedJob(parent, 7204);
    recordChildExit(zombie.identifier, 41 << 8);
    recordChildExit(parent.identifier, 0);
    assert(pendingAdoptions.count == 2 && child.parentStatusPending && child.parentIdentifier == init.identifier);
    assert(LSBChildStatusGetParentPID(child.childStatus) == init.guestPID);
    int fd;
    unsigned object;
    enqueueSpawn(spawnRequest(&fd, &object), child);
    assert(replyFor(object)->error == EAGAIN && !startingJob);
    LSBReceivedMessage *exec = spawnRequest(&fd, &object);
    exec->kind = LSB_REQUEST_EXEC;
    exec->audit = child.audit;
    handleMessage(exec);
    LSBJob *replacement = startingJob;
    reportedPID = getpid() + 400;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 7205, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 7205, replacement.identifier));
    assert(!findJob(child.identifier) && replacement.parentStatusPending && replacement.parentIdentifier == init.identifier);
    assert(LSBChildStatusGetParentPID(replacement.childStatus) == init.guestPID);
    LSBPendingAdoption *pending = pendingAdoption(replacement.identifier);
    assert(pending && pending.observation.nativePID == reportedPID && pending.observation.execGeneration == 1);
    recordChildExit(replacement.identifier, SIGTERM);
    assert(pendingAdoptions.count == 2);
    pid_t waited;
    int status;
    assert(!LSBChildStatusTryWait(init.childStatus, 10000, 0, &waited, &status));
    retryAdoptions();
    assert(pendingAdoptions.count == 1);
    assert(!LSBChildStatusTryWait(init.childStatus, child.guestPID, 0, &waited, &status) && status == SIGTERM);
    retryAdoptions();
    assert(!pendingAdoptions.count && !adoptionRetrySource);
    assert(!LSBChildStatusTryWait(init.childStatus, zombie.guestPID, 0, &waited, &status) && status == (41 << 8));
}

static void testLaunchdDeathAndCyclesDoNotAdopt(void) {
    resetBroker();
    LSBJob *parent = managedJob(nil, 7301), *init = managedJob(parent, 7302), *child = managedJob(parent, 7303);
    init.simulatorLaunchd = YES; // A descendant cannot become its sibling's ancestor through adoption.
    recordChildExit(parent.identifier, 0);
    assert(!child.parentIdentifier && !init.parentIdentifier && LSBChildStatusGetParentPID(child.childStatus) == 0);
    resetBroker();
    init = managedJob(nil, 7401);
    init.simulatorLaunchd = YES;
    fillExitedLedger(init);
    parent = managedJob(nil, 7402);
    child = managedJob(parent, 7403);
    recordChildExit(parent.identifier, 0);
    assert(pendingAdoptions.count == 1);
    recordChildExit(init.identifier, SIGKILL);
    assert(!pendingAdoptions.count && !adoptionRetrySource && !child.parentIdentifier);
    assert(LSBChildStatusGetParentPID(child.childStatus) == 0);
}

static RecordedReply *requestKill(LSBJob *parent, pid_t targetPID, int signalNumber) {
    LSBReceivedMessage *message = messageFor(LSB_REQUEST_KILL, parent.pid, parent.pidVersion, 0);
    unsigned object = ((TestMessageObject *)message->object)->identifier;
    message->targetPID = targetPID;
    message->signalNumber = signalNumber;
    handleMessage(message);
    RecordedReply *reply = replyFor(object);
    assert(reply && !reply->requestID && !reply->deliveredPlan && (!reply->error || !reply->pid));
    return reply;
}

static void testKillAuthenticationAndExit(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 40);
    LSBJob *child = runTestJob(parent, 41);
    parent.pid = getpid() + 100;
    audit_token_t parentAudit = parent.audit;
    parentAudit.val[5] = (unsigned)parent.pid;
    parent.audit = parentAudit;
    LSBJob *other = runTestJob(nil, 42);
    other.pid = getpid() + 200;
    unsigned checks = identityChecks;
    assert(!requestKill(parent, child.pid, 0)->error && identityChecks == checks + 1 && !deliveredSignals && !submittedExtensionSignals);
    assert(requestKill(parent, child.pid, -1)->error == EINVAL);
    assert(requestKill(parent, child.pid, NSIG)->error == EINVAL);
    assert(requestKill(parent, 0, SIGTERM)->error == ENOTSUP);
    assert(requestKill(parent, -1, SIGTERM)->error == ENOTSUP);
    assert(requestKill(parent, -20, SIGTERM)->error == ENOTSUP);
    assert(requestKill(parent, other.pid, SIGTERM)->error == ESRCH);
    assert(requestKill(parent, parent.pid, SIGTERM)->error == ESRCH);
    assert(requestKill(parent, getpid() + 300, SIGTERM)->error == ESRCH);
    LSBJob *impostor = [LSBJob new];
    impostor.pid = parent.pid;
    impostor.pidVersion = parent.pidVersion + 1;
    assert(requestKill(impostor, child.pid, SIGTERM)->error == EACCES);
    parent.phase = JobDispatched;
    assert(requestKill(parent, child.pid, SIGTERM)->error == EACCES);
    parent.phase = JobRunning;
    child.phase = JobDispatched;
    assert(requestKill(parent, child.pid, SIGTERM)->error == ESRCH);
    child.phase = JobRunning;
    assert(identityChecks == checks + 1 && !deliveredSignals && !submittedExtensionSignals);
    identityCheckError = ESRCH;
    assert(requestKill(parent, child.pid, 0)->error == ESRCH);
    assert(requestKill(parent, child.pid, SIGTERM)->error == ESRCH);
    assert(!deliveredSignals && !submittedExtensionSignals);
    identityCheckError = 0;
    RecordedReply *reply = requestKill(parent, child.pid, SIGTERM);
    assert(!reply->error && reply->pid == child.pid && deliveredSignals == 1 && !submittedExtensionSignals && lastSignal == SIGTERM);
    audit_token_t childAudit = child.audit;
    assert(!memcmp(&signaledAudit, &childAudit, sizeof(childAudit)));
    // Successful signaling does not invent an exit. Only the later monitor event makes waitpid reapable.
    pid_t waited;
    int status;
    assert(findJob(child.identifier) && LSBChildStatusTryWait(parent.childStatus, child.pid, getpgrp(), &waited, &status) == EAGAIN);
    recordChildExit(child.identifier, SIGTERM);
    assert(!LSBChildStatusTryWait(parent.childStatus, child.pid, getpgrp(), &waited, &status));
    assert(waited == child.pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(requestKill(parent, child.pid, 0)->error == ESRCH && requestKill(parent, child.pid, SIGTERM)->error == ESRCH);
}

static void testKillFallbackErrors(void) {
    resetBroker();
    LSBJob *parent = runTestJob(nil, 50);
    LSBJob *child = runTestJob(parent, 51);
    directSignalError = ESRCH;
    assert(requestKill(parent, child.pid, SIGTERM)->error == ESRCH && deliveredSignals == 1 && !submittedExtensionSignals);
    directSignalError = EINVAL;
    assert(requestKill(parent, child.pid, SIGTERM)->error == EINVAL && deliveredSignals == 2 && !submittedExtensionSignals);
    directSignalError = EPERM;
    assert(!requestKill(parent, child.pid, SIGTERM)->error && submittedExtensionSignals == 1 && signaledRequest == child.identifier);
    assert(lastSignal == SIGTERM);
    directSignalError = EACCES;
    assert(!requestKill(parent, child.pid, SIGKILL)->error && submittedExtensionSignals == 2 && lastSignal == SIGKILL);
    extensionSignalError = ENOMEM;
    assert(requestKill(parent, child.pid, SIGTERM)->error == ENOMEM && submittedExtensionSignals == 3);
    extensionSignalError = ESRCH;
    assert(requestKill(parent, child.pid, SIGTERM)->error == ESRCH && submittedExtensionSignals == 4);
    assert(findJob(child.identifier) && jobs.count == 2);
}

static audit_token_t ownAudit(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    assert(count == TASK_AUDIT_TOKEN_COUNT && token.val[5] == (unsigned)getpid());
    return token;
}

static bool hasRight(mach_port_t port, mach_port_type_t right) {
    mach_port_type_t type = 0;
    return !mach_port_type(mach_task_self(), port, &type) && (type & right);
}

static void runMainQueueUntil(BOOL (^done)(void)) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!done() && deadline.timeIntervalSinceNow > 0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
    assert(done());
}

static void testDisplayEndpointRegistrationCompletesAll(void) {
    for (unsigned failure = 0; failure < 4; ++failure) {
        resetBroker();
        LSBJob *launchd = runTestJob(nil, 790);
        launchd.simulatorLaunchd = YES;
        RecordedIndigoServer *indigo = [RecordedIndigoServer new];
        RecordedFramebufferServer *framebuffer = [RecordedFramebufferServer new];
        RecordedIOSurfaceServer *iosurface = [RecordedIOSurfaceServer new];
        launchd.indigo = indigo;
        launchd.framebuffer = framebuffer;
        launchd.iosurface = iosurface;
        launchd.servicesPending = YES;
        assert(LSBHostFramebufferServer() == framebuffer);
        if (failure) endpointErrors[failure - 1] = EACCES;
        mach_port_t service;
        assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &service));
        assert(!mach_port_insert_right(mach_task_self(), service, service, MACH_MSG_TYPE_MAKE_SEND));
        registerDisplayServices(launchd, service);
        // Even fast native registrations cannot release a child before main observes completion of all calls.
        assert(launchd.servicesPending && !launchd.servicesError);
        runMainQueueUntil(^BOOL { return !launchd.servicesPending; });
        assert(launchd.servicesError == (failure ? EACCES : 0));
        assert(endpointCalls == (failure ? failure : 3u));
        assert(!strcmp(registeredEndpoints[0], "IndigoHIDRegistrationPort") && registeredPorts[0] == indigo.registrationPort);
        audit_token_t expected = launchd.audit;
        assert(!memcmp(&registeredAudits[0], &expected, sizeof(expected)));
        if (failure != 1) {
            assert(!strcmp(registeredEndpoints[1], "com.apple.IOSurface.Remote"));
            assert(registeredPorts[1] == iosurface.registrationPort && !memcmp(&registeredAudits[1], &expected, sizeof(expected)));
        }
        if (!failure || failure == 3) {
            assert(!strcmp(registeredEndpoints[2], "com.apple.CoreSimulator.SimFramebufferServer"));
            assert(registeredPorts[2] == framebuffer.registrationPort && !memcmp(&registeredAudits[2], &expected, sizeof(expected)));
        }
        finishJob(launchd, ECANCELED);
        assert(framebuffer.closes == 1 && iosurface.closes == 1 && !LSBHostFramebufferServer());
        assert(!mach_port_deallocate(mach_task_self(), service));
        assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
    }
}

static void testDisplayRegistrationSharesStartupBudget(void) {
    resetBroker();
    LSBJob *launchd = runTestJob(nil, 795);
    launchd.simulatorLaunchd = YES;
    launchd.indigo = [RecordedIndigoServer new];
    launchd.framebuffer = [RecordedFramebufferServer new];
    launchd.iosurface = [RecordedIOSurfaceServer new];
    launchd.servicesPending = YES;
    endpointHook = ^(unsigned index) { if (!index) usleep(20000); };
    mach_port_t service;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &service));
    assert(!mach_port_insert_right(mach_task_self(), service, service, MACH_MSG_TYPE_MAKE_SEND));
    registerDisplayServices(launchd, service);
    // Proxy handoff remains live while the worker holds its original registration request.
    int fd;
    unsigned object;
    enqueueSpawn(spawnRequest(&fd, &object), launchd);
    LSBJob *proxy = startingJob;
    assert(proxy && launchd.servicesPending);
    reportedPID = getpid();
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 796, 0));
    handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 796, proxy.identifier));
    assert(replyFor(object) && !replyFor(object)->error && launchd.servicesPending);
    runMainQueueUntil(^BOOL { return !launchd.servicesPending; });
    assert(!launchd.servicesError && endpointCalls == 3);
    assert(registeredTimeouts[0] > 29000 && registeredTimeouts[0] <= 30000);
    assert(registeredTimeouts[1] < registeredTimeouts[0] && registeredTimeouts[2] <= registeredTimeouts[1]);
    kern_return_t status = 42;
    audit_token_t expected = launchd.audit;
    assert(registerDisplayEndpoint(service, &expected, "expired", 111, 0, &status) == ETIMEDOUT);
    assert(status == -1 && endpointCalls == 3); // Exhaustion sends no extra mutation and never retries an unknown outcome.
    finishJob(launchd, ECANCELED);
    assert(!mach_port_deallocate(mach_task_self(), service));
    assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
}

static mach_port_t newServiceEndpoint(void) {
    mach_port_t endpoint = MACH_PORT_NULL;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &endpoint));
    assert(!mach_port_insert_right(mach_task_self(), endpoint, endpoint, MACH_MSG_TYPE_MAKE_SEND));
    return endpoint;
}

static void testNativeConfigdBridgeRegistration(void) {
    enum { Success, Unselected, LookupFailure, RegistrationFailure, UnknownTimeout, MissingEndpoint };
    for (unsigned test = Success; test <= MissingEndpoint; ++test) {
        resetBroker();
        LSBJob *launchd = runTestJob(nil, 797);
        launchd.simulatorLaunchd = YES;
        launchd.nativeConfigdSelected = test != Unselected;
        launchd.indigo = [RecordedIndigoServer new];
        launchd.framebuffer = [RecordedFramebufferServer new];
        launchd.iosurface = [RecordedIOSurfaceServer new];
        launchd.servicesPending = YES;
        mach_port_t service = newServiceEndpoint(), native = newServiceEndpoint();
        nativeConfigdEndpoint = test == MissingEndpoint ? MACH_PORT_NULL : native;
        if (test == LookupFailure) nativeConfigdLookupError = KERN_FAILURE;
        if (test == RegistrationFailure) endpointErrors[0] = EACCES;
        if (test == UnknownTimeout) endpointErrors[0] = ETIMEDOUT;
        endpointHook = ^(unsigned index) {
            if (index || test == Unselected) return;
            mach_port_urefs_t references = 0;
            assert(nativeConfigdLookupCalls == 1 && registeredPorts[0] == native);
            assert(!strcmp(registeredEndpoints[0], "com.apple.SystemConfiguration.configd"));
            assert(!mach_port_get_refs(mach_task_self(), native, MACH_PORT_RIGHT_SEND, &references) && references == 2);
            usleep(20000); // The following display registrations share this request's original startup budget.
        };
        registerDisplayServices(launchd, service);
        assert(launchd.servicesPending && !launchd.servicesError);
        runMainQueueUntil(^BOOL { return !launchd.servicesPending; });
        int expectedError = test == LookupFailure ? EIO : test == RegistrationFailure ? EACCES :
                            test == UnknownTimeout ? ETIMEDOUT : test == MissingEndpoint ? EPIPE : 0;
        unsigned expectedCalls = test == Success ? 4 : test == Unselected ? 3 :
                                 test == RegistrationFailure || test == UnknownTimeout ? 1 : 0;
        assert(launchd.servicesError == expectedError && endpointCalls == expectedCalls);
        assert(nativeConfigdLookupCalls == (test == Unselected ? 0u : 1u));
        if (!expectedError) {
            unsigned first = test == Success ? 1 : 0;
            assert(!strcmp(registeredEndpoints[first], "IndigoHIDRegistrationPort"));
            assert(!strcmp(registeredEndpoints[first + 1], "com.apple.IOSurface.Remote"));
            assert(!strcmp(registeredEndpoints[first + 2], "com.apple.CoreSimulator.SimFramebufferServer"));
            if (test == Success) assert(registeredTimeouts[1] < registeredTimeouts[0]);
            for (unsigned i = 0; i < endpointCalls; ++i) {
                audit_token_t expected = launchd.audit;
                assert(!memcmp(&registeredAudits[i], &expected, sizeof(expected)));
            }
        }
        mach_port_urefs_t references = 0;
        assert(!mach_port_get_refs(mach_task_self(), native, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        assert(!mach_port_get_refs(mach_task_self(), service, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        registerDisplayServices(launchd, service);
        assert(endpointCalls == expectedCalls && nativeConfigdLookupCalls == (test == Unselected ? 0u : 1u)); // Never retry an unknown result.
        finishJob(launchd, ECANCELED);
        assert(!mach_port_deallocate(mach_task_self(), native));
        assert(!mach_port_mod_refs(mach_task_self(), native, MACH_PORT_RIGHT_RECEIVE, -1));
        assert(!mach_port_deallocate(mach_task_self(), service));
        assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
    }
}

static void testNativeHapticsBridgeRegistration(void) {
    enum { Success, Headless, CreationFailure, RegistrationFailure, UnknownTimeout, MissingEndpoint, CopyFailure, DisplayFailure, Disabled, InvalidOptIn };
    for (unsigned test = Success; test <= InvalidOptIn; ++test) {
        resetBroker();
        if (test != Disabled) assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", test == InvalidOptIn ? "true" : "1", 1));
        sessionHapticsEnabled = testConfiguredHaptics();
        LSBJob *launchd = runTestJob(nil, 799);
        launchd.simulatorLaunchd = YES;
        if (test != Headless) {
            launchd.indigo = [RecordedIndigoServer new];
            launchd.framebuffer = [RecordedFramebufferServer new];
            launchd.iosurface = [RecordedIOSurfaceServer new];
        }
        launchd.servicesPending = YES;
        mach_port_t service = newServiceEndpoint();
        if (test == CreationFailure) hapticCreateError = ENOTSUP;
        if (test == MissingEndpoint) hapticMissingEndpoint = YES;
        if (test == CopyFailure) hapticCopyFailure = YES;
        if (test == RegistrationFailure) endpointErrors[3] = EACCES;
        if (test == UnknownTimeout) endpointErrors[3] = ETIMEDOUT;
        if (test == DisplayFailure) endpointErrors[0] = EACCES;
        prepareHapticServer(launchd);
        unsigned expectedCreates = test == Headless || test == Disabled || test == InvalidOptIn ? 0 : 1;
        assert(hapticCreates == expectedCreates);
        mach_port_t relayPort = launchd.haptics.registrationPort;
        endpointHook = ^(unsigned index) {
            if (index < 3) return;
            assert(index == 3 && !strcmp(registeredEndpoints[index], "com.apple.audio.hapticd") && registeredPorts[index] == relayPort);
            mach_port_urefs_t references = 0;
            assert(!mach_port_get_refs(mach_task_self(), relayPort, MACH_PORT_RIGHT_SEND, &references) && references == 2);
            audit_token_t expected = launchd.audit;
            assert(!memcmp(&registeredAudits[index], &expected, sizeof(expected)));
        };
        registerDisplayServices(launchd, service);
        savedHaptics = !sessionHapticsEnabled; // Preferences after capture cannot change this session's publication.
        unsetenv("LSB_EXPERIMENTAL_HAPTICS");
        runMainQueueUntil(^BOOL { return !launchd.servicesPending; });
        unsigned expectedCalls = test == Headless ? 0 : test == DisplayFailure ? 1 :
            test == Disabled || test == InvalidOptIn || test == CreationFailure || test == MissingEndpoint || test == CopyFailure ? 3 : 4;
        assert(endpointCalls == expectedCalls && launchd.servicesError == (test == DisplayFailure ? EACCES : 0));
        mach_port_urefs_t references = 0;
        assert(!mach_port_get_refs(mach_task_self(), service, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        registerDisplayServices(launchd, service);
        assert(endpointCalls == expectedCalls && hapticCreates == expectedCreates); // Unknown registration never retries.
        finishJob(launchd, ECANCELED);
        assert(hapticCloses == (expectedCreates && test != CreationFailure ? 1u : 0u));
        assert(!mach_port_deallocate(mach_task_self(), service));
        assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
    }
}

static void testHapticRegistrationKeepsOwnedRight(void) {
    resetBroker();
    sessionHapticsEnabled = YES;
    LSBJob *launchd = terminalLaunchd();
    launchd.indigo = [RecordedIndigoServer new];
    launchd.servicesPending = YES;
    prepareHapticServer(launchd);
    LSBHapticServer *relay = launchd.haptics;
    mach_port_t endpoint = relay.registrationPort, service = newServiceEndpoint();
    dispatch_semaphore_t entered = dispatch_semaphore_create(0), release = dispatch_semaphore_create(0);
    endpointHook = ^(unsigned index) {
        if (!index) return;
        assert(index == 1 && registeredPorts[index] == endpoint);
        dispatch_semaphore_signal(entered);
        assert(!dispatch_semaphore_wait(release, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
    };
    registerDisplayServices(launchd, service);
    assert(!dispatch_semaphore_wait(entered, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
    mach_port_urefs_t references = 0;
    assert(!mach_port_get_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, &references) && references == 2);
    finishJob(launchd, ECANCELED);
    assert(hapticCloses == 1 && !relay.registrationPort);
    assert(!mach_port_get_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, &references) && references == 1);
    dispatch_semaphore_signal(release);
    runMainQueueUntil(^BOOL { return !hasRight(endpoint, MACH_PORT_TYPE_SEND); });
    // The relay's receive right remains until its final object is released, making send-reference ownership observable.
    assert(hasRight(endpoint, MACH_PORT_TYPE_RECEIVE));
    assert(!mach_port_deallocate(mach_task_self(), service));
    assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
    resetBroker();
}

static BOOL authorizeHapticAudit(audit_token_t audit) {
    assert(hapticAuthorization);
    __block BOOL done = NO, allowed = NO;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        allowed = hapticAuthorization(&audit);
        dispatch_async(dispatch_get_main_queue(), ^{ done = YES; });
    });
    runMainQueueUntil(^BOOL { return done; });
    return allowed;
}

static void testHapticRelayAuthentication(void) {
    resetBroker();
    sessionHapticsEnabled = YES;
    LSBJob *launchd = terminalLaunchd(), *client = managedJob(launchd, 6601);
    client.peerIdentityError = 0;
    launchd.indigo = [RecordedIndigoServer new];
    prepareHapticServer(launchd);
    assert(hapticCreates == 1 && launchd.haptics);
    assert(authorizeHapticAudit(client.audit));
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t invalid = client.audit;
        ++invalid.val[word];
        assert(!authorizeHapticAudit(invalid));
    }
    for (unsigned test = 0; test < 8; ++test) {
        client.executionRole = test == 0 ? LSB_EXECUTION_NATIVE_METAL_SERVICE : LSB_EXECUTION_SIMULATOR;
        client.phase = test == 1 ? JobDispatched : JobRunning;
        client.retiredForExec = test == 2;
        client.finishing = test == 3;
        client.peerIdentityError = test == 4 ? ESRCH : 0;
        sessionDraining = test == 5;
        sessionHapticsEnabled = test != 6;
        if (test == 7) ++rebootGeneration;
        assert(!authorizeHapticAudit(client.audit));
    }
    client.finishing = client.retiredForExec = sessionDraining = NO;
    sessionHapticsEnabled = YES;
    --rebootGeneration;
    assert(authorizeHapticAudit(client.audit));
    finishJob(launchd, ECANCELED);
    assert(hapticCloses == 1 && !authorizeHapticAudit(client.audit));
    resetBroker();
}

static void testAudioOutputAuthenticationAndOwnership(void) {
    for (unsigned kind = LSB_REQUEST_OPEN_AUDIO_OUTPUT; kind <= LSB_REQUEST_CLOSE_AUDIO_OUTPUT; ++kind) {
        for (unsigned test = 0; test < 16; ++test) {
            resetBroker();
            sessionAudioEnabled = test != 8;
            LSBJob *client = managedJob(nil, 6601);
            client.peerIdentityError = test == 13 ? ESRCH : 0;
            client.executionRole = test == 9 ? LSB_EXECUTION_NATIVE_METAL_SERVICE : LSB_EXECUTION_SIMULATOR;
            client.phase = test == 10 ? JobDispatched : JobRunning;
            client.retiredForExec = test == 11;
            client.finishing = test == 12;
            sessionDraining = test == 14;
            LSBReceivedMessage *request = messageFor(kind, client.pid, client.pidVersion, 0);
            request->audit = client.audit;
            if (test < 8) ++request->audit.val[test];
            if (test == 15) [jobs removeObject:client];
            handleMessage(request);
            int expectedError = test == 8 ? EPERM : test == 14 ? ECANCELED : EACCES;
            assert(replies[replyCount - 1].error == expectedError && !audioCreates && !client.audioOutput);
        }
    }
    for (unsigned test = 0; test < 4; ++test) {
        resetBroker();
        sessionAudioEnabled = YES;
        LSBJob *client = managedJob(nil, 6601);
        client.peerIdentityError = 0;
        if (test == 1) audioCreateError = ENOMEM;
        if (test == 2) audioReplySendError = EPIPE;
        handleMessage(messageFor(LSB_REQUEST_OPEN_AUDIO_OUTPUT, client.pid, client.pidVersion, 0));
        assert(audioCreates == 1 && replies[replyCount - 1].error == audioCreateError);
        if (test == 1 || test == 2) {
            assert(!client.audioOutput && !audioSharedMemoryReply && audioInvalidates == (test == 2 ? 1u : 0u));
            continue;
        }
        assert(client.audioOutput && audioSharedMemoryReply == client.audioOutput.sharedMemory && !audioInvalidates);
        LSBAudioOutput *first = client.audioOutput;
        handleMessage(messageFor(LSB_REQUEST_OPEN_AUDIO_OUTPUT, client.pid, client.pidVersion, 0));
        assert(replies[replyCount - 1].error == EBUSY && audioCreates == 1 && client.audioOutput == first && !audioInvalidates);
        if (test == 3) finishJob(client, ECANCELED);
        else {
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                handleMessage(messageFor(LSB_REQUEST_CLOSE_AUDIO_OUTPUT, client.pid, client.pidVersion, 0));
                assert(!replies[replyCount - 1].error && audioInvalidates == 1 && !client.audioOutput);
            }
        }
        assert(audioInvalidates == 1 && !client.audioOutput);
    }
    resetBroker();
}

static void testDirectConfigdWaitsForSelectedBridge(void) {
    for (unsigned variant = 0; variant < 4; ++variant) {
        resetBroker();
        LSBJob *launchd = runTestJob(nil, 798);
        launchd.simulatorLaunchd = YES;
        launchd.nativeConfigdSelected = variant != 3;
        launchd.servicesPending = YES;
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        NSString *physical = [runtimeRoot stringByAppendingPathComponent:@"usr/libexec/configd_sim"];
        setRequestProgram(request, variant == 1 ? physical.fileSystemRepresentation : "/usr/libexec/configd_sim");
        unsigned launches = submittedLaunches;
        enqueueSpawn(request, launchd);
        if (variant != 3) {
            assert(!startingJob && submittedLaunches == launches && !replyFor(object));
            launchd.servicesPending = NO;
            launchd.servicesError = variant == 2 ? EACCES : 0;
            startNextJob();
        }
        if (variant == 2) {
            assert(!startingJob && submittedLaunches == launches && replyFor(object)->error == EACCES);
            assertClosed(fd);
        } else {
            LSBJob *child = startingJob;
            assert(child && submittedLaunches == launches + 1 && !child.nativeConfigdSelected);
            handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 799, 0));
            handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 799, child.identifier));
            assert(child.phase == JobRunning && !replyFor(object)->error);
        }
    }
}

static LSBJob *queueNativeMetalSessionWithSpringBoard(LSBJob **launchd, const LSBNativeMetalControl **reader, BOOL springBoard) {
    resetBroker();
    enableIndigo = YES;
    enableSpringBoardMetal = springBoard;
    int fd;
    unsigned object;
    LSBReceivedMessage *initial = spawnRequest(&fd, &object);
    releaseMessageObject(initial->object);
    initial->object = NULL;
    setRequestProgram(initial, "/sbin/launchd_sim");
    assert(!LSBPlanAddString(&initial->plan, "LSB_SIMULATOR_LAUNCHD=1", &initial->plan.envp[initial->plan.envc++]));
    assert(!enqueueInitialJob(initial, LSB_EXECUTION_SIMULATOR));
    assert(jobs.count == (springBoard ? 4u : 3u) && startingJob == jobs[0] && submittedLaunches == 1);
    LSBJob *helper = jobs[0];
    *launchd = jobs.lastObject;
    assert(helper.executionRole == LSB_EXECUTION_NATIVE_METAL_SERVICE && helper.nativeMetalControl && !helper.parentIdentifier);
    assert(submittedLaunchOptions == LSB_EXTENSION_LAUNCH_GPU);
    assert((*launchd).simulatorLaunchd && !(*launchd).parentIdentifier && (*launchd).nativeMetalServiceIdentifier == helper.identifier);
    assert((*launchd).phase == JobQueued && !helper.request->plan.envc && helper.request->plan.argc == 1);
    assert(!strcmp(helper.request->plan.strings + helper.request->plan.path, "/__LiveSimBooterNativeMetalService"));
    assert(!strcmp(helper.request->plan.strings + helper.request->plan.argv[0], "Metal.backboardd"));
    unsigned index = helper.request->plan.fdCount - 1;
    assert(helper.request->plan.fds[index].number == 3 && !helper.request->plan.fds[index].flags);
    assert((fcntl(helper.request->fds[index], F_GETFL) & O_ACCMODE) == O_RDONLY);
    assert(!LSBNativeMetalControlImport(helper.request->fds[index], reader));
    if (springBoard) {
        LSBJob *second = jobs[1];
        struct stat firstFile, secondFile;
        assert(!fstat(helper.request->fds[index], &firstFile));
        assert(!fstat(second.request->fds[second.request->plan.fdCount - 1], &secondFile));
        assert(firstFile.st_dev != secondFile.st_dev || firstFile.st_ino != secondFile.st_ino);
        assert(helper.nativeMetalControl != second.nativeMetalControl && second.nativeMetalControl);
        assert(!strcmp(second.request->plan.strings + second.request->plan.argv[0], "Metal.SpringBoard"));
        assert((*launchd).springBoardMetalSelected && (*launchd).springBoardMetalServiceIdentifier == second.identifier);
    }
    LSBJob *publicHelper = findJob((*launchd).publicMetalServiceIdentifier);
    assert(publicHelper && publicHelper == jobs[jobs.count - 2] && publicHelper != helper && !publicHelper.parentIdentifier);
    assert(publicHelper.nativeMetalControl && publicHelper.nativeMetalControl != helper.nativeMetalControl);
    assert(!strcmp(publicHelper.request->plan.strings + publicHelper.request->plan.argv[0], "Metal.public"));
    LSBNativeMetalAuthorization authorization;
    assert(!LSBNativeMetalControlRead(publicHelper.nativeMetalControl, &authorization) && authorization.authorized && authorization.publicAccess);
    audit_token_t empty = {{0}};
    assert(!memcmp(&authorization.audit, &empty, sizeof(empty)) && !publicHelper.nativeMetalAuthorizedIdentifier);
    assert(!LSBNativeMetalControlRead(*reader, &authorization) && !authorization.authorized && !authorization.publicAccess);
    reportedPID = getpid() + 300;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 900, 0));
    assert(helper.phase == JobDispatched && replies[replyCount - 1].executionRole == LSB_EXECUTION_NATIVE_METAL_SERVICE);
    assert(!replies[replyCount - 1].launchdTank && (*launchd).phase == JobQueued && submittedLaunches == 1);
    return helper;
}

static LSBJob *queueNativeMetalSession(LSBJob **launchd, const LSBNativeMetalControl **reader) {
    return queueNativeMetalSessionWithSpringBoard(launchd, reader, NO);
}

static void readyPublicMetalHelper(LSBJob *launchd) {
    LSBJob *helper = findJob(launchd.publicMetalServiceIdentifier);
    assert(helper && helper.executionRole == LSB_EXECUTION_NATIVE_METAL_SERVICE);
    if (helper.phase == JobRunning) return;
    assert(startingJob == helper && helper.phase == JobStarting && launchd.phase == JobQueued);
    reportedPID = getpid() + (pid_t)helper.identifier + 500;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 990, 0));
    assert(helper.phase == JobDispatched && launchd.phase == JobQueued);
    mach_port_t endpoint = newServiceEndpoint();
    [publicTestEndpoints addObject:@(endpoint)];
    LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, helper.pid, helper.pidVersion, helper.identifier);
    ready->nativeServicePort = endpoint;
    handleMessage(ready);
    assert(helper.phase == JobRunning && startingJob == launchd);
}

static void makeSessionLaunchdRunning(LSBJob *launchd) {
    readyPublicMetalHelper(launchd);
    assert(startingJob == launchd && launchd.phase == JobStarting);
    launchd.phase = JobRunning;
    launchd.pid = launchd.guestPID = getpid() + 100;
    launchd.pidVersion = 901;
    launchd.audit = (audit_token_t){{1, 2, 3, 4, 5, (unsigned)launchd.pid, 7, 901}};
    disposeMessage(launchd.request);
    launchd.request = NULL;
    startingJob = nil;
}

static void testNativeMetalReadyGatesLaunchd(void) {
    for (unsigned failure = 0; failure < 4; ++failure) {
        LSBJob *launchd;
        const LSBNativeMetalControl *reader;
        LSBJob *helper = queueNativeMetalSession(&launchd, &reader);
        LSBReceivedMessage *forged = messageFor(LSB_REQUEST_READY, helper.pid, helper.pidVersion, helper.identifier);
        forged->audit.val[1] = 123;
        handleMessage(forged);
        assert(replies[replyCount - 1].error == EACCES && helper.phase == JobDispatched && submittedLaunches == 1);
        mach_port_t endpoint = failure == 1 ? MACH_PORT_NULL : newServiceEndpoint();
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, helper.pid, helper.pidVersion, helper.identifier);
        ready->nativeServicePort = endpoint;
        if (failure == 2) ready->error = ENOENT;
        if (failure == 3) readySendError = EPIPE;
        handleMessage(ready);
        if (!failure) {
            assert(helper.phase == JobRunning && helper.nativeServicePort == endpoint && hasRight(endpoint, MACH_PORT_TYPE_SEND));
            assert(startingJob == findJob(launchd.publicMetalServiceIdentifier) && submittedLaunches == 2);
            assert(!helper.parentIdentifier && !launchd.parentIdentifier);
            makeSessionLaunchdRunning(launchd);
            finishJob(launchd, ECANCELED);
        } else assert(!jobs.count && !startingJob && submittedLaunches == 1);
        LSBNativeMetalAuthorization authorization;
        assert(!LSBNativeMetalControlRead(reader, &authorization) && !authorization.authorized);
        if (MACH_PORT_VALID(endpoint)) {
            assert(!hasRight(endpoint, MACH_PORT_TYPE_SEND));
            assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
        }
        LSBNativeMetalControlUnmap(reader);
    }
    // An ordinary simulator child cannot donate a native service endpoint through READY.
    resetBroker();
    int fd;
    unsigned object;
    enqueueSpawn(spawnRequest(&fd, &object), nil);
    LSBJob *guest = startingJob;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 905, 0));
    mach_port_t endpoint = newServiceEndpoint();
    LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, getpid(), 905, guest.identifier);
    ready->nativeServicePort = endpoint;
    handleMessage(ready);
    assert(!jobs.count && replyFor(object)->error == EPROTO && !hasRight(endpoint, MACH_PORT_TYPE_SEND));
    assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
}

static void testNativeMetalOwnerLifecycle(void) {
    for (unsigned failure = 0; failure < 7; ++failure) {
        LSBJob *launchd;
        const LSBNativeMetalControl *reader;
        LSBJob *helper = queueNativeMetalSession(&launchd, &reader);
        mach_port_t endpoint = newServiceEndpoint();
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, helper.pid, helper.pidVersion, helper.identifier);
        ready->nativeServicePort = endpoint;
        handleMessage(ready);
        makeSessionLaunchdRunning(launchd);
        reportedPID = getpid();
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        setRequestProgram(request, "/usr/libexec/backboardd");
        enqueueSpawn(request, launchd);
        LSBJob *backboard = startingJob;
        LSBReceivedMessage *checkin = messageFor(LSB_REQUEST_CHECKIN, getpid(), 902, 0);
        checkin->audit.val[1] = 501;
        checkin->audit.val[6] = 77;
        audit_token_t identity = checkin->audit;
        handleMessage(checkin);
        LSBNativeMetalAuthorization authorization;
        assert(!LSBNativeMetalControlRead(reader, &authorization) && authorization.authorized);
        assert(!memcmp(&authorization.audit, &identity, sizeof(identity)) && helper.nativeMetalAuthorizedIdentifier == backboard.identifier);
        ready = messageFor(LSB_REQUEST_READY, getpid(), 902, backboard.identifier);
        if (failure == 1) ready->error = ENOEXEC;
        handleMessage(ready);
        if (failure != 1) {
            assert(backboard.phase == JobRunning && !startingJob);
            unsigned launches = submittedLaunches;
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                request = spawnRequest(&fd, &object);
                request->kind = LSB_REQUEST_EXEC;
                request->audit = identity;
                handleMessage(request);
                assert(replyFor(object)->error == ENOTSUP && submittedLaunches == launches && !backboard.replacementIdentifier);
                assert(!LSBNativeMetalControlRead(reader, &authorization) && authorization.authorized);
                assert(!memcmp(&authorization.audit, &identity, sizeof(identity)));
            }
            if (failure == 2) recordChildExit(helper.identifier, SIGABRT);
            else if (failure == 3) finishJob(launchd, ECANCELED);
            else if (failure >= 4) {
                if (failure == 5) {
                    // A vanished audited instance is already exiting; preserve the monitor rather than fabricating assertion failure.
                    identityCheckError = ESRCH;
                    unsigned releases = releasedLaunches;
                    extensionCallback(helper.identifier, 0, ECANCELED, NULL);
                    assert(findJob(helper.identifier) && findJob(backboard.identifier) && releases == releasedLaunches);
                    assert(!deliveredSignals && !submittedExtensionSignals);
                    assert(!LSBNativeMetalControlRead(reader, &authorization) && authorization.authorized);
                    identityCheckError = 0;
                    recordChildExit(helper.identifier, 0);
                } else {
                    directSignalError = EPERM;
                    if (failure == 6) identityCheckError = EACCES;
                    signalHook = ^{
                        LSBNativeMetalAuthorization current;
                        assert(!LSBNativeMetalControlRead(reader, &current) && !current.authorized);
                    };
                    extensionCallback(helper.identifier, 0, ECANCELED, NULL);
                    if (failure == 4) assert(deliveredSignals == 2 && submittedExtensionSignals == 2 && lastSignal == SIGKILL);
                    else assert(!deliveredSignals && !submittedExtensionSignals); // No identity proof, no signal by bare PID or instance.
                    signalHook = nil;
                    identityCheckError = 0;
                }
                unsigned signals = deliveredSignals, releases = releasedLaunches;
                extensionCallback(helper.identifier, 0, ECANCELED, NULL);
                assert(deliveredSignals == signals && releasedLaunches == releases); // Removed jobs ignore late invalidation callbacks.
            } else recordChildExit(backboard.identifier, 0);
        }
        assert(!findJob(helper.identifier) && !findJob(backboard.identifier));
        assert(!LSBNativeMetalControlRead(reader, &authorization) && !authorization.authorized);
        assert(!helper.nativeMetalAuthorizedIdentifier && !helper.nativeServicePort && !hasRight(endpoint, MACH_PORT_TYPE_SEND));
        if (failure != 3) {
            assert(findJob(launchd.identifier) && launchd.servicesError && !launchd.servicesPending);
            request = spawnRequest(&fd, &object);
            setRequestProgram(request, "/usr/libexec/backboardd");
            enqueueSpawn(request, launchd);
            assert(replyFor(object)->error == launchd.servicesError && !startingJob);
        }
        assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
        LSBNativeMetalControlUnmap(reader);
    }
}

static void testNativeProbeAssertionLoss(void) {
    resetBroker();
    LSBJob *simulator = runTestJob(nil, 910);
    unsigned checks = identityChecks;
    extensionCallback(simulator.identifier, 0, ECANCELED, NULL);
    assert(findJob(simulator.identifier) && identityChecks == checks && !deliveredSignals); // Existing simulator policy is unchanged.
    resetBroker();
    LSBJob *probe = auditJob(getpid() + 301, getpid() + 301, 911, NO);
    probe.executionRole = LSB_EXECUTION_NATIVE_METAL_PROBE;
    directSignalError = EPERM;
    extensionEvent(probe.identifier, 0, ECANCELED, NULL);
    assert(!findJob(probe.identifier) && deliveredSignals == 1 && submittedExtensionSignals == 1);
    assert(signaledRequest == probe.identifier && lastSignal == SIGKILL);
    audit_token_t expected = probe.audit;
    assert(!memcmp(&signaledAudit, &expected, sizeof(expected)));
    resetBroker();
    probe = auditJob(getpid() + 302, getpid() + 302, 912, NO);
    probe.executionRole = LSB_EXECUTION_NATIVE_METAL_PROBE;
    identityCheckError = ESRCH;
    extensionEvent(probe.identifier, 0, ECANCELED, NULL);
    assert(findJob(probe.identifier) && !releasedLaunches && !deliveredSignals && !submittedExtensionSignals);
    recordChildExit(probe.identifier, 0);
    assert(!findJob(probe.identifier) && releasedLaunches == 1 && !deliveredSignals && !submittedExtensionSignals);
}

static void testMetalEndpointRegistration(void) {
    for (unsigned failure = 0; failure < 3; ++failure) {
        LSBJob *launchd;
        const LSBNativeMetalControl *reader;
        LSBJob *helper = queueNativeMetalSession(&launchd, &reader);
        mach_port_t endpoint = newServiceEndpoint(), service = newServiceEndpoint();
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, helper.pid, helper.pidVersion, helper.identifier);
        ready->nativeServicePort = endpoint;
        handleMessage(ready);
        makeSessionLaunchdRunning(launchd);
        launchd.indigo = [RecordedIndigoServer new];
        launchd.framebuffer = [RecordedFramebufferServer new];
        launchd.iosurface = [RecordedIOSurfaceServer new];
        launchd.servicesPending = YES;
        if (failure) endpointErrors[failure + 2] = EACCES;
        registerDisplayServices(launchd, service);
        runMainQueueUntil(^BOOL { return !launchd.servicesPending; });
        assert(endpointCalls == (failure == 1 ? 4u : 5u) && launchd.servicesError == (failure ? EACCES : 0));
        assert(!strcmp(registeredEndpoints[3], "com.apple.metal.simulator") && registeredPorts[3] == findJob(launchd.publicMetalServiceIdentifier).nativeServicePort);
        if (failure != 1) assert(!strcmp(registeredEndpoints[4], "com.apple.metal.simulator.backboardd") && registeredPorts[4] == endpoint);
        audit_token_t expected = launchd.audit;
        assert(!memcmp(&registeredAudits[3], &expected, sizeof(expected)));
        finishJob(launchd, ECANCELED);
        assert(!hasRight(endpoint, MACH_PORT_TYPE_SEND));
        assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
        assert(!mach_port_deallocate(mach_task_self(), service));
        assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
        LSBNativeMetalControlUnmap(reader);
    }
}

static void testMetalRegistrationCannotClearHelperDeath(void) {
    for (unsigned publicDeath = 0; publicDeath < 2; ++publicDeath) {
        LSBJob *launchd;
        const LSBNativeMetalControl *reader;
        LSBJob *helper = queueNativeMetalSession(&launchd, &reader);
        mach_port_t endpoint = newServiceEndpoint(), service = newServiceEndpoint();
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, helper.pid, helper.pidVersion, helper.identifier);
        ready->nativeServicePort = endpoint;
        handleMessage(ready);
        makeSessionLaunchdRunning(launchd);
        LSBJob *dying = publicDeath ? findJob(launchd.publicMetalServiceIdentifier) : helper;
        mach_port_t dyingPort = dying.nativeServicePort;
        launchd.servicesPending = YES;
        dispatch_semaphore_t entered = dispatch_semaphore_create(0), release = dispatch_semaphore_create(0);
        endpointHook = ^(unsigned index) {
            if (index) return;
            dispatch_semaphore_signal(entered);
            assert(!dispatch_semaphore_wait(release, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
        };
        registerDisplayServices(launchd, service);
        assert(!dispatch_semaphore_wait(entered, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
        recordChildExit(dying.identifier, SIGABRT);
        assert(launchd.servicesError == EPIPE && !launchd.servicesPending);
        // Each in-flight endpoint registration owns a reference even after its helper is retired.
        assert(hasRight(dyingPort, MACH_PORT_TYPE_SEND));
        launchd.servicesPending = YES;
        dispatch_semaphore_signal(release);
        runMainQueueUntil(^BOOL { return !launchd.servicesPending; });
        assert(launchd.servicesError == EPIPE && endpointCalls == 2 && !hasRight(dyingPort, MACH_PORT_TYPE_SEND));
        finishJob(launchd, ECANCELED);
        assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
        assert(!mach_port_deallocate(mach_task_self(), service));
        assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
        LSBNativeMetalControlUnmap(reader);
    }
}

static void testHostSpringBoardMetalSelection(void) {
    char temporary[] = "/tmp/LSBBrokerSpringBoard.XXXXXX";
    assert(mkdtemp(temporary));
    char canonical[PATH_MAX];
    assert(realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    NSFileManager *manager = NSFileManager.defaultManager;
    NSArray *cases = @[
        @{@"Label": @"com.apple.SpringBoard", @"Program": @"/System/Library/CoreServices/SpringBoard.app/SpringBoard", @"expected": @YES},
        @{@"Label": @"com.apple.SpringBoard", @"ProgramArguments": @[@"/System/Library/CoreServices/SpringBoard.app/SpringBoard"], @"expected": @YES},
        @{@"Label": @"com.apple.SpringBoard", @"Program": @"/bin/df",
          @"ProgramArguments": @[@"/System/Library/CoreServices/SpringBoard.app/SpringBoard"], @"expected": @NO},
        @{@"Label": @"com.apple.SpringBoard", @"Program": @"/System/Library/CoreServices/SpringBoard.app/SpringBoardOther", @"expected": @NO},
        @{@"Label": @"com.apple.other", @"Program": @"/System/Library/CoreServices/SpringBoard.app/SpringBoard", @"expected": @NO}
    ];
    for (NSDictionary *test in cases) {
        resetBroker();
        runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
        NSString *daemons = [runtimeRoot stringByAppendingPathComponent:@"System/Library/LaunchDaemons"];
        assert([manager createDirectoryAtPath:daemons withIntermediateDirectories:YES attributes:nil error:NULL]);
        NSDictionary *backboard = @{@"MachServices": @{@"com.apple.backboardd.virtualframebuffer": @YES}};
        assert([backboard writeToFile:[daemons stringByAppendingPathComponent:@"com.apple.backboardd.plist"] atomically:YES]);
        NSDictionary *preferences = @{@"Label": @"com.apple.cfprefsd.xpc.daemon", @"ProgramArguments": @[@"/usr/sbin/cfprefsd", @"daemon"],
                                     @"MachServices": @{@"com.apple.cfprefsd.daemon": @YES}};
        assert([preferences writeToFile:[daemons stringByAppendingPathComponent:@"com.apple.cfprefsd.xpc.daemon.plist"] atomically:YES]);
        NSString *label = test[@"Label"], *path = [daemons stringByAppendingPathComponent:[label stringByAppendingString:@".plist"]];
        NSMutableDictionary *stock = test.mutableCopy;
        [stock removeObjectForKey:@"expected"];
        assert([stock writeToFile:path atomically:YES]);
        LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
        const char *argv[] = {"LiveSimBooter", "--launchd-services", label.UTF8String, NULL};
        assert(!initialPlan(initial, 3, argv, NULL));
        assert(enableSpringBoardMetal == [test[@"expected"] boolValue]);
        BOOL selected = YES;
        const char *unselected[] = {"com.apple.other", NULL};
        assert(!configuredSpringBoardMetal(&initial->plan, unselected, &selected) && !selected);
        disposeMessage(initial);
    }
    assert([manager removeItemAtPath:directory error:NULL]);
    resetBroker();
    LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
    const char *argv[] = {"LiveSimBooter", "/System/Library/CoreServices/SpringBoard.app/SpringBoard", NULL};
    const char *env[] = {"LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS=1", "LSB_NATIVE_METAL_SERVICE=1", NULL};
    assert(!initialPlan(initial, 2, argv, env) && !enableSpringBoardMetal && !enableIndigo);
    disposeMessage(initial); // Neither an executable path nor guest environment can provision the host-native role.
}

static void testIconLaunchUsesGraphicalLaunchd(void) {
    char temporary[] = "/tmp/LSBIconLaunch.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    NSString *root = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    NSString *daemons = [root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"];
    NSString *angels = [root stringByAppendingPathComponent:@"System/Library/LaunchAngels"];
    NSFileManager *manager = NSFileManager.defaultManager;
    assert([manager createDirectoryAtPath:daemons withIntermediateDirectories:YES attributes:nil error:NULL]);
    assert([manager createDirectoryAtPath:angels withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSArray *selected = @[@"com.apple.notifyd", @"com.apple.lsd", @"com.apple.runningboardd", @"com.apple.containermanagerd",
        @"com.apple.fontservicesd", @"com.apple.containermanagerd.system", @"com.apple.distnoted.xpc.daemon", @"com.apple.configd_sim",
        @"com.apple.trustd", @"com.apple.sleepd", @"com.apple.systemstatusd", @"com.apple.SpringBoard",
        @"com.apple.iconservices.iconservicesagent", @"com.apple.TextInput.kbd",
        @"com.apple.assistantd", @"com.apple.PosterBoard", @"com.apple.DragUI.druid"];
    // iOS 26.0 23A343 trustd uses the stock certificate store and one guest Mach service; no native trust service is exposed.
    NSDictionary *trustEnvironment = @{@"DEBUGSCOPE":
        @"-policy-node,policy-set,policy,alloc,trust,bind,profile,trace,dbconn,OTR,serverxpc,sqlite3,error_thee_well, ringSigning",
        @"WAIT4DEBUGGER": @"NO"};
    NSDictionary *enabled = @{@"#IfFeatureFlagEnabled": @"SystemStatus/EmbeddedDaemon", @"#Then": @YES};
    NSDictionary *disabled = @{@"#IfFeatureFlagDisabled": @"SystemStatus/EmbeddedDaemon", @"#Then": @YES};
    NSArray *statusServices = @[@"com.apple.systemstatus", @"com.apple.systemstatus.publisher", @"com.apple.systemstatus.activityattribution"];
    // iOS 26.0 23A343 LaunchDaemons/com.apple.assistantd.plist: preserve its service declarations, including dictation.
    NSArray *assistantServiceNames = @[@"com.apple.aps.assistantd.fmip", @"com.apple.assistant.analytics",
        @"com.apple.assistant.analytics-observation", @"com.apple.assistant.audio-session-assertion", @"com.apple.assistant.client",
        @"com.apple.assistant.device-context", @"com.apple.assistant.dictation", @"com.apple.assistant.flow_service",
        @"com.apple.assistant.security", @"com.apple.assistant.settings", @"com.apple.assistant.sync", @"com.apple.assistantd.aps",
        @"com.apple.assistantd.managedstorage", @"com.apple.private.alloy.siri.device-idswake", @"com.apple.private.alloy.siri.icloud-idswake",
        @"com.apple.private.alloy.siri.location-idswake", @"com.apple.private.alloy.siri.proxy-idswake", @"com.apple.siri.analytics.assistant",
        @"com.apple.siri.client_lite", @"com.apple.siri.external_request", @"com.apple.siri.location",
        @"com.apple.siri.orchestration.capabilities", @"com.apple.siri.orchestration.intelligencesession", @"com.apple.siri.pairedsync",
        @"com.apple.siri.pommes_search_xpc_service", @"com.apple.siri.tether", @"com.apple.siri.vocabularyupdates"];
    NSMutableDictionary *assistantServices = [NSMutableDictionary dictionary];
    for (NSString *name in assistantServiceNames) assistantServices[name] = @YES;
    for (NSString *label in selected) {
        NSString *jobDirectory = daemons;
        NSString *program = [label isEqual:@"com.apple.SpringBoard"] ? @"/System/Library/CoreServices/SpringBoard.app/SpringBoard" : @"/bin/df";
        NSDictionary *job = @{@"Label": label, @"ProgramArguments": @[program]};
        if ([label isEqual:@"com.apple.TextInput.kbd"]) job = @{@"Label": label, @"ProgramArguments": @[@"/System/Library/TextInput/kbd"],
            @"MachServices": @{@"com.apple.TextInput": @YES, @"com.apple.TextInput.preferences": @YES},
            @"EnvironmentVariables": @{@"MallocNanoZone": @"0"}};
        if ([label isEqual:@"com.apple.assistantd"]) job = @{@"Label": label,
            @"ProgramArguments": @[@"/System/Library/PrivateFrameworks/AssistantServices.framework/assistantd"],
            @"MachServices": assistantServices, @"RunAtLoad": @YES, @"KeepAlive ": @YES, @"EnableTransactions": @YES,
            @"ProcessType": @"Adaptive", @"UserName": @"mobile"};
        if ([label isEqual:@"com.apple.trustd"]) job = @{@"Label": label, @"ProgramArguments": @[@"/usr/libexec/trustd"],
            @"MachServices": @{@"com.apple.trustd": @YES}, @"EnvironmentVariables": trustEnvironment,
            @"UserName": @"_trustd", @"GroupName": @"_trustd", @"POSIXSpawnType": @"Adaptive", @"Umask": @54,
            @"MinimalBootProfile": @YES, @"EnableTransactions": @YES, @"EnablePressuredExit": @YES,
            @"_PanicOnCrash": @{@"AutomationOnly": @YES, @"PanicOnConsecutiveCrash": @YES}};
        if ([label isEqual:@"com.apple.PosterBoard"]) {
            jobDirectory = angels;
            job = @{@"Label": label, @"ProgramArguments": @[@"/Applications/PosterBoard.app/PosterBoard"], @"RunAtLoad": @NO,
                @"KeepAlive": @NO, @"EnableTransactions": @NO, @"EnablePressuredExit": @NO, @"ProcessType": @"App", @"POSIXSpawnType": @"App",
                @"MachServices": @{@"com.apple.posterboardservices.services": @{@"_ExperimentalNonLaunching": @YES}},
                @"_AdditionalProperties": @{@"RunningBoard": @{@"Managed": @YES, @"Reported": @YES}}};
        }
        if ([label isEqual:@"com.apple.systemstatusd"] || [label isEqual:@"com.apple.SpringBoard"]) {
            BOOL separate = [label isEqual:@"com.apple.systemstatusd"];
            NSString *statusProgram = separate ? @"/System/Library/PrivateFrameworks/SystemStatusServer.framework/Support/systemstatusd" : program;
            job = @{@"Label": label, @"ProgramArguments": @[statusProgram],
                @"MachServices": [NSDictionary dictionaryWithObjects:@[separate ? enabled : disabled, separate ? enabled : disabled,
                    separate ? enabled : disabled] forKeys:statusServices]};
        }
        assert([job writeToFile:[jobDirectory stringByAppendingPathComponent:[label stringByAppendingString:@".plist"]] atomically:YES]);
    }
    NSDictionary *backboard = @{@"MachServices": @{@"com.apple.backboardd.virtualframebuffer": @YES}};
    assert([backboard writeToFile:[daemons stringByAppendingPathComponent:@"com.apple.backboardd.plist"] atomically:YES]);
    NSDictionary *preferences = @{@"Label": @"com.apple.cfprefsd.xpc.daemon", @"ProgramArguments": @[@"/usr/sbin/cfprefsd", @"daemon"],
        @"MachServices": @{@"com.apple.cfprefsd.daemon": @YES}};
    assert([preferences writeToFile:[daemons stringByAppendingPathComponent:@"com.apple.cfprefsd.xpc.daemon.plist"] atomically:YES]);
    NSString *firstDirectory = nil;
    for (unsigned explicitMode = 0; explicitMode < 2; ++explicitMode) {
        resetBroker();
        runtimeRoot = root;
        const char *arguments[] = {"LiveSimBooter", "--launchd-services", NULL};
        const char *environment[] = {"LSB_FAKE_PTY_STATE=/untrusted", NULL};
        LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
        assert(!initialPlan(initial, explicitMode ? 2 : 1, arguments, environment));
        assert(enableIndigo && enableSpringBoardMetal && initialNativeConfigdPending && !hostTerminal && !initialTerminalPending);
        assert(!strcmp(initial->plan.strings + initial->plan.path, "/sbin/launchd_sim"));
        NSString *configuration = [NSString stringWithUTF8String:initial->plan.strings + initial->plan.argv[1]];
        NSDictionary *bootstrap = [NSDictionary dictionaryWithContentsOfFile:configuration][@"Bootstrap"];
        NSString *jobDirectory = [bootstrap[@"Paths"] firstObject];
        if (firstDirectory) assert([jobDirectory isEqual:firstDirectory]);
        else firstDirectory = jobDirectory;
        NSMutableSet *expected = [NSMutableSet new];
        for (NSString *label in selected) [expected addObject:[label stringByAppendingString:@".plist"]];
        [expected addObjectsFromArray:@[@"com.apple.backboardd.plist", @"com.apple.cfprefsd.xpc.daemon.plist",
            @"com.kdt.LiveSimBooter.terminal-fish.plist", @"com.kdt.LiveSimBooter.display-settings.plist"]];
        assert([[NSSet setWithArray:[manager contentsOfDirectoryAtPath:jobDirectory error:NULL]] isEqual:expected]);
        assert(!bootstrap[@"Self"][@"LSB_FAKE_PTY_STATE"] && !bootstrap[@"Child"][@"LSB_FAKE_PTY_STATE"]);
        NSDictionary *terminal = [NSDictionary dictionaryWithContentsOfFile:
            [jobDirectory stringByAppendingPathComponent:@"com.kdt.LiveSimBooter.terminal-fish.plist"]];
        assert(([terminal[@"ProgramArguments"] isEqual:@[@"/usr/libexec/terminal-fish"]]));
        assert([terminal[@"RunAtLoad"] boolValue] && ![terminal[@"KeepAlive"] boolValue]);
        NSDictionary *keyboard = [NSDictionary dictionaryWithContentsOfFile:
            [jobDirectory stringByAppendingPathComponent:@"com.apple.TextInput.kbd.plist"]];
        assert(([keyboard[@"ProgramArguments"] isEqual:@[@"/System/Library/TextInput/kbd"]]));
        assert([keyboard[@"MachServices"][@"com.apple.TextInput"] boolValue]);
        assert([keyboard[@"MachServices"][@"com.apple.TextInput.preferences"] boolValue]);
        assert([keyboard[@"EnvironmentVariables"][@"MallocNanoZone"] isEqual:@"0"]);
        assert([keyboard[@"RunAtLoad"] boolValue] && ![keyboard[@"KeepAlive"] boolValue]);
        NSDictionary *assistant = [NSDictionary dictionaryWithContentsOfFile:
            [jobDirectory stringByAppendingPathComponent:@"com.apple.assistantd.plist"]];
        assert(([assistant[@"ProgramArguments"] isEqual:@[@"/System/Library/PrivateFrameworks/AssistantServices.framework/assistantd"]]));
        assert([assistant[@"MachServices"] isEqual:assistantServices] && [assistant[@"MachServices"][@"com.apple.assistant.dictation"] boolValue]);
        assert([assistant[@"RunAtLoad"] boolValue] && ![assistant[@"KeepAlive"] boolValue] && ![assistant[@"EnableTransactions"] boolValue]);
        assert(!assistant[@"UserName"] && !assistant[@"ProcessType"] && !assistant[@"KeepAlive "]);
        NSDictionary *trust = [NSDictionary dictionaryWithContentsOfFile:[jobDirectory stringByAppendingPathComponent:@"com.apple.trustd.plist"]];
        assert(([trust[@"ProgramArguments"] isEqual:@[@"/usr/libexec/trustd"]]));
        assert(([trust[@"MachServices"] isEqual:@{@"com.apple.trustd": @YES}]));
        assert(CFGetTypeID((__bridge CFTypeRef)trust[@"MachServices"][@"com.apple.trustd"]) == CFBooleanGetTypeID());
        assert([trust[@"EnvironmentVariables"] isEqual:trustEnvironment]);
        for (NSString *key in @[@"RunAtLoad", @"KeepAlive", @"EnableTransactions"]) {
            assert(CFGetTypeID((__bridge CFTypeRef)trust[key]) == CFBooleanGetTypeID());
            assert([trust[key] boolValue] == [key isEqual:@"RunAtLoad"]);
        }
        for (NSString *key in @[@"UserName", @"GroupName", @"POSIXSpawnType", @"Umask", @"MinimalBootProfile", @"EnablePressuredExit", @"_PanicOnCrash"])
            assert(!trust[key]);
        assert([trust[@"WorkingDirectory"] isEqual:@"/private/var/mobile/"]);
        assert([trust[@"StandardOutPath"] isEqual:@"/private/var/mobile/Library/Logs/com.apple.trustd.log"]);
        assert([trust[@"StandardErrorPath"] isEqual:trust[@"StandardOutPath"]]);
        NSDictionary *poster = [NSDictionary dictionaryWithContentsOfFile:
            [jobDirectory stringByAppendingPathComponent:@"com.apple.PosterBoard.plist"]];
        assert(([poster[@"ProgramArguments"] isEqual:@[@"/Applications/PosterBoard.app/PosterBoard"]]));
        assert(![poster[@"RunAtLoad"] boolValue] && ![poster[@"KeepAlive"] boolValue]);
        assert([poster[@"MachServices"][@"com.apple.posterboardservices.services"][@"_ExperimentalNonLaunching"] boolValue]);
        assert([poster[@"_AdditionalProperties"][@"RunningBoard"][@"Managed"] boolValue]);
        assert([poster[@"_AdditionalProperties"][@"RunningBoard"][@"Reported"] boolValue]);
        NSDictionary *status = [NSDictionary dictionaryWithContentsOfFile:
            [jobDirectory stringByAppendingPathComponent:@"com.apple.systemstatusd.plist"]];
        NSDictionary *springBoard = [NSDictionary dictionaryWithContentsOfFile:
            [jobDirectory stringByAppendingPathComponent:@"com.apple.SpringBoard.plist"]];
        assert(([status[@"ProgramArguments"] isEqual:@[@"/System/Library/PrivateFrameworks/SystemStatusServer.framework/Support/systemstatusd"]]));
        for (NSString *name in statusServices) {
            assert([status[@"MachServices"][name] isEqual:enabled]);
            assert([springBoard[@"MachServices"][name] isEqual:disabled]);
        }
        assert([status[@"RunAtLoad"] boolValue] && ![status[@"KeepAlive"] boolValue] && ![status[@"EnableTransactions"] boolValue]);
        assert(!enqueueInitialJob(initial, LSB_EXECUTION_SIMULATOR));
        assert(jobs.lastObject.simulatorLaunchd && jobs.lastObject.nativeConfigdSelected);
        assert(!initialNativeConfigdPending); // Native helper plans reset the temporary flag; the launchd snapshot must survive them.
        for (LSBJob *job in jobs) if (job != jobs.lastObject) assert(!job.nativeConfigdSelected);
    }
    resetBroker();
    runtimeRoot = root;
    const char *arguments[] = {"LiveSimBooter", "--launchd-services", "com.apple.notifyd", NULL};
    const char *environment[] = {"LSB_NATIVE_CONFIGD=1", "com.apple.SystemConfiguration.configd=1", NULL};
    LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
    assert(!initialPlan(initial, 3, arguments, environment) && !initialNativeConfigdPending);
    NSString *customConfiguration = [NSString stringWithUTF8String:initial->plan.strings + initial->plan.argv[1]];
    NSString *customJobs = [NSDictionary dictionaryWithContentsOfFile:customConfiguration][@"Bootstrap"][@"Paths"][0];
    assert(![manager fileExistsAtPath:[customJobs stringByAppendingPathComponent:@"com.apple.trustd.plist"]]);
    assert(!enqueueInitialJob(initial, LSB_EXECUTION_SIMULATOR));
    assert(jobs.lastObject.simulatorLaunchd && !jobs.lastObject.nativeConfigdSelected); // Guest environment cannot request a native bridge.
    resetBroker();
    assert([manager removeItemAtPath:directory error:NULL]);
}

static LSBJob *readyMetalHelper(LSBJob *helper, mach_port_t endpoint, uint32_t generation) {
    if (helper.phase == JobStarting) {
        reportedPID = getpid() + (pid_t)helper.identifier + 300;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, generation, 0));
    }
    assert(helper.phase == JobDispatched);
    LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, helper.pid, helper.pidVersion, helper.identifier);
    ready->nativeServicePort = endpoint;
    handleMessage(ready);
    assert(helper.phase == JobRunning && helper.nativeServicePort == endpoint);
    return helper;
}

static LSBJob *startGraphicsOwner(LSBJob *launchd, const char *program, uint32_t generation) {
    int fd;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    setRequestProgram(request, program);
    enqueueSpawn(request, launchd);
    LSBJob *owner = startingJob;
    assert(owner);
    reportedPID = getpid(); // READY obtains a real process group; distinct audit generations model the independent owners.
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, generation, 0));
    assert(owner.phase == JobDispatched);
    handleMessage(messageFor(LSB_REQUEST_READY, owner.pid, owner.pidVersion, owner.identifier));
    assert(owner.phase == JobRunning && replyFor(object) && !replyFor(object)->error);
    return owner;
}

static void testPublicMetalReadyFailureRevokesSession(void) {
    for (unsigned failure = 0; failure < 3; ++failure) {
        LSBJob *launchd;
        const LSBNativeMetalControl *privateReader, *publicReader;
        LSBJob *privateHelper = queueNativeMetalSession(&launchd, &privateReader);
        LSBJob *publicHelper = findJob(launchd.publicMetalServiceIdentifier);
        assert(!LSBNativeMetalControlImport(publicHelper.request->fds[publicHelper.request->plan.fdCount - 1], &publicReader));
        mach_port_t privatePort = newServiceEndpoint();
        readyMetalHelper(privateHelper, privatePort, 900);
        assert(startingJob == publicHelper && launchd.phase == JobQueued);
        reportedPID = getpid() + 501;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 991, 0));
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, publicHelper.pid, publicHelper.pidVersion, publicHelper.identifier);
        ready->audit.val[1] ^= 1;
        handleMessage(ready);
        assert(replies[replyCount - 1].error == EACCES && publicHelper.phase == JobDispatched && launchd.phase == JobQueued);
        ready = messageFor(LSB_REQUEST_READY, publicHelper.pid, publicHelper.pidVersion, publicHelper.identifier);
        mach_port_t publicPort = failure ? newServiceEndpoint() : MACH_PORT_NULL;
        ready->nativeServicePort = publicPort;
        if (failure == 1) ready->error = ENOEXEC;
        if (failure == 2) readySendError = EPIPE;
        handleMessage(ready);
        assert(!jobs.count && !startingJob && submittedLaunches == 2);
        LSBNativeMetalAuthorization authorization;
        assert(!LSBNativeMetalControlRead(publicReader, &authorization) && !authorization.authorized && !authorization.publicAccess);
        assert(!LSBNativeMetalControlRead(privateReader, &authorization) && !authorization.authorized);
        assert(!hasRight(privatePort, MACH_PORT_TYPE_SEND));
        assert(!mach_port_mod_refs(mach_task_self(), privatePort, MACH_PORT_RIGHT_RECEIVE, -1));
        if (MACH_PORT_VALID(publicPort)) {
            assert(!hasRight(publicPort, MACH_PORT_TYPE_SEND));
            assert(!mach_port_mod_refs(mach_task_self(), publicPort, MACH_PORT_RIGHT_RECEIVE, -1));
        }
        LSBNativeMetalControlUnmap(privateReader);
        LSBNativeMetalControlUnmap(publicReader);
    }
}

static void testPublicMetalLifetimeAndClientIndependence(void) {
    for (unsigned helperFails = 0; helperFails < 2; ++helperFails) {
        LSBJob *launchd;
        const LSBNativeMetalControl *privateReader, *publicReader;
        LSBJob *privateHelper = queueNativeMetalSession(&launchd, &privateReader);
        LSBJob *publicHelper = findJob(launchd.publicMetalServiceIdentifier);
        assert(!LSBNativeMetalControlImport(publicHelper.request->fds[publicHelper.request->plan.fdCount - 1], &publicReader));
        mach_port_t privatePort = newServiceEndpoint();
        readyMetalHelper(privateHelper, privatePort, 900);
        // The public service must also become ready before releasing simulator launchd.
        assert(startingJob == publicHelper && launchd.phase == JobQueued);
        makeSessionLaunchdRunning(launchd);
        mach_port_t publicPort = publicHelper.nativeServicePort;
        assert(publicPort != privatePort && submittedLaunches == 3);
        RecordedIOSurfaceServer *iosurface = [RecordedIOSurfaceServer new];
        launchd.iosurface = iosurface;
        LSBJob *backboard = startGraphicsOwner(launchd, "/usr/libexec/backboardd", 920);
        LSBNativeMetalAuthorization privateAuth, publicAuth;
        assert(!LSBNativeMetalControlRead(privateReader, &privateAuth) && privateAuth.authorized && !privateAuth.publicAccess);
        assert(!LSBNativeMetalControlRead(publicReader, &publicAuth) && publicAuth.authorized && publicAuth.publicAccess);
        LSBJob *firstClient = startGraphicsOwner(launchd, "/System/Library/CoreServices/iconservicesagent", 921);
        LSBJob *secondClient = startGraphicsOwner(launchd, "/Applications/Calculator.app/Calculator", 922);
        assert(!publicHelper.nativeMetalAuthorizedIdentifier);
        recordChildExit(firstClient.identifier, 0);
        assert(findJob(publicHelper.identifier) && findJob(secondClient.identifier) && !iosurface.closes);
        LSBNativeMetalAuthorization unchanged;
        assert(!LSBNativeMetalControlRead(publicReader, &unchanged) && !memcmp(&unchanged, &publicAuth, sizeof(unchanged)));
        // Public graphics access must not bind the client to its old native process during exec replacement.
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        request->kind = LSB_REQUEST_EXEC;
        request->audit = secondClient.audit;
        handleMessage(request);
        LSBJob *replacement = startingJob;
        assert(replacement && replacement.identifier == secondClient.replacementIdentifier && !replyFor(object));
        reportedPID = getpid() + 800;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 923, 0));
        handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 923, replacement.identifier));
        assert(replacement.phase == JobRunning && !findJob(secondClient.identifier) && !replyFor(object)->error);
        assert(!LSBNativeMetalControlRead(publicReader, &unchanged) && !memcmp(&unchanged, &publicAuth, sizeof(unchanged)));
        if (helperFails) {
            recordChildExit(publicHelper.identifier, SIGABRT);
            assert(!findJob(publicHelper.identifier) && launchd.servicesError == EPIPE && !launchd.servicesPending);
            assert(findJob(privateHelper.identifier) && findJob(backboard.identifier) && findJob(replacement.identifier));
            assert(!LSBNativeMetalControlRead(privateReader, &unchanged) && !memcmp(&unchanged, &privateAuth, sizeof(unchanged)));
            assert(!iosurface.closes);
        }
        finishJob(launchd, ECANCELED);
        assert(!findJob(publicHelper.identifier) && !findJob(privateHelper.identifier) && iosurface.closes == 1);
        assert(!LSBNativeMetalControlRead(publicReader, &publicAuth) && !publicAuth.authorized && !publicAuth.publicAccess);
        assert(!hasRight(publicPort, MACH_PORT_TYPE_SEND) && !hasRight(privatePort, MACH_PORT_TYPE_SEND));
        assert(!mach_port_mod_refs(mach_task_self(), privatePort, MACH_PORT_RIGHT_RECEIVE, -1));
        LSBNativeMetalControlUnmap(privateReader);
        LSBNativeMetalControlUnmap(publicReader);
    }
}

static void testSecondMetalHelperFailureIsIndependent(void) {
    for (unsigned failure = 0; failure < 2; ++failure) {
        LSBJob *launchd;
        const LSBNativeMetalControl *firstReader, *secondReader;
        LSBJob *first = queueNativeMetalSessionWithSpringBoard(&launchd, &firstReader, YES), *second = jobs[1];
        assert(!LSBNativeMetalControlImport(second.request->fds[second.request->plan.fdCount - 1], &secondReader));
        mach_port_t endpoint = newServiceEndpoint();
        readyMetalHelper(first, endpoint, 900);
        assert(startingJob == second && launchd.phase == JobQueued && submittedLaunches == 2);
        reportedPID = getpid() + 302;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 901, 0));
        LSBReceivedMessage *forged = messageFor(LSB_REQUEST_READY, second.pid, second.pidVersion, first.identifier);
        handleMessage(forged);
        assert(replies[replyCount - 1].error == ESRCH && second.phase == JobDispatched && launchd.phase == JobQueued);
        forged = messageFor(LSB_REQUEST_READY, second.pid, second.pidVersion, second.identifier);
        forged->audit.val[1] ^= 1;
        handleMessage(forged);
        assert(replies[replyCount - 1].error == EACCES && second.phase == JobDispatched);
        LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, second.pid, second.pidVersion, second.identifier);
        ready->error = failure ? ENOEXEC : 0; // Missing endpoint is a distinct READY protocol failure.
        handleMessage(ready);
        assert(!findJob(second.identifier) && findJob(first.identifier) && first.phase == JobRunning);
        assert(launchd.springBoardMetalError == (failure ? ENOEXEC : EPROTO) && !launchd.servicesError);
        assert(startingJob == findJob(launchd.publicMetalServiceIdentifier));
        makeSessionLaunchdRunning(launchd);
        LSBJob *backboard = startGraphicsOwner(launchd, "/usr/libexec/backboardd", 902);
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        setRequestProgram(request, "/System/Library/CoreServices/SpringBoard.app/SpringBoard");
        enqueueSpawn(request, launchd);
        assert(replyFor(object)->error == launchd.springBoardMetalError && !startingJob);
        LSBNativeMetalAuthorization authorization;
        assert(!LSBNativeMetalControlRead(firstReader, &authorization) && authorization.authorized);
        audit_token_t expected = backboard.audit;
        assert(!memcmp(&authorization.audit, &expected, sizeof(expected)));
        assert(!LSBNativeMetalControlRead(secondReader, &authorization) && !authorization.authorized);
        finishJob(launchd, ECANCELED);
        assert(!hasRight(endpoint, MACH_PORT_TYPE_SEND));
        assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
        LSBNativeMetalControlUnmap(firstReader);
        LSBNativeMetalControlUnmap(secondReader);
    }
}

static void testSeparateMetalOwners(void) {
    for (unsigned failure = 0; failure < 5; ++failure) {
        LSBJob *launchd;
        const LSBNativeMetalControl *firstReader, *secondReader;
        LSBJob *first = queueNativeMetalSessionWithSpringBoard(&launchd, &firstReader, YES), *second = jobs[1];
        assert(!LSBNativeMetalControlImport(second.request->fds[second.request->plan.fdCount - 1], &secondReader));
        mach_port_t firstPort = newServiceEndpoint(), secondPort = newServiceEndpoint();
        readyMetalHelper(first, firstPort, 900);
        assert(startingJob == second && launchd.phase == JobQueued);
        readyMetalHelper(second, secondPort, 903);
        assert(startingJob == findJob(launchd.publicMetalServiceIdentifier) && firstPort != secondPort && submittedLaunches == 3);
        makeSessionLaunchdRunning(launchd);
        RecordedIOSurfaceServer *iosurface = [RecordedIOSurfaceServer new];
        launchd.iosurface = iosurface;
        LSBJob *backboard = startGraphicsOwner(launchd, "/usr/libexec/backboardd", 904);
        LSBJob *springBoard = startGraphicsOwner(launchd, "/System/Library/CoreServices/SpringBoard.app/SpringBoard", 905);
        LSBNativeMetalAuthorization firstAuth, secondAuth;
        assert(!LSBNativeMetalControlRead(firstReader, &firstAuth) && firstAuth.authorized);
        assert(!LSBNativeMetalControlRead(secondReader, &secondAuth) && secondAuth.authorized);
        audit_token_t firstAudit = backboard.audit, secondAudit = springBoard.audit;
        assert(!memcmp(&firstAuth.audit, &firstAudit, sizeof(firstAudit)) && !memcmp(&secondAuth.audit, &secondAudit, sizeof(secondAudit)));
        assert(first.nativeMetalAuthorizedIdentifier == backboard.identifier && second.nativeMetalAuthorizedIdentifier == springBoard.identifier);
        int fd;
        unsigned object;
        LSBReceivedMessage *exec = spawnRequest(&fd, &object);
        exec->kind = LSB_REQUEST_EXEC;
        enqueueExec(exec, springBoard);
        assert(replyFor(object)->error == ENOTSUP && !springBoard.replacementIdentifier);
        if (failure == 4) finishJob(launchd, ECANCELED);
        else if (failure == 3) recordChildExit(backboard.identifier, 0);
        else if (failure == 2) extensionCallback(second.identifier, 0, ECANCELED, NULL);
        else if (failure == 1) recordChildExit(second.identifier, SIGABRT);
        else recordChildExit(springBoard.identifier, 0);
        if (failure < 3) {
            assert(!findJob(second.identifier) && !findJob(springBoard.identifier) && findJob(first.identifier) && findJob(backboard.identifier));
            assert(!launchd.servicesError && launchd.springBoardMetalError && !launchd.springBoardMetalPending);
            LSBNativeMetalAuthorization unchanged;
            assert(!LSBNativeMetalControlRead(firstReader, &unchanged) && !memcmp(&unchanged, &firstAuth, sizeof(unchanged)));
        } else if (failure == 3) {
            assert(!findJob(first.identifier) && !findJob(backboard.identifier) && findJob(second.identifier) && findJob(springBoard.identifier));
            LSBNativeMetalAuthorization unchanged;
            assert(!LSBNativeMetalControlRead(secondReader, &unchanged) && !memcmp(&unchanged, &secondAuth, sizeof(unchanged)));
        } else assert(!findJob(first.identifier) && !findJob(second.identifier));
        finishJob(launchd, ECANCELED);
        assert(!LSBNativeMetalControlRead(firstReader, &firstAuth) && !firstAuth.authorized);
        assert(!LSBNativeMetalControlRead(secondReader, &secondAuth) && !secondAuth.authorized);
        assert(!hasRight(firstPort, MACH_PORT_TYPE_SEND) && !hasRight(secondPort, MACH_PORT_TYPE_SEND));
        assert(!mach_port_mod_refs(mach_task_self(), firstPort, MACH_PORT_RIGHT_RECEIVE, -1));
        assert(!mach_port_mod_refs(mach_task_self(), secondPort, MACH_PORT_RIGHT_RECEIVE, -1));
        LSBNativeMetalControlUnmap(firstReader);
        LSBNativeMetalControlUnmap(secondReader);
    }
}

static void testSpringBoardRegistrationIsolation(void) {
    for (unsigned failure = 0; failure < 3; ++failure) {
        LSBJob *launchd;
        const LSBNativeMetalControl *reader;
        LSBJob *first = queueNativeMetalSessionWithSpringBoard(&launchd, &reader, YES), *second = jobs[1];
        mach_port_t firstPort = newServiceEndpoint(), secondPort = newServiceEndpoint(), service = newServiceEndpoint();
        readyMetalHelper(first, firstPort, 900);
        readyMetalHelper(second, secondPort, 903);
        makeSessionLaunchdRunning(launchd);
        launchd.indigo = [RecordedIndigoServer new];
        launchd.framebuffer = [RecordedFramebufferServer new];
        launchd.iosurface = [RecordedIOSurfaceServer new];
        launchd.servicesPending = YES;
        if (failure == 1) endpointErrors[5] = EACCES;
        dispatch_semaphore_t entered = dispatch_semaphore_create(0), release = dispatch_semaphore_create(0);
        endpointHook = ^(unsigned index) {
            if (index != 5) return;
            dispatch_semaphore_signal(entered);
            assert(!dispatch_semaphore_wait(release, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
        };
        registerDisplayServices(launchd, service);
        assert(!dispatch_semaphore_wait(entered, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
        runMainQueueUntil(^BOOL { return !launchd.servicesPending; });
        assert(!launchd.servicesError && launchd.springBoardMetalPending && endpointCalls == 6);
        assert(!strcmp(registeredEndpoints[3], "com.apple.metal.simulator") && registeredPorts[3] == findJob(launchd.publicMetalServiceIdentifier).nativeServicePort);
        assert(!strcmp(registeredEndpoints[4], "com.apple.metal.simulator.backboardd") && registeredPorts[4] == firstPort);
        assert(!strcmp(registeredEndpoints[5], "com.apple.metal.simulator.SpringBoard") && registeredPorts[5] == secondPort);
        audit_token_t expected = launchd.audit;
        assert(!memcmp(&registeredAudits[5], &expected, sizeof(expected)));
        assert(registeredTimeouts[5] <= registeredTimeouts[4]);
        // The blocked sixth RPC cannot hold back the existing owner, but must keep SpringBoard from receiving its plan.
        LSBJob *backboard = startGraphicsOwner(launchd, "/usr/libexec/backboardd", 906);
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        setRequestProgram(request, "/System/Library/CoreServices/SpringBoard.app/SpringBoard");
        enqueueSpawn(request, launchd);
        LSBJob *springBoard = jobs.lastObject;
        assert(springBoard.phase == JobQueued && !startingJob && !replyFor(object));
        if (failure == 2) {
            recordChildExit(second.identifier, SIGABRT);
            assert(launchd.springBoardMetalError == EPIPE && !launchd.servicesError && !findJob(springBoard.identifier));
            assert(hasRight(secondPort, MACH_PORT_TYPE_SEND)); // The in-flight worker owns a separate endpoint reference.
            launchd.springBoardMetalPending = YES; // Observe the already-issued worker's late completion below.
        }
        dispatch_semaphore_signal(release);
        runMainQueueUntil(^BOOL { return !launchd.springBoardMetalPending; });
        assert(!launchd.servicesError && findJob(first.identifier) && findJob(backboard.identifier));
        if (!failure) assert(!launchd.springBoardMetalError && startingJob == springBoard && springBoard.phase == JobStarting);
        else {
            int expectedError = failure == 1 ? EACCES : EPIPE;
            assert(launchd.springBoardMetalError == expectedError && replyFor(object)->error == expectedError);
            assert(!findJob(springBoard.identifier) && !startingJob);
        }
        if (failure == 2) assert(!hasRight(secondPort, MACH_PORT_TYPE_SEND));
        finishJob(launchd, ECANCELED);
        assert(!hasRight(firstPort, MACH_PORT_TYPE_SEND) && !hasRight(secondPort, MACH_PORT_TYPE_SEND));
        assert(!mach_port_mod_refs(mach_task_self(), firstPort, MACH_PORT_RIGHT_RECEIVE, -1));
        assert(!mach_port_mod_refs(mach_task_self(), secondPort, MACH_PORT_RIGHT_RECEIVE, -1));
        assert(!mach_port_deallocate(mach_task_self(), service));
        assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
        LSBNativeMetalControlUnmap(reader);
    }
}

static void testSpringBoardMetalRestart(void) {
    for (unsigned failure = 0; failure < 7; ++failure) {
        LSBJob *launchd;
        const LSBNativeMetalControl *backboardReader, *retiredReader, *replacementReader;
        LSBJob *backboardHelper = queueNativeMetalSessionWithSpringBoard(&launchd, &backboardReader, YES), *retiredHelper = jobs[1];
        assert(!LSBNativeMetalControlImport(retiredHelper.request->fds[retiredHelper.request->plan.fdCount - 1], &retiredReader));
        mach_port_t backboardPort = newServiceEndpoint(), retiredPort = newServiceEndpoint(), service = newServiceEndpoint();
        readyMetalHelper(backboardHelper, backboardPort, 900);
        readyMetalHelper(retiredHelper, retiredPort, 903);
        makeSessionLaunchdRunning(launchd);
        launchd.launchdHandshake = calloc(1, sizeof(*launchd.launchdHandshake));
        launchd.launchdHandshake->serviceSend = service;
        launchd.launchdHandshake->completed = true;
        RecordedIOSurfaceServer *iosurface = [RecordedIOSurfaceServer new];
        RecordedFramebufferServer *framebuffer = [RecordedFramebufferServer new];
        launchd.iosurface = iosurface;
        launchd.framebuffer = framebuffer;
        LSBJob *backboard = startGraphicsOwner(launchd, "/usr/libexec/backboardd", 904);
        LSBJob *springBoard = startGraphicsOwner(launchd, "/System/Library/CoreServices/SpringBoard.app/SpringBoard", 905);
        LSBJob *publicHelper = findJob(launchd.publicMetalServiceIdentifier);
        LSBNativeMetalAuthorization backboardBefore, publicBefore, observed;
        assert(!LSBNativeMetalControlRead(backboardReader, &backboardBefore));
        assert(!LSBNativeMetalControlRead(publicHelper.nativeMetalControl, &publicBefore));
        if (failure == 1) recordChildExit(retiredHelper.identifier, SIGABRT);
        else recordChildExit(springBoard.identifier, SIGTRAP);
        assert(!findJob(retiredHelper.identifier) && !findJob(springBoard.identifier));
        assert(launchd.springBoardMetalRestartAllowed && launchd.springBoardMetalError && !launchd.springBoardMetalPending);
        assert(!LSBNativeMetalControlRead(retiredReader, &observed) && !observed.authorized);
        assert(!hasRight(retiredPort, MACH_PORT_TYPE_SEND));
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        setRequestProgram(request, "/System/Library/CoreServices/SpringBoard.app/SpringBoard");
        enqueueSpawn(request, launchd);
        LSBJob *replacementHelper = findJob(launchd.springBoardMetalServiceIdentifier), *replacement = nil;
        for (LSBJob *job in jobs) if (job.request == request) replacement = job;
        assert(replacement && replacement.phase == JobQueued && startingJob == replacementHelper && !replyFor(object));
        assert(replacementHelper.identifier != retiredHelper.identifier && replacementHelper.nativeMetalControl != retiredHelper.nativeMetalControl);
        assert(!strcmp(replacementHelper.request->plan.strings + replacementHelper.request->plan.argv[0], "Metal.SpringBoard"));
        assert(!launchd.springBoardMetalRestartAllowed && launchd.springBoardMetalPending && !launchd.springBoardMetalError);
        assert(!LSBNativeMetalControlImport(replacementHelper.request->fds[replacementHelper.request->plan.fdCount - 1], &replacementReader));
        assert(!LSBNativeMetalControlRead(replacementReader, &observed) && !observed.authorized && !observed.publicAccess);
        extensionCallback(retiredHelper.identifier, 0, ECANCELED, NULL); // Late completion from the retired extension cannot poison this generation.
        assert(!launchd.springBoardMetalError && startingJob == replacementHelper);
        if (failure == 1) unregisterError = ESRCH; // Native dead-name cleanup may already have removed the old endpoint.
        if (failure == 5) unregisterError = EACCES;
        if (failure == 6) unregisterError = ETIMEDOUT;
        mach_port_t replacementPort = MACH_PORT_NULL;
        if (failure == 4) {
            reportedPID = getpid() + 500;
            handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 906, 0));
            LSBReceivedMessage *ready = messageFor(LSB_REQUEST_READY, replacementHelper.pid, replacementHelper.pidVersion, replacementHelper.identifier);
            ready->error = ENOEXEC;
            handleMessage(ready);
            assert(launchd.springBoardMetalError == ENOEXEC && !unregisterCalls && !endpointCalls && !startingJob && replyFor(object)->error == ENOEXEC);
        } else if (failure >= 5) {
            replacementPort = newServiceEndpoint();
            readyMetalHelper(replacementHelper, replacementPort, 906);
            runMainQueueUntil(^BOOL { return !launchd.springBoardMetalPending; });
            assert(unregisterCalls == 1 && !endpointCalls && !startingJob && !findJob(replacement.identifier));
            assert(replyFor(object)->error == unregisterError && launchd.springBoardMetalError == unregisterError);
            assert(!launchd.springBoardMetalRestartAllowed); // Denied or unknown removal never retries, nor sends registration.
        } else {
            dispatch_semaphore_t entered = dispatch_semaphore_create(0), release = dispatch_semaphore_create(0);
            endpointHook = ^(unsigned index) {
                assert(index == 0);
                dispatch_semaphore_signal(entered);
                assert(!dispatch_semaphore_wait(release, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
            };
            if (failure == 2) endpointErrors[0] = EACCES;
            replacementPort = newServiceEndpoint();
            readyMetalHelper(replacementHelper, replacementPort, 906);
            assert(!dispatch_semaphore_wait(entered, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
            assert(endpointCalls == 1 && !strcmp(registeredEndpoints[0], "com.apple.metal.simulator.SpringBoard"));
            assert(registeredPorts[0] == replacementPort && registeredTimeouts[0] <= 30000);
            audit_token_t expected = launchd.audit;
            assert(!memcmp(&registeredAudits[0], &expected, sizeof(expected)));
            assert(unregisterCalls == 1 && !strcmp(unregisteredEndpoint, registeredEndpoints[0]));
            assert(!memcmp(&unregisterAudit, &expected, sizeof(expected)) && registeredTimeouts[0] <= unregisterTimeout);
            assert(replacement.phase == JobQueued && !startingJob && !replyFor(object));
            if (failure == 3) {
                recordChildExit(replacementHelper.identifier, SIGABRT);
                assert(launchd.springBoardMetalError == EPIPE && !findJob(replacement.identifier));
                launchd.springBoardMetalPending = YES; // Observe the already-issued registration's late completion.
            }
            dispatch_semaphore_signal(release);
            runMainQueueUntil(^BOOL { return !launchd.springBoardMetalPending; });
            if (failure < 2) {
                assert(!launchd.springBoardMetalError && startingJob == replacement && !replyFor(object));
                reportedPID = getpid();
                handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 907, 0));
                handleMessage(messageFor(LSB_REQUEST_READY, replacement.pid, replacement.pidVersion, replacement.identifier));
                assert(replacement.phase == JobRunning && !replyFor(object)->error);
                assert(replacementHelper.nativeMetalAuthorizedIdentifier == replacement.identifier);
                assert(!LSBNativeMetalControlRead(replacementReader, &observed) && observed.authorized && !observed.publicAccess);
                expected = replacement.audit;
                assert(!memcmp(&observed.audit, &expected, sizeof(expected)));
            } else {
                int expectedError = failure == 2 ? EACCES : EPIPE;
                assert(!findJob(replacement.identifier) && !startingJob && replyFor(object)->error == expectedError);
                assert(!launchd.springBoardMetalRestartAllowed);
            }
        }
        {
            assert(findJob(backboardHelper.identifier) && findJob(backboard.identifier) && findJob(publicHelper.identifier));
            assert(!launchd.servicesPending && !launchd.servicesError && !iosurface.closes && !framebuffer.closes);
            assert(!LSBNativeMetalControlRead(backboardReader, &observed) && !memcmp(&observed, &backboardBefore, sizeof(observed)));
            assert(!LSBNativeMetalControlRead(publicHelper.nativeMetalControl, &observed) && !memcmp(&observed, &publicBefore, sizeof(observed)));
        }
        finishJob(launchd, ECANCELED);
        assert(!findJob(backboardHelper.identifier) && !findJob(publicHelper.identifier) && !findJob(replacementHelper.identifier));
        assert(!LSBNativeMetalControlRead(replacementReader, &observed) && !observed.authorized);
        assert(!hasRight(backboardPort, MACH_PORT_TYPE_SEND) && !hasRight(retiredPort, MACH_PORT_TYPE_SEND));
        assert(!mach_port_mod_refs(mach_task_self(), backboardPort, MACH_PORT_RIGHT_RECEIVE, -1));
        assert(!mach_port_mod_refs(mach_task_self(), retiredPort, MACH_PORT_RIGHT_RECEIVE, -1));
        if (replacementPort) {
            assert(!hasRight(replacementPort, MACH_PORT_TYPE_SEND));
            assert(!mach_port_mod_refs(mach_task_self(), replacementPort, MACH_PORT_RIGHT_RECEIVE, -1));
        }
        LSBLaunchdHandshakeDispose(launchd.launchdHandshake);
        assert(!hasRight(service, MACH_PORT_TYPE_SEND));
        assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
        LSBNativeMetalControlUnmap(backboardReader);
        LSBNativeMetalControlUnmap(retiredReader);
        LSBNativeMetalControlUnmap(replacementReader);
    }
}

static NSData *entitlementImage(NSData *der, BOOL hasSection) {
    struct mach_header_64 header = {.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = MH_EXECUTE, .ncmds = 1,
        .sizeofcmds = sizeof(struct segment_command_64) + sizeof(struct section_64)};
    uint32_t offset = sizeof(header) + header.sizeofcmds;
    struct segment_command_64 segment = {.cmd = LC_SEGMENT_64, .cmdsize = header.sizeofcmds, .segname = "__TEXT",
        .vmaddr = 0x100000000, .vmsize = offset + der.length, .filesize = offset + der.length, .nsects = 1};
    struct section_64 section = {.segname = "__TEXT", .addr = segment.vmaddr + offset, .size = der.length, .offset = offset};
    strcpy(section.sectname, hasSection ? "__ents_der" : "__other");
    NSMutableData *image = [NSMutableData dataWithBytes:&header length:sizeof(header)];
    [image appendBytes:&segment length:sizeof(segment)];
    [image appendBytes:&section length:sizeof(section)];
    [image appendData:der];
    return image;
}

static NSData *entitlementBlob(NSData *der) {
    uint32_t size = (uint32_t)der.length + 8;
    unsigned char header[] = {0xfa, 0xde, 0x71, 0x72, (unsigned char)(size >> 24), (unsigned char)(size >> 16),
                              (unsigned char)(size >> 8), (unsigned char)size};
    NSMutableData *blob = [NSMutableData dataWithBytes:header length:sizeof(header)];
    [blob appendData:der];
    return blob;
}

static RecordedReply *requestEntitlementsFormat(audit_token_t caller, audit_token_t target, unsigned operation) {
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_COPY_ENTITLEMENTS, caller.val[5], caller.val[7], 0);
    request->audit = caller;
    request->targetAudit = target;
    request->entitlementOperation = operation;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    entitlementsReply = nil;
    handleMessage(request);
    RecordedReply *reply = replyFor(object);
    assert(reply && !reply->requestID && !reply->pid && !reply->deliveredPlan);
    if (reply->error) assert(!entitlementsReply);
    return reply;
}

static RecordedReply *requestEntitlements(audit_token_t caller, audit_token_t target) {
    return requestEntitlementsFormat(caller, target, 16);
}

static NSDictionary *decodedXMLSnapshot(NSData *snapshot) {
    assert(snapshot.length > 8);
    const unsigned char *header = snapshot.bytes;
    uint32_t size = (uint32_t)header[4] << 24 | (uint32_t)header[5] << 16 | (uint32_t)header[6] << 8 | header[7];
    assert(header[0] == 0xfa && header[1] == 0xde && header[2] == 0x71 && header[3] == 0x71 && size == snapshot.length);
    NSData *xml = [snapshot subdataWithRange:NSMakeRange(8, snapshot.length - 8)];
    id result = [NSPropertyListSerialization propertyListWithData:xml options:NSPropertyListImmutable format:NULL error:NULL];
    assert([result isKindOfClass:NSDictionary.class]);
    return result;
}

static void testGuestEntitlementsSnapshotsAndAuthentication(void) {
    resetBroker();
    char temporary[] = "/tmp/LSBBrokerEntitlements.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    NSString *bin = [runtimeRoot stringByAppendingPathComponent:@"bin"], *path = [bin stringByAppendingPathComponent:@"probe"];
    NSFileManager *manager = NSFileManager.defaultManager;
    assert([manager createDirectoryAtPath:bin withIntermediateDirectories:YES attributes:nil error:NULL]);
    const unsigned char empty[] = {0x70, 5, 2, 1, 1, 0xb0, 0};
    const unsigned char keyed[] = {0x70, 13, 2, 1, 1, 0xb0, 8, 0x30, 6, 0x0c, 1, 'a', 1, 1, 0xff};
    NSData *firstDER = [NSData dataWithBytes:empty length:sizeof(empty)], *secondDER = [NSData dataWithBytes:keyed length:sizeof(keyed)];
    NSData *firstBlob = entitlementBlob(firstDER), *secondBlob = entitlementBlob(secondDER);
    const unsigned char hashBytes[LSB_GUEST_CDHASH_BYTES] = {1, 2, 3, 4}, secondHashBytes[LSB_GUEST_CDHASH_BYTES] = {5, 6, 7, 8};
    NSData *firstHash = [NSData dataWithBytes:hashBytes length:sizeof(hashBytes)];
    NSData *secondHash = [NSData dataWithBytes:secondHashBytes length:sizeof(secondHashBytes)];
    codeDirectoryHash = firstHash;
    assert([entitlementImage(firstDER, YES) writeToFile:path atomically:YES]);
    int fd;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    setRequestProgram(request, "/bin/probe");
    enqueueSpawn(request, nil);
    LSBJob *first = startingJob;
    checkInReplyHook = ^{
        assert(first.phase == JobDispatched && !first.guestEntitlementsError && [first.guestEntitlements isEqualToData:firstBlob]);
        assert(!first.guestXMLEntitlementsError && [decodedXMLSnapshot(first.guestXMLEntitlements) isEqual:@{}]);
        assert(!first.guestCodeDirectoryHashError && [first.guestCodeDirectoryHash isEqualToData:firstHash]);
        // Replace the file before the plan reply returns. The job's already-captured image metadata must remain immutable.
        assert([entitlementImage(secondDER, YES) writeToFile:path atomically:YES]);
        codeDirectoryHash = secondHash;
    };
    LSBReceivedMessage *checkin = messageFor(LSB_REQUEST_CHECKIN, getpid(), 1400, 0);
    checkin->audit.val[1] = 501;
    handleMessage(checkin);
    checkInReplyHook = nil;
    assert(requestEntitlements(first.audit, first.audit)->error == EACCES); // Not yet committed by READY.
    handleMessage(messageFor(LSB_REQUEST_READY, first.pid, first.pidVersion, first.identifier));
    assert(first.phase == JobRunning && !first.request);
    assert(!requestEntitlements(first.audit, first.audit)->error && [entitlementsReply isEqualToData:firstBlob]);
    assert(!requestEntitlementsFormat(first.audit, first.audit, 7)->error && [decodedXMLSnapshot(entitlementsReply) isEqual:@{}]);
    assert(!requestEntitlementsFormat(first.audit, first.audit, 5)->error && [entitlementsReply isEqualToData:firstHash]);

    request = spawnRequest(&fd, &object);
    setRequestProgram(request, path.fileSystemRepresentation); // The loader also accepts this exact physical RuntimeRoot path.
    enqueueSpawn(request, nil);
    LSBJob *second = startingJob;
    checkin = messageFor(LSB_REQUEST_CHECKIN, getpid(), 1401, 0);
    checkin->audit.val[1] = 502;
    handleMessage(checkin);
    assert(second.phase == JobDispatched && [second.guestEntitlements isEqualToData:secondBlob]);
    assert(!second.guestCodeDirectoryHashError && [second.guestCodeDirectoryHash isEqualToData:secondHash]);
    assert(requestEntitlements(first.audit, second.audit)->error == ESRCH);
    handleMessage(messageFor(LSB_REQUEST_READY, second.pid, second.pidVersion, second.identifier));
    assert(second.phase == JobRunning && !second.request && !unlink(path.fileSystemRepresentation));
    assert(!requestEntitlements(first.audit, second.audit)->error && [entitlementsReply isEqualToData:secondBlob]);
    assert(!requestEntitlementsFormat(first.audit, second.audit, 7)->error && [decodedXMLSnapshot(entitlementsReply) isEqual:@{@"a": @YES}]);
    assert(!requestEntitlements(second.audit, first.audit)->error && [entitlementsReply isEqualToData:firstBlob]);
    assert(!requestEntitlementsFormat(first.audit, second.audit, 5)->error && [entitlementsReply isEqualToData:secondHash]);
    assert(!requestEntitlementsFormat(second.audit, first.audit, 5)->error && [entitlementsReply isEqualToData:firstHash]);

    audit_token_t caller = first.audit, target = second.audit;
    assert(requestEntitlementsFormat(caller, target, 0)->error == EINVAL);
    assert(requestEntitlementsFormat(caller, target, 17)->error == EINVAL);
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t changed = caller;
        changed.val[word] ^= 1;
        assert(requestEntitlements(changed, target)->error == EACCES);
        assert(requestEntitlementsFormat(changed, target, 5)->error == EACCES);
        changed = target;
        changed.val[word] ^= 1;
        assert(requestEntitlements(caller, changed)->error == ESRCH);
        assert(requestEntitlementsFormat(caller, changed, 5)->error == ESRCH);
    }
    first.retiredForExec = YES;
    assert(requestEntitlements(caller, target)->error == EACCES);
    first.retiredForExec = NO;
    first.finishing = YES;
    assert(requestEntitlements(caller, target)->error == EACCES);
    first.finishing = NO;
    second.retiredForExec = YES;
    assert(requestEntitlements(caller, target)->error == ESRCH);
    second.retiredForExec = NO;
    second.finishing = YES;
    assert(requestEntitlements(caller, target)->error == ESRCH);
    second.finishing = NO;
    for (unsigned role = LSB_EXECUTION_NATIVE_METAL_PROBE; role <= LSB_EXECUTION_NATIVE_METAL_SERVICE; ++role) {
        first.executionRole = role;
        assert(requestEntitlements(caller, target)->error == EACCES);
        first.executionRole = LSB_EXECUTION_SIMULATOR;
        second.executionRole = role;
        assert(requestEntitlements(caller, target)->error == ESRCH);
        second.executionRole = LSB_EXECUTION_SIMULATOR;
    }
    second.guestEntitlementsError = ENOENT;
    assert(requestEntitlements(caller, target)->error == ENOENT);
    assert(!requestEntitlementsFormat(caller, target, 5)->error && [entitlementsReply isEqualToData:secondHash]);
    second.guestEntitlementsError = EINVAL;
    assert(requestEntitlements(caller, target)->error == EINVAL);
    second.guestEntitlementsError = 0;
    second.guestXMLEntitlementsError = EILSEQ;
    assert(requestEntitlementsFormat(caller, target, 7)->error == EILSEQ);
    assert(!requestEntitlements(caller, target)->error && [entitlementsReply isEqualToData:secondBlob]);
    assert(!requestEntitlementsFormat(caller, target, 5)->error && [entitlementsReply isEqualToData:secondHash]);
    second.guestCodeDirectoryHashError = ENOENT;
    assert(requestEntitlementsFormat(caller, target, 5)->error == ENOENT);
    assert(!requestEntitlements(caller, target)->error && [entitlementsReply isEqualToData:secondBlob]);
    second.guestCodeDirectoryHashError = EINVAL;
    assert(requestEntitlementsFormat(caller, target, 5)->error == EINVAL);
    second.guestCodeDirectoryHashError = 0;
    recordChildExit(second.identifier, 0);
    LSBJob *reused = auditJob((pid_t)target.val[5], 9999, target.val[7] + 1, NO);
    reused.guestEntitlements = secondBlob;
    reused.guestEntitlementsError = 0;
    reused.guestCodeDirectoryHash = firstHash;
    reused.guestCodeDirectoryHashError = 0;
    assert(requestEntitlements(caller, target)->error == ESRCH); // A reused native PID cannot recover retired metadata.
    assert(requestEntitlementsFormat(caller, target, 5)->error == ESRCH);
    assert(!requestEntitlementsFormat(caller, reused.audit, 5)->error && [entitlementsReply isEqualToData:firstHash]);
    assert(!requestEntitlements(caller, reused.audit)->error && [entitlementsReply isEqualToData:secondBlob]);
    audit_token_t unmanaged = {{0}};
    unmanaged.val[5] = getpid() + 400;
    unmanaged.val[7] = 1;
    assert(requestEntitlements(caller, unmanaged)->error == ESRCH && requestEntitlements(unmanaged, caller)->error == EACCES);
    resetBroker();
    assert([manager removeItemAtPath:directory error:NULL]);
}

static mach_port_urefs_t sendReferences(mach_port_t port) {
    mach_port_urefs_t references = 0;
    kern_return_t result = mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &references);
    if (result == KERN_INVALID_NAME || result == KERN_INVALID_RIGHT) return 0; // The final owned reference was released.
    assert(result == KERN_SUCCESS);
    return references;
}

static void testPeerIdentityKernelValidation(void) {
    resetBroker();
    useRealTaskInfo = YES;
    LSBPeerProcessIdentity identity = {0};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&identity.audit, &count) == KERN_SUCCESS);
    assert(proc_pidinfo(getpid(), LSB_PEER_SHORT_INFO_FLAVOR, 0, identity.shortInfo, sizeof(identity.shortInfo)) == sizeof(identity.shortInfo));
    assert(task_name_for_pid(mach_task_self(), getpid(), &identity.taskName) == KERN_SUCCESS);
    audit_token_t expected = identity.audit;
    assert(!validatePeerTaskIdentity(&identity, &expected));
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t altered = expected;
        altered.val[word] ^= 1;
        assert(validatePeerTaskIdentity(&identity, &altered) == EACCES);
    }
    identity.shortInfo[0] ^= 1;
    assert(validatePeerTaskIdentity(&identity, &expected) == EACCES);
    identity.shortInfo[0] ^= 1;
    mach_port_t taskName = identity.taskName, receive, once;
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &receive) == KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(), receive, receive, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    identity.taskName = receive;
    assert(validatePeerTaskIdentity(&identity, &expected) == EACCES); // An ordinary receive/send port is not a task name.
    mach_msg_type_name_t disposition;
    assert(mach_port_extract_right(mach_task_self(), receive, MACH_MSG_TYPE_MAKE_SEND_ONCE, &once, &disposition) == KERN_SUCCESS);
    identity.taskName = once;
    assert(validatePeerTaskIdentity(&identity, &expected) == EACCES);
    assert(mach_port_deallocate(mach_task_self(), once) == KERN_SUCCESS);
    identity.taskName = MACH_PORT_NULL;
    assert(validatePeerTaskIdentity(&identity, &expected) == EACCES);
    identity.taskName = receive;
    assert(mach_port_mod_refs(mach_task_self(), receive, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS);
    assert(validatePeerTaskIdentity(&identity, &expected) == EACCES); // Dead name must not enter the snapshot.
    assert(mach_port_deallocate(mach_task_self(), receive) == KERN_SUCCESS);
    identity.taskName = taskName;
    LSBPeerProcessIdentityDispose(&identity);
    useRealTaskInfo = NO;
}

static LSBJob *runIdentityJob(const char *path, pid_t nativePID, unsigned generation, unsigned char marker) {
    int fd;
    unsigned object;
    reportedPID = nativePID;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    setRequestProgram(request, path);
    enqueueSpawn(request, nil);
    LSBJob *job = startingJob;
    assert(job);
    LSBReceivedMessage *checkin = messageFor(LSB_REQUEST_CHECKIN, nativePID, generation, 0);
    memset(checkin->peerIdentity.shortInfo + sizeof(uint32_t), marker, LSB_PEER_SHORT_INFO_BYTES - sizeof(uint32_t));
    checkInReplyHook = ^{
        assert(!job.peerIdentityError && job.peerIdentity.shortInfo[LSB_PEER_SHORT_INFO_BYTES - 1] == marker);
        assert(MACH_PORT_VALID(job.peerIdentity.taskName)); // Captured before the guest can make its first request.
    };
    handleMessage(checkin);
    checkInReplyHook = nil;
    assert(job.phase == JobDispatched && !job.peerIdentityError);
    setRequestProgram(job.request, "/bin/replaced-after-snapshot");
    handleMessage(messageFor(LSB_REQUEST_READY, nativePID, generation, job.identifier));
    assert(job.phase == JobRunning && !job.request && !replyFor(object)->error);
    return job;
}

static RecordedReply *requestProcessList(audit_token_t caller) {
    processListReply = nil;
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_LIST_PROCESSES, caller.val[5], caller.val[7], 0);
    request->audit = caller;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    RecordedReply *reply = replyFor(object);
    assert(reply && (reply->error ? !processListReply : processListReply != nil));
    return reply;
}

static void testProcessListEntitlementsAndAuthentication(void) {
    resetBroker();
    for (id value in @[@YES, @NO, @1, @0, @"true", @[], @{}]) {
        NSDictionary *entitlements = @{@"proc_info-allow": value, @"com.apple.druid": value, @"task_for_pid-allow": value};
        BOOL expected = CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && [value boolValue];
        assert(booleanGuestEntitlement(entitlements, @"proc_info-allow") == expected);
        assert(booleanGuestEntitlement(entitlements, @"com.apple.druid") == expected);
        assert(booleanGuestEntitlement(entitlements, @"task_for_pid-allow") == expected);
        assert(!booleanGuestEntitlement(entitlements, @"missing"));
    }
    assert(!booleanGuestEntitlement(nil, @"proc_info-allow"));
    char temporary[] = "/tmp/LSBProcessList.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    NSString *bin = [runtimeRoot stringByAppendingPathComponent:@"bin"];
    NSFileManager *manager = NSFileManager.defaultManager;
    assert([manager createDirectoryAtPath:bin withIntermediateDirectories:YES attributes:nil error:NULL]);
    const unsigned char der[] = {0x70, 27, 2, 1, 1, 0xb0, 22, 0x30, 20, 0x0c, 15,
        'p', 'r', 'o', 'c', '_', 'i', 'n', 'f', 'o', '-', 'a', 'l', 'l', 'o', 'w', 1, 1, 0xff};
    NSData *grantDER = [NSData dataWithBytes:der length:sizeof(der)];
    NSString *path = [bin stringByAppendingPathComponent:@"inspect"];
    assert([entitlementImage(grantDER, YES) writeToFile:path atomically:YES]);
    LSBJob *caller = runIdentityJob("/bin/inspect", getpid() + 500, 1800, 1);
    assert(caller.guestProcessInfoAllowed && !caller.guestEntitlementsError && !caller.guestXMLEntitlementsError);
    assert(!unlink(path.fileSystemRepresentation)); // Permission and path remain the immutable launch snapshot.
    assert([[@"plain image" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:path atomically:YES]);
    LSBJob *target = runIdentityJob("/bin/inspect", getpid() + 501, 1801, 2);
    assert(!target.guestProcessInfoAllowed);
    target.guestPID = 42;
    target.parentIdentifier = caller.identifier;
    LSBChildStatusSetParentPID(target.childStatus, caller.guestPID);
    assert(!requestProcessList(caller.audit)->error);
    const LSBProcessList *list = processListReply.bytes;
    assert(list->count == 2 && list->records[1].guestPID == 42 && list->records[1].parentPID == caller.guestPID);
    LSBPeerProcessIdentity identity = target.peerIdentity;
    assert(!memcmp(&list->records[1].audit, &identity.audit, sizeof(audit_token_t)));
    assert(!strcmp(list->records[1].executablePath, "/bin/inspect"));
    assert(requestProcessList(target.audit)->error == EPERM);
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t changed = caller.audit; changed.val[word] ^= 1;
        assert(requestProcessList(changed)->error == EACCES);
    }
    for (unsigned condition = 0; condition < 6; ++condition) {
        target.retiredForExec = condition == 0; target.finishing = condition == 1;
        target.phase = condition == 2 ? JobDispatched : JobRunning;
        target.executionRole = condition == 3 ? LSB_EXECUTION_NATIVE_METAL_SERVICE : LSB_EXECUTION_SIMULATOR;
        target.peerIdentityError = condition == 4 ? EIO : 0;
        if (condition == 5) ++target.pidVersion;
        assert(!requestProcessList(caller.audit)->error && ((const LSBProcessList *)processListReply.bytes)->count == 1);
        if (condition == 5) --target.pidVersion;
    }
    target.peerIdentityError = 0; target.retiredForExec = target.finishing = NO;
    target.phase = JobRunning; target.executionRole = LSB_EXECUTION_SIMULATOR;
    caller.retiredForExec = YES;
    assert(requestProcessList(caller.audit)->error == EACCES); caller.retiredForExec = NO;
    caller.phase = JobDispatched;
    assert(requestProcessList(caller.audit)->error == EACCES); caller.phase = JobRunning;
    caller.executionRole = LSB_EXECUTION_NATIVE_METAL_SERVICE;
    assert(requestProcessList(caller.audit)->error == EACCES); caller.executionRole = LSB_EXECUTION_SIMULATOR;
    sessionDraining = YES;
    assert(requestProcessList(caller.audit)->error == ECANCELED); sessionDraining = NO;
    resetBroker();
    assert([manager removeItemAtPath:directory error:NULL]);
}

static RecordedReply *requestProcessIdentity(audit_token_t caller, pid_t target) {
    LSBPeerProcessIdentityDispose(&identityReply);
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_COPY_PROCESS_IDENTITY, caller.val[5], caller.val[7], 0);
    request->audit = caller;
    request->targetPID = target;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    RecordedReply *reply = replyFor(object);
    assert(reply && (reply->error ? !MACH_PORT_VALID(identityReply.taskName) : MACH_PORT_VALID(identityReply.taskName)));
    return reply;
}

static LSBJob *debuggerJob(pid_t nativePID, pid_t guestPID, unsigned generation, const char *path) {
    LSBJob *job = auditJob(nativePID, guestPID, generation, NO);
    LSBPeerProcessIdentity identity = {.audit = job.audit};
    memcpy(identity.shortInfo, &nativePID, sizeof(nativePID));
    strlcpy(identity.executablePath, path, sizeof(identity.executablePath));
    assert(!task_name_for_pid(mach_task_self(), getpid(), &identity.taskName));
    job.peerIdentity = identity;
    job.peerIdentityError = 0;
    return job;
}

static void giveTestTaskPort(LSBJob *job) {
    assert(!MACH_PORT_VALID(job.taskPort));
    job.taskPort = mach_task_self();
    assert(!mach_port_mod_refs(mach_task_self(), job.taskPort, MACH_PORT_RIGHT_SEND, 1));
    job.taskCaptureError = 0;
}

static RecordedReply *requestProcessTask(audit_token_t caller, pid_t target) {
    if (MACH_PORT_VALID(processTaskReply)) mach_port_deallocate(mach_task_self(), processTaskReply);
    processTaskReply = MACH_PORT_NULL;
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_COPY_PROCESS_TASK, caller.val[5], caller.val[7], 0);
    request->audit = caller;
    request->targetPID = target;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    RecordedReply *reply = replyFor(object);
    assert(reply && (reply->error || reply->sendError ? !MACH_PORT_VALID(processTaskReply) : MACH_PORT_VALID(processTaskReply)));
    return reply;
}

static void testProcessTaskAuthenticationAndOwnership(void) {
    resetBroker();
    LSBJob *caller = debuggerJob(8500, 8400, 9500, "/Applications/CocoaTop.app/CocoaTop");
    LSBJob *target = debuggerJob(8600, 8401, 9501, "/bin/df");
    giveTestTaskPort(target);
    mach_port_t task = target.taskPort;
    mach_port_urefs_t initial = sendReferences(task);
    assert(requestProcessTask(caller.audit, target.guestPID)->error == EPERM && sendReferences(task) == initial);
    caller.guestProcessInfoAllowed = YES;
    assert(requestProcessTask(caller.audit, target.guestPID)->error == EPERM); // List authorization is not task-control authorization.
    caller.guestTaskForPIDAllowed = YES;
    RecordedReply *reply = requestProcessTask(caller.audit, target.guestPID);
    audit_token_t targetAudit = target.audit;
    assert(!reply->error && reply->pid == target.guestPID && processTaskReply == task && sendReferences(task) == initial + 1);
    assert(!memcmp(&processTaskReplyAudit, &targetAudit, sizeof(targetAudit)));
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t forged = caller.audit; forged.val[word] ^= 1;
        assert(requestProcessTask(forged, target.guestPID)->error == EACCES && sendReferences(task) == initial);
    }
    for (unsigned condition = 0; condition < 5; ++condition) {
        caller.retiredForExec = condition == 0; caller.finishing = condition == 1;
        caller.phase = condition == 2 ? JobDispatched : JobRunning;
        caller.executionRole = condition == 3 ? LSB_EXECUTION_NATIVE_METAL_PROBE :
            condition == 4 ? LSB_EXECUTION_NATIVE_METAL_SERVICE : LSB_EXECUTION_SIMULATOR;
        assert(requestProcessTask(caller.audit, target.guestPID)->error == EACCES);
    }
    caller.retiredForExec = caller.finishing = NO; caller.phase = JobRunning; caller.executionRole = LSB_EXECUTION_SIMULATOR;
    assert(requestProcessTask(caller.audit, 0)->error == EINVAL && requestProcessTask(caller.audit, -1)->error == EINVAL);
    assert(requestProcessTask(caller.audit, target.pid)->error == ESRCH); // Native PID is never an alias for this logical-target API.
    assert(requestProcessTask(caller.audit, 999999)->error == ESRCH);
    for (unsigned condition = 0; condition < 6; ++condition) {
        target.retiredForExec = condition == 0; target.finishing = condition == 1;
        target.phase = condition == 2 ? JobDispatched : JobRunning;
        target.executionRole = condition == 3 ? LSB_EXECUTION_NATIVE_METAL_PROBE :
            condition == 4 ? LSB_EXECUTION_NATIVE_METAL_SERVICE : LSB_EXECUTION_SIMULATOR;
        target.peerIdentityError = condition == 5 ? EIO : 0;
        assert(requestProcessTask(caller.audit, target.guestPID)->error == ESRCH);
    }
    target.retiredForExec = target.finishing = NO; target.phase = JobRunning;
    target.executionRole = LSB_EXECUTION_SIMULATOR; target.peerIdentityError = 0;
    ++target.pidVersion;
    assert(requestProcessTask(caller.audit, target.guestPID)->error == ESRCH);
    --target.pidVersion;
    mismatchTaskAudit = YES;
    assert(requestProcessTask(caller.audit, target.guestPID)->error == ESRCH);
    mismatchTaskAudit = NO;
    taskInfoError = KERN_INVALID_ARGUMENT;
    assert(requestProcessTask(caller.audit, target.guestPID)->error == ESRCH);
    taskInfoError = KERN_SUCCESS;
    LSBJob *duplicate = debuggerJob(8601, target.guestPID, 9502, "/bin/df");
    giveTestTaskPort(duplicate);
    assert(requestProcessTask(caller.audit, target.guestPID)->error == ESRCH);
    finishJob(duplicate, 0);
    target.taskPort = MACH_PORT_NULL;
    assert(requestProcessTask(caller.audit, target.guestPID)->error == ENOTSUP);
    mach_port_t invalid = MACH_PORT_NULL;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &invalid));
    assert(!mach_port_insert_right(mach_task_self(), invalid, invalid, MACH_MSG_TYPE_MAKE_SEND));
    target.taskPort = invalid;
    assert(requestProcessTask(caller.audit, target.guestPID)->error == ESRCH); // A receive/send endpoint cannot be exported as a task.
    assert(!mach_port_mod_refs(mach_task_self(), invalid, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(requestProcessTask(caller.audit, target.guestPID)->error == ESRCH); // Nor can a dead name.
    assert(!mach_port_deallocate(mach_task_self(), invalid));
    target.taskPort = task;
    taskReplySendError = EPIPE;
    assert(requestProcessTask(caller.audit, target.guestPID)->sendError == EPIPE && sendReferences(task) == initial);
    taskReplySendError = 0;
    sessionDraining = YES;
    assert(requestProcessTask(caller.audit, target.guestPID)->error == ECANCELED);
    sessionDraining = NO;
    assert(!requestProcessTask(caller.audit, target.guestPID)->error && sendReferences(task) == initial + 1);
    finishJob(target, 0);
    assert(!MACH_PORT_VALID(target.taskPort) && sendReferences(task) == initial); // The successful reply keeps its own reference.
    assert(requestProcessTask(caller.audit, target.guestPID)->error == ESRCH && sendReferences(task) == initial - 1);
    resetBroker();
}

static void testTaskEntitlementSnapshot(void) {
    resetBroker();
    char temporary[] = "/tmp/LSBTaskEntitlement.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = @(canonical);
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    NSString *bin = [runtimeRoot stringByAppendingPathComponent:@"bin"], *path = [bin stringByAppendingPathComponent:@"inspect"];
    NSFileManager *manager = NSFileManager.defaultManager;
    assert([manager createDirectoryAtPath:bin withIntermediateDirectories:YES attributes:nil error:NULL]);
    const unsigned char der[] = {0x70, 30, 2, 1, 1, 0xb0, 25, 0x30, 23, 0x0c, 18,
        't', 'a', 's', 'k', '_', 'f', 'o', 'r', '_', 'p', 'i', 'd', '-', 'a', 'l', 'l', 'o', 'w', 1, 1, 0xff};
    NSData *grant = [NSData dataWithBytes:der length:sizeof(der)];
    assert([entitlementImage(grant, YES) writeToFile:path atomically:YES]);
    LSBJob *allowed = runIdentityJob("/bin/inspect", getpid() + 550, 9550, 1);
    assert(allowed.guestTaskForPIDAllowed && !allowed.guestProcessInfoAllowed && !allowed.guestEntitlementsError);
    assert([[@"plain image" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:path atomically:YES]);
    LSBJob *denied = runIdentityJob("/bin/inspect", getpid() + 551, 9551, 2);
    assert(!denied.guestTaskForPIDAllowed && denied.guestEntitlementsError);
    assert(!requestProcessTask(allowed.audit, denied.guestPID)->error); // Grant remains bound to the original image snapshot.
    assert(requestProcessTask(denied.audit, allowed.guestPID)->error == EPERM);
    resetBroker();
    assert([manager removeItemAtPath:directory error:NULL]);
}

static void testTaskCaptureStartupAndCleanup(void) {
    enum { Captured, CreateFailure, ReceiveFailure, MissingCapture };
    for (unsigned test = Captured; test <= MissingCapture; ++test) {
        resetBroker();
        automaticTaskCapture = NO;
        if (test == CreateFailure) taskCaptureCreateError = ENOSPC;
        int fd;
        unsigned object;
        enqueueSpawn(spawnRequest(&fd, &object), nil);
        LSBJob *job = startingJob;
        uint64_t identifier = job.identifier;
        assert(job.taskCaptureError == ENOENT && !MACH_PORT_VALID(job.taskPort));
        LSBReceivedMessage *checkin = messageFor(LSB_REQUEST_CHECKIN, getpid(), 9600 + test, 0);
        unsigned checkinObject = ((TestMessageObject *)checkin->object)->identifier;
        handleMessage(checkin);
        assert(taskCaptureCreates == 1 && !taskCaptureReceives);
        if (test == CreateFailure) {
            assert(replyFor(checkinObject)->error == ENOSPC && !findJob(identifier));
            assert(!MACH_PORT_VALID(job.taskPort) && !job.taskCaptureSource && !MACH_PORT_VALID(job.taskCapture.port));
            continue;
        }
        assert(job.phase == JobDispatched && job.taskCaptureSource && MACH_PORT_VALID(job.taskCapture.port));
        mach_port_t endpoint = job.taskCapture.port;
        assert(hasRight(endpoint, MACH_PORT_TYPE_RECEIVE) && hasRight(endpoint, MACH_PORT_TYPE_SEND));
        if (test == Captured) for (unsigned word = 0; word < 8; ++word) {
            LSBReceivedMessage *forged = messageFor(LSB_REQUEST_READY, job.pid, job.pidVersion, identifier);
            forged->audit.val[word] ^= 1;
            unsigned forgedObject = ((TestMessageObject *)forged->object)->identifier;
            handleMessage(forged);
            assert(replyFor(forgedObject)->error == (word == 5 || word == 7 ? ESRCH : EACCES));
            assert(job.phase == JobDispatched && job.taskCapture.port == endpoint && job.taskCaptureSource && !taskCaptureReceives);
        }
        taskCaptureReceiveError = EAGAIN;
        receiveGuestTaskPort(identifier);
        assert(job.taskCaptureError == ENOENT && !MACH_PORT_VALID(job.taskPort) && job.taskCaptureSource && taskCaptureReceives == 1);
        if (test == ReceiveFailure) {
            taskCaptureReceiveError = EACCES;
            receiveGuestTaskPort(identifier);
            assert(!findJob(identifier) && !MACH_PORT_VALID(job.taskPort));
        } else {
            if (test == Captured) {
                taskCaptureReceiveError = 0;
                receiveGuestTaskPort(identifier);
                assert(!job.taskCaptureError && MACH_PORT_VALID(job.taskPort));
            }
            handleMessage(messageFor(LSB_REQUEST_READY, getpid(), 9600 + test, identifier));
            assert(replyFor(object)->error == (test == Captured ? 0 : EPROTO));
            if (test == Captured) assert(job.phase == JobRunning && MACH_PORT_VALID(job.taskPort));
            else assert(!findJob(identifier));
        }
        assert(!job.taskCaptureSource && !MACH_PORT_VALID(job.taskCapture.port));
        runMainQueueUntil(^BOOL { return !hasRight(endpoint, MACH_PORT_TYPE_RECEIVE); });
        assert(!sendReferences(endpoint));
        if (test == Captured) {
            mach_port_t task = job.taskPort;
            mach_port_urefs_t before = sendReferences(task);
            finishJob(job, 0);
            assert(!MACH_PORT_VALID(job.taskPort) && sendReferences(task) == before - 1);
        }
    }
    resetBroker();
}

static RecordedReply *requestDebuggerRegistration(audit_token_t caller, mach_port_t endpoint) {
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_REGISTER_DEBUGGER_PORT, caller.val[5], caller.val[7], 0);
    request->audit = caller;
    if (MACH_PORT_VALID(endpoint)) assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, 1));
    request->processDebuggerPort = endpoint;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    return replyFor(object);
}

static RecordedReply *requestDebuggerActivation(audit_token_t caller) {
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_REGISTER_DEBUGGER_PORT, caller.val[5], caller.val[7], 0);
    request->audit = caller; request->activateDebuggerPort = true;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    return replyFor(object);
}

static RecordedReply *requestDebuggerInfo(audit_token_t caller, pid_t target) {
    memset(&debuggerInfoReply, 0, sizeof(debuggerInfoReply));
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_COPY_DEBUGGER_INFO, caller.val[5], caller.val[7], 0);
    request->audit = caller; request->targetPID = target;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    runMainQueueUntil(^BOOL { return replyFor(object) != NULL; });
    return replyFor(object);
}

static void testCooperativeDebuggerAuthentication(void) {
    resetBroker();
    LSBJob *caller = debuggerJob(8200, 8100, 9300, "/System/Library/PrivateFrameworks/DragUI.framework/Support/druid");
    LSBJob *target = debuggerJob(8300, 8201, 9301, "/System/Library/CoreServices/SpringBoard.app/SpringBoard");
    caller.guestDragUIAllowed = YES;
    mach_port_t endpoint;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &endpoint));
    assert(!mach_port_insert_right(mach_task_self(), endpoint, endpoint, MACH_MSG_TYPE_MAKE_SEND));
    mach_port_urefs_t initial = sendReferences(endpoint);
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t changed = target.audit; changed.val[word] ^= 1;
        assert(requestDebuggerRegistration(changed, endpoint)->error == EACCES && sendReferences(endpoint) == initial);
    }
    assert(requestDebuggerRegistration(target.audit, MACH_PORT_NULL)->error == EINVAL);
    assert(requestDebuggerActivation(target.audit)->error == EINVAL && !target.processDebuggerReady);
    assert(!requestDebuggerRegistration(target.audit, endpoint)->error && target.processDebuggerPort == endpoint);
    assert(sendReferences(endpoint) == initial + 1);
    assert(requestDebuggerRegistration(target.audit, endpoint)->error == EALREADY && sendReferences(endpoint) == initial + 1);
    assert(!target.processDebuggerReady && requestDebuggerInfo(caller.audit, target.pid)->error == ENOTSUP && !debuggerQueryCalls);
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t changed = target.audit; changed.val[word] ^= 1;
        assert(requestDebuggerActivation(changed)->error == EACCES && !target.processDebuggerReady);
    }
    assert(!requestDebuggerActivation(target.audit)->error && target.processDebuggerReady);
    assert(requestDebuggerActivation(target.audit)->error == EALREADY);
    assert(!requestDebuggerInfo(caller.audit, target.pid)->error && debuggerInfoReply.kp_proc.p_pid == target.pid &&
           debuggerInfoReply.kp_proc.p_flag == P_TRACED && debuggerQueryCalls == 1 && !caller.debuggerQueryPending);
    assert(sendReferences(endpoint) == initial + 1);
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t changed = caller.audit; changed.val[word] ^= 1;
        assert(requestDebuggerInfo(changed, target.pid)->error == EACCES && debuggerQueryCalls == 1);
    }
    caller.guestDragUIAllowed = NO;
    assert(requestDebuggerInfo(caller.audit, target.pid)->error == EACCES); caller.guestDragUIAllowed = YES;
    LSBPeerProcessIdentity original = caller.peerIdentity, modified = original;
    strcpy(modified.executablePath, "/Plugins/druid"); caller.peerIdentity = modified;
    assert(requestDebuggerInfo(caller.audit, target.pid)->error == EACCES); caller.peerIdentity = original;
    caller.debuggerQueryPending = YES;
    assert(requestDebuggerInfo(caller.audit, target.pid)->error == EBUSY); caller.debuggerQueryPending = NO;
    assert(requestDebuggerInfo(caller.audit, target.guestPID)->error == ESRCH && debuggerQueryCalls == 1);
    for (unsigned condition = 0; condition < 5; ++condition) {
        target.retiredForExec = condition == 0; target.finishing = condition == 1;
        target.phase = condition == 2 ? JobDispatched : JobRunning;
        target.executionRole = condition == 3 ? LSB_EXECUTION_NATIVE_METAL_SERVICE : LSB_EXECUTION_SIMULATOR;
        target.peerIdentityError = condition == 4 ? EIO : 0;
        assert(requestDebuggerInfo(caller.audit, target.pid)->error == ESRCH && debuggerQueryCalls == 1);
    }
    target.retiredForExec = target.finishing = NO; target.phase = JobRunning;
    target.executionRole = LSB_EXECUTION_SIMULATOR; target.peerIdentityError = 0;
    debuggerQueryError = ETIMEDOUT;
    assert(requestDebuggerInfo(caller.audit, target.pid)->error == ETIMEDOUT && !debuggerInfoReply.kp_proc.p_pid && debuggerQueryCalls == 2);
    debuggerQueryError = 0;
    debuggerQueryHook = ^{ target.retiredForExec = YES; };
    assert(requestDebuggerInfo(caller.audit, target.pid)->error == ESRCH && !debuggerInfoReply.kp_proc.p_pid && debuggerQueryCalls == 3);
    target.retiredForExec = NO;
    debuggerQueryHook = ^{ audit_token_t audit = target.audit; ++audit.val[7]; target.audit = audit; ++target.pidVersion; };
    assert(requestDebuggerInfo(caller.audit, target.pid)->error == ESRCH && !debuggerInfoReply.kp_proc.p_pid && debuggerQueryCalls == 4);
    debuggerQueryHook = nil;
    releaseGuestProcessIdentity(target);
    assert(!target.processDebuggerPort && sendReferences(endpoint) == initial);
    assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, -1));
    assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
    resetBroker();
}

static RecordedReply *requestNativeChildPID(audit_token_t caller, pid_t target) {
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_RESOLVE_NATIVE_CHILD_PID, caller.val[5], caller.val[7], 0);
    request->audit = caller;
    request->targetPID = target;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    RecordedReply *reply = replyFor(object);
    assert(reply && !reply->requestID && !reply->deliveredPlan && (reply->error ? reply->pid == 0 : reply->pid > 0));
    return reply;
}

static void testPeerIdentitySnapshotsAndAuthentication(void) {
    resetBroker();
    char temporary[] = "/tmp/LSBBrokerPeerIdentity.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    NSFileManager *manager = NSFileManager.defaultManager;
    for (NSString *path in @[@"usr/libexec/runningboardd", @"System/Library/CoreServices/SpringBoard.app/SpringBoard", @"bin/df"]) {
        NSString *physical = [runtimeRoot stringByAppendingPathComponent:path];
        assert([manager createDirectoryAtPath:physical.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:physical atomically:YES]);
    }
    NSString *alias = [[NSString stringWithUTF8String:temporary] stringByAppendingString:@"/RuntimeRoot/usr/libexec/runningboardd"];
    LSBJob *caller = runIdentityJob(alias.fileSystemRepresentation, getpid() + 100, 1500, 0xa1);
    LSBJob *target = runIdentityJob("/System/Library/CoreServices/SpringBoard.app/SpringBoard", getpid() + 101, 1501, 0xb2);
    assert(!strcmp(caller.peerIdentity.executablePath, "/usr/libexec/runningboardd"));
    assert(!requestProcessIdentity(caller.audit, target.pid)->error);
    audit_token_t targetAudit = target.audit;
    assert(!memcmp(&identityReply.audit, &targetAudit, sizeof(targetAudit)));
    assert(identityReply.shortInfo[63] == 0xb2 && LSBPeerProcessIdentityPID(&identityReply) == (unsigned)target.pid);
    assert(!strcmp(identityReply.executablePath, "/System/Library/CoreServices/SpringBoard.app/SpringBoard"));
    assert(requestProcessIdentity(target.audit, caller.pid)->error == EACCES);
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t altered = caller.audit;
        altered.val[word] ^= 1;
        assert(requestProcessIdentity(altered, target.pid)->error == EACCES);
    }
    assert(requestProcessIdentity(caller.audit, -1)->error == EINVAL);
    assert(requestProcessIdentity(caller.audit, getpid() + 200)->error == ESRCH);
    for (unsigned condition = 0; condition < 5; ++condition) {
        if (condition == 0) target.retiredForExec = YES;
        if (condition == 1) target.finishing = YES;
        if (condition == 2) target.phase = JobDispatched;
        if (condition == 3) target.executionRole = LSB_EXECUTION_NATIVE_METAL_PROBE;
        if (condition == 4) target.executionRole = LSB_EXECUTION_NATIVE_METAL_SERVICE;
        assert(requestProcessIdentity(caller.audit, target.pid)->error == ESRCH);
        target.retiredForExec = target.finishing = NO;
        target.phase = JobRunning;
        target.executionRole = LSB_EXECUTION_SIMULATOR;
        if (condition == 0) caller.retiredForExec = YES;
        if (condition == 1) caller.finishing = YES;
        if (condition == 2) caller.phase = JobDispatched;
        if (condition == 3) caller.executionRole = LSB_EXECUTION_NATIVE_METAL_PROBE;
        if (condition == 4) caller.executionRole = LSB_EXECUTION_NATIVE_METAL_SERVICE;
        assert(requestProcessIdentity(caller.audit, target.pid)->error == EACCES);
        caller.retiredForExec = caller.finishing = NO;
        caller.phase = JobRunning;
        caller.executionRole = LSB_EXECUTION_SIMULATOR;
    }
    LSBPeerProcessIdentity original = caller.peerIdentity, altered = original;
    strcpy(altered.executablePath, "/Plugins/runningboardd");
    caller.peerIdentity = altered;
    assert(requestProcessIdentity(caller.audit, target.pid)->error == EACCES);
    caller.peerIdentity = original;
    mismatchTaskAudit = YES;
    assert(requestProcessIdentity(caller.audit, target.pid)->error == ESRCH);
    mismatchTaskAudit = NO;
    taskInfoError = KERN_INVALID_ARGUMENT;
    assert(requestProcessIdentity(caller.audit, target.pid)->error == ESRCH);
    taskInfoError = KERN_SUCCESS;
    assert(!requestProcessIdentity(caller.audit, target.pid)->error);
    mach_port_t name = target.peerIdentity.taskName;
    mach_port_urefs_t before = sendReferences(name);
    pid_t reusedPID = target.pid;
    finishJob(target, 0);
    assert(!MACH_PORT_VALID(target.peerIdentity.taskName) && sendReferences(name) == before - 1);
    assert(MACH_PORT_VALID(identityReply.taskName)); // The copied reply retains its own reference after job retirement.
    assert(requestProcessIdentity(caller.audit, reusedPID)->error == ESRCH);
    LSBJob *replacement = runIdentityJob("/bin/df", reusedPID, 1502, 0xc3);
    assert(!requestProcessIdentity(caller.audit, reusedPID)->error && identityReply.audit.val[7] == replacement.pidVersion);
    assert(identityReply.shortInfo[63] == 0xc3 && !strcmp(identityReply.executablePath, "/bin/df"));
    resetBroker();
    assert([manager removeItemAtPath:directory error:NULL]);
}

static void testLaunchdChildIdentityAndSignals(void) {
    resetBroker();
    char temporary[] = "/tmp/LSBBrokerLaunchdIdentity.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    NSFileManager *manager = NSFileManager.defaultManager;
    for (NSString *path in @[@"sbin/launchd_sim", @"bin/df"]) {
        NSString *physical = [runtimeRoot stringByAppendingPathComponent:path];
        assert([manager createDirectoryAtPath:physical.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:physical atomically:YES]);
    }
    LSBJob *caller = runIdentityJob("/sbin/launchd_sim", getpid() + 100, 1700, 0xa1);
    caller.simulatorLaunchd = YES;
    LSBJob *child = runIdentityJob("/bin/df", getpid() + 101, 1701, 0xb2);
    child.guestPID = getpid() + 201;
    child.parentIdentifier = caller.identifier;
    // The retired proxy's logical PID can be an unrelated native process now. Only the owned guest child may be returned.
    LSBJob *unrelated = runIdentityJob("/bin/df", child.guestPID, 1702, 0xc3);
    assert(!requestProcessIdentity(caller.audit, child.guestPID)->error);
    assert(identityReply.audit.val[5] == (unsigned)child.pid && identityReply.audit.val[7] == child.pidVersion);
    assert(identityReply.shortInfo[63] == 0xb2);
    assert(requestProcessIdentity(caller.audit, child.pid)->error == ESRCH); // This launchd RPC accepts only logical child IDs.
    assert(requestProcessIdentity(caller.audit, caller.guestPID)->error == ESRCH);
    assert(!requestKill(caller, child.guestPID, SIGTERM)->error && signaledAudit.val[5] == (unsigned)child.pid);
    assert(!requestKill(caller, child.pid, SIGSTOP)->error && lastSignal == SIGSTOP && signaledAudit.val[7] == child.pidVersion);
    unsigned before = deliveredSignals;
    unrelated.guestPID = getpid() + 202;
    assert(requestKill(caller, unrelated.guestPID, SIGTERM)->error == ESRCH && deliveredSignals == before);
    assert(requestProcessIdentity(caller.audit, unrelated.guestPID)->error == ESRCH);
    child.parentIdentifier = unrelated.identifier;
    assert(requestProcessIdentity(caller.audit, child.guestPID)->error == ESRCH);
    assert(requestKill(caller, child.pid, SIGTERM)->error == ESRCH && deliveredSignals == before);
    child.parentIdentifier = caller.identifier;
    for (unsigned condition = 0; condition < 4; ++condition) {
        if (condition == 0) child.retiredForExec = YES;
        if (condition == 1) child.finishing = YES;
        if (condition == 2) child.phase = JobDispatched;
        if (condition == 3) child.executionRole = LSB_EXECUTION_NATIVE_METAL_SERVICE;
        assert(requestProcessIdentity(caller.audit, child.guestPID)->error == ESRCH);
        assert(requestKill(caller, child.pid, SIGTERM)->error == ESRCH && deliveredSignals == before);
        child.retiredForExec = child.finishing = NO;
        child.phase = JobRunning;
        child.executionRole = LSB_EXECUTION_SIMULATOR;
    }
    caller.simulatorLaunchd = NO;
    assert(requestProcessIdentity(caller.audit, child.guestPID)->error == EACCES);
    assert(requestKill(caller, child.pid, SIGTERM)->error == ESRCH && deliveredSignals == before);
    assert(!requestKill(caller, child.guestPID, 0)->error); // Ordinary parents retain their existing logical child permission.
    caller.simulatorLaunchd = YES;
    LSBPeerProcessIdentity original = caller.peerIdentity, spoof = original;
    strcpy(spoof.executablePath, "/Plugins/launchd_sim");
    caller.peerIdentity = spoof;
    assert(requestProcessIdentity(caller.audit, child.guestPID)->error == EACCES);
    assert(requestKill(caller, child.pid, SIGTERM)->error == ESRCH && deliveredSignals == before);
    caller.peerIdentity = original;
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t forged = caller.audit;
        forged.val[word] ^= 1;
        assert(requestProcessIdentity(forged, child.guestPID)->error == EACCES);
        LSBReceivedMessage *request = messageFor(LSB_REQUEST_KILL, forged.val[5], forged.val[7], 0);
        request->audit = forged; request->targetPID = child.pid; request->signalNumber = SIGTERM;
        unsigned object = ((TestMessageObject *)request->object)->identifier;
        handleMessage(request);
        assert(replyFor(object)->error == EACCES && deliveredSignals == before);
    }
    // Even an impossible/inconsistent ledger with overlapping logical/native aliases must fail closed.
    unrelated.parentIdentifier = caller.identifier;
    unrelated.guestPID = child.pid;
    assert(requestKill(caller, child.pid, SIGTERM)->error == ESRCH && deliveredSignals == before);
    unrelated.parentIdentifier = 0;
    mismatchTaskAudit = YES;
    assert(requestProcessIdentity(caller.audit, child.guestPID)->error == ESRCH);
    mismatchTaskAudit = NO;
    identityCheckError = ESRCH;
    assert(requestKill(caller, child.pid, SIGTERM)->error == ESRCH && deliveredSignals == before);
    identityCheckError = 0;
    assert(!requestKill(caller, child.pid, SIGCONT)->error && deliveredSignals == before + 1 && lastSignal == SIGCONT);
    assert(child.phase == JobRunning && !child.finishing); // A successful signal never synthesizes a wait status or retires the job.
    resetBroker();
    assert([manager removeItemAtPath:directory error:NULL]);
}

static void testLaunchdNativeChildPIDResolution(void) {
    resetBroker();
    char temporary[] = "/tmp/LSBBrokerNativeChild.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    NSFileManager *manager = NSFileManager.defaultManager;
    for (NSString *path in @[@"sbin/launchd_sim", @"bin/df", @"usr/libexec/runningboardd"]) {
        NSString *physical = [runtimeRoot stringByAppendingPathComponent:path];
        assert([manager createDirectoryAtPath:physical.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:physical atomically:YES]);
    }
    LSBJob *caller = runIdentityJob("/sbin/launchd_sim", getpid() + 100, 1800, 0xa1);
    caller.simulatorLaunchd = YES;
    LSBJob *child = runIdentityJob("/bin/df", getpid() + 101, 1801, 0xb2);
    child.parentIdentifier = caller.identifier;
    assert(requestNativeChildPID(caller.audit, child.pid)->pid == child.pid); // Before exec the two identities may be equal.
    child.guestPID = getpid() + 201;
    LSBPeerProcessIdentity nativeIdentity = child.peerIdentity;
    mach_port_urefs_t references = sendReferences(nativeIdentity.taskName);
    assert(requestNativeChildPID(caller.audit, child.pid)->pid == child.guestPID);
    LSBPeerProcessIdentity after = child.peerIdentity;
    assert(!memcmp(&after, &nativeIdentity, sizeof(after)) && sendReferences(after.taskName) == references);
    assert(requestNativeChildPID(caller.audit, child.guestPID)->error == ESRCH); // No implicit logical-ID fallback.
    assert(requestNativeChildPID(caller.audit, caller.pid)->error == ESRCH);
    assert(requestNativeChildPID(caller.audit, -1)->error == EINVAL);
    assert(requestNativeChildPID(caller.audit, 0)->error == EINVAL);

    LSBJob *unrelated = runIdentityJob("/bin/df", child.guestPID, 1802, 0xc3);
    unrelated.guestPID = getpid() + 202;
    assert(requestNativeChildPID(caller.audit, unrelated.pid)->error == ESRCH);
    unrelated.parentIdentifier = caller.identifier;
    assert(requestNativeChildPID(caller.audit, unrelated.pid)->pid == unrelated.guestPID); // Exact native match wins over another job's logical alias.
    unrelated.parentIdentifier = 0;
    child.parentIdentifier = unrelated.identifier;
    assert(requestNativeChildPID(caller.audit, child.pid)->error == ESRCH);
    child.parentIdentifier = caller.identifier;
    for (unsigned condition = 0; condition < 6; ++condition) {
        if (condition == 0) child.retiredForExec = YES;
        if (condition == 1) child.finishing = YES;
        if (condition == 2) child.phase = JobDispatched;
        if (condition == 3) child.executionRole = LSB_EXECUTION_NATIVE_METAL_SERVICE;
        if (condition == 4) child.peerIdentityError = EIO;
        if (condition == 5) child.guestPID = 0;
        assert(requestNativeChildPID(caller.audit, child.pid)->error == ESRCH);
        child.retiredForExec = child.finishing = NO;
        child.phase = JobRunning;
        child.executionRole = LSB_EXECUTION_SIMULATOR;
        child.peerIdentityError = 0;
        child.guestPID = getpid() + 201;
    }
    for (unsigned condition = 0; condition < 6; ++condition) {
        if (condition == 0) caller.retiredForExec = YES;
        if (condition == 1) caller.finishing = YES;
        if (condition == 2) caller.phase = JobDispatched;
        if (condition == 3) caller.executionRole = LSB_EXECUTION_NATIVE_METAL_PROBE;
        if (condition == 4) caller.peerIdentityError = EIO;
        if (condition == 5) caller.simulatorLaunchd = NO;
        assert(requestNativeChildPID(caller.audit, child.pid)->error == EACCES);
        caller.retiredForExec = caller.finishing = NO;
        caller.phase = JobRunning;
        caller.executionRole = LSB_EXECUTION_SIMULATOR;
        caller.peerIdentityError = 0;
        caller.simulatorLaunchd = YES;
    }
    LSBPeerProcessIdentity originalCaller = caller.peerIdentity, wrongPath = originalCaller;
    strcpy(wrongPath.executablePath, "/Plugins/launchd_sim");
    caller.peerIdentity = wrongPath;
    assert(requestNativeChildPID(caller.audit, child.pid)->error == EACCES);
    caller.peerIdentity = originalCaller;
    for (unsigned word = 0; word < 8; ++word) {
        audit_token_t forged = caller.audit;
        forged.val[word] ^= 1;
        assert(requestNativeChildPID(forged, child.pid)->error == EACCES);
    }
    LSBJob *rbs = runIdentityJob("/usr/libexec/runningboardd", getpid() + 103, 1803, 0xd4);
    assert(requestNativeChildPID(rbs.audit, child.pid)->error == EACCES); // Kind-8 peer inspection does not grant logical namespace access.
    mismatchTaskAudit = YES;
    assert(requestNativeChildPID(caller.audit, child.pid)->error == ESRCH);
    mismatchTaskAudit = NO;
    taskInfoError = KERN_INVALID_ARGUMENT;
    assert(requestNativeChildPID(caller.audit, child.pid)->error == ESRCH);
    taskInfoError = KERN_SUCCESS;
    ++child.pidVersion;
    assert(requestNativeChildPID(caller.audit, child.pid)->error == ESRCH);
    --child.pidVersion;
    unrelated.parentIdentifier = caller.identifier;
    pid_t unrelatedPID = unrelated.pid;
    unrelated.pid = child.pid;
    assert(requestNativeChildPID(caller.audit, child.pid)->error == ESRCH); // Inconsistent duplicate native identity is never selected arbitrarily.
    unrelated.pid = unrelatedPID;
    unrelated.parentIdentifier = 0;

    pid_t retiredPID = child.pid, stablePID = child.guestPID;
    child.retiredForExec = YES;
    LSBJob *replacement = runIdentityJob("/bin/df", getpid() + 104, 1804, 0xe5);
    replacement.parentIdentifier = caller.identifier;
    replacement.guestPID = stablePID;
    assert(requestNativeChildPID(caller.audit, retiredPID)->error == ESRCH);
    assert(requestNativeChildPID(caller.audit, replacement.pid)->pid == stablePID);
    finishJob(child, 0);
    LSBJob *reused = runIdentityJob("/bin/df", retiredPID, 1805, 0xf6);
    assert(requestNativeChildPID(caller.audit, retiredPID)->error == ESRCH); // Reused but unowned native identity remains unowned.
    reused.parentIdentifier = caller.identifier;
    assert(requestNativeChildPID(caller.audit, retiredPID)->pid == reused.guestPID && reused.guestPID != stablePID);
    finishJob(replacement, 0);
    assert(requestNativeChildPID(caller.audit, getpid() + 104)->error == ESRCH);
    resetBroker();
    assert([manager removeItemAtPath:directory error:NULL]);
}

static void testPeerExecutablePathContainment(void) {
    resetBroker();
    char temporary[] = "/tmp/LSBBrokerPeerPaths.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    NSFileManager *manager = NSFileManager.defaultManager;
    NSString *bin = [runtimeRoot stringByAppendingPathComponent:@"usr/libexec"];
    assert([manager createDirectoryAtPath:bin withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *image = [bin stringByAppendingPathComponent:@"runningboardd"], *outside = [directory stringByAppendingPathComponent:@"outside"];
    assert([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:image atomically:YES]);
    assert([[@"outside" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:outside atomically:YES]);
    NSString *aliasRoot = [directory stringByAppendingPathComponent:@"AliasRoot"];
    assert(!symlink(runtimeRoot.fileSystemRepresentation, aliasRoot.fileSystemRepresentation));
    assert(!symlink(outside.fileSystemRepresentation, [bin stringByAppendingPathComponent:@"escape"].fileSystemRepresentation));
    assert(!symlink(image.fileSystemRepresentation, [bin stringByAppendingPathComponent:@"inside"].fileSystemRepresentation));
    int fd;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    char output[LSB_PATH_BYTES];
    for (NSString *path in @[@"/usr/libexec/runningboardd", image, [aliasRoot stringByAppendingPathComponent:@"usr/libexec/runningboardd"],
                            @"/usr/libexec/inside"]) {
        setRequestProgram(request, path.fileSystemRepresentation);
        assert(!copyGuestExecutablePath(&request->plan, output) && !strcmp(output, "/usr/libexec/runningboardd"));
    }
    const char *invalid[] = {"/usr/libexec/../libexec/runningboardd", "/usr//libexec/runningboardd",
                            "/usr/./libexec/runningboardd", "/usr/libexec/runningboardd/"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        setRequestProgram(request, invalid[i]);
        assert(copyGuestExecutablePath(&request->plan, output) == EINVAL && !output[0]);
    }
    for (NSString *path in @[@"/usr/libexec/escape", [bin stringByAppendingPathComponent:@"escape"]]) {
        setRequestProgram(request, path.fileSystemRepresentation);
        assert(copyGuestExecutablePath(&request->plan, output) == EACCES && !output[0]);
    }
    setRequestProgram(request, "/usr/libexec");
    assert(copyGuestExecutablePath(&request->plan, output) == EINVAL && !output[0]);
    setRequestProgram(request, "runningboardd");
    assert(copyGuestExecutablePath(&request->plan, output) == ENOTSUP && !output[0]);
    setRequestProgram(request, "/usr/libexec/missing");
    assert(copyGuestExecutablePath(&request->plan, output) == ENOENT && !output[0]);
    disposeMessage(request);
    resetBroker();
    assert([manager removeItemAtPath:directory error:NULL]);
}

static void testPeerIdentityCheckInFailures(void) {
    for (unsigned failure = 0; failure < 4; ++failure) {
        resetBroker();
        int fd;
        unsigned object;
        enqueueSpawn(spawnRequest(&fd, &object), nil);
        LSBJob *job = startingJob;
        LSBReceivedMessage *checkin = messageFor(LSB_REQUEST_CHECKIN, getpid(), 1510, 0);
        mach_port_t name = checkin->peerIdentity.taskName;
        mach_port_urefs_t before = sendReferences(name);
        preserveCheckInIdentity = YES;
        if (failure == 0) checkin->peerIdentity.audit.val[0] ^= 1;
        if (failure == 1) checkin->peerIdentity.shortInfo[0] ^= 1;
        if (failure == 2) mismatchTaskAudit = YES;
        if (failure == 3) taskInfoError = KERN_INVALID_ARGUMENT;
        handleMessage(checkin);
        assert(!jobs.count && job.finishing && replyFor(object)->error == EACCES);
        assert(!MACH_PORT_VALID(job.peerIdentity.taskName) && sendReferences(name) == before - 1);
    }
    resetBroker();
    int fd;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    setRequestProgram(request, "/tmp/RuntimeRoot/../usr/libexec/runningboardd");
    enqueueSpawn(request, nil);
    LSBJob *job = startingJob;
    handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 1511, 0));
    assert(job.phase == JobDispatched && job.peerIdentityError == EINVAL && !MACH_PORT_VALID(job.peerIdentity.taskName));
    resetBroker();
    LSBJob *existing = auditJob(getpid() + 500, getpid(), 1512, NO);
    existing.executed = YES;
    enqueueSpawn(spawnRequest(&fd, &object), nil);
    job = startingJob;
    LSBReceivedMessage *collision = messageFor(LSB_REQUEST_CHECKIN, getpid(), 1513, 0);
    mach_port_t name = collision->peerIdentity.taskName;
    mach_port_urefs_t before = sendReferences(name);
    mismatchTaskAudit = YES; // The PID collision must reject before snapshot validation or ownership transfer.
    handleMessage(collision);
    assert(replyFor(object)->error == EAGAIN && job.finishing && !MACH_PORT_VALID(job.peerIdentity.taskName));
    assert(sendReferences(name) == before - 1 && findJob(existing.identifier) == existing);
    resetBroker();
}

static void testGuestEntitlementsPathBoundaries(void) {
    resetBroker();
    char temporary[] = "/tmp/LSBBrokerEntitlementsPath.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    NSFileManager *manager = NSFileManager.defaultManager;
    assert([manager createDirectoryAtPath:runtimeRoot withIntermediateDirectories:YES attributes:nil error:NULL]);
    const unsigned char payload[] = {0x70, 5, 2, 1, 1, 0xb0, 0};
    NSData *der = [NSData dataWithBytes:payload length:sizeof(payload)], *snapshot = nil;
    NSString *image = [runtimeRoot stringByAppendingPathComponent:@"probe"], *outside = [directory stringByAppendingPathComponent:@"outside"];
    assert([entitlementImage(der, YES) writeToFile:image atomically:YES] && [entitlementImage(der, YES) writeToFile:outside atomically:YES]);
    NSString *escape = [runtimeRoot stringByAppendingPathComponent:@"escape"];
    assert(!symlink(outside.fileSystemRepresentation, escape.fileSystemRepresentation));
    NSString *aliasRoot = [directory stringByAppendingPathComponent:@"AliasRoot"];
    assert(!symlink(runtimeRoot.fileSystemRepresentation, aliasRoot.fileSystemRepresentation));
    int fd;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&fd, &object);
    const char *badPaths[] = {"probe", "/missing", "/escape", "/../outside"};
    const int expectedErrors[] = {ENOTSUP, ENOENT, EACCES, EINVAL};
    for (unsigned i = 0; i < sizeof(expectedErrors) / sizeof(*expectedErrors); ++i) {
        setRequestProgram(request, badPaths[i]);
        snapshot = [NSData data];
        assert(copyGuestEntitlements(&request->plan, &snapshot, NULL, NULL) == expectedErrors[i] && !snapshot);
    }
    NSString *temporaryAlias = [[NSString stringWithUTF8String:temporary] stringByAppendingString:@"/RuntimeRoot/probe"];
    NSString *joined = [runtimeRoot stringByAppendingString:@"//probe"];
    for (NSString *path in @[@"/probe", image, [aliasRoot stringByAppendingPathComponent:@"probe"], temporaryAlias, joined]) {
        setRequestProgram(request, path.fileSystemRepresentation);
        assert(!copyGuestEntitlements(&request->plan, &snapshot, NULL, NULL) && [snapshot isEqualToData:entitlementBlob(der)]);
        char guestPath[LSB_PATH_BYTES];
        assert(!copyGuestExecutablePath(&request->plan, guestPath) && !strcmp(guestPath, "/probe"));
    }
    // Only launchd's doubled physical-root separator is accepted. Neither additional empties nor traversal become valid.
    for (NSString *path in @[@"//probe", [runtimeRoot stringByAppendingString:@"///probe"],
                            [runtimeRoot stringByAppendingString:@"//../outside"], [runtimeRoot stringByAppendingString:@"//escape"],
                            [aliasRoot stringByAppendingString:@"//probe"]]) {
        setRequestProgram(request, path.fileSystemRepresentation);
        int error = copyGuestEntitlements(&request->plan, &snapshot, NULL, NULL);
        assert(error && !snapshot);
    }
    runtimeRoot = aliasRoot;
    setRequestProgram(request, image.fileSystemRepresentation);
    assert(!copyGuestEntitlements(&request->plan, &snapshot, NULL, NULL) && [snapshot isEqualToData:entitlementBlob(der)]);
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    setRequestProgram(request, escape.fileSystemRepresentation);
    assert(copyGuestEntitlements(&request->plan, &snapshot, NULL, NULL) == EACCES && !snapshot);
    setRequestProgram(request, "/probe");
    const unsigned char hashBytes[LSB_GUEST_CDHASH_BYTES] = {0x51, 0x52};
    codeDirectoryHash = [NSData dataWithBytes:hashBytes length:sizeof(hashBytes)];
    afterCodeDirectoryRead = ^(int descriptor) {
        char current[PATH_MAX];
        assert(!fcntl(descriptor, F_GETPATH, current));
        int writer = open(current, O_WRONLY | O_APPEND);
        assert(writer >= 0 && write(writer, "changed", 7) == 7 && !close(writer));
    };
    NSData *hashSnapshot = nil;
    int hashError = 0;
    assert(copyGuestEntitlements(&request->plan, &snapshot, &hashSnapshot, &hashError) == EAGAIN);
    assert(hashError == EAGAIN && !snapshot && !hashSnapshot);
    afterCodeDirectoryRead = nil;
    assert([entitlementImage(der, YES) writeToFile:image atomically:YES]);
    LSBJob *native = [LSBJob new];
    native.executionRole = LSB_EXECUTION_NATIVE_METAL_SERVICE;
    native.request = request;
    snapshotGuestEntitlements(native);
    assert(native.guestEntitlementsError == EACCES && !native.guestEntitlements && !native.guestXMLEntitlements);
    assert(!guestEntitlementErrorLogs); // Native helpers intentionally have no guest metadata.
    native.executionRole = LSB_EXECUTION_SIMULATOR;
    xmlConversionError = EILSEQ;
    snapshotGuestEntitlements(native);
    assert(!native.guestEntitlementsError && [native.guestEntitlements isEqualToData:entitlementBlob(der)]);
    assert(native.guestXMLEntitlementsError == EILSEQ && !native.guestXMLEntitlements);
    assert(guestEntitlementErrorLogs == 1);
    xmlConversionError = 0;
    native.request = NULL;
    assert([entitlementImage(der, NO) writeToFile:image atomically:YES]);
    assert(copyGuestEntitlements(&request->plan, &snapshot, NULL, NULL) == ENOENT && !snapshot);
    const unsigned char malformed[] = {0x70, 0xff};
    assert([entitlementImage([NSData dataWithBytes:malformed length:sizeof(malformed)], YES) writeToFile:image atomically:YES]);
    assert(copyGuestEntitlements(&request->plan, &snapshot, NULL, NULL) == EINVAL && !snapshot);
    assert(!unlink(image.fileSystemRepresentation) && !mkfifo(image.fileSystemRepresentation, 0600));
    assert(copyGuestEntitlements(&request->plan, &snapshot, NULL, NULL) == EINVAL && !snapshot); // Opening a nonregular image must not block.
    native.request = request;
    for (unsigned i = 0; i < 20; ++i) snapshotGuestEntitlements(native);
    assert(guestEntitlementErrorLogs == 13); // Twelve detailed failures and one suppression notice, never per-request log flooding.
    setRequestProgram(request, "/RuntimeRoot//usr/libexec/xpcproxy_sim");
    for (unsigned i = 0; i < 8; ++i) snapshotGuestEntitlements(native);
    assert(guestProxySigningErrorLogs == 3 && guestEntitlementErrorLogs == 13);
    assert(native.guestEntitlementsError == EINVAL && native.guestCodeDirectoryHashError == EINVAL); // Quotas never relax validation.
    native.request = NULL;
    disposeMessage(request);
    resetBroker();
    assert([manager removeItemAtPath:directory error:NULL]);
}

static void testAuditResolutionSeesCommittedExecOnly(void) {
    for (unsigned failReady = 0; failReady < 2; ++failReady) {
        resetBroker();
        LSBJob *launchd = runTestJob(nil, 800), *old = runTestJob(launchd, 801);
        launchd.simulatorLaunchd = YES;
        int fd;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&fd, &object);
        request->kind = LSB_REQUEST_EXEC;
        request->audit = old.audit;
        handleMessage(request);
        LSBJob *replacement = startingJob;
        reportedPID = getpid() + 200;
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, reportedPID, 802, 0));
        assert(replacement.phase == JobDispatched);
        assert(auditReply(launchd.audit, replacement.audit)->error == ESRCH);
        RecordedReply *reply = auditReply(launchd.audit, old.audit);
        assert(!reply->error && reply->pid == old.guestPID);
        __block BOOL queried = NO;
        readyReplyHook = ^{
            assert(replacement.phase == JobDispatched); // The kernel may wake the candidate here, before publication.
            dispatch_async(dispatch_get_main_queue(), ^{
                RecordedReply *newReply = auditReply(launchd.audit, replacement.audit);
                RecordedReply *oldReply = auditReply(launchd.audit, old.audit);
                assert(failReady ? newReply->error == ESRCH : (!newReply->error && newReply->pid == old.guestPID));
                assert(failReady ? (!oldReply->error && oldReply->pid == old.guestPID) : oldReply->error == ESRCH);
                queried = YES;
            });
        };
        readySendError = failReady ? EPIPE : 0;
        handleMessage(messageFor(LSB_REQUEST_READY, reportedPID, 802, replacement.identifier));
        readyReplyHook = nil;
        readySendError = 0;
        assert(!queried);
        runMainQueueUntil(^BOOL { return queried; });
        assert(failReady ? findJob(old.identifier) != nil : findJob(replacement.identifier) != nil);
        assert(retiredIdentityCount == 1);
    }
}

static LSBReceivedMessage *launchdRequest(int *fd, unsigned *object, const char *path, const char *marker) {
    LSBReceivedMessage *message = spawnRequest(fd, object);
    assert(!LSBPlanAddString(&message->plan, path, &message->plan.path));
    message->plan.argv[0] = message->plan.path;
    if (marker) {
        assert(!LSBPlanAddString(&message->plan, marker, &message->plan.envp[0]));
        message->plan.envc = 1;
    }
    return message;
}

static LSBJob *checkInLaunchd(unsigned *parentObject) {
    int fd;
    enqueueSpawn(launchdRequest(&fd, parentObject, "/sbin/launchd_sim", "LSB_SIMULATOR_LAUNCHD=1"), nil);
    LSBJob *job = startingJob;
    assert(job.simulatorLaunchd);
    audit_token_t audit = ownAudit();
    LSBReceivedMessage *checkIn = messageFor(LSB_REQUEST_CHECKIN, getpid(), audit.val[7], 0);
    checkIn->audit = audit;
    handleMessage(checkIn);
    assertClosed(fd);
    return job;
}

static void testLaunchdModeRequiresInitialPathAndMarker(void) {
    for (unsigned variant = 0; variant < 4; ++variant) {
        resetBroker();
        LSBJob *parent = nil;
        if (variant == 3) {
            parent = [LSBJob new];
            parent.identifier = 99;
            assert(!prepareChildStatus(parent));
        }
        int fd;
        unsigned object;
        const char *path = variant == 0 ? "/bin/df" : "/sbin/launchd_sim";
        const char *marker = variant == 1 ? NULL : variant == 2 ? "LSB_SIMULATOR_LAUNCHD=0" : "LSB_SIMULATOR_LAUNCHD=1";
        enqueueSpawn(launchdRequest(&fd, &object, path, marker), parent);
        assert(startingJob && !startingJob.simulatorLaunchd);
        handleMessage(messageFor(LSB_REQUEST_CHECKIN, getpid(), 71, 0));
        assert(planReplies() == 1 && !replies[replyCount - 1].launchdTank && !startingJob.launchdHandshake && !startingJob.launchdSource);
        finishJob(startingJob, ECANCELED);
        assert(replyFor(object)->error == ECANCELED);
        assertClosed(fd);
    }
}

static void testLaunchdHandshakeDispatchAndRights(void) {
    resetBroker();
    __weak LSBJob *retired;
    mach_port_t service, replyPort;
    @autoreleasepool {
        unsigned object;
        LSBJob *job = checkInLaunchd(&object);
        retired = job;
        mach_port_t tank = replies[replyCount - 1].launchdTank;
        service = job.launchdHandshake->serviceSend;
        assert(tank && job.launchdSource && hasRight(tank, MACH_PORT_TYPE_RECEIVE) && !replyFor(object));
        handleMessage(messageFor(LSB_REQUEST_READY, getpid(), job.pidVersion, job.identifier));
        assert(job.phase == JobRunning && replyFor(object) && !replyFor(object)->error);
        assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &replyPort));
        mach_msg_header_t request = {
            .msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE),
            .msgh_size = sizeof(request), .msgh_remote_port = tank, .msgh_local_port = replyPort, .msgh_id = 708
        };
        assert(!mach_msg(&request, MACH_SEND_MSG, sizeof(request), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL));
        runMainQueueUntil(^BOOL{ return job.launchdHandshake->completed && !job.launchdHandshake->tankReceive; });
        assert(!job.launchdSource && !job.launchdHandshake->tankSend && !job.launchdHandshake->serviceReceive);
        assert(!hasRight(tank, MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_RECEIVE));
        assert(hasRight(service, MACH_PORT_TYPE_SEND) && !hasRight(service, MACH_PORT_TYPE_RECEIVE));
        struct {
            mach_msg_header_t header;
            mach_msg_body_t body;
            mach_msg_port_descriptor_t service;
            mach_msg_trailer_t trailer;
        } response = {0};
        assert(!mach_msg(&response.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(response), replyPort, 0, MACH_PORT_NULL));
        assert(response.header.msgh_id == 808 && response.header.msgh_size == 40 && (response.header.msgh_bits & MACH_MSGH_BITS_COMPLEX));
        assert(response.body.msgh_descriptor_count == 1 && response.service.name == service);
        assert(response.service.type == MACH_MSG_PORT_DESCRIPTOR && response.service.disposition == MACH_MSG_TYPE_MOVE_RECEIVE);
        assert(hasRight(service, MACH_PORT_TYPE_RECEIVE) && hasRight(service, MACH_PORT_TYPE_SEND));
        finishJob(job, 0);
        // Only the service send right belongs to the broker now; the received endpoint must survive job disposal.
        assert(!jobs.count && !startingJob && hasRight(service, MACH_PORT_TYPE_SEND));
    }
    runMainQueueUntil(^BOOL{ return retired == nil; });
    assert(hasRight(service, MACH_PORT_TYPE_RECEIVE) && !hasRight(service, MACH_PORT_TYPE_SEND));
    assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_mod_refs(mach_task_self(), replyPort, MACH_PORT_RIGHT_RECEIVE, -1));
}

static void testLaunchdCancellationAndFailedBootstrap(void) {
    for (unsigned failedSend = 0; failedSend < 2; ++failedSend) {
        resetBroker();
        __weak LSBJob *retired;
        mach_port_t tank, service;
        @autoreleasepool {
            unsigned object;
            launchdBootstrapSendError = failedSend ? EPIPE : 0;
            LSBJob *job = checkInLaunchd(&object);
            retired = job;
            tank = job.launchdHandshake->tankReceive;
            service = job.launchdHandshake->serviceReceive;
            assert(tank && service && !job.launchdHandshake->completed);
            if (!failedSend) finishJob(job, ECANCELED);
            assert(!jobs.count && !startingJob && !job.launchdSource && !launchLaneFailed);
            assert(replyFor(object)->error == (failedSend ? EPIPE : ECANCELED));
            // finishJob cancels the watch; its cancellation handler owns the later RECEIVE-right destruction.
            assert(hasRight(tank, MACH_PORT_TYPE_RECEIVE) && hasRight(service, MACH_PORT_TYPE_RECEIVE));
            runMainQueueUntil(^BOOL{ return !job.launchdHandshake->tankReceive; });
            assert(!job.launchdHandshake->tankSend && hasRight(service, MACH_PORT_TYPE_RECEIVE));
        }
        runMainQueueUntil(^BOOL{ return retired == nil; });
        assert(!hasRight(tank, MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_DEAD_NAME));
        assert(!hasRight(service, MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_DEAD_NAME));
    }
}

static void rememberRebootInputs(void) {
    const char *arguments[] = {"LiveSimBooter", "--launchd", NULL};
    const char *environment[] = {"LSB_RETAIN_HOST_CACHE=0", "LSB_WORKQUEUE_HARDWARE_BREAKPOINT=0", NULL};
    assert(!snapshotRebootInputs(2, arguments, environment));
}

static RecordedReply *requestReboot(LSBJob *sender) {
    LSBReceivedMessage *request = messageFor(LSB_REQUEST_REBOOT_USERSPACE, sender.pid, sender.pidVersion, 0);
    request->audit = sender.audit;
    unsigned object = ((TestMessageObject *)request->object)->identifier;
    handleMessage(request);
    RecordedReply *reply = replyFor(object);
    assert(reply && !reply->pid && !reply->requestID && !reply->deliveredPlan);
    return reply;
}

static void testRebootInputSnapshot(void) {
    resetBroker();
    char argument[] = "--launchd", environmentValue[] = "LSB_RETAIN_HOST_CACHE=0";
    const char *arguments[] = {"LiveSimBooter", argument, NULL};
    const char *environment[] = {environmentValue, NULL};
    assert(!snapshotRebootInputs(2, arguments, environment));
    memset(argument, 'x', sizeof(argument) - 1);
    memset(environmentValue, 'x', sizeof(environmentValue) - 1);
    assert(rebootArguments.count == 2 && rebootEnvironment.count == 1);
    assert(!strcmp(rebootArguments[1].bytes, "--launchd"));
    assert(!strcmp(rebootEnvironment[0].bytes, "LSB_RETAIN_HOST_CACHE=0"));
    NSArray *savedArguments = rebootArguments, *savedEnvironment = rebootEnvironment;
    assert(snapshotRebootInputs(0, arguments, environment) == E2BIG);
    assert(snapshotRebootInputs(LSB_MAX_VECTOR + 1, arguments, environment) == E2BIG);
    const char *missing[] = {"LiveSimBooter", NULL};
    assert(snapshotRebootInputs(2, missing, NULL) == E2BIG);
    char *oversized = malloc(LSB_STRING_BYTES + 1);
    assert(oversized);
    memset(oversized, 'x', LSB_STRING_BYTES);
    oversized[LSB_STRING_BYTES] = 0;
    const char *largeEnvironment[] = {oversized, NULL};
    assert(snapshotRebootInputs(1, arguments, largeEnvironment) == E2BIG);
    free(oversized);
    const char *tooMany[LSB_MAX_VECTOR + 2];
    for (unsigned i = 0; i <= LSB_MAX_VECTOR; ++i) tooMany[i] = "X=1";
    tooMany[LSB_MAX_VECTOR + 1] = NULL;
    assert(snapshotRebootInputs(1, arguments, tooMany) == E2BIG);
    assert(rebootArguments == savedArguments && rebootEnvironment == savedEnvironment);
}

static void testRebootAuthentication(void) {
    for (unsigned scenario = 0; scenario < 15; ++scenario) {
        resetBroker();
        rememberRebootInputs();
        LSBJob *launchd = terminalLaunchd();
        LSBReceivedMessage *request = messageFor(LSB_REQUEST_REBOOT_USERSPACE, launchd.pid, launchd.pidVersion, 0);
        request->audit = launchd.audit;
        int expected = EACCES;
        switch (scenario) {
            case 0: ++request->audit.val[5]; break;
            case 1: ++request->audit.val[7]; break;
            case 2: ++request->audit.val[1]; break; // Same PID/version is insufficient: require the complete audit token.
            case 3: launchd.parentIdentifier = 99; break;
            case 4: launchd.executionRole = LSB_EXECUTION_NATIVE_METAL_SERVICE; break;
            case 5: launchd.simulatorLaunchd = NO; break;
            case 6: launchd.peerIdentityError = EINVAL; break;
            case 7: {
                LSBPeerProcessIdentity identity = launchd.peerIdentity;
                strcpy(identity.executablePath, "/sbin/other");
                launchd.peerIdentity = identity;
                break;
            }
            case 8: launchd.retiredForExec = YES; break;
            case 9: launchd.finishing = YES; break;
            case 10: launchd.phase = JobDispatched; break;
            case 11: rebootArguments = nil; expected = ENOTSUP; break;
            case 12: rebootSendError = EPIPE; expected = 0; break;
            case 13: sessionDraining = YES; expected = ECANCELED; break;
            case 14: expected = 0; break;
        }
        unsigned object = ((TestMessageObject *)request->object)->identifier;
        uint64_t generation = rebootGeneration;
        handleMessage(request);
        RecordedReply *reply = replyFor(object);
        assert(reply && reply->error == expected && reply->sendError == rebootSendError);
        assert(!reply->pid && !reply->requestID && !reply->deliveredPlan);
        assert(launchd.rebootRequested == (scenario == 14));
        assert(jobs.count == 1 && rebootGeneration == generation && !submittedLaunches && !deliveredSignals);
        assert(disposedMessages == 1);
    }
}

static void testRebootRequiresCleanLaunchdExit(void) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        resetBroker();
        rememberRebootInputs();
        LSBJob *launchd = terminalLaunchd();
        if (scenario) assert(!requestReboot(launchd)->error && launchd.rebootRequested);
        uint64_t generation = rebootGeneration;
        if (scenario == 3) recordMonitorError(launchd.identifier, EIO);
        else recordChildExit(launchd.identifier, scenario == 1 ? 1 << 8 : scenario == 2 ? SIGTERM : 0);
        assert(!jobs.count && !sessionDraining && rebootGeneration == generation && !submittedLaunches);
    }
}

static NSString *makeRebootRuntime(void) {
    char temporary[] = "/tmp/LSBReboot.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *directory = [NSString stringWithUTF8String:canonical];
    runtimeRoot = [directory stringByAppendingPathComponent:@"RuntimeRoot"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:runtimeRoot withIntermediateDirectories:YES attributes:nil error:NULL]);
    return directory;
}

static NSDictionary *rebootConfiguration(void) {
    NSString *data = [runtimeRoot.stringByDeletingLastPathComponent stringByAppendingPathComponent:@LSB_DATA_DIRECTORY];
    return [NSDictionary dictionaryWithContentsOfFile:[data stringByAppendingPathComponent:@"var/run/LiveSimBooter-launchd-bootstrap.plist"]][@"Bootstrap"];
}

static void testRebootWaitsForOldGenerationAndRegeneratesConfiguration(void) {
    resetBroker();
    NSString *directory = makeRebootRuntime();
    rememberRebootInputs();
    const char *arguments[] = {"LiveSimBooter", "--launchd", NULL};
    const char *environment[] = {"LSB_RETAIN_HOST_CACHE=0", "LSB_WORKQUEUE_HARDWARE_BREAKPOINT=0", NULL};
    LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
    assert(initial && !initialPlan(initial, 2, arguments, environment));
    NSString *oldUUID = rebootConfiguration()[@"Self"][@"LSB_SIMULATOR_BOOT_UUID"];
    assert([[NSUUID alloc] initWithUUIDString:oldUUID]);
    disposeMessage(initial);
    LSBJob *launchd = terminalLaunchd(), *child = managedJob(launchd, 6601), *nativeHelper = managedJob(nil, 6602);
    nativeHelper.executionRole = LSB_EXECUTION_NATIVE_METAL_SERVICE;
    LSBJob *queued = managedJob(nil, 6603);
    queued.phase = JobQueued; queued.pid = 0; queued.audit = (audit_token_t){{0}};
    uint64_t childID = child.identifier;
    runningBoardError = 0;
    LSBRunningBoardMonitor *existing = nil;
    audit_token_t childAudit = child.audit;
    assert(!LSBRunningBoardMonitorStart(&childAudit, ^(int status, int error) {
        if (error) recordMonitorError(childID, error);
        else recordChildExit(childID, status);
    }, &existing));
    child.runningBoardMonitor = existing;
    LSBRunningBoardExitHandler queuedExit = existing.completion;
    // Simulate an exit callback already queued before the broker receives launchd's clean exit.
    dispatch_async(dispatch_get_main_queue(), ^{ queuedExit(SIGKILL, 0); });
    assert(!requestReboot(launchd)->error && !sessionDraining);
    uint64_t generation = rebootGeneration;
    recordChildExit(launchd.identifier, 0);
    assert(sessionDraining && !rebootDrainFailed && rebootGeneration == generation + 1);
    assert(!jobs.count && !startingJob && !submittedLaunches && rebootPendingDeaths.count == 2);
    assert(rebootDeathMonitors.count == 2 && rebootDeathMonitors[0] == existing && existing.completion);
    assert(runningBoardStarts == 2 && deliveredSignals == 2 && lastSignal == SIGKILL);
    assert(retiredIdentityCount && !hostTerminal);
    LSBRunningBoardExitHandler fallbackExit = ((LSBRunningBoardMonitor *)rebootDeathMonitors[1]).completion;
    assert(fallbackExit);
    // Neither releasing the extension assertion nor an unrelated/stale completion proves this generation has exited.
    extensionEvent(nativeHelper.identifier, nativeHelper.pid, ECANCELED, NULL);
    completeUserspaceReboot(generation);
    assert(sessionDraining && !submittedLaunches && rebootPendingDeaths.count == 2);
    runMainQueueUntil(^BOOL{ return rebootPendingDeaths.count == 1; });
    assert(sessionDraining && !submittedLaunches && existing.completion != nil);
    LSBReceivedMessage *blocked = messageFor(LSB_REQUEST_SPAWN, child.pid, child.pidVersion, 0);
    unsigned blockedObject = ((TestMessageObject *)blocked->object)->identifier;
    handleMessage(blocked);
    assert(replyFor(blockedObject)->error == ECANCELED);
    fallbackExit(SIGKILL, 0);
    assert(!sessionDraining && !rebootDrainFailed && !rebootPendingDeaths.count);
    assert(jobs.count == 1 && startingJob && submittedLaunches == 1 && startingJob.simulatorLaunchd);
    assert(startingJob.identifier > queued.identifier && !retiredIdentityCount && !retiredIdentitiesFull);
    assert(!existing.completion && !rebootDeathMonitors.count && !rebootExitSources.count);
    NSDictionary *bootstrap = rebootConfiguration();
    NSString *newUUID = bootstrap[@"Self"][@"LSB_SIMULATOR_BOOT_UUID"];
    assert([[NSUUID alloc] initWithUUIDString:newUUID] && ![newUUID isEqual:oldUUID]);
    assert([bootstrap[@"Child"][@"LSB_SIMULATOR_BOOT_UUID"] isEqual:newUUID]);
    assert([bootstrap[@"Self"][@"LSB_RETAIN_HOST_CACHE"] isEqual:@"0"]);
    assert([bootstrap[@"Child"][@"LSB_WORKQUEUE_HARDWARE_BREAKPOINT"] isEqual:@"0"]);
    queuedExit(SIGKILL, 0); fallbackExit(SIGKILL, 0);
    assert(jobs.count == 1 && submittedLaunches == 1); // Late old callbacks cannot restart the new generation again.
    resetBroker();
    assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
}

static void testRebootDrainFailureGuards(void) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        resetBroker();
        rememberRebootInputs();
        LSBJob *launchd = terminalLaunchd(), *child = managedJob(launchd, 6610);
        runningBoardError = scenario == 0 ? EACCES : 0;
        if (scenario == 2) { child.phase = JobStarting; child.pid = 0; child.audit = (audit_token_t){{0}}; startingJob = child; }
        if (scenario == 3) {
            uint64_t identifier = child.identifier;
            LSBRunningBoardMonitor *existing = [LSBRunningBoardMonitor new];
            existing.completion = ^(int status, int error) {
                if (error) recordMonitorError(identifier, error); else recordChildExit(identifier, status);
            };
            child.runningBoardMonitor = existing;
        }
        if (scenario == 4) { directSignalError = EPERM; extensionSignalError = EPERM; }
        assert(!requestReboot(launchd)->error);
        recordChildExit(launchd.identifier, 0);
        assert(sessionDraining && !jobs.count && !submittedLaunches);
        if (scenario == 1 || scenario == 3) {
            assert(rebootPendingDeaths.count == 1 && rebootDeathMonitors.count == 1);
            LSBRunningBoardExitHandler completion = ((LSBRunningBoardMonitor *)rebootDeathMonitors[0]).completion;
            completion(0, EIO);
        }
        if (scenario == 4) {
            // Failed signal submission leaves the independent death barrier armed; it must not permit a concurrent new session.
            assert(!rebootDrainFailed && rebootPendingDeaths.count == 1 && rebootDeathMonitors.count == 1);
            completeUserspaceReboot(rebootGeneration);
            assert(sessionDraining && !submittedLaunches && rebootPendingDeaths.count == 1);
        } else assert(rebootDrainFailed && !rebootPendingDeaths.count && !rebootDeathMonitors.count);
        assert(sessionDraining && !submittedLaunches && !startingJob);
        assert(launchLaneFailed == (scenario == 2));
    }
}

static void testRebootPreservesQueuedKqueueCallbacks(void) {
    for (unsigned failedRead = 0; failedRead < 2; ++failedRead) {
        resetBroker();
        NSString *directory = makeRebootRuntime();
        rememberRebootInputs();
        LSBJob *launchd = terminalLaunchd(), *child = runTestJob(launchd, 71);
        assert(child.exitMonitor && !child.runningBoardMonitor);
        dispatch_source_t source = child.exitMonitor;
        int descriptor = (int)dispatch_source_get_handle(source);
        struct kevent registration;
        EV_SET(&registration, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
        assert(!kevent(descriptor, &registration, 1, NULL, 0, NULL));
        assert(!requestReboot(launchd)->error);
        recordChildExit(launchd.identifier, 0);
        assert(sessionDraining && !jobs.count && !child.exitMonitor && rebootExitSources.count == 1);
        assert(rebootExitSources[0] == source && rebootPendingDeaths.count == 1 && !dispatch_source_testcancel(source));
        assert(submittedLaunches == 1 && !runningBoardStarts);
        monitorReadError = failedRead ? EIO : 0;
        monitorWaitStatus = SIGKILL;
        // The production event handler must read this retained monitor even though findJob now returns nil.
        EV_SET(&registration, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        assert(!kevent(descriptor, &registration, 1, NULL, 0, NULL));
        runMainQueueUntil(^BOOL{ return rebootPendingDeaths.count == 0; });
        assert(monitorReads && !rebootExitSources.count && dispatch_source_testcancel(source));
        assert(sessionDraining == failedRead && rebootDrainFailed == failedRead);
        assert(submittedLaunches == (failedRead ? 1 : 2) && jobs.count == (failedRead ? 0 : 1));
        runMainQueueUntil(^BOOL{ return fcntl(descriptor, F_GETFD) < 0 && errno == EBADF; });
        resetBroker();
        assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
    }
}

static NSDictionary *deviceProfile(LSBGuestDeviceType *type) {
    NSDictionary *profile = @{@"capabilities": @{
        @"DeviceSupportsClosedLoopHaptics": @NO,
        @"modelIdentifier": type.modelIdentifier, @"productClass": type.productClass,
        @"displays": @[@{@"deviceName": @"primary", @"width": @(type.width), @"height": @(type.height),
            @"scale": @(type.scale), @"hdpi": @(type.pitch), @"vdpi": @(type.pitch)}]
    }};
    assert(!LSBValidateGuestDeviceCapabilities(profile, type));
    return profile;
}

static NSString *stageDeviceProfile(LSBGuestDeviceType *type, NSDictionary *profile) {
    NSURL *url = [type capabilitiesURLInRuntimeRoot:[NSURL fileURLWithPath:runtimeRoot isDirectory:YES]];
    assert([NSFileManager.defaultManager createDirectoryAtURL:url.URLByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:profile format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
    assert([data writeToURL:url options:NSDataWritingAtomic error:NULL]);
    return url.path;
}

static NSString *activeDeviceProfilePath(void) {
    return [runtimeRoot stringByAppendingPathComponent:@"Library/Developer/CoreSimulator/DeviceType/capabilities.plist"];
}

static NSDictionary *activeDeviceProfile(void) {
    return [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:activeDeviceProfilePath()] error:NULL];
}

static unsigned hapticsEnvironmentCount(const LSBSpawnPlan *plan) {
    unsigned count = 0;
    for (unsigned i = 0; i < plan->envc; ++i) if (!strcmp(plan->strings + plan->envp[i], "LSB_EXPERIMENTAL_HAPTICS=1")) ++count;
    return count;
}

static void testSessionEnvironmentIsHostSelected(void) {
    for (unsigned flags = 0; flags < 4; ++flags) {
        resetBroker();
        sessionHapticsEnabled = flags & 1;
        sessionAudioEnabled = !!(flags & 2);
        savedHaptics = !sessionHapticsEnabled;
        // A later native environment change, like a guest-supplied flag, cannot change the already committed snapshot.
        assert(!setenv("LSB_AUDIO_OUTPUT", sessionAudioEnabled ? "0" : "1", 1));
        int descriptor;
        unsigned object;
        LSBReceivedMessage *request = spawnRequest(&descriptor, &object);
        for (const char **entry = (const char *[]){"LSB_EXPERIMENTAL_HAPTICS=0", "KEEP=1", "LSB_EXPERIMENTAL_HAPTICS=1",
                "LSB_EXPERIMENTAL_HAPTICS=1", "LSB_EXPERIMENTAL_HAPTICS_SUFFIX=1", "LSB_AUDIO_OUTPUT=0", "LSB_AUDIO_OUTPUT=1",
                "LSB_AUDIO_OUTPUT=1", "LSB_AUDIO_OUTPUT_SUFFIX=1", NULL}; *entry; ++entry)
            assert(!LSBPlanAddString(&request->plan, *entry, &request->plan.envp[request->plan.envc++]));
        LSBJob *job = enqueueJob(request, nil, LSB_EXECUTION_SIMULATOR);
        unsigned count = 3 + sessionHapticsEnabled + sessionAudioEnabled;
        assert(job && request->plan.envc == count && hapticsEnvironmentCount(&request->plan) == sessionHapticsEnabled);
        assert(!strcmp(request->plan.strings + request->plan.envp[0], "KEEP=1"));
        assert(!strcmp(request->plan.strings + request->plan.envp[1], "LSB_EXPERIMENTAL_HAPTICS_SUFFIX=1"));
        assert(!strcmp(request->plan.strings + request->plan.envp[2], "LSB_AUDIO_OUTPUT_SUFFIX=1"));
        unsigned audioFlags = 0;
        for (unsigned i = 0; i < request->plan.envc; ++i) {
            const char *entry = request->plan.strings + request->plan.envp[i];
            if (!strncmp(entry, "LSB_AUDIO_OUTPUT=", strlen("LSB_AUDIO_OUTPUT="))) {
                assert(!strcmp(entry, "LSB_AUDIO_OUTPUT=1"));
                ++audioFlags;
            }
        }
        assert(audioFlags == sessionAudioEnabled);
        uint32_t bytes = request->plan.stringBytes;
        assert(!applySessionEnvironment(&request->plan) && request->plan.stringBytes == bytes && request->plan.envc == count);
        resetBroker();
        assertClosed(descriptor);
    }
    resetBroker();
    sessionHapticsEnabled = YES;
    int descriptor;
    unsigned object;
    LSBReceivedMessage *request = spawnRequest(&descriptor, &object);
    uint32_t offset;
    assert(!LSBPlanAddString(&request->plan, "KEEP=1", &offset));
    for (unsigned i = 0; i < LSB_MAX_VECTOR; ++i) request->plan.envp[request->plan.envc++] = offset;
    assert(applySessionEnvironment(&request->plan) == E2BIG);
    disposeMessage(request);
    assertClosed(descriptor);
    resetBroker();
}

static void testHapticsColdApplyRequiresStockProfile(void) {
    resetBroker();
    NSString *directory = makeRebootRuntime();
    jobs = nil;
    LSBGuestDeviceType *device = LSBDefaultGuestDeviceType();
    NSDictionary *stock = deviceProfile(device), *projected = nil;
    assert(!LSBProjectGuestHapticsCapability(stock, kCFBooleanTrue, &projected));
    assert(!LSBInstallGuestDeviceCapabilities(runtimeRoot, projected));
    __block unsigned calls = 0;
    __block int result = -1;
    LSBHostApplyHapticsEnabled(YES, ^(int error, BOOL restarted) { ++calls; result = error; assert(!restarted); });
    assert(calls == 1 && result == ENOENT && !hapticsPreferenceWrites && !devicePreferenceWrites);
    assert(!jobs && !submittedLaunches && !LSBHostCurrentHapticsEnabled() && [activeDeviceProfile() isEqual:projected]);
    stageDeviceProfile(device, stock);
    LSBHostApplyHapticsEnabled(YES, ^(int error, BOOL restarted) { ++calls; result = error; assert(!restarted); });
    assert(calls == 2 && !result && hapticsPreferenceWrites == 1 && savedHaptics && LSBHostCurrentHapticsEnabled());
    assert(!jobs && !submittedLaunches && [activeDeviceProfile() isEqual:projected]);
    LSBHostApplyHapticsEnabled(YES, ^(int error, BOOL restarted) { ++calls; assert(!error && !restarted); });
    assert(calls == 3 && hapticsPreferenceWrites == 1);
    assert(!setenv("LSB_EXPERIMENTAL_HAPTICS", "1", 1));
    LSBHostApplyHapticsEnabled(NO, ^(int error, BOOL restarted) { ++calls; assert(error == EPERM && !restarted); });
    assert(calls == 4 && savedHaptics && hapticsPreferenceWrites == 1);
    unsetenv("LSB_EXPERIMENTAL_HAPTICS");
    LSBHostApplyHapticsEnabled(NO, ^(int error, BOOL restarted) { ++calls; assert(!error && !restarted); });
    assert(calls == 5 && !savedHaptics && hapticsPreferenceWrites == 2 && !jobs && !submittedLaunches);
    resetBroker();
    assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
}

static void testHapticsApplyUsesImmutableSessionDecision(void) {
    for (unsigned enable = 0; enable < 2; ++enable) {
        resetBroker();
        NSString *directory = makeRebootRuntime();
        rememberRebootInputs();
        sessionDeviceType = LSBDefaultGuestDeviceType();
        sessionHapticsEnabled = savedHaptics = !enable;
        sessionAudioEnabled = YES;
        testNativeHaptics = YES;
        NSMutableDictionary *stock = [deviceProfile(sessionDeviceType) mutableCopy];
        NSMutableDictionary *values = [stock[@"capabilities"] mutableCopy];
        [values removeObjectForKey:@"DeviceSupportsClosedLoopHaptics"];
        stock[@"capabilities"] = values.copy;
        NSString *staged = stageDeviceProfile(sessionDeviceType, stock);
        assert(!testInstallGuestDeviceCapabilitiesWithNativeHaptics(runtimeRoot, stock, !enable));
        NSDictionary *oldProfile = activeDeviceProfile();
        terminalLaunchd();
        enableIndigo = YES;
        runningBoardError = 0;
        __block unsigned calls = 0;
        LSBHostApplyHapticsEnabled(enable, ^(int error, BOOL restarted) { ++calls; assert(!error && restarted); });
        assert(sessionDraining && rebootPendingDeaths.count == 1 && !calls && !hapticsPreferenceWrites);
        assert(LSBHostCurrentHapticsEnabled() == !enable && [activeDeviceProfile() isEqual:oldProfile]);
        assert(!unlink(staged.fileSystemRepresentation)); // Preflight owns immutable stock; no post-drain reread.
        savedHaptics = !enable;
        LSBRunningBoardExitHandler exit = ((LSBRunningBoardMonitor *)rebootDeathMonitors[0]).completion;
        exit(SIGKILL, 0);
        assert(calls == 1 && hapticsPreferenceWrites == 1 && savedHaptics == enable && sessionHapticsEnabled == enable);
        assert(!sessionDraining && !pendingHapticsPreference && startingJob.simulatorLaunchd && submittedLaunches == 1);
        assert(hapticsEnvironmentCount(&startingJob.request->plan) == enable);
        assert(sessionAudioEnabled && !audioPreferenceWrites);
        NSDictionary *active = activeDeviceProfile();
        if (enable) {
            id value = active[@"capabilities"][@"DeviceSupportsClosedLoopHaptics"];
            assert(CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && [value boolValue]);
        } else assert([active isEqual:stock]); // Restore absence as well as false; never restore the previously projected file.
        savedHaptics = !enable;
        assert(LSBHostCurrentHapticsEnabled() == enable); // External defaults edits cannot alter an active session.
        resetBroker();
        assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
    }
}

static void testHapticsPublicationFailureKeepsPreference(void) {
    resetBroker();
    NSString *directory = makeRebootRuntime();
    rememberRebootInputs();
    sessionDeviceType = LSBDefaultGuestDeviceType();
    stageDeviceProfile(sessionDeviceType, deviceProfile(sessionDeviceType));
    assert(!LSBInstallGuestDeviceCapabilities(runtimeRoot, deviceProfile(sessionDeviceType)));
    terminalLaunchd();
    enableIndigo = YES;
    runningBoardError = 0;
    __block unsigned calls = 0;
    LSBHostApplyHapticsEnabled(YES, ^(int error, BOOL restarted) { ++calls; assert(error && restarted); });
    assert(!calls && sessionDraining);
    assert(!unlink(activeDeviceProfilePath().fileSystemRepresentation));
    assert(!mkdir(activeDeviceProfilePath().fileSystemRepresentation, 0700));
    LSBRunningBoardExitHandler exit = ((LSBRunningBoardMonitor *)rebootDeathMonitors[0]).completion;
    exit(SIGKILL, 0);
    assert(calls == 1 && !hapticsPreferenceWrites && !savedHaptics && !sessionHapticsEnabled && !submittedLaunches && rebootDrainFailed);
    resetBroker();
    assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
}

static void testAudioColdApplyAndForceOverride(void) {
    resetBroker();
    NSString *directory = makeRebootRuntime();
    jobs = nil;
    savedHaptics = YES;
    LSBGuestDeviceType *device = LSBDefaultGuestDeviceType();
    __block unsigned calls = 0;
    LSBHostApplyAudioOutputEnabled(YES, ^(int error, BOOL restarted) { ++calls; assert(error == ENOENT && !restarted); });
    assert(calls == 1 && !audioPreferenceWrites && !LSBHostCurrentAudioOutputEnabled());
    stageDeviceProfile(device, deviceProfile(device));
    LSBHostApplyAudioOutputEnabled(YES, ^(int error, BOOL restarted) { ++calls; assert(!error && !restarted); });
    assert(calls == 2 && audioPreferenceWrites == 1 && savedAudio && LSBHostCurrentAudioOutputEnabled());
    assert(savedHaptics && !hapticsPreferenceWrites && !jobs && !submittedLaunches);
    LSBHostApplyAudioOutputEnabled(YES, ^(int error, BOOL restarted) { ++calls; assert(!error && !restarted); });
    assert(calls == 3 && audioPreferenceWrites == 1);
    assert(!setenv("LSB_AUDIO_OUTPUT", "1", 1));
    LSBHostApplyAudioOutputEnabled(NO, ^(int error, BOOL restarted) { ++calls; assert(error == EPERM && !restarted); });
    assert(calls == 4 && savedAudio && audioPreferenceWrites == 1);
    unsetenv("LSB_AUDIO_OUTPUT");
    LSBHostApplyAudioOutputEnabled(NO, ^(int error, BOOL restarted) { ++calls; assert(!error && !restarted); });
    assert(calls == 5 && !savedAudio && audioPreferenceWrites == 2 && savedHaptics && !hapticsPreferenceWrites);
    resetBroker();
    assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
}

static void testAudioApplyUsesImmutableSessionDecision(void) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        resetBroker();
        BOOL enable = scenario != 0;
        NSString *directory = makeRebootRuntime();
        rememberRebootInputs();
        sessionDeviceType = LSBDefaultGuestDeviceType();
        sessionAudioEnabled = savedAudio = !enable;
        sessionHapticsEnabled = savedHaptics = testNativeHaptics = YES;
        NSDictionary *stock = deviceProfile(sessionDeviceType);
        NSString *staged = stageDeviceProfile(sessionDeviceType, stock);
        assert(!testInstallGuestDeviceCapabilitiesWithNativeHaptics(runtimeRoot, stock, YES));
        NSDictionary *oldProfile = activeDeviceProfile();
        terminalLaunchd();
        enableIndigo = YES;
        runningBoardError = 0;
        __block unsigned calls = 0;
        LSBHostApplyAudioOutputEnabled(enable, ^(int error, BOOL restarted) {
            ++calls; assert(restarted && (scenario == 2 ? error != 0 : error == 0));
        });
        assert(sessionDraining && rebootPendingDeaths.count == 1 && !calls && !audioPreferenceWrites);
        assert(LSBHostCurrentAudioOutputEnabled() == !enable && [activeDeviceProfile() isEqual:oldProfile]);
        assert(!unlink(staged.fileSystemRepresentation));
        savedAudio = !enable;
        savedHaptics = NO; // The pending decision preserves the committed haptics session, not later preference edits.
        if (scenario == 2) {
            assert(!unlink(activeDeviceProfilePath().fileSystemRepresentation));
            assert(!mkdir(activeDeviceProfilePath().fileSystemRepresentation, 0700));
        }
        LSBRunningBoardExitHandler exit = ((LSBRunningBoardMonitor *)rebootDeathMonitors[0]).completion;
        exit(SIGKILL, 0);
        assert(calls == 1 && !hapticsPreferenceWrites && sessionHapticsEnabled && !pendingAudioPreference);
        if (scenario == 2) {
            assert(!audioPreferenceWrites && savedAudio == !enable && sessionAudioEnabled == !enable && rebootDrainFailed && !submittedLaunches);
        } else {
            assert(audioPreferenceWrites == 1 && savedAudio == enable && sessionAudioEnabled == enable);
            assert(!sessionDraining && startingJob.simulatorLaunchd && submittedLaunches == 1);
            assert(hapticsEnvironmentCount(&startingJob.request->plan) == 1 && [activeDeviceProfile() isEqual:oldProfile]);
            unsigned audioFlags = 0;
            for (unsigned index = 0; index < startingJob.request->plan.envc; ++index)
                if (!strcmp(startingJob.request->plan.strings + startingJob.request->plan.envp[index], "LSB_AUDIO_OUTPUT=1")) ++audioFlags;
            assert(audioFlags == enable);
            savedAudio = !enable;
            assert(LSBHostCurrentAudioOutputEnabled() == enable);
        }
        resetBroker();
        assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
    }
}

static void testDeviceApplyPreflight(void) {
    LSBGuestDeviceType *target = LSBGuestDeviceTypeForIdentifier(@"iphone-se-2");
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        resetBroker();
        NSString *directory = makeRebootRuntime();
        rememberRebootInputs();
        LSBJob *launchd = terminalLaunchd();
        enableIndigo = YES;
        sessionDeviceType = LSBDefaultGuestDeviceType();
        NSDictionary *oldProfile = deviceProfile(sessionDeviceType);
        assert(!LSBInstallGuestDeviceCapabilities(runtimeRoot, oldProfile));
        if (scenario != 2) {
            NSString *staged = stageDeviceProfile(target, deviceProfile(target));
            if (scenario == 3) assert([[@"invalid plist" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:staged atomically:YES]);
        }
        if (scenario == 1) startingJob = launchd;
        if (scenario == 5) jobs = nil; // Native Start gate has not launched any guest yet.
        __block unsigned calls = 0;
        __block int result = -1;
        __block BOOL restarted = YES;
        uint64_t generation = rebootGeneration;
        LSBHostApplyDeviceType(scenario == 0 ? nil : scenario == 4 ? sessionDeviceType : target, ^(int error, BOOL restart) {
            ++calls; result = error; restarted = restart;
        });
        int expected[] = {EINVAL, EBUSY, ENOENT, EINVAL, 0, 0};
        assert(calls == 1 && result == expected[scenario] && !restarted);
        assert(!sessionDraining && rebootGeneration == generation && !pendingDeviceType && !deviceChangeCompletion);
        assert(!submittedLaunches && !deliveredSignals && [activeDeviceProfile() isEqual:oldProfile]);
        assert(devicePreferenceWrites == (scenario == 5 ? 1 : 0));
        if (scenario == 5) assert(testConfiguredDeviceType() == target);
        else assert(findJob(launchd.identifier) == launchd);
        resetBroker();
        assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
    }
}

static void testDeviceApplyWaitsForRootAndPersistsSelection(void) {
    resetBroker();
    NSString *directory = makeRebootRuntime();
    rememberRebootInputs();
    sessionDeviceType = LSBDefaultGuestDeviceType();
    NSDictionary *oldProfile = deviceProfile(sessionDeviceType);
    assert(!LSBInstallGuestDeviceCapabilities(runtimeRoot, oldProfile));
    LSBGuestDeviceType *target = LSBGuestDeviceTypeForIdentifier(@"ipad-a16");
    NSDictionary *newProfile = deviceProfile(target);
    NSString *stagedPath = stageDeviceProfile(target, newProfile);
    LSBJob *launchd = terminalLaunchd();
    managedJob(launchd, 6701);
    enableIndigo = YES;
    runningBoardError = 0;
    assert(LSBHostCanApplyDeviceType());
    __block unsigned calls = 0;
    __block int result = -1;
    __block BOOL restarted = NO;
    LSBHostApplyDeviceType(target, ^(int error, BOOL restart) { ++calls; result = error; restarted = restart; });
    assert(sessionDraining && !jobs.count && rebootPendingDeaths.count == 2 && rebootDeathMonitors.count == 2);
    assert(!calls && !devicePreferenceWrites && !submittedLaunches && deliveredSignals == 2);
    assert(!LSBHostCanApplyDeviceType() && LSBHostCurrentDeviceType() == LSBDefaultGuestDeviceType());
    assert([activeDeviceProfile() isEqual:oldProfile]);
    __block unsigned concurrentCalls = 0;
    LSBHostApplyDeviceType(LSBGuestDeviceTypeForIdentifier(@"iphone-11"), ^(int error, BOOL restart) {
        ++concurrentCalls; assert(error == EBUSY && !restart);
    });
    assert(concurrentCalls == 1 && !calls && pendingDeviceType == target);
    LSBRunningBoardExitHandler rootExit = ((LSBRunningBoardMonitor *)rebootDeathMonitors[0]).completion;
    LSBRunningBoardExitHandler childExit = ((LSBRunningBoardMonitor *)rebootDeathMonitors[1]).completion;
    childExit(SIGKILL, 0);
    assert(rebootPendingDeaths.count == 1 && !calls && !submittedLaunches);
    assert([rebootPendingDeaths containsObject:@(launchd.identifier)]);
    assert([activeDeviceProfile() isEqual:oldProfile]);
    assert(!unlink(stagedPath.fileSystemRepresentation)); // Installation uses the validated snapshot, never a later source reread.
    rootExit(SIGKILL, 0);
    assert(calls == 1 && !result && restarted && !sessionDraining && !rebootDrainFailed);
    assert(!pendingDeviceType && !pendingDeviceCapabilities && !deviceChangeCompletion);
    assert(submittedLaunches == 1 && jobs.count == 1 && startingJob.simulatorLaunchd);
    assert([activeDeviceProfile() isEqual:newProfile]);
    assert(devicePreferenceWrites == 1 && testConfiguredDeviceType() == target && LSBHostCurrentDeviceType() == target);
    sessionDeviceType = nil;
    assert(LSBHostCurrentDeviceType() == target); // Selection survives dropping the in-memory session selection.
    sessionDeviceType = target;
    for (NSString *scope in @[@"Self", @"Child"]) {
        assert([rebootConfiguration()[scope][@"SIMULATOR_PRODUCT_CLASS"] isEqual:target.productClass]);
        assert([rebootConfiguration()[scope][@"SIMULATOR_MODEL_IDENTIFIER"] isEqual:target.modelIdentifier]);
    }
    rootExit(SIGKILL, 0); childExit(SIGKILL, 0);
    assert(calls == 1 && submittedLaunches == 1);
    // The next graphical launchd receives a new framebuffer with the selected immutable geometry.
    enableIndigo = YES;
    LSBJob *geometryJob = managedJob(nil, getpid());
    geometryJob.simulatorLaunchd = YES;
    assert(!prepareLaunchdHandshake(geometryJob));
    LSBFramebufferGeometry geometry = geometryJob.framebuffer.geometry;
    assert(geometry.width == target.width && geometry.height == target.height && geometry.scale == target.scale && geometry.dotPitch == target.pitch);
    resetBroker();
    assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
}

static void testDeviceApplyFailureDoesNotOverlapSessions(void) {
    for (unsigned installFailure = 0; installFailure < 2; ++installFailure) {
        resetBroker();
        NSString *directory = makeRebootRuntime();
        rememberRebootInputs();
        sessionDeviceType = LSBDefaultGuestDeviceType();
        NSDictionary *oldProfile = deviceProfile(sessionDeviceType);
        assert(!LSBInstallGuestDeviceCapabilities(runtimeRoot, oldProfile));
        LSBGuestDeviceType *target = LSBGuestDeviceTypeForIdentifier(@"iphone-8-plus");
        stageDeviceProfile(target, deviceProfile(target));
        LSBJob *launchd = terminalLaunchd();
        managedJob(launchd, 6702);
        enableIndigo = YES;
        runningBoardError = 0;
        __block unsigned calls = 0;
        __block int result = 0;
        LSBHostApplyDeviceType(target, ^(int error, BOOL restarted) { (void)restarted; ++calls; result = error; });
        assert(!calls && sessionDraining && rebootPendingDeaths.count == 2);
        LSBRunningBoardExitHandler rootExit = ((LSBRunningBoardMonitor *)rebootDeathMonitors[0]).completion;
        LSBRunningBoardExitHandler childExit = ((LSBRunningBoardMonitor *)rebootDeathMonitors[1]).completion;
        if (installFailure) {
            assert(!unlink(activeDeviceProfilePath().fileSystemRepresentation));
            assert(!mkdir(activeDeviceProfilePath().fileSystemRepresentation, 0700));
        }
        rootExit(SIGKILL, installFailure ? 0 : EIO);
        assert(!calls && !submittedLaunches && rebootPendingDeaths.count == 1);
        childExit(SIGKILL, 0);
        assert(calls == 1 && result && sessionDraining && rebootDrainFailed && !LSBHostCanApplyDeviceType());
        assert(!submittedLaunches && !jobs.count && !devicePreferenceWrites && !pendingDeviceType && !deviceChangeCompletion);
        assert(LSBHostCurrentDeviceType() == LSBDefaultGuestDeviceType());
        if (!installFailure) assert([activeDeviceProfile() isEqual:oldProfile]);
        completeUserspaceReboot(rebootGeneration);
        assert(calls == 1 && !submittedLaunches); // An error cannot replay completion or begin a second generation.
        resetBroker();
        assert([NSFileManager.defaultManager removeItemAtPath:directory error:NULL]);
    }
}

int main(void) {
    @autoreleasepool {
        alarm(10);
        testIdentityAndSuccessfulHandoff();
        testReadySendFailure();
        testChildReadyFailure(ENOEXEC);
        testChildReadyFailure(EBADF);
        testUnboundFailureStopsQueue();
        testSubmissionFailureAndSpawnAuthentication();
        testInitialGuestArguments();
        testLaunchdTerminalLineage();
        testNativeProbeRoleIsHostOnly();
        testProductionStatusIsHostOnly();
        testExitStatusesAndRepeatedReaping();
        testExecReplacement();
        testExecFailurePreservesCaller();
        testGlobalJobCapacity();
        testExecDoesNotConsumeAnotherChildSlot();
        testFullLaunchdBatchFitsExecOverlap();
        testAuditOwnerAuthenticationAndIdentity();
        testAuditIdentityHistoryNeverEvicts();
        testAuditResolutionSeesCommittedExecOnly();
        testGraphicsRegistrationGatesExecOnly();
        testGraphicsOwnerExecIsRejectedWithoutChangingAuthorization();
        testDirectBackboardWaitsForServices();
        testDisplayEndpointRegistrationCompletesAll();
        testDisplayRegistrationSharesStartupBudget();
        testNativeConfigdBridgeRegistration();
        testNativeHapticsBridgeRegistration();
        testHapticRegistrationKeepsOwnedRight();
        testHapticRelayAuthentication();
        testAudioOutputAuthenticationAndOwnership();
        testSessionEnvironmentIsHostSelected();
        testHapticsColdApplyRequiresStockProfile();
        testHapticsApplyUsesImmutableSessionDecision();
        testHapticsPublicationFailureKeepsPreference();
        testAudioColdApplyAndForceOverride();
        testAudioApplyUsesImmutableSessionDecision();
        testDirectConfigdWaitsForSelectedBridge();
        testNativeMetalReadyGatesLaunchd();
        testNativeMetalOwnerLifecycle();
        testNativeProbeAssertionLoss();
        testMetalEndpointRegistration();
        testMetalRegistrationCannotClearHelperDeath();
        testHostSpringBoardMetalSelection();
        testIconLaunchUsesGraphicalLaunchd();
        testPublicMetalReadyFailureRevokesSession();
        testPublicMetalLifetimeAndClientIndependence();
        testSecondMetalHelperFailureIsIndependent();
        testSeparateMetalOwners();
        testSpringBoardRegistrationIsolation();
        testSpringBoardMetalRestart();
        testGuestEntitlementsSnapshotsAndAuthentication();
        testGuestEntitlementsPathBoundaries();
        testPeerIdentityKernelValidation();
        testPeerIdentitySnapshotsAndAuthentication();
        testProcessListEntitlementsAndAuthentication();
        testTaskCaptureStartupAndCleanup();
        testTaskEntitlementSnapshot();
        testProcessTaskAuthenticationAndOwnership();
        testCooperativeDebuggerAuthentication();
        testLaunchdChildIdentityAndSignals();
        testLaunchdNativeChildPIDResolution();
        testPeerExecutablePathContainment();
        testPeerIdentityCheckInFailures();
        testMonitorFailureAndEarlyExit();
        testAuditedGenerationDisappearsBeforeBootstrap();
        testRunningBoardFallback();
        testParentDeathDetachesOrphans();
        testLaunchdAdoptsRunningAndZombieChildren();
        testAdoptionCapacityPreservesExitAndExec();
        testLaunchdDeathAndCyclesDoNotAdopt();
        testKillAuthenticationAndExit();
        testKillFallbackErrors();
        testLaunchdModeRequiresInitialPathAndMarker();
        testLaunchdHandshakeDispatchAndRights();
        testLaunchdCancellationAndFailedBootstrap();
        testRebootInputSnapshot();
        testRebootAuthentication();
        testRebootRequiresCleanLaunchdExit();
        testRebootWaitsForOldGenerationAndRegeneratesConfiguration();
        testRebootDrainFailureGuards();
        testRebootPreservesQueuedKqueueCallbacks();
        testDeviceApplyPreflight();
        testDeviceApplyWaitsForRootAndPersistsSelection();
        testDeviceApplyFailureDoesNotOverlapSessions();
        resetBroker();
        puts("Broker: authentication, FD ownership, exit/kill, orphan cleanup, graphics registration/exec gating, Mach handoff, and source cancellation passed");
    }
    return 0;
}
