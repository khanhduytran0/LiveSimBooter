#include "../ProcessSupport/RuntimeLayout.h"
#import <Foundation/Foundation.h>
#include "Broker.h"
#include "ExtensionLauncher.h"
#include "ChildMonitor.h"
#include "RunningBoardMonitor.h"
#include "SessionControlQueue.h"
#import "DeviceConfiguration.h"
#import "RuntimeStorage.h"
#include "NativeProductionStatus.h"
#include "StoreAppGroups.h"
#include "ChildStatus.h"
#include "SpawnTransport.h"
#include "PrivateXPC.h"
#include "LaunchdHandshake.h"
#include "LaunchdProbe.h"
#include "LaunchdConfiguration.h"
#include "LaunchdEndpoint.h"
#include "NativeMetalControl.h"
#include "GuestEntitlements.h"
#include "GuestEntitlementsConversion.h"
#include "PeerDebugger.h"
#import "IndigoHIDServer.h"
#import "FramebufferServer.h"
#import "IOSurfaceServer.h"
#import "TerminalSession.h"
#include "FakePTY.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

enum { JobQueued, JobStarting, JobDispatched, JobRunning };
@interface LSBJob : NSObject
@property uint64_t identifier;
@property int phase;
@property uint32_t executionRole;
@property pid_t pid;
@property pid_t guestPID;
@property uint64_t replacesIdentifier, replacementIdentifier;
@property BOOL executed;
@property uint32_t pidVersion;
@property pid_t extensionPID;
@property LSBReceivedMessage *request;
@property dispatch_source_t exitMonitor;
@property (nonatomic, strong) LSBRunningBoardMonitor *runningBoardMonitor;
@property audit_token_t audit;
@property uint64_t parentIdentifier;
@property LSBChildTicket parentTicket;
@property BOOL parentStatusPublished, parentStatusPending, retiredForExec;
@property LSBChildStatus *childStatus;
@property int statusFD, wakeReadFD, wakeWriteFD;
@property BOOL terminalSession;
@property BOOL simulatorLaunchd;
@property BOOL rebootRequested;
@property LSBLaunchdHandshake *launchdHandshake;
@property dispatch_source_t launchdSource;
@property (nonatomic, strong) LSBIndigoHIDServer *indigo;
@property (nonatomic, strong) LSBFramebufferServer *framebuffer;
@property (nonatomic, strong) LSBIOSurfaceServer *iosurface;
@property BOOL servicesPending;
@property int servicesError;
@property BOOL nativeConfigdSelected;
@property uint64_t indigoAuthorizedIdentifier;
@property uint64_t framebufferAuthorizedIdentifier;
@property uint64_t nativeMetalServiceIdentifier, nativeMetalAuthorizedIdentifier;
@property uint64_t springBoardMetalServiceIdentifier, publicMetalServiceIdentifier;
@property BOOL springBoardMetalSelected;
@property BOOL springBoardMetalPending;
@property BOOL springBoardMetalRestartAllowed;
@property int springBoardMetalError;
@property LSBNativeMetalControl *nativeMetalControl;
@property mach_port_t nativeServicePort;
@property (nonatomic, copy) NSData *guestEntitlements;
@property int guestEntitlementsError;
@property (nonatomic, copy) NSData *guestXMLEntitlements;
@property int guestXMLEntitlementsError;
@property BOOL guestProcessInfoAllowed;
@property BOOL guestTaskForPIDAllowed;
@property BOOL guestDragUIAllowed, debuggerQueryPending, processDebuggerReady;
@property mach_port_t processDebuggerPort;
@property mach_port_t taskPort;
@property LSBTaskPortCaptureOffer taskCapture;
@property dispatch_source_t taskCaptureSource;
@property int taskCaptureError;
@property (nonatomic, copy) NSData *guestCodeDirectoryHash;
@property int guestCodeDirectoryHashError;
@property LSBPeerProcessIdentity peerIdentity;
@property int peerIdentityError;
@property BOOL finishing;
@end
@implementation LSBJob
- (instancetype)init {
    if ((self = [super init])) {
        _statusFD = _wakeReadFD = _wakeWriteFD = -1;
        _guestEntitlementsError = ENOENT;
        _guestXMLEntitlementsError = ENOENT;
        _guestCodeDirectoryHashError = ENOENT;
        _peerIdentityError = ENOENT;
        _taskCaptureError = ENOENT;
    }
    return self;
}
- (void)dealloc {
    LSBPeerProcessIdentityDispose(&_peerIdentity);
    if (MACH_PORT_VALID(_processDebuggerPort)) mach_port_deallocate(mach_task_self(), _processDebuggerPort);
    if (_taskCaptureSource) dispatch_source_cancel(_taskCaptureSource);
    if (MACH_PORT_VALID(_taskPort)) mach_port_deallocate(mach_task_self(), _taskPort);
    if (_nativeMetalControl) LSBNativeMetalControlUnmap(_nativeMetalControl);
    if (MACH_PORT_VALID(_nativeServicePort)) mach_port_deallocate(mach_task_self(), _nativeServicePort);
    if (_launchdHandshake) { LSBLaunchdHandshakeDispose(_launchdHandshake); free(_launchdHandshake); }
    if (_childStatus) munmap(_childStatus, LSB_CHILD_STATUS_MAPPING_SIZE);
    if (_statusFD >= 0) close(_statusFD);
    if (_wakeReadFD >= 0) close(_wakeReadFD);
    if (_wakeWriteFD >= 0) close(_wakeWriteFD);
}
@end

@interface LSBPendingAdoption : NSObject
@property uint64_t childIdentifier, parentIdentifier;
@property LSBChildObservation observation;
@end
@implementation LSBPendingAdoption
@end

static NSMutableArray<LSBJob *> *jobs;
static NSMutableArray<LSBPendingAdoption *> *pendingAdoptions;
static dispatch_source_t adoptionRetrySource;
static LSBJob *startingJob;
static NSString *runtimeRoot;
static uint64_t nextIdentifier = 1;
static BOOL launchLaneFailed;
static unsigned finishingDepth;
static LSBTerminalSession *hostTerminal;
static BOOL initialTerminalPending;
static BOOL initialNativeConfigdPending;
static BOOL enableIndigo;
static BOOL enableSpringBoardMetal;
static NSArray<NSData *> *rebootArguments, *rebootEnvironment;
static BOOL sessionDraining, rebootDrainFailed;
static uint64_t rebootGeneration;
static NSMutableSet<NSNumber *> *rebootPendingDeaths;
static NSMutableArray<LSBRunningBoardMonitor *> *rebootDeathMonitors;
static NSMutableArray<dispatch_source_t> *rebootExitSources;
static LSBGuestDeviceType *sessionDeviceType, *pendingDeviceType;
static NSDictionary *pendingDeviceCapabilities;
static void (^deviceChangeCompletion)(int, BOOL);
// Allow every launchd child to overlap its exec replacement, plus launchd and the backboardd, SpringBoard and public Metal helpers.
enum { BrokerJobCapacity = 2 * LSB_CHILD_STATUS_CAPACITY + 4, DisplayRegistrationTimeoutSeconds = 30,
       RetiredIdentityCapacity = 256, AuditResolutionLogLimit = 32 };
typedef struct { audit_token_t audit; pid_t guestPID; } LSBRetiredIdentity;
static LSBRetiredIdentity retiredIdentities[RetiredIdentityCapacity];
static unsigned retiredIdentityCount, auditResolutionLogs;
static unsigned peerIdentityLogs;
static unsigned cooperativeDebuggerLogs;
static unsigned guestEntitlementErrorLogs;
static unsigned guestProxySigningErrorLogs;
static BOOL retiredIdentitiesFull;
NSString *const LSBHostIndigoDidChangeNotification = @"LSBHostIndigoDidChange";
NSString *const LSBHostFramebufferDidChangeNotification = @"LSBHostFramebufferDidChange";
NSString *const LSBHostTerminalDidChangeNotification = @"LSBHostTerminalDidChange";
NSString *const LSBHostSessionStateDidChangeNotification = @"LSBHostSessionStateDidChange";
static BOOL sessionStateNotificationScheduled;
static void startNextJob(void);
static void beginUserspaceReboot(LSBJob *exitedLaunchd);
static void completeUserspaceReboot(uint64_t generation);
static int prepareRuntimeRoot(void);
static int signalJob(LSBJob *child, int signalNumber);
static int signalTerminal(pid_t foreground, int signalNumber);
static BOOL launchdOwnsChildIdentity(LSBJob *job);
static LSBJob *enqueueNativeMetalHelper(BOOL publicAccess, const char *name, int *result);
static void registerReplacementSpringBoardMetal(LSBJob *active, LSBJob *helper);
static void finishJob(LSBJob *job, int error);

LSBTerminalSession *LSBHostTerminal(void) { return hostTerminal; }
LSBGuestDeviceType *LSBHostCurrentDeviceType(void) { return jobs ? sessionDeviceType ?: LSBConfiguredGuestDeviceType() : LSBConfiguredGuestDeviceType(); }
BOOL LSBHostCanApplyDeviceType(void) {
    if (sessionDraining || deviceChangeCompletion || launchLaneFailed || startingJob) return NO;
    if (!jobs) return YES;
    for (LSBJob *job in jobs) if (job.simulatorLaunchd && !job.parentIdentifier && job.phase == JobRunning) return enableIndigo;
    return NO;
}
static void finishDeviceChange(int error) {
    void (^completion)(int, BOOL) = deviceChangeCompletion;
    deviceChangeCompletion = nil;
    pendingDeviceType = nil;
    pendingDeviceCapabilities = nil;
    if (completion) completion(error, YES);
}
void LSBHostApplyDeviceType(LSBGuestDeviceType *type, void (^completion)(int, BOOL)) {
    if (!NSThread.isMainThread) { if (completion) completion(EPERM, NO); return; }
    if (!type || type != LSBGuestDeviceTypeForIdentifier(type.identifier)) { if (completion) completion(EINVAL, NO); return; }
    if (!LSBHostCanApplyDeviceType()) { if (completion) completion(EBUSY, NO); return; }
    if (type == LSBHostCurrentDeviceType()) { if (completion) completion(0, NO); return; }
    int error = runtimeRoot ? 0 : prepareRuntimeRoot();
    NSDictionary *capabilities = nil;
    if (!error) error = LSBCopyGuestDeviceCapabilities(runtimeRoot, type, &capabilities);
    if (error) { if (completion) completion(error, NO); return; }
    if (!jobs) {
        // The Start gate has not opened. Persist the validated choice; installation happens before the first guest.
        LSBSaveConfiguredGuestDeviceType(type);
        if (completion) completion(0, NO);
        return;
    }
    pendingDeviceType = type;
    pendingDeviceCapabilities = capabilities;
    deviceChangeCompletion = [completion copy];
    fprintf(stderr, "[broker] Applying device %s through simulator session restart\n", type.identifier.UTF8String);
    // Host-requested restart uses the same drain as guest reboot. Unlike launchctl's graceful shutdown, every tracked
    // guest (including launchd) is terminated here; no profile or framebuffer is replaced until death is observed.
    beginUserspaceReboot(nil);
}
int LSBHostUsesSimulatorDisplay(void) { return enableIndigo; }
LSBIndigoHIDServer *LSBHostIndigoHIDServer(void) {
    for (LSBJob *job in jobs) if (job.simulatorLaunchd && job.indigo) return job.indigo;
    return nil;
}
LSBFramebufferServer *LSBHostFramebufferServer(void) {
    for (LSBJob *job in jobs) if (job.simulatorLaunchd && job.framebuffer) return job.framebuffer;
    return nil;
}

static LSBJob *findJob(uint64_t identifier) {
    for (LSBJob *job in jobs) if (job.identifier == identifier) return job;
    return nil;
}

static BOOL isBackboardProgram(const LSBReceivedMessage *request) {
    const char *program = request->plan.strings + request->plan.path;
    if (!strcmp(program, "/usr/libexec/backboardd")) return YES;
    NSString *path = [runtimeRoot stringByAppendingPathComponent:@"usr/libexec/backboardd"];
    return !strcmp(program, path.fileSystemRepresentation);
}

static BOOL isSpringBoardProgram(const LSBReceivedMessage *request) {
    if (!request) return NO;
    const char *program = request->plan.strings + request->plan.path;
    static const char path[] = "/System/Library/CoreServices/SpringBoard.app/SpringBoard";
    if (!strcmp(program, path)) return YES;
    NSString *physical = [runtimeRoot stringByAppendingString:@"/System/Library/CoreServices/SpringBoard.app/SpringBoard"];
    return !strcmp(program, physical.fileSystemRepresentation);
}

static BOOL isConfigdProgram(const LSBReceivedMessage *request) {
    const char *program = request->plan.strings + request->plan.path;
    if (!strcmp(program, "/usr/libexec/configd_sim")) return YES;
    NSString *physical = [runtimeRoot stringByAppendingPathComponent:@"usr/libexec/configd_sim"];
    return !strcmp(program, physical.fileSystemRepresentation);
}

static BOOL containedPath(const char *path, const char *root) {
    size_t length = strlen(root);
    return length > 1 && !strncmp(path, root, length) && path[length] == '/';
}

static int openGuestExecutable(const LSBSpawnPlan *plan, int *descriptor, char guestPath[LSB_PATH_BYTES]) {
    *descriptor = -1;
    if (guestPath) guestPath[0] = '\0';
    int error = LSBValidateSpawnPlan(plan);
    if (error) return error;
    const char *path = plan->strings + plan->path;
    // Relative executable resolution depends on ordered child-only CHDIR/FCHDIR actions. Never guess an image in the host cwd.
    if (path[0] != '/') return ENOTSUP;
    char root[PATH_MAX], candidate[PATH_MAX], canonical[PATH_MAX], descriptorPath[PATH_MAX], descriptorCanonical[PATH_MAX];
    int rootError = realpath(runtimeRoot.fileSystemRepresentation, root) ? 0 : errno;
    const char *rootSeparator = !rootError && containedPath(path, root) ? path + strlen(root) : NULL;
    if (!rootError && !rootSeparator && !strncmp(root, "/private/var/", 13) && containedPath(path, root + 8))
        rootSeparator = path + strlen(root + 8);
    // Reject lexical traversal without Foundation's platform-dependent /private path standardization.
    for (const char *part = path + 1; *part;) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        // Simulator launchd joins its root plus "/" with the absolute xpcproxy path. Permit exactly that duplicate boundary,
        // then still resolve and validate the opened descriptor below; empty components elsewhere remain invalid.
        if (!length && rootSeparator && part == rootSeparator + 1 && part[1] && part[1] != '/') { ++part; continue; }
        if (!length || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.')) return EINVAL;
        if (end && !end[1]) return EINVAL;
        part += length + (end != NULL);
    }
    if (rootError) return rootError;
    BOOL physical = realpath(path, canonical) && containedPath(canonical, root);
    if (!physical) {
        // An explicit physical path cannot escape through a symlink and be reinterpreted as a guest path.
        if (containedPath(path, root)) return realpath(path, canonical) ? EACCES : errno;
        int length = snprintf(candidate, sizeof(candidate), "%s%s", root, path);
        if (length < 0 || (size_t)length >= sizeof(candidate)) return ENAMETOOLONG;
        if (!realpath(candidate, canonical)) return errno;
        if (!containedPath(canonical, root)) return EACCES;
    }
    int fd = open(canonical, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno;
    struct stat info;
    if (fstat(fd, &info)) error = errno;
    else if (!S_ISREG(info.st_mode)) error = EINVAL;
    else if (fcntl(fd, F_GETPATH, descriptorPath) || !realpath(descriptorPath, descriptorCanonical)) error = errno;
    else if (!containedPath(descriptorCanonical, root)) error = EACCES;
    else if (strcmp(canonical, descriptorCanonical)) error = EAGAIN;
    if (!error && guestPath) {
        const char *guest = descriptorCanonical + strlen(root);
        size_t size = strlen(guest) + 1;
        if (size > LSB_PATH_BYTES) error = ENAMETOOLONG;
        else memcpy(guestPath, guest, size);
    }
    if (error) close(fd);
    else *descriptor = fd;
    return error;
}

static int copyGuestEntitlements(const LSBSpawnPlan *plan, NSData **snapshot, NSData **hashSnapshot, int *hashError) {
    *snapshot = nil;
    if (hashSnapshot) *hashSnapshot = nil;
    int fd = -1, error = openGuestExecutable(plan, &fd, NULL);
    if (hashError) *hashError = error;
    if (error) return error;
    struct stat before, after;
    if (fstat(fd, &before)) {
        error = errno;
        if (hashError) *hashError = error;
        close(fd);
        return error;
    }
    if (hashSnapshot && hashError) {
        unsigned char hash[LSB_GUEST_CDHASH_BYTES];
        *hashError = LSBReadMachOCodeDirectoryHash(fd, hash);
        if (!*hashError) {
            *hashSnapshot = [NSData dataWithBytes:hash length:sizeof(hash)];
            if (!*hashSnapshot) *hashError = ENOMEM;
        }
    }
    unsigned char *bytes = NULL;
    size_t size = 0;
    error = LSBReadMachODEREntitlements(fd, &bytes, &size);
    // Individual readers validate their own read window. Also keep the hash and DER from one unchanged file generation.
    int consistencyError = fstat(fd, &after) ? errno : 0;
    if (!consistencyError && (before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec || before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
        before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec || before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec)) consistencyError = EAGAIN;
    close(fd);
    if (consistencyError) {
        free(bytes);
        if (hashSnapshot) *hashSnapshot = nil;
        if (hashError) *hashError = consistencyError;
        return consistencyError;
    }
    if (!error) {
        *snapshot = [[NSData alloc] initWithBytesNoCopy:bytes length:size freeWhenDone:YES];
        if (!*snapshot) { free(bytes); error = ENOMEM; }
    }
    return error;
}

static void logGuestEntitlementError(LSBJob *job) {
    if (!job.guestEntitlementsError && !job.guestXMLEntitlementsError && !job.guestCodeDirectoryHashError) return;
    const char *path = job.request && !LSBValidateSpawnPlan(&job.request->plan) ? job.request->plan.strings + job.request->plan.path : "<invalid>";
    const char *leaf = strrchr(path, '/');
    BOOL proxy = leaf && !strcmp(leaf + 1, "xpcproxy_sim"); // Diagnostic quota only; this does not authorize or resolve any executable.
    unsigned *count = proxy ? &guestProxySigningErrorLogs : &guestEntitlementErrorLogs, limit = proxy ? 2 : 12;
    if (*count < limit) {
        fprintf(stderr, "[broker] signing snapshot request=%llu pid=%d DER-error=%d XML-error=%d CDHash-error=%d program=%.240s\n",
                job.identifier, job.pid, job.guestEntitlementsError, job.guestXMLEntitlementsError, job.guestCodeDirectoryHashError, path);
        ++*count;
    } else if (*count == limit) {
        fprintf(stderr, "[broker] Further %s signing snapshot errors suppressed\n", proxy ? "xpcproxy" : "guest");
        ++*count;
    }
}

static NSDictionary *guestEntitlementsDictionary(NSData *blob) {
    if (!LSBGuestEntitlementsHasValidBlobHeaderForOperation(blob.bytes, blob.length, 7)) return nil;
    NSData *plist = [blob subdataWithRange:NSMakeRange(8, blob.length - 8)];
    id object = [NSPropertyListSerialization propertyListWithData:plist options:NSPropertyListImmutable format:NULL error:NULL];
    return [object isKindOfClass:NSDictionary.class] ? object : nil;
}

// Only an actual plist Boolean true authorizes inspection; integers/strings and absent or malformed metadata fail closed.
static BOOL booleanGuestEntitlement(NSDictionary *entitlements, NSString *key) {
    id value = entitlements[key];
    return value && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() && CFBooleanGetValue((__bridge CFBooleanRef)value);
}

static void snapshotGuestEntitlements(LSBJob *job) {
    job.guestProcessInfoAllowed = NO;
    job.guestTaskForPIDAllowed = NO;
    job.guestDragUIAllowed = NO;
    job.guestEntitlements = nil;
    job.guestXMLEntitlements = nil;
    job.guestCodeDirectoryHash = nil;
    if (job.executionRole != LSB_EXECUTION_SIMULATOR) {
        job.guestEntitlementsError = job.guestXMLEntitlementsError = job.guestCodeDirectoryHashError = EACCES;
        return;
    }
    NSData *snapshot = nil, *hashSnapshot = nil;
    int hashError = ENOENT;
    job.guestEntitlementsError = job.request ? copyGuestEntitlements(&job.request->plan, &snapshot, &hashSnapshot, &hashError) : ENOENT;
    job.guestCodeDirectoryHash = hashSnapshot;
    job.guestCodeDirectoryHashError = hashError;
    job.guestXMLEntitlementsError = job.guestEntitlementsError;
    if (job.guestEntitlementsError) { logGuestEntitlementError(job); return; }
    job.guestEntitlements = snapshot;
    void *xml = NULL;
    size_t size = 0;
    job.guestXMLEntitlementsError = LSBCopyXMLFromDEREntitlements(snapshot.bytes, snapshot.length, &xml, &size);
    if (!job.guestXMLEntitlementsError) {
        job.guestXMLEntitlements = [[NSData alloc] initWithBytesNoCopy:xml length:size freeWhenDone:YES];
        if (!job.guestXMLEntitlements) { free(xml); job.guestXMLEntitlementsError = ENOMEM; }
        else {
            NSDictionary *entitlements = guestEntitlementsDictionary(job.guestXMLEntitlements);
            job.guestProcessInfoAllowed = booleanGuestEntitlement(entitlements, @"proc_info-allow");
            job.guestTaskForPIDAllowed = booleanGuestEntitlement(entitlements, @"task_for_pid-allow");
            job.guestDragUIAllowed = booleanGuestEntitlement(entitlements, @"com.apple.druid");
        }
    }
    logGuestEntitlementError(job);
}

static int validatePeerTaskIdentity(const LSBPeerProcessIdentity *identity, const audit_token_t *expected) {
    if (!MACH_PORT_VALID(identity->taskName) || memcmp(&identity->audit, expected, sizeof(*expected))) return EACCES;
    uint32_t pid = LSBPeerProcessIdentityPID(identity);
    if (!pid || pid > INT_MAX || pid != expected->val[5]) return EACCES;
    mach_port_type_t type = 0;
    if (mach_port_type(mach_task_self(), identity->taskName, &type) != KERN_SUCCESS || !(type & MACH_PORT_TYPE_SEND) ||
        (type & (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND_ONCE | MACH_PORT_TYPE_DEAD_NAME | MACH_PORT_TYPE_PORT_SET))) return EACCES;
    audit_token_t audit = {{0}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    if (task_info(identity->taskName, TASK_AUDIT_TOKEN, (task_info_t)&audit, &count) != KERN_SUCCESS ||
        count != TASK_AUDIT_TOKEN_COUNT || memcmp(&audit, expected, sizeof(audit))) return EACCES;
    // TASK_AUDIT_TOKEN proves identity, not task-port flavor. The trusted native CHECKIN path obtains this right with task_name_for_pid(self).
    return 0;
}

static int copyGuestExecutablePath(const LSBSpawnPlan *plan, char output[LSB_PATH_BYTES]) {
    int fd = -1, error = openGuestExecutable(plan, &fd, output);
    if (fd >= 0) close(fd);
    return error;
}

static int snapshotGuestProcessIdentity(LSBJob *job, LSBReceivedMessage *message) {
    job.peerIdentityError = EACCES;
    if (job.executionRole != LSB_EXECUTION_SIMULATOR) return 0;
    int error = validatePeerTaskIdentity(&message->peerIdentity, &message->audit);
    if (error) return error;
    LSBPeerProcessIdentity snapshot = message->peerIdentity;
    memset(snapshot.executablePath, 0, sizeof(snapshot.executablePath));
    job.peerIdentityError = copyGuestExecutablePath(&job.request->plan, snapshot.executablePath);
    if (job.peerIdentityError) return 0;
    job.peerIdentity = snapshot;
    job.peerIdentityError = 0;
    message->peerIdentity.taskName = MACH_PORT_NULL; // Transfer the imported send reference; message disposal keeps no extra reference.
    return 0;
}

static void stopGuestTaskCapture(LSBJob *job) {
    if (job.taskCaptureSource) dispatch_source_cancel(job.taskCaptureSource);
    job.taskCaptureSource = nil;
    job.taskCapture = (LSBTaskPortCaptureOffer){0};
}

static void releaseGuestProcessIdentity(LSBJob *job) {
    LSBPeerProcessIdentity snapshot = job.peerIdentity;
    job.peerIdentity = (LSBPeerProcessIdentity){0};
    job.peerIdentityError = ESRCH;
    LSBPeerProcessIdentityDispose(&snapshot);
    if (MACH_PORT_VALID(job.processDebuggerPort)) mach_port_deallocate(mach_task_self(), job.processDebuggerPort);
    job.processDebuggerPort = MACH_PORT_NULL;
    job.processDebuggerReady = NO;
    stopGuestTaskCapture(job);
    if (MACH_PORT_VALID(job.taskPort)) mach_port_deallocate(mach_task_self(), job.taskPort);
    job.taskPort = MACH_PORT_NULL;
    job.taskCaptureError = ESRCH;
}

static void receiveGuestTaskPort(uint64_t identifier) {
    LSBJob *job = findJob(identifier);
    if (!job || job.phase != JobDispatched || job.finishing || job.executionRole != LSB_EXECUTION_SIMULATOR) return;
    LSBTaskPortCaptureOffer offer = job.taskCapture;
    audit_token_t expected = job.audit;
    mach_port_t task = MACH_PORT_NULL;
    int error = LSBTaskPortCaptureReceive(&offer, &expected, &task);
    if (error == EAGAIN) return;
    if (!error && MACH_PORT_VALID(job.taskPort)) error = EALREADY;
    if (error) {
        if (MACH_PORT_VALID(task)) mach_port_deallocate(mach_task_self(), task);
        job.taskCaptureError = error;
        fprintf(stderr, "[broker] task capture failed request=%llu native=%d error=%d\n", identifier, job.pid, error);
        finishJob(job, error);
        return;
    }
    job.taskPort = task;
    job.taskCaptureError = 0;
    // Keep the endpoint alive until READY confirms that the native child has restored its thread exception handlers.
}

static int prepareGuestTaskCapture(LSBJob *job) {
    if (job.executionRole != LSB_EXECUTION_SIMULATOR) return 0;
    LSBTaskPortCaptureOffer offer = {0};
    int error = LSBTaskPortCaptureCreate(&offer);
    if (error) return error;
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, offer.port, 0, dispatch_get_main_queue());
    if (!source) {
        mach_port_mod_refs(mach_task_self(), offer.port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), offer.port);
        return ENOMEM;
    }
    uint64_t identifier = job.identifier;
    dispatch_source_set_cancel_handler(source, ^{
        mach_port_mod_refs(mach_task_self(), offer.port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), offer.port);
    });
    dispatch_source_set_event_handler(source, ^{ receiveGuestTaskPort(identifier); });
    job.taskCapture = offer;
    job.taskCaptureSource = source;
    job.taskCaptureError = ENOENT;
    dispatch_resume(source);
    return 0;
}

static void logPeerIdentityRequest(pid_t sender, pid_t target, uint32_t version, int error) {
    if (peerIdentityLogs < 12) {
        fprintf(stderr, "[broker] peer identity sender=%d target=%d version=%u error=%d\n", sender, target, version, error);
        ++peerIdentityLogs;
    } else if (peerIdentityLogs == 12) {
        fputs("[broker] Further peer identity diagnostics suppressed\n", stderr);
        ++peerIdentityLogs;
    }
}

static int readControlledPlist(NSString *path, NSString *directory, NSDictionary **output) {
    *output = nil;
    struct stat info;
    if (lstat(path.fileSystemRepresentation, &info)) return errno;
    if (!S_ISREG(info.st_mode)) return S_ISLNK(info.st_mode) ? ELOOP : EINVAL;
    if (info.st_size <= 0 || info.st_size > 1024 * 1024) return EFBIG;
    char canonical[PATH_MAX], parent[PATH_MAX];
    if (!realpath(path.fileSystemRepresentation, canonical) || !realpath(directory.fileSystemRepresentation, parent)) return errno;
    size_t length = strlen(parent);
    if (strncmp(canonical, parent, length) || canonical[length] != '/') return EPERM;
    NSError *error = nil;
    *output = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:path] error:&error];
    if (!*output) return [error.domain isEqualToString:NSCocoaErrorDomain] &&
        error.code == NSFileReadCorruptFileError ? EINVAL : EIO;
    return 0;
}

static int configuredSpringBoardMetal(const LSBSpawnPlan *plan, const char *const labels[], BOOL *enabled) {
    *enabled = NO;
    BOOL selected = NO;
    for (unsigned i = 0; labels && labels[i]; ++i) if (!strcmp(labels[i], "com.apple.SpringBoard")) selected = YES;
    if (!selected) return 0;
    NSString *data = [runtimeRoot.stringByDeletingLastPathComponent stringByAppendingPathComponent:@LSB_DATA_DIRECTORY];
    NSString *config = [data stringByAppendingPathComponent:@"var/run/LiveSimBooter-launchd-bootstrap.plist"];
    if (plan->argc != 2 || strcmp(plan->strings + plan->argv[1], config.fileSystemRepresentation)) return EINVAL;
    NSDictionary *bootstrap;
    int error = readControlledPlist(config, data, &bootstrap);
    if (error) return error;
    if (![bootstrap[@"Bootstrap"] isKindOfClass:NSDictionary.class]) return EINVAL;
    NSArray *paths = bootstrap[@"Bootstrap"][@"Paths"];
    if (![paths isKindOfClass:NSArray.class] || paths.count != 1 || ![paths[0] isKindOfClass:NSString.class]) return EINVAL;
    NSString *directory = paths[0], *library = [data stringByAppendingPathComponent:@"Library"];
    if (![directory hasPrefix:[library stringByAppendingString:@"/LiveSimBooter-CoreDaemons-"]]) return EINVAL;
    NSDictionary *job;
    error = readControlledPlist([directory stringByAppendingPathComponent:@"com.apple.SpringBoard.plist"], library, &job);
    if (error) return error;
    id program = job[@"Program"];
    NSArray *arguments = job[@"ProgramArguments"];
    if (!program && [arguments isKindOfClass:NSArray.class] && arguments.count) program = arguments[0];
    *enabled = [job[@"Label"] isEqual:@"com.apple.SpringBoard"] &&
        [program isEqual:@"/System/Library/CoreServices/SpringBoard.app/SpringBoard"];
    return 0;
}

static void disposeMessage(LSBReceivedMessage *message) {
    if (!message) return;
    LSBTransportDisposeMessage(message);
    free(message);
}

static void replyError(LSBReceivedMessage *message, int error) {
    if (message->object) LSBTransportReply(message, error, 0, message->requestID, NULL, NULL, NULL);
    disposeMessage(message);
}

static int prepareChildStatus(LSBJob *job) {
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/.child-status.XXXXXX", runtimeRoot.stringByDeletingLastPathComponent.fileSystemRepresentation);
    if (length < 0 || (size_t)length >= sizeof(path)) return ENAMETOOLONG;
    job.statusFD = mkstemp(path);
    if (job.statusFD < 0) return errno;
    int unlinkError = unlink(path) ? errno : 0;
    if (unlinkError) return unlinkError;
    if (fcntl(job.statusFD, F_SETFD, FD_CLOEXEC) < 0 || ftruncate(job.statusFD, LSB_CHILD_STATUS_MAPPING_SIZE) < 0) return errno;
    void *mapping = mmap(NULL, LSB_CHILD_STATUS_MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, job.statusFD, 0);
    if (mapping == MAP_FAILED) return errno;
    job.childStatus = mapping;
    LSBChildStatusInitialize(job.childStatus);
    int wake[2];
    if (pipe(wake)) return errno;
    job.wakeReadFD = wake[0];
    job.wakeWriteFD = wake[1];
    for (unsigned i = 0; i < 2; ++i) {
        if (fcntl(wake[i], F_SETFD, FD_CLOEXEC) < 0 || fcntl(wake[i], F_SETFL, O_NONBLOCK) < 0) return errno;
    }
    if (fcntl(wake[1], F_SETNOSIGPIPE, 1) < 0) return errno;
    return 0;
}

static int inheritChildStatus(LSBJob *job, LSBJob *old) {
    // An exec retains the process's children. Both native instances briefly map the same ledger and wake endpoint.
    job.statusFD = fcntl(old.statusFD, F_DUPFD_CLOEXEC, 3);
    if (job.statusFD < 0) return errno;
    job.wakeReadFD = fcntl(old.wakeReadFD, F_DUPFD_CLOEXEC, 3);
    if (job.wakeReadFD < 0) return errno;
    job.wakeWriteFD = fcntl(old.wakeWriteFD, F_DUPFD_CLOEXEC, 3);
    if (job.wakeWriteFD < 0) return errno;
    void *mapping = mmap(NULL, LSB_CHILD_STATUS_MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, job.statusFD, 0);
    if (mapping == MAP_FAILED) return errno;
    job.childStatus = mapping;
    return LSBChildStatusValidate(job.childStatus);
}

static void wakeParent(LSBJob *parent) {
    const char hint = 1;
    ssize_t written;
    do { written = write(parent.wakeWriteFD, &hint, 1); } while (written < 0 && errno == EINTR);
    if (written < 0 && errno != EAGAIN) fprintf(stderr, "[broker] Child wake failed: %s\n", strerror(errno));
}

static void signalChildExit(LSBJob *parent) {
    wakeParent(parent);
    audit_token_t identity = parent.audit;
    int error = LSBChildSignal(&identity, SIGCHLD);
    if (error == EACCES || error == EPERM) {
        error = LSBSignalExtension(parent.identifier, SIGCHLD);
        if (!error) fprintf(stderr, "[broker] SIGCHLD submitted through launchd for request=%llu pid=%d\n", parent.identifier, parent.pid);
    }
    if (error && error != ESRCH) fprintf(stderr, "[broker] SIGCHLD delivery failed: %s\n", strerror(error));
}

static LSBPendingAdoption *pendingAdoption(uint64_t childIdentifier) {
    for (LSBPendingAdoption *pending in pendingAdoptions) if (pending.childIdentifier == childIdentifier) return pending;
    return nil;
}

static void retryAdoptions(void) {
    for (LSBPendingAdoption *pending in [pendingAdoptions copy]) {
        LSBJob *parent = findJob(pending.parentIdentifier), *child = findJob(pending.childIdentifier);
        if (!parent || parent.phase != JobRunning || (!pending.observation.exited && !child)) {
            if (child) {
                child.parentIdentifier = 0;
                child.parentStatusPending = NO;
                LSBChildStatusSetParentPID(child.childStatus, 0);
            }
            [pendingAdoptions removeObject:pending];
            continue;
        }
        LSBChildTicket ticket;
        LSBChildObservation observation = pending.observation;
        int error = LSBChildStatusImport(parent.childStatus, &observation, &ticket);
        if (error == EAGAIN) continue;
        if (error) {
            fprintf(stderr, "[broker] Orphan import guest=%d failed: %s\n", observation.guestPID, strerror(error));
            continue;
        }
        if (child) { child.parentTicket = ticket; child.parentStatusPending = NO; }
        [pendingAdoptions removeObject:pending];
        if (observation.exited) signalChildExit(parent);
        else wakeParent(parent);
        fprintf(stderr, "[broker] adopted guest=%d by guest=%d native=%d exited=%d\n",
                observation.guestPID, parent.guestPID, observation.nativePID, observation.exited);
    }
    if (!pendingAdoptions.count && adoptionRetrySource) {
        dispatch_source_cancel(adoptionRetrySource);
        adoptionRetrySource = nil;
    }
}

static void queueAdoption(LSBJob *parent, LSBJob *child, const LSBChildObservation *observation) {
    if (!pendingAdoptions) pendingAdoptions = [NSMutableArray new];
    LSBPendingAdoption *pending = [LSBPendingAdoption new];
    pending.parentIdentifier = parent.identifier;
    pending.childIdentifier = child.identifier;
    pending.observation = *observation;
    [pendingAdoptions addObject:pending];
    if (child) {
        child.parentIdentifier = parent.identifier;
        child.parentStatusPending = YES;
        LSBChildStatusSetParentPID(child.childStatus, parent.guestPID);
    }
    retryAdoptions();
    if (pendingAdoptions.count && !adoptionRetrySource) {
        // Keep copied statuses until launchd reaps a slot. Backpressure on new spawns bounds this backlog.
        adoptionRetrySource = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(adoptionRetrySource, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), 10 * NSEC_PER_MSEC, NSEC_PER_MSEC);
        dispatch_source_set_event_handler(adoptionRetrySource, ^{ retryAdoptions(); });
        dispatch_resume(adoptionRetrySource);
    }
}

static LSBJob *adoptiveLaunchd(LSBJob *exiting) {
    // Simulator launchd is a host-rooted process. Requiring that root also excludes a descendant or an adoption cycle.
    for (LSBJob *job in jobs)
        if (job != exiting && job.simulatorLaunchd && job.phase == JobRunning && !job.parentIdentifier) return job;
    return nil;
}

static void retireAuditIdentity(LSBJob *job) {
    if (job.pid <= 0 || job.audit.val[5] != (uint32_t)job.pid) return;
    if (retiredIdentityCount < RetiredIdentityCapacity) {
        retiredIdentities[retiredIdentityCount++] = (LSBRetiredIdentity){job.audit, job.guestPID};
    } else if (!retiredIdentitiesFull) {
        // Never evict an old identity and accidentally let it alias a guest job through the native fallback.
        retiredIdentitiesFull = YES;
        fprintf(stderr, "[broker] Audit identity history full; unmanaged owner resolution now fails closed\n");
    }
}

static int resolveAuditPID(const audit_token_t *audit, pid_t *result) {
    *result = 0;
    if (!audit || !audit->val[5] || audit->val[5] > INT_MAX) return EINVAL;
    pid_t native = (pid_t)audit->val[5];
    // Exact full-token matching precedes PID alias checks, including when a native PID has been reused legitimately.
    for (LSBJob *job in jobs) {
        audit_token_t current = job.audit;
        if (memcmp(audit, &current, sizeof(current))) continue;
        if (job.phase != JobRunning || job.retiredForExec || job.guestPID <= 0) return ESRCH;
        *result = job.guestPID;
        if (native != *result && auditResolutionLogs < AuditResolutionLogLimit) {
            fprintf(stderr, "[broker] audit owner native=%d version=%u -> guest=%d\n", native, audit->val[7], *result);
            if (++auditResolutionLogs == AuditResolutionLogLimit) fprintf(stderr, "[broker] Further audit owner resolution logs suppressed\n");
        }
        return 0;
    }
    // A different credential/generation, a pending replacement, or a native identity equal to a stable guest PID cannot inherit ownership.
    for (LSBJob *job in jobs) if (native == job.pid || native == job.guestPID) return ESRCH;
    for (unsigned index = 0; index < retiredIdentityCount; ++index) {
        const LSBRetiredIdentity *old = &retiredIdentities[index];
        if (native == (pid_t)old->audit.val[5] || native == old->guestPID) return ESRCH;
    }
    if (retiredIdentitiesFull) return ESRCH;
    // Only authenticated simulator launchd can supply this token, copied from its own kernel-audited request.
    *result = native;
    return 0;
}

static void finishJob(LSBJob *job, int error) {
    if (!findJob(job.identifier) || job.finishing) return;
    job.finishing = YES;
    releaseGuestProcessIdentity(job);
    ++finishingDepth;
    if (error && job.request && !LSBValidateSpawnPlan(&job.request->plan)) {
        // The request is still owned until READY succeeds. Preserve its program in early failure diagnostics, without argv or env.
        char program[LSB_PATH_BYTES];
        const char *path = job.request->plan.strings + job.request->plan.path;
        size_t length = strnlen(path, sizeof(program) - 1);
        for (size_t i = 0; i < length; ++i) program[i] = (unsigned char)path[i] < 0x20 || path[i] == 0x7f ? '?' : path[i];
        program[length] = '\0';
        fprintf(stderr, "[broker] request=%llu pid=%d finished error=%d program=%s\n", job.identifier, job.pid, error, program);
    } else fprintf(stderr, "[broker] request=%llu pid=%d finished error=%d\n", job.identifier, job.pid, error);
    // A timed-out launch can still start later. Never let that unbound child claim another request's plan.
    if (job.phase == JobStarting && !job.pid) {
        launchLaneFailed = YES;
        fprintf(stderr, "[broker] Unbound extension launch failed; restart host before submitting another launch\n");
    }
    if (job.request) {
        if (job.request->object) LSBTransportReply(job.request, error ? error : ECHILD, 0, job.identifier, NULL, NULL, NULL);
        disposeMessage(job.request);
        job.request = NULL;
    }
    if (job.exitMonitor) { dispatch_source_cancel(job.exitMonitor); job.exitMonitor = nil; }
    if (job.launchdSource) { dispatch_source_cancel(job.launchdSource); job.launchdSource = nil; }
    if (job.indigo) [job.indigo close];
    BOOL framebufferChanged = job.framebuffer != nil;
    if (job.framebuffer) [job.framebuffer close];
    if (job.iosurface) [job.iosurface close];
    // This helper belongs to the isolated launchd session, never to its simulated child ledger.
    // Revoke before terminating either endpoint owner; a queued native request must stop authenticating immediately.
    if (job.nativeMetalControl) {
        int revokeError = LSBNativeMetalControlPublish(job.nativeMetalControl, NULL);
        if (revokeError) fprintf(stderr, "[Metal] authorization revoke failed: %s\n", strerror(revokeError));
        if (job.pid > 0) {
            int stopError = signalJob(job, SIGKILL);
            if (stopError && stopError != ESRCH) fprintf(stderr, "[Metal] helper termination failed: %s\n", strerror(stopError));
        }
        uint64_t authorized = job.nativeMetalAuthorizedIdentifier;
        LSBJob *owner = findJob(authorized);
        job.nativeMetalAuthorizedIdentifier = 0;
        if (MACH_PORT_VALID(job.nativeServicePort)) {
            mach_port_deallocate(mach_task_self(), job.nativeServicePort);
            job.nativeServicePort = MACH_PORT_NULL;
        }
        for (LSBJob *dependent in [jobs copy]) if (dependent.nativeMetalServiceIdentifier == job.identifier || dependent.publicMetalServiceIdentifier == job.identifier) {
            dependent.servicesPending = NO;
            dependent.servicesError = error ? error : EPIPE;
            if (dependent.phase != JobRunning) finishJob(dependent, dependent.servicesError);
        } else if (dependent.springBoardMetalServiceIdentifier == job.identifier) {
            // A replacement SpringBoard needs a fresh private process-reference namespace, not the retired owner's helper.
            // Permit one new helper when launchd next requests SpringBoard. Failed startup/registration never causes a retry loop.
            dependent.springBoardMetalRestartAllowed = owner.phase == JobRunning && dependent.phase == JobRunning &&
                !dependent.finishing && !dependent.servicesError;
            dependent.springBoardMetalError = error ? error : EPIPE;
            dependent.springBoardMetalPending = NO;
        }
        if (owner && !owner.finishing) {
            int stopError = signalJob(owner, SIGKILL);
            if (stopError && stopError != ESRCH) fprintf(stderr, "[Metal] owner termination failed: %s\n", strerror(stopError));
            finishJob(owner, EPIPE);
        }
    }
    for (LSBJob *helper in [jobs copy]) {
        if (helper.finishing || helper.executionRole != LSB_EXECUTION_NATIVE_METAL_SERVICE) continue;
        if (helper.identifier != job.nativeMetalServiceIdentifier && helper.identifier != job.springBoardMetalServiceIdentifier &&
            helper.identifier != job.publicMetalServiceIdentifier &&
            helper.nativeMetalAuthorizedIdentifier != job.identifier) continue;
        int revokeError = LSBNativeMetalControlPublish(helper.nativeMetalControl, NULL);
        if (revokeError) fprintf(stderr, "[Metal] authorization revoke failed: %s\n", strerror(revokeError));
        finishJob(helper, ECANCELED);
    }
    for (LSBJob *owner in jobs) {
        if (owner.indigoAuthorizedIdentifier == job.identifier) {
            [owner.indigo authorizeClientWithAuditToken:NULL];
            owner.indigoAuthorizedIdentifier = 0;
        }
        if (owner.framebufferAuthorizedIdentifier == job.identifier) {
            [owner.framebuffer authorizeClientWithAuditToken:NULL];
            owner.framebufferAuthorizedIdentifier = 0;
            framebufferChanged = YES;
        }

    }
    if (job.runningBoardMonitor) { LSBRunningBoardMonitorCancel(job.runningBoardMonitor); job.runningBoardMonitor = nil; }
    LSBJob *old = findJob(job.replacesIdentifier);
    if (old && old.replacementIdentifier == job.identifier) old.replacementIdentifier = 0;
    LSBJob *replacement = findJob(job.replacementIdentifier);
    job.replacementIdentifier = 0;
    if (replacement) finishJob(replacement, ECANCELED);
    LSBJob *parent = findJob(job.parentIdentifier);
    if (parent && !job.parentStatusPublished && !job.parentStatusPending && !LSBChildStatusAbandon(parent.childStatus, job.parentTicket)) wakeParent(parent);
    LSBPendingAdoption *pending = pendingAdoption(job.identifier);
    if (pending) [pendingAdoptions removeObject:pending]; // Unknown monitor failure must not invent a successful exit.
    LSBJob *adopter = job.retiredForExec ? nil : adoptiveLaunchd(job);
    for (LSBJob *child in [jobs copy]) {
        if (child.parentIdentifier != job.identifier) continue;
        if (child.phase != JobRunning) {
            child.parentIdentifier = 0;
            finishJob(child, ECANCELED);
            continue;
        }
        LSBChildObservation observation;
        int observed = LSBChildStatusObserve(job.childStatus, child.guestPID, &observation);
        if (adopter && !observed) queueAdoption(adopter, child, &observation);
        else {
            child.parentIdentifier = 0;
            child.parentStatusPending = NO;
            LSBChildStatusSetParentPID(child.childStatus, 0);
            fprintf(stderr, "[broker] orphan guest=%d has no simulated wait owner; getppid=0\n", child.guestPID);
        }
        if (!observed) LSBChildStatusAbandon(job.childStatus, observation.ticket);
    }
    // Exited but unreaped children no longer have LSBJob objects. Preserve their statuses as well.
    if (!job.retiredForExec && adopter) for (unsigned index = 0; index < LSB_CHILD_STATUS_CAPACITY; ++index) {
        LSBChildObservation observation;
        if (!LSBChildStatusObserveIndex(job.childStatus, index, &observation) && observation.exited) {
            queueAdoption(adopter, nil, &observation);
            LSBChildStatusAbandon(job.childStatus, observation.ticket);
        }
    }
    LSBReleaseExtension(job.identifier);
    retireAuditIdentity(job);
    if (startingJob == job) startingJob = nil;
    [jobs removeObject:job];
    if (framebufferChanged) [NSNotificationCenter.defaultCenter postNotificationName:LSBHostFramebufferDidChangeNotification object:nil];
    retryAdoptions();
    BOOL terminalAlive = NO;
    for (LSBJob *remaining in jobs) if (remaining.terminalSession) terminalAlive = YES;
    if (job.terminalSession && !terminalAlive) {
        // Let the socket reader drain output before EOF closes its endpoint and removes the shared state file.
        hostTerminal.signalHandler = nil;
    }
    --finishingDepth;
    startNextJob();
}

static void recordChildExit(uint64_t identifier, int waitStatus) {
    if (sessionDraining && [rebootPendingDeaths containsObject:@(identifier)]) {
        if (!WIFEXITED(waitStatus) && !WIFSIGNALED(waitStatus)) rebootDrainFailed = YES;
        [rebootPendingDeaths removeObject:@(identifier)];
        completeUserspaceReboot(rebootGeneration);
        return;
    }
    LSBJob *job = findJob(identifier);
    if (!job) return;
    fprintf(stderr, "[broker] request=%llu pid=%d exited wait-status=0x%x\n", identifier, job.pid, waitStatus);
    LSBJob *parent = findJob(job.parentIdentifier);
    if (job.phase == JobRunning && parent) {
        LSBPendingAdoption *pending = job.parentStatusPending ? pendingAdoption(job.identifier) : nil;
        if (pending) {
            LSBChildObservation observation = pending.observation;
            observation.exited = 1;
            observation.status = waitStatus;
            pending.observation = observation;
            pending.childIdentifier = 0;
            job.parentStatusPublished = YES;
        } else {
            int error = LSBChildStatusPublishExit(parent.childStatus, job.parentTicket, waitStatus);
            if (error) fprintf(stderr, "[broker] Could not publish child status: %s\n", strerror(error));
            else { job.parentStatusPublished = YES; signalChildExit(parent); }
        }
    }
    if (job.rebootRequested && job.simulatorLaunchd && job.phase == JobRunning && WIFEXITED(waitStatus) && !WEXITSTATUS(waitStatus)) {
        beginUserspaceReboot(job);
        return;
    }
    finishJob(job, job.phase == JobRunning ? 0 : ECHILD);
}

static void recordMonitorError(uint64_t identifier, int error) {
    if (sessionDraining && [rebootPendingDeaths containsObject:@(identifier)]) {
        rebootDrainFailed = YES;
        [rebootPendingDeaths removeObject:@(identifier)];
        completeUserspaceReboot(rebootGeneration);
    } else finishJob(findJob(identifier), error);
}

static void extensionEvent(uint64_t identifier, pid_t pid, int error, void *context) {
    (void)context;
    LSBJob *job = findJob(identifier);
    if (!job) return;
    if (pid > 0) {
        job.extensionPID = pid;
        if (job.pid && job.pid != pid) { finishJob(job, EACCES); return; }
    }
    if (!error) return;
    if (job.phase != JobRunning) { finishJob(job, error); return; }
    if (job.executionRole == LSB_EXECUTION_SIMULATOR) return;
    audit_token_t identity = job.audit;
    // Process exit also invalidates assertions. Keep its monitor alive to report the actual exit, and never signal a reused PID.
    if (LSBRunningBoardValidateIdentity(&identity) == ESRCH) return;
    fprintf(stderr, "[Metal] native request=%llu lost its process assertion: %s\n", identifier, strerror(error));
    // Service cleanup revokes its shared authorization before terminating. The construction probe has no shared control page.
    if (!job.nativeMetalControl) {
        int stopError = signalJob(job, SIGKILL);
        if (stopError && stopError != ESRCH) fprintf(stderr, "[Metal] probe termination failed: %s\n", strerror(stopError));
    }
    finishJob(job, error);
}

static void startNextJob(void) {
    // Coalesce recursive queue transitions and notify UIKit after the main-thread transaction has settled.
    if (!sessionStateNotificationScheduled) {
        sessionStateNotificationScheduled = YES;
        dispatch_async(dispatch_get_main_queue(), ^{
            sessionStateNotificationScheduled = NO;
            [NSNotificationCenter.defaultCenter postNotificationName:LSBHostSessionStateDidChangeNotification object:nil];
        });
    }
    if (startingJob || finishingDepth || sessionDraining) return;
    if (launchLaneFailed) {
        for (LSBJob *job in [jobs copy]) if (job.phase == JobQueued) { finishJob(job, EBUSY); return; }
        return;
    }
    for (LSBJob *job in jobs) {
        if (job.phase != JobQueued) continue;
        BOOL waitingForMetal = NO;
        for (NSNumber *dependency in @[@(job.nativeMetalServiceIdentifier), @(job.publicMetalServiceIdentifier)]) {
            if (!dependency.unsignedLongLongValue) continue;
            LSBJob *helper = findJob(dependency.unsignedLongLongValue);
            if (!helper) { finishJob(job, EPIPE); return; }
            if (helper.phase != JobRunning) { waitingForMetal = YES; break; }
            if (!MACH_PORT_VALID(helper.nativeServicePort)) { finishJob(job, EPIPE); return; }
        }
        if (waitingForMetal) continue;
        if (job.springBoardMetalServiceIdentifier && !job.springBoardMetalError) {
            LSBJob *helper = findJob(job.springBoardMetalServiceIdentifier);
            if (!helper) job.springBoardMetalError = EPIPE;
            else if (helper.phase != JobRunning) continue;
            else if (!MACH_PORT_VALID(helper.nativeServicePort)) job.springBoardMetalError = EPIPE;
        }
        LSBJob *old = findJob(job.replacesIdentifier);
        LSBJob *parent = findJob(old ? old.parentIdentifier : job.parentIdentifier);
        // Let xpcproxy return READY so launchd can answer registration before SETEXEC or a direct backboardd launch.
        BOOL springBoard = parent.springBoardMetalSelected && isSpringBoardProgram(job.request);
        BOOL needsServices = old || isBackboardProgram(job.request) || springBoard || (parent.nativeConfigdSelected && isConfigdProgram(job.request));
        if (needsServices && parent.servicesPending) continue;
        if (needsServices && parent.servicesError) { finishJob(job, parent.servicesError); return; }
        if (springBoard && parent.springBoardMetalRestartAllowed) {
            parent.springBoardMetalRestartAllowed = NO;
            // enqueueJob can advance the launch lane synchronously. Install the dependency first and rescan the changed job list.
            ++finishingDepth;
            int error = 0;
            LSBJob *helper = enqueueNativeMetalHelper(NO, "Metal.SpringBoard", &error);
            parent.springBoardMetalServiceIdentifier = helper.identifier;
            parent.springBoardMetalError = error;
            parent.springBoardMetalPending = helper != nil;
            --finishingDepth;
            fprintf(stderr, "[Metal] SpringBoard replacement helper request=%llu error=%d\n", helper.identifier, error);
            startNextJob();
            return;
        }
        if (springBoard && parent.springBoardMetalPending) continue;
        if (springBoard && parent.springBoardMetalError) { finishJob(job, parent.springBoardMetalError); return; }
        startingJob = job;
        job.phase = JobStarting;
        uint64_t identifier = job.identifier;
        uint32_t launchOptions = job.executionRole == LSB_EXECUTION_NATIVE_METAL_PROBE || job.executionRole == LSB_EXECUTION_NATIVE_METAL_SERVICE
            ? LSB_EXTENSION_LAUNCH_GPU : 0;
        int error = LSBLaunchExtensionWithOptions(LSB_EXTENSION_IDENTIFIER, identifier, launchOptions, extensionEvent, NULL);
        if (error) { job.phase = JobQueued; finishJob(job, error); return; }
        fprintf(stderr, "[broker] extension request=%llu submitted\n", identifier);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            LSBJob *pending = findJob(identifier);
            if (pending && pending.phase != JobRunning) finishJob(pending, ETIMEDOUT);
        });
        return;
    }
}

static BOOL isTerminalHelper(const LSBReceivedMessage *message) {
    const char *path = message->plan.strings + message->plan.path;
    static const char helper[] = "/usr/libexec/terminal-fish";
    if (!strcmp(path, helper)) return YES;
    NSString *physical = [runtimeRoot stringByAppendingString:@"/usr/libexec/terminal-fish"];
    if (!strcmp(path, physical.fileSystemRepresentation)) return YES;
    const char *canonical = physical.fileSystemRepresentation;
    if (!strncmp(canonical, "/private/var/", 13) && !strcmp(path, canonical + 8)) return YES;
    NSString *joined = [runtimeRoot stringByAppendingString:@"//usr/libexec/terminal-fish"];
    canonical = joined.fileSystemRepresentation;
    return !strcmp(path, canonical) || (!strncmp(canonical, "/private/var/", 13) && !strcmp(path, canonical + 8));
}

static int prepareLaunchdTerminal(LSBReceivedMessage *message, LSBJob *sender, BOOL replacing, LSBTerminalSession **terminal) {
    *terminal = nil;
    if (!isTerminalHelper(message)) return 0;
    audit_token_t audit = sender.audit;
    if (!enableIndigo || !sender || sender.phase != JobRunning || sender.finishing || sender.retiredForExec ||
        sender.executionRole != LSB_EXECUTION_SIMULATOR || memcmp(&audit, &message->audit, sizeof(audit))) return EACCES;
    LSBJob *owner = replacing ? findJob(sender.parentIdentifier) : sender;
    LSBPeerProcessIdentity identity = sender.peerIdentity;
    if (replacing && (sender.peerIdentityError || strcmp(identity.executablePath, "/usr/libexec/xpcproxy_sim"))) return EACCES;
    if (!owner || owner.phase != JobRunning || owner.finishing || owner.retiredForExec || owner.parentIdentifier ||
        !launchdOwnsChildIdentity(owner)) return EACCES;
    for (LSBJob *job in jobs) if (job.terminalSession) return EBUSY;
    int error = 0;
    LSBTerminalSession *created = [[LSBTerminalSession alloc] initWithDirectory:runtimeRoot.stringByDeletingLastPathComponent error:&error];
    if (!error && !created) error = ENOMEM;
    if (!error) error = [created attachToMessageReplacingStandardIO:message];
    if (error) [created close];
    else *terminal = created;
    return error;
}

static void publishTerminal(LSBTerminalSession *terminal) {
    [hostTerminal close];
    hostTerminal = terminal;
    terminal.signalHandler = ^int(pid_t foreground, int number) { return signalTerminal(foreground, number); };
    // The UI attaches its output callback synchronously before the socket reader can consume the first prompt.
    [NSNotificationCenter.defaultCenter postNotificationName:LSBHostTerminalDidChangeNotification object:nil];
    [terminal start];
}

static LSBJob *enqueueJob(LSBReceivedMessage *message, LSBJob *parent, uint32_t executionRole) {
    if (executionRole > LSB_EXECUTION_NATIVE_METAL_SERVICE) { replyError(message, EINVAL); return nil; }
    // Only a locally constructed host request can select the native role. Guest RPCs never carry this authority.
    if (executionRole != LSB_EXECUTION_SIMULATOR &&
        (parent || message->object || message->ports.registeredPresent || message->ports.bootstrapPresent)) {
        replyError(message, EACCES); return nil;
    }
    retryAdoptions();
    if (pendingAdoptions.count) { replyError(message, EAGAIN); return nil; }
    if (launchLaneFailed) { replyError(message, EBUSY); return nil; }
    if (jobs.count >= BrokerJobCapacity) { replyError(message, EAGAIN); return nil; }
    LSBJob *job = [LSBJob new];
    job.executionRole = executionRole;
    int error = prepareChildStatus(job);
    if (error) { replyError(message, error); return nil; }
    if (parent) {
        LSBChildTicket ticket;
        error = LSBChildStatusReserve(parent.childStatus, &ticket);
        if (error) { replyError(message, error); return nil; }
        job.parentIdentifier = parent.identifier;
        job.parentTicket = ticket;
    }
    LSBTerminalSession *terminal = nil;
    error = prepareLaunchdTerminal(message, parent, NO, &terminal);
    if (error) {
        if (parent) LSBChildStatusAbandon(parent.childStatus, job.parentTicket);
        replyError(message, error); return nil;
    }
    job.identifier = nextIdentifier++;
    job.terminalSession = terminal || (parent ? parent.terminalSession : initialTerminalPending && !message->object &&
        executionRole == LSB_EXECUTION_SIMULATOR && strcmp(message->plan.strings + message->plan.path, "/sbin/launchd_sim"));
    if (!parent && job.terminalSession) initialTerminalPending = NO;
    if (!parent && !strcmp(message->plan.strings + message->plan.path, "/sbin/launchd_sim")) {
        for (unsigned i = 0; i < message->plan.envc; ++i)
            if (!strcmp(message->plan.strings + message->plan.envp[i], "LSB_SIMULATOR_LAUNCHD=1")) job.simulatorLaunchd = YES;
    }
    job.request = message;
    [jobs addObject:job];
    if (terminal) publishTerminal(terminal);
    startNextJob();
    return findJob(job.identifier);
}

static void enqueueSpawn(LSBReceivedMessage *message, LSBJob *parent) {
    enqueueJob(message, parent, LSB_EXECUTION_SIMULATOR);
}

static void enqueueExec(LSBReceivedMessage *message, LSBJob *old) {
    if (launchLaneFailed) { replyError(message, EBUSY); return; }
    if (old.replacementIdentifier || old.simulatorLaunchd) { replyError(message, ENOTSUP); return; }
    // A graphics replacement would discard live event ports, surfaces and gestures; token rollback cannot restore those capabilities.
    for (LSBJob *owner in jobs) if (owner.indigoAuthorizedIdentifier == old.identifier || owner.framebufferAuthorizedIdentifier == old.identifier ||
                                  owner.nativeMetalAuthorizedIdentifier == old.identifier) {
        replyError(message, ENOTSUP); return;
    }
    if (jobs.count >= BrokerJobCapacity) { replyError(message, EAGAIN); return; }
    LSBJob *job = [LSBJob new];
    int error = inheritChildStatus(job, old);
    if (error) { replyError(message, error); return; }
    LSBTerminalSession *terminal = nil;
    error = prepareLaunchdTerminal(message, old, YES, &terminal);
    if (error) { replyError(message, error); return; }
    job.identifier = nextIdentifier++;
    job.guestPID = old.guestPID;
    job.terminalSession = terminal || old.terminalSession;
    job.replacesIdentifier = old.identifier;
    old.replacementIdentifier = job.identifier;
    job.request = message;
    [jobs addObject:job];
    if (terminal) publishTerminal(terminal);
    fprintf(stderr, "[broker] exec guest=%d old-native=%d replacement-request=%llu\n", old.guestPID, old.pid, job.identifier);
    startNextJob();
}

static int signalJob(LSBJob *child, int signalNumber) {
    audit_token_t identity = child.audit;
    // A retained job can outlive the process until its queued exit notification runs. Validate the full PID generation.
    // proc_signal_with_audittoken does not support signal 0, so the identity lookup also implements the existence check.
    int error = LSBRunningBoardValidateIdentity(&identity);
    if (error || !signalNumber) return error;
    error = LSBChildSignal(&identity, signalNumber);
    if (error == EACCES || error == EPERM) {
        error = LSBSignalExtension(child.identifier, signalNumber);
        if (!error) fprintf(stderr, "[broker] signal=%d submitted through launchd for request=%llu pid=%d\n",
                            signalNumber, child.identifier, child.pid);
    }
    return error;
}

static BOOL launchdOwnsChildIdentity(LSBJob *job) {
    LSBPeerProcessIdentity identity = job.peerIdentity;
    return job.simulatorLaunchd && !job.peerIdentityError && !strcmp(identity.executablePath, "/sbin/launchd_sim");
}

static int signalChild(LSBJob *parent, pid_t targetPID, int signalNumber) {
    if (signalNumber < 0 || signalNumber >= NSIG) return EINVAL;
    if (targetPID <= 0) return ENOTSUP;
    BOOL nativeAlias = launchdOwnsChildIdentity(parent);
    LSBJob *target = nil;
    for (LSBJob *job in jobs) {
        if (job.parentIdentifier != parent.identifier || job.phase != JobRunning || job.retiredForExec || job.finishing ||
            job.executionRole != LSB_EXECUTION_SIMULATOR) continue;
        // launchd's direct PID paths use the logical ID; its audit-token signal paths use the current native ID.
        // An alias never widens ownership, and a conflicting logical/native match must not signal either process.
        if (job.guestPID == targetPID || (nativeAlias && job.pid == targetPID)) {
            if (target && target != job) return ESRCH;
            target = job;
        }
    }
    return target ? signalJob(target, signalNumber) : ESRCH;
}

static int signalTerminal(pid_t foreground, int signalNumber) {
    (void)foreground;
    // This first terminal runs with job control disabled: the shell and its descendants share terminal signals.
    // It does not implement kernel process groups or stopped/continued job notifications.
    if (signalNumber != SIGINT && signalNumber != SIGQUIT && signalNumber != SIGWINCH) return ENOTSUP;
    int firstError = 0;
    BOOL found = NO;
    for (LSBJob *job in jobs) {
        if (!job.terminalSession || job.phase != JobRunning) continue;
        found = YES;
        int error = signalJob(job, signalNumber);
        if (error && error != ESRCH && !firstError) firstError = error;
    }
    return found ? firstError : ESRCH;
}

static int registerDisplayEndpoint(mach_port_t service, const audit_token_t *audit, const char *name,
                                   mach_port_t endpoint, int64_t deadline, kern_return_t *status) {
    *status = -1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return errno;
    int64_t remaining = deadline - (int64_t)now.tv_sec * NSEC_PER_SEC - now.tv_nsec;
    if (remaining <= 0) return ETIMEDOUT;
    uint32_t milliseconds = (uint32_t)((remaining + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC);
    return LSBLaunchdRegisterEndpointWithTimeout(service, audit, name, endpoint, milliseconds, status);
}

static int unregisterDisplayEndpoint(mach_port_t service, const audit_token_t *audit, const char *name,
                                     int64_t deadline, kern_return_t *status) {
    *status = -1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return errno;
    int64_t remaining = deadline - (int64_t)now.tv_sec * NSEC_PER_SEC - now.tv_nsec;
    if (remaining <= 0) return ETIMEDOUT;
    uint32_t milliseconds = (uint32_t)((remaining + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC);
    return LSBLaunchdUnregisterEndpointWithTimeout(service, audit, name, milliseconds, status);
}

static void registerReplacementSpringBoardMetal(LSBJob *active, LSBJob *helper) {
    mach_port_t service = LSBLaunchdHandshakeServicePort(active.launchdHandshake), endpoint = helper.nativeServicePort;
    int error = 0;
    if (!MACH_PORT_VALID(service) || !MACH_PORT_VALID(endpoint)) error = EPIPE;
    else if (mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_SEND, 1)) error = EPIPE;
    else if (mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, 1)) {
        mach_port_deallocate(mach_task_self(), service);
        error = EPIPE;
    }
    if (error) {
        active.springBoardMetalPending = NO;
        active.springBoardMetalError = error;
        return;
    }
    uint64_t identifier = active.identifier, helperIdentifier = helper.identifier;
    audit_token_t expected = active.audit;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        struct timespec now = {0};
        int registrationError = clock_gettime(CLOCK_MONOTONIC, &now) ? errno : 0;
        int64_t deadline = ((int64_t)now.tv_sec + DisplayRegistrationTimeoutSeconds) * NSEC_PER_SEC + now.tv_nsec;
        kern_return_t status = -1, removalStatus = -1;
        // Bridge registration rejects an existing name. Retire only this session's previous private endpoint before replacing it.
        // ESRCH is a confirmed absence (for example after dead-name cleanup); an unknown/denied removal must not proceed.
        if (!registrationError) registrationError = unregisterDisplayEndpoint(service, &expected, "com.apple.metal.simulator.SpringBoard",
                                                                              deadline, &removalStatus);
        if (registrationError == ESRCH) registrationError = 0;
        if (!registrationError) registrationError = registerDisplayEndpoint(service, &expected, "com.apple.metal.simulator.SpringBoard",
                                                                            endpoint, deadline, &status);
        mach_port_deallocate(mach_task_self(), endpoint);
        mach_port_deallocate(mach_task_self(), service);
        dispatch_async(dispatch_get_main_queue(), ^{
            LSBJob *current = findJob(identifier), *currentHelper = findJob(helperIdentifier);
            if (!current || current.finishing || current.springBoardMetalServiceIdentifier != helperIdentifier) return;
            current.springBoardMetalPending = NO;
            if (!current.springBoardMetalError)
                current.springBoardMetalError = !currentHelper || currentHelper.phase != JobRunning ? EPIPE : registrationError;
            fprintf(stderr, "[Metal] SpringBoard replacement registration request=%llu error=%d unregister=%d bootstrap=%d\n",
                    helperIdentifier, current.springBoardMetalError, removalStatus, status);
            startNextJob();
        });
    });
}

static void registerDisplayServices(LSBJob *active, mach_port_t service) {
    if (!active.servicesPending) return;
    LSBJob *helper = findJob(active.nativeMetalServiceIdentifier);
    mach_port_t metal = helper.nativeServicePort;
    LSBJob *publicHelper = findJob(active.publicMetalServiceIdentifier);
    mach_port_t publicMetal = publicHelper.nativeServicePort;
    if ((active.nativeMetalServiceIdentifier && (!helper || helper.phase != JobRunning || !MACH_PORT_VALID(metal))) ||
        (active.publicMetalServiceIdentifier && (!publicHelper || publicHelper.phase != JobRunning || !MACH_PORT_VALID(publicMetal)))) {
        active.servicesPending = NO;
        active.servicesError = EPIPE;
        startNextJob();
        return;
    }
    LSBJob *springBoardHelper = findJob(active.springBoardMetalServiceIdentifier);
    mach_port_t springBoardMetal = springBoardHelper.nativeServicePort;
    if (active.springBoardMetalSelected && !active.springBoardMetalError &&
        (!springBoardHelper || springBoardHelper.phase != JobRunning || !MACH_PORT_VALID(springBoardMetal))) active.springBoardMetalError = EPIPE;
    if (active.springBoardMetalError) springBoardMetal = MACH_PORT_NULL;
    active.springBoardMetalPending = MACH_PORT_VALID(springBoardMetal);
    kern_return_t copied = mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_SEND, 1);
    if (copied) { active.servicesPending = NO; active.servicesError = EPIPE; startNextJob(); return; }
    if (MACH_PORT_VALID(metal) && mach_port_mod_refs(mach_task_self(), metal, MACH_PORT_RIGHT_SEND, 1)) {
        mach_port_deallocate(mach_task_self(), service);
        active.servicesPending = NO;
        active.servicesError = EPIPE;
        startNextJob();
        return;
    }
    if (MACH_PORT_VALID(publicMetal) && mach_port_mod_refs(mach_task_self(), publicMetal, MACH_PORT_RIGHT_SEND, 1)) {
        if (MACH_PORT_VALID(metal)) mach_port_deallocate(mach_task_self(), metal);
        mach_port_deallocate(mach_task_self(), service);
        active.servicesPending = NO;
        active.servicesError = EPIPE;
        startNextJob();
        return;
    }
    if (MACH_PORT_VALID(springBoardMetal) && mach_port_mod_refs(mach_task_self(), springBoardMetal, MACH_PORT_RIGHT_SEND, 1)) {
        active.springBoardMetalError = EPIPE;
        active.springBoardMetalPending = NO;
        springBoardMetal = MACH_PORT_NULL;
    }
    audit_token_t expected = active.audit;
    uint64_t identifier = active.identifier;
    uint64_t helperIdentifier = helper.identifier, springBoardHelperIdentifier = springBoardHelper.identifier;
    uint64_t publicHelperIdentifier = publicHelper.identifier;
    LSBIndigoHIDServer *indigo = active.indigo;
    LSBFramebufferServer *framebuffer = active.framebuffer;
    LSBIOSurfaceServer *iosurface = active.iosurface;
    BOOL nativeConfigdSelected = active.nativeConfigdSelected;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        // Launchd can spend several seconds synchronously spawning its initial xpcproxy batch before servicing the final registration.
        // Keep each original request/reply alive within one bounded startup budget; timeout is an unknown outcome, not retry permission.
        struct timespec now = {0};
        int clockError = clock_gettime(CLOCK_MONOTONIC, &now) ? errno : 0;
        int64_t deadline = ((int64_t)now.tv_sec + DisplayRegistrationTimeoutSeconds) * NSEC_PER_SEC + now.tv_nsec;
        kern_return_t configdLookupStatus = -1, configdStatus = -1;
        int configdError = clockError;
        if (!configdError && nativeConfigdSelected) {
            // CoreSimulator supplies this host mirror endpoint to configd_sim's SimulatorSupport plugin.
            // Only the host-selected fixed service is bridged; configd_sim still owns its three suffixed guest services.
            const char *name = "com.apple.SystemConfiguration.configd";
            mach_port_t endpoint = MACH_PORT_NULL;
            configdLookupStatus = bootstrap_look_up(bootstrap_port, name, &endpoint);
            configdError = configdLookupStatus ? EIO : !MACH_PORT_VALID(endpoint) ? EPIPE :
                registerDisplayEndpoint(service, &expected, name, endpoint, deadline, &configdStatus);
            // Registration borrows the right with COPY_SEND. Lookup's owned reference is released even on error/unknown timeout.
            if (MACH_PORT_VALID(endpoint)) mach_port_deallocate(mach_task_self(), endpoint);
        }
        kern_return_t indigoStatus = -1, iosurfaceStatus = -1, framebufferStatus = -1, metalStatus = -1, backboardMetalStatus = -1;
        int indigoError = configdError ? configdError : indigo ? registerDisplayEndpoint(service, &expected, "IndigoHIDRegistrationPort",
                                                                                       indigo.registrationPort, deadline, &indigoStatus) : 0;
        int endpointError = indigoError, iosurfaceError = ECANCELED, framebufferError = ECANCELED;
        if (!endpointError && iosurface) {
            iosurfaceError = registerDisplayEndpoint(service, &expected, "com.apple.IOSurface.Remote",
                                                     iosurface.registrationPort, deadline, &iosurfaceStatus);
            endpointError = iosurfaceError;
        }
        if (!endpointError && framebuffer) {
            framebufferError = registerDisplayEndpoint(service, &expected, "com.apple.CoreSimulator.SimFramebufferServer",
                                                        framebuffer.registrationPort, deadline, &framebufferStatus);
            endpointError = framebufferError;
        }
        int metalError = ECANCELED, backboardMetalError = ECANCELED;
        if (!endpointError && MACH_PORT_VALID(publicMetal)) {
            metalError = registerDisplayEndpoint(service, &expected, "com.apple.metal.simulator", publicMetal, deadline, &metalStatus);
            endpointError = metalError;
        }
        if (!endpointError && MACH_PORT_VALID(metal)) {
            backboardMetalError = registerDisplayEndpoint(service, &expected, "com.apple.metal.simulator.backboardd",
                                                          metal, deadline, &backboardMetalStatus);
            endpointError = backboardMetalError;
        }
        kern_return_t hapticsLookupStatus = -1, hapticsStatus = -1;
        int hapticsError = ECANCELED;
        BOOL hapticsEnabled = LSBExperimentalHapticsEnabled();
        if (!endpointError && indigo && hapticsEnabled) {
            // Stock 23A343 CoreHaptics uses this NSXPC service. Publish only the native host's fixed lookup result;
            // clients still connect with their real extension audit identity, and native hapticd enforces its own policy.
            const char *name = "com.apple.audio.hapticd";
            mach_port_t endpoint = MACH_PORT_NULL;
            hapticsLookupStatus = bootstrap_look_up(bootstrap_port, name, &endpoint);
            hapticsError = hapticsLookupStatus ? EIO : !MACH_PORT_VALID(endpoint) ? EPIPE :
                registerDisplayEndpoint(service, &expected, name, endpoint, deadline, &hapticsStatus);
            if (MACH_PORT_VALID(endpoint)) mach_port_deallocate(mach_task_self(), endpoint);
            // Optional hardware access must not turn a missing service or rejected registration into a failed simulator boot.
        }
        // Release baseline jobs immediately after their registrations; the separate SpringBoard route must not delay backboardd.
        void (^finishBaseline)(void) = ^{
            LSBJob *current = findJob(identifier);
            if (!current) return;
            current.servicesPending = NO;
            LSBJob *currentHelper = findJob(helperIdentifier), *currentPublicHelper = findJob(publicHelperIdentifier);
            // A delayed registration reply must not clear an error from helper death or authorize a replacement endpoint.
            BOOL helperLost = (helperIdentifier && (!currentHelper || currentHelper.phase != JobRunning)) ||
                (publicHelperIdentifier && (!currentPublicHelper || currentPublicHelper.phase != JobRunning));
            if (!current.servicesError) current.servicesError = helperLost ? EPIPE : endpointError;
            if (nativeConfigdSelected) fprintf(stderr, "[configd] native bridge error=%d lookup=%d bootstrap=%d\n",
                                               configdError, configdLookupStatus, configdStatus);
            if (indigo && hapticsEnabled) fprintf(stderr, "[haptics] native bridge error=%d lookup=%d bootstrap=%d\n",
                                hapticsError, hapticsLookupStatus, hapticsStatus);
            if (indigo) fprintf(stderr, "[Indigo] registration error=%d bootstrap=%d\n", indigoError, indigoStatus);
            if (iosurface) fprintf(stderr, "[IOSurface] registration error=%d bootstrap=%d\n", iosurfaceError, iosurfaceStatus);
            if (framebuffer) fprintf(stderr, "[framebuffer] registration error=%d bootstrap=%d\n", framebufferError, framebufferStatus);
            if (helperIdentifier) fprintf(stderr, "[Metal] registration error=%d bootstrap=%d backboard-error=%d bootstrap=%d\n",
                                          metalError, metalStatus, backboardMetalError, backboardMetalStatus);
            startNextJob();
        };
        if (MACH_PORT_VALID(springBoardMetal)) dispatch_async(dispatch_get_main_queue(), finishBaseline);
        kern_return_t springBoardStatus = -1;
        int springBoardError = endpointError;
        if (!springBoardError && MACH_PORT_VALID(springBoardMetal))
            springBoardError = registerDisplayEndpoint(service, &expected, "com.apple.metal.simulator.SpringBoard",
                                                       springBoardMetal, deadline, &springBoardStatus);
        if (MACH_PORT_VALID(metal)) mach_port_deallocate(mach_task_self(), metal);
        if (MACH_PORT_VALID(publicMetal)) mach_port_deallocate(mach_task_self(), publicMetal);
        if (MACH_PORT_VALID(springBoardMetal)) mach_port_deallocate(mach_task_self(), springBoardMetal);
        mach_port_deallocate(mach_task_self(), service);
        if (!MACH_PORT_VALID(springBoardMetal)) dispatch_async(dispatch_get_main_queue(), finishBaseline);
        if (springBoardHelperIdentifier) dispatch_async(dispatch_get_main_queue(), ^{
            LSBJob *current = findJob(identifier);
            if (!current || current.finishing || current.springBoardMetalServiceIdentifier != springBoardHelperIdentifier) return;
            current.springBoardMetalPending = NO;
            LSBJob *currentHelper = findJob(springBoardHelperIdentifier);
            if (!current.springBoardMetalError)
                current.springBoardMetalError = !currentHelper || currentHelper.phase != JobRunning ? EPIPE : springBoardError;
            fprintf(stderr, "[Metal] SpringBoard registration error=%d bootstrap=%d\n", springBoardError, springBoardStatus);
            startNextJob();
        });
    });
}

static int prepareLaunchdHandshake(LSBJob *job) {
    if (job.nativeConfigdSelected) job.servicesPending = YES;
    if (enableIndigo) {
        int error = 0;
        job.indigo = [[LSBIndigoHIDServer alloc] initWithError:&error];
        if (!job.indigo) return error ? error : ENOMEM;
        LSBGuestDeviceType *deviceType = sessionDeviceType ?: LSBDefaultGuestDeviceType();
        LSBFramebufferGeometry geometry = {(uint32_t)deviceType.width, (uint32_t)deviceType.height,
                                           (uint32_t)deviceType.scale, (uint32_t)deviceType.pitch};
        job.framebuffer = [[LSBFramebufferServer alloc] initWithGeometry:geometry error:&error];
        if (!job.framebuffer) return error ? error : ENOMEM;
        job.iosurface = [[LSBIOSurfaceServer alloc] initWithError:&error];
        if (!job.iosurface) return error ? error : ENOMEM;
        job.servicesPending = YES;
        [NSNotificationCenter.defaultCenter postNotificationName:LSBHostIndigoDidChangeNotification object:nil];
        [NSNotificationCenter.defaultCenter postNotificationName:LSBHostFramebufferDidChangeNotification object:nil];
    }
    job.launchdHandshake = calloc(1, sizeof(*job.launchdHandshake));
    if (!job.launchdHandshake) return ENOMEM;
    audit_token_t audit = job.audit;
    int error = LSBLaunchdHandshakeInitialize(job.launchdHandshake, &audit);
    if (error) return error;
    mach_port_t tank = LSBLaunchdHandshakeTankPort(job.launchdHandshake);
    job.launchdSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, tank, 0, dispatch_get_main_queue());
    if (!job.launchdSource) return ENOMEM;
    uint64_t identifier = job.identifier;
    // Retain the state until its kevent registration has retired; successful MIG transfer leaves the service send right alive.
    dispatch_source_set_cancel_handler(job.launchdSource, ^{ LSBLaunchdHandshakeCloseTank(job.launchdHandshake); });
    dispatch_source_set_event_handler(job.launchdSource, ^{
        LSBJob *active = findJob(identifier);
        if (!active || !active.launchdHandshake) return;
        int result = LSBLaunchdHandshakeHandleNext(active.launchdHandshake);
        if (result == EAGAIN) return;
        if (!result || result == EALREADY) {
            fprintf(stderr, "[launchd] transferred service receive right to pid=%d; host send port=%u\n", active.pid,
                    LSBLaunchdHandshakeServicePort(active.launchdHandshake));
            const char *probe = getenv("LSB_LAUNCHD_HEALTHCHECK");
            mach_port_t service = LSBLaunchdHandshakeServicePort(active.launchdHandshake);
            if (active.servicesPending) registerDisplayServices(active, service);
            if (probe && !strcmp(probe, "1") && !mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_SEND, 1)) {
                audit_token_t expected = active.audit;
                pid_t process = active.pid;
                // This bounded diagnostic must not stall UIKit or the broker while launchd finishes entering its event loop.
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    kern_return_t bootstrapStatus = -1;
                    int probeError = LSBLaunchdProbeService(service, &expected, &bootstrapStatus);
                    fprintf(stderr, "[launchd] service lookup pid=%d error=%d bootstrap=%d\n", process, probeError, bootstrapStatus);
                    mach_port_deallocate(mach_task_self(), service);
                });
            }
            dispatch_source_cancel(active.launchdSource);
            active.launchdSource = nil;
        } else fprintf(stderr, "[launchd] tank request rejected for pid=%d: %s\n", active.pid, strerror(result));
    });
    dispatch_resume(job.launchdSource);
    return 0;
}

static int copyGuestProcessList(LSBProcessList **output, size_t *size) {
    *output = NULL;
    *size = 0;
    _Static_assert(BrokerJobCapacity <= LSB_PROCESS_LIST_CAPACITY, "process list must cover every broker job");
    LSBProcessList *list = calloc(1, LSBProcessListSize(LSB_PROCESS_LIST_CAPACITY));
    if (!list) return ENOMEM;
    list->version = LSB_PROCESS_LIST_VERSION;
    list->recordSize = sizeof(LSBProcessRecord);
    for (LSBJob *job in jobs) {
        if (job.phase != JobRunning || job.retiredForExec || job.finishing || job.executionRole != LSB_EXECUTION_SIMULATOR ||
            job.guestPID <= 0 || job.peerIdentityError) continue;
        LSBPeerProcessIdentity identity = job.peerIdentity;
        audit_token_t audit = job.audit;
        if (job.pid <= 0 || job.pid != (pid_t)audit.val[5] || job.pidVersion != audit.val[7] ||
            memcmp(&identity.audit, &audit, sizeof(audit))) continue;
        if (list->count == LSB_PROCESS_LIST_CAPACITY) { free(list); return EOVERFLOW; }
        LSBProcessRecord *record = &list->records[list->count++];
        record->guestPID = job.guestPID;
        LSBJob *parent = findJob(job.parentIdentifier);
        record->parentPID = job.childStatus ? LSBChildStatusGetParentPID(job.childStatus) : parent.guestPID;
        record->audit = audit;
        memcpy(record->shortInfo, identity.shortInfo, sizeof(record->shortInfo));
        memcpy(record->executablePath, identity.executablePath, sizeof(record->executablePath));
    }
    int error = LSBValidateProcessList(list, LSBProcessListSize(list->count));
    if (error) { free(list); return error; }
    *output = list;
    *size = LSBProcessListSize(list->count);
    return 0;
}

static BOOL authenticatedSimulatorJob(LSBJob *job, const audit_token_t *expected) {
    audit_token_t audit = job.audit;
    return job && [jobs containsObject:job] && job.phase == JobRunning && !job.retiredForExec && !job.finishing &&
        job.executionRole == LSB_EXECUTION_SIMULATOR && !job.peerIdentityError && job.pid > 0 &&
        audit.val[5] == (uint32_t)job.pid && audit.val[7] == job.pidVersion && !memcmp(&audit, expected, sizeof(audit));
}

static BOOL debuggerTargetCurrent(LSBJob *job, const audit_token_t *expected) {
    LSBPeerProcessIdentity identity = job.peerIdentity;
    return authenticatedSimulatorJob(job, expected) && !validatePeerTaskIdentity(&identity, expected);
}

static BOOL capturedTaskCurrent(LSBJob *job, const audit_token_t *expected) {
    if (!debuggerTargetCurrent(job, expected) || job.taskCaptureError || !MACH_PORT_VALID(job.taskPort)) return NO;
    mach_port_type_t type = 0;
    if (mach_port_type(mach_task_self(), job.taskPort, &type) || !(type & MACH_PORT_TYPE_SEND) ||
        (type & (MACH_PORT_TYPE_RECEIVE | MACH_PORT_TYPE_SEND_ONCE | MACH_PORT_TYPE_DEAD_NAME | MACH_PORT_TYPE_PORT_SET))) return NO;
    audit_token_t actual = {{0}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    // Control capability provenance comes only from the kernel exception receiver, never from CHECKIN's name-only right.
    return !task_info(job.taskPort, TASK_AUDIT_TOKEN, (task_info_t)&actual, &count) && count == TASK_AUDIT_TOKEN_COUNT &&
        !memcmp(&actual, expected, sizeof(actual));
}

static BOOL traceCooperativeDebugger(void) {
    const char *enabled = getenv("LSB_TRACE_PEER_DEBUGGER_STATE");
    if (!enabled || strcmp(enabled, "1") || cooperativeDebuggerLogs >= 128) return NO;
    ++cooperativeDebuggerLogs;
    return YES;
}

static void replyDebuggerRegistration(LSBReceivedMessage *message, int error) {
    int saved = errno;
    if (traceCooperativeDebugger()) fprintf(stderr, "[broker] debugger endpoint native=%u generation=%u phase=%s result=%d\n",
        message->audit.val[5], message->audit.val[7], message->activateDebuggerPort ? "activate" : "prepare", error);
    errno = saved;
    replyError(message, error);
}

static void finishPeerDebuggerQuery(LSBReceivedMessage *message, uint32_t generation, int error, const struct kinfo_proc *info) {
    int saved = errno;
    if (traceCooperativeDebugger()) {
        fprintf(stderr, "[broker] debugger cooperative sender=%u target=%d generation=%u result=%d",
                message->audit.val[5], message->targetPID, generation, error);
        if (!error) fprintf(stderr, " fresh-flags=0x%x traced=%d", info->kp_proc.p_flag, !!(info->kp_proc.p_flag & P_TRACED));
        fputc('\n', stderr);
    }
    errno = saved;
    LSBTransportReplyDebuggerInfo(message, error, error ? NULL : info);
    disposeMessage(message);
}

static void beginPeerDebuggerQuery(LSBJob *sender, LSBReceivedMessage *message) {
    LSBPeerProcessIdentity caller = sender.peerIdentity;
    if (!authenticatedSimulatorJob(sender, &message->audit) || !sender.guestDragUIAllowed ||
        strcmp(caller.executablePath, "/System/Library/PrivateFrameworks/DragUI.framework/Support/druid")) {
        finishPeerDebuggerQuery(message, 0, EACCES, NULL); return;
    }
    if (message->targetPID <= 0) { finishPeerDebuggerQuery(message, 0, EINVAL, NULL); return; }
    if (sender.debuggerQueryPending) { finishPeerDebuggerQuery(message, 0, EBUSY, NULL); return; }
    LSBJob *target = nil;
    for (LSBJob *job in jobs) if (job.pid == message->targetPID && job.phase == JobRunning && !job.retiredForExec && !job.finishing &&
        job.executionRole == LSB_EXECUTION_SIMULATOR) {
        if (target) { finishPeerDebuggerQuery(message, 0, ESRCH, NULL); return; }
        target = job;
    }
    audit_token_t expected = target.audit;
    if (!debuggerTargetCurrent(target, &expected)) { finishPeerDebuggerQuery(message, expected.val[7], ESRCH, NULL); return; }
    mach_port_t endpoint = target.processDebuggerPort;
    if (!target.processDebuggerReady || !MACH_PORT_VALID(endpoint)) { finishPeerDebuggerQuery(message, expected.val[7], ENOTSUP, NULL); return; }
    if (mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, 1)) {
        finishPeerDebuggerQuery(message, expected.val[7], ESRCH, NULL); return;
    }
    sender.debuggerQueryPending = YES;
    // The target's existing guest exception thread services this self-query. UIKit and the broker remain responsive if it is paused.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        struct kinfo_proc info;
        int error = LSBPeerDebuggerQuery(endpoint, &expected, 500, &info);
        mach_port_deallocate(mach_task_self(), endpoint);
        dispatch_async(dispatch_get_main_queue(), ^{
            sender.debuggerQueryPending = NO;
            int result = error;
            if (sessionDraining) result = ECANCELED;
            else if (!authenticatedSimulatorJob(sender, &message->audit)) result = EACCES;
            else if (!debuggerTargetCurrent(target, &expected) || !target.processDebuggerReady || target.processDebuggerPort != endpoint) result = ESRCH;
            finishPeerDebuggerQuery(message, expected.val[7], result, result ? NULL : &info);
        });
    });
}

static void handleMessage(LSBReceivedMessage *message) {
    pid_t pid = (pid_t)message->audit.val[5];
    uint32_t generation = message->audit.val[7];
    LSBJob *sender = nil;
    for (LSBJob *job in jobs) if (job.pid == pid && job.pidVersion == generation) { sender = job; break; }
    if (sessionDraining) { replyError(message, ECANCELED); return; }
    switch (message->kind) {
        case LSB_REQUEST_COPY_PROCESS_TASK: {
            if (!authenticatedSimulatorJob(sender, &message->audit)) { replyError(message, EACCES); return; }
            if (!sender.guestTaskForPIDAllowed) { replyError(message, EPERM); return; }
            if (message->targetPID <= 0) { replyError(message, EINVAL); return; }
            LSBJob *target = nil;
            for (LSBJob *job in jobs) if (job.guestPID == message->targetPID && job.phase == JobRunning &&
                !job.retiredForExec && !job.finishing && job.executionRole == LSB_EXECUTION_SIMULATOR) {
                if (target) { replyError(message, ESRCH); return; }
                target = job;
            }
            audit_token_t expected = target.audit;
            int error = !debuggerTargetCurrent(target, &expected) ? ESRCH :
                target.taskCaptureError || !MACH_PORT_VALID(target.taskPort) ? ENOTSUP :
                capturedTaskCurrent(target, &expected) ? 0 : ESRCH;
            LSBTransportReplyProcessTask(message, error, error ? MACH_PORT_NULL : target.taskPort, error ? NULL : &expected);
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_REGISTER_DEBUGGER_PORT: {
            if (!debuggerTargetCurrent(sender, &message->audit)) { replyDebuggerRegistration(message, EACCES); return; }
            if (message->activateDebuggerPort) {
                if (MACH_PORT_VALID(message->processDebuggerPort) || !MACH_PORT_VALID(sender.processDebuggerPort)) {
                    replyDebuggerRegistration(message, EINVAL); return;
                }
                if (sender.processDebuggerReady) { replyDebuggerRegistration(message, EALREADY); return; }
                sender.processDebuggerReady = YES;
                replyDebuggerRegistration(message, 0);
                return;
            }
            mach_port_type_t type = 0;
            if (!MACH_PORT_VALID(message->processDebuggerPort) || mach_port_type(mach_task_self(), message->processDebuggerPort, &type) ||
                !(type & MACH_PORT_TYPE_SEND) || (type & (MACH_PORT_TYPE_SEND_ONCE | MACH_PORT_TYPE_DEAD_NAME | MACH_PORT_TYPE_PORT_SET))) {
                replyDebuggerRegistration(message, EINVAL); return;
            }
            if (MACH_PORT_VALID(sender.processDebuggerPort)) { replyDebuggerRegistration(message, EALREADY); return; }
            sender.processDebuggerPort = message->processDebuggerPort;
            message->processDebuggerPort = MACH_PORT_NULL;
            replyDebuggerRegistration(message, 0);
            return;
        }
        case LSB_REQUEST_COPY_DEBUGGER_INFO:
            beginPeerDebuggerQuery(sender, message);
            return;
        case LSB_REQUEST_REBOOT_USERSPACE: {
            audit_token_t expected = sender.audit;
            if (!sender || sender.phase != JobRunning || sender.retiredForExec || sender.finishing || sender.parentIdentifier ||
                sender.executionRole != LSB_EXECUTION_SIMULATOR || !launchdOwnsChildIdentity(sender) ||
                memcmp(&message->audit, &expected, sizeof(expected))) { replyError(message, EACCES); return; }
            int error = rebootArguments.count ? 0 : ENOTSUP;
            int sent = LSBTransportReply(message, error, 0, 0, NULL, NULL, NULL);
            if (!error && !sent) {
                sender.rebootRequested = YES;
                fprintf(stderr, "[broker] Simulator reboot armed by launchd pid=%d; waiting for clean exit\n", sender.pid);
            }
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_SPAWN:
            if (!sender || sender.phase != JobRunning || sender.executionRole != LSB_EXECUTION_SIMULATOR) { replyError(message, EACCES); return; }
            enqueueSpawn(message, sender);
            return;
        case LSB_REQUEST_EXEC:
            if (!sender || sender.phase != JobRunning || sender.executionRole != LSB_EXECUTION_SIMULATOR) { replyError(message, EACCES); return; }
            enqueueExec(message, sender);
            return;
        case LSB_REQUEST_KILL: {
            audit_token_t senderAudit = sender.audit;
            if (!sender || sender.phase != JobRunning || sender.retiredForExec || sender.finishing ||
                sender.executionRole != LSB_EXECUTION_SIMULATOR || memcmp(&message->audit, &senderAudit, sizeof(senderAudit))) {
                replyError(message, EACCES); return;
            }
            int error = signalChild(sender, message->targetPID, message->signalNumber);
            LSBTransportReply(message, error, error ? 0 : message->targetPID, 0, NULL, NULL, NULL);
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_RESOLVE_AUDIT_PID: {
            if (!sender || !sender.simulatorLaunchd || sender.phase != JobRunning || sender.retiredForExec) { replyError(message, EACCES); return; }
            audit_token_t senderAudit = sender.audit;
            if (memcmp(&message->audit, &senderAudit, sizeof(senderAudit))) { replyError(message, EACCES); return; }
            pid_t resolved = 0;
            int error = resolveAuditPID(&message->targetAudit, &resolved);
            LSBTransportReply(message, error, error ? 0 : resolved, 0, NULL, NULL, NULL);
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_LIST_PROCESSES: {
            audit_token_t expected = sender.audit;
            if (!sender || sender.phase != JobRunning || sender.retiredForExec || sender.finishing ||
                sender.executionRole != LSB_EXECUTION_SIMULATOR || memcmp(&message->audit, &expected, sizeof(expected))) {
                replyError(message, EACCES); return;
            }
            if (!sender.guestProcessInfoAllowed) { replyError(message, EPERM); return; }
            LSBProcessList *list = NULL;
            size_t size = 0;
            int error = copyGuestProcessList(&list, &size);
            LSBTransportReplyProcessList(message, error, list, size);
            free(list);
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_COPY_ENTITLEMENTS: {
            audit_token_t senderAudit = sender.audit;
            if (!sender || sender.phase != JobRunning || sender.retiredForExec || sender.finishing ||
                sender.executionRole != LSB_EXECUTION_SIMULATOR || memcmp(&message->audit, &senderAudit, sizeof(senderAudit))) {
                replyError(message, EACCES); return;
            }
            unsigned operation = message->entitlementOperation;
            if (operation != 5 && operation != 7 && operation != 16) { replyError(message, EINVAL); return; }
            LSBJob *target = nil;
            for (LSBJob *job in jobs) {
                audit_token_t expected = job.audit;
                if (job.pid > 0 && job.pid == (pid_t)message->targetAudit.val[5] && job.pidVersion == message->targetAudit.val[7] &&
                    job.phase == JobRunning && !job.retiredForExec && !job.finishing && job.executionRole == LSB_EXECUTION_SIMULATOR &&
                    !memcmp(&message->targetAudit, &expected, sizeof(expected))) { target = job; break; }
            }
            int error = target ? operation == 5 ? target.guestCodeDirectoryHashError :
                operation == 7 ? target.guestXMLEntitlementsError : target.guestEntitlementsError : ESRCH;
            NSData *snapshot = operation == 5 ? target.guestCodeDirectoryHash :
                operation == 7 ? target.guestXMLEntitlements : target.guestEntitlements;
            if (!error && !snapshot.length) error = ENOENT;
            LSBTransportReplyEntitlements(message, error, error ? NULL : snapshot.bytes, error ? 0 : snapshot.length);
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_COPY_PROCESS_IDENTITY: {
            audit_token_t senderAudit = sender.audit;
            LSBPeerProcessIdentity caller = sender.peerIdentity;
            BOOL launchd = launchdOwnsChildIdentity(sender);
            if (!sender || sender.phase != JobRunning || sender.retiredForExec || sender.finishing ||
                sender.executionRole != LSB_EXECUTION_SIMULATOR || memcmp(&message->audit, &senderAudit, sizeof(senderAudit)) ||
                sender.peerIdentityError || (!launchd && strcmp(caller.executablePath, "/usr/libexec/runningboardd"))) {
                logPeerIdentityRequest(pid, message->targetPID, 0, EACCES);
                replyError(message, EACCES); return;
            }
            if (message->targetPID <= 0) {
                logPeerIdentityRequest(pid, message->targetPID, 0, EINVAL);
                replyError(message, EINVAL); return;
            }
            LSBJob *target = nil;
            for (LSBJob *job in jobs) {
                BOOL matches = launchd ? job.guestPID == message->targetPID && job.parentIdentifier == sender.identifier : job.pid == message->targetPID;
                if (matches && job.phase == JobRunning && !job.retiredForExec && !job.finishing &&
                    job.executionRole == LSB_EXECUTION_SIMULATOR) { target = job; break; }
            }
            LSBPeerProcessIdentity snapshot = target.peerIdentity;
            audit_token_t targetAudit = target.audit;
            int error = target ? target.peerIdentityError : ESRCH;
            if (!error && validatePeerTaskIdentity(&snapshot, &targetAudit)) error = ESRCH;
            logPeerIdentityRequest(pid, message->targetPID, target.pidVersion, error);
            LSBTransportReplyProcessIdentity(message, error, error ? NULL : &snapshot);
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_RESOLVE_NATIVE_CHILD_PID: {
            audit_token_t senderAudit = sender.audit;
            if (!sender || sender.phase != JobRunning || sender.retiredForExec || sender.finishing ||
                sender.executionRole != LSB_EXECUTION_SIMULATOR || !launchdOwnsChildIdentity(sender) ||
                memcmp(&message->audit, &senderAudit, sizeof(senderAudit))) { replyError(message, EACCES); return; }
            if (message->targetPID <= 0) { replyError(message, EINVAL); return; }
            LSBJob *target = nil;
            for (LSBJob *job in jobs) {
                if (job.pid != message->targetPID || job.parentIdentifier != sender.identifier || job.phase != JobRunning ||
                    job.retiredForExec || job.finishing || job.executionRole != LSB_EXECUTION_SIMULATOR) continue;
                // The request explicitly names a native process. A logical-ID match cannot identify an exec replacement here.
                if (target) { replyError(message, ESRCH); return; }
                target = job;
            }
            LSBPeerProcessIdentity identity = target.peerIdentity;
            audit_token_t audit = target.audit;
            int error = !target || target.guestPID <= 0 || target.peerIdentityError || audit.val[5] != (uint32_t)message->targetPID ||
                audit.val[7] != target.pidVersion || validatePeerTaskIdentity(&identity, &audit) ? ESRCH : 0;
            LSBTransportReply(message, error, error ? 0 : target.guestPID, 0, NULL, NULL, NULL);
            disposeMessage(message);
            return;
        }
        case LSB_REQUEST_CHECKIN: {
            // PlugInKit launches our registered extension. Match its PID to the kernel's sender identity; iOS denies
            // cross-process SecTask entitlement queries even for our own extension.
            LSBJob *job = startingJob;
            if (sender || !job || job.phase != JobStarting) {
                replyError(message, ESRCH); return;
            }
            if (!job.extensionPID) job.extensionPID = LSBExtensionProcessIdentifier(job.identifier);
            if (job.extensionPID <= 0) { replyError(message, EAGAIN); return; }
            if (job.extensionPID != pid) { replyError(message, EACCES); return; }
            // A retired native PID can be reused while its guest identity still belongs to an exec replacement.
            if (!job.guestPID) for (LSBJob *active in jobs) {
                if (active != job && active.executed && active.guestPID == pid && active.pid != pid) {
                    replyError(message, EAGAIN); finishJob(job, EAGAIN); return;
                }
            }
            int identityError = snapshotGuestProcessIdentity(job, message);
            if (identityError) { replyError(message, identityError); finishJob(job, identityError); return; }
            job.pid = pid;
            if (!job.guestPID) job.guestPID = pid;
            job.pidVersion = generation;
            job.audit = message->audit;
            job.phase = JobDispatched;
            // Read the original guest image while its plan still exists, before replying and before guest constructors can query it.
            // Later RPCs use only this immutable snapshot: no broker-to-guest callback, filesystem reread, or native signature substitution.
            snapshotGuestEntitlements(job);
            uint64_t identifier = job.identifier;
            int monitor = -1;
            int error = LSBChildMonitorCreate(pid, &monitor);
            audit_token_t identity = job.audit;
            if (error == EACCES || error == EPERM) {
                LSBRunningBoardMonitor *fallback = nil;
                error = LSBRunningBoardMonitorStart(&identity, ^(int waitStatus, int monitorError) {
                    if (monitorError) recordMonitorError(identifier, monitorError);
                    else recordChildExit(identifier, waitStatus);
                }, &fallback);
                if (error) { replyError(message, error); finishJob(job, error); return; }
                job.runningBoardMonitor = fallback;
                fprintf(stderr, "[broker] Using RunningBoard exit monitor for pid=%d\n", pid);
            } else {
                if (error) { replyError(message, error); finishJob(job, error); return; }
                // kqueue registers by PID. Verify the audited generation after registering; RBS already uses the full token.
                error = LSBRunningBoardValidateIdentity(&identity);
                if (error) { close(monitor); replyError(message, error); finishJob(job, error); return; }
                job.exitMonitor = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)monitor, 0, dispatch_get_main_queue());
                if (!job.exitMonitor) { close(monitor); replyError(message, ENOMEM); finishJob(job, ENOMEM); return; }
                dispatch_source_set_cancel_handler(job.exitMonitor, ^{ close(monitor); });
                dispatch_source_set_event_handler(job.exitMonitor, ^{
                    if (!findJob(identifier) && ![rebootPendingDeaths containsObject:@(identifier)]) return;
                    int waitStatus = 0;
                    int monitorError = LSBChildMonitorRead(monitor, pid, &waitStatus);
                    if (!monitorError) recordChildExit(identifier, waitStatus);
                    else if (monitorError != EAGAIN) {
                        fprintf(stderr, "[broker] Exit monitor failed: %s\n", strerror(monitorError));
                        recordMonitorError(identifier, monitorError);
                    }
                });
                dispatch_resume(job.exitMonitor);
            }
            if (job.simulatorLaunchd) {
                error = prepareLaunchdHandshake(job);
                if (error) { replyError(message, error); finishJob(job, error); return; }
            }
            LSBJob *old = findJob(job.replacesIdentifier);
            LSBJob *parent = findJob(old ? old.parentIdentifier : job.parentIdentifier);
            BOOL springBoard = parent.simulatorLaunchd && parent.springBoardMetalSelected && isSpringBoardProgram(job.request);
            uint64_t metalHelper = springBoard ? parent.springBoardMetalServiceIdentifier :
                isBackboardProgram(job.request) ? parent.nativeMetalServiceIdentifier : 0;
            if (metalHelper || springBoard) {
                LSBJob *helper = findJob(metalHelper);
                audit_token_t identity = job.audit;
                error = springBoard && parent.springBoardMetalError ? parent.springBoardMetalError :
                    !helper || helper.phase != JobRunning || !MACH_PORT_VALID(helper.nativeServicePort) ? EPIPE :
                    helper.nativeMetalAuthorizedIdentifier ? EBUSY : LSBNativeMetalControlPublish(helper.nativeMetalControl, &identity);
                if (error) { replyError(message, error); finishJob(job, error); return; }
                helper.nativeMetalAuthorizedIdentifier = job.identifier;
                fprintf(stderr, "[Metal] authorized %s native=%d version=%u helper=%d\n", springBoard ? "SpringBoard" : "backboard",
                        pid, generation, helper.pid);
            }
            if ((parent.indigo || parent.framebuffer) && isBackboardProgram(job.request)) {
                audit_token_t backboardIdentity = job.audit;
                if (parent.indigo) {
                    [parent.indigo authorizeClientWithAuditToken:&backboardIdentity];
                    parent.indigoAuthorizedIdentifier = job.identifier;
                }
                if (parent.framebuffer) {
                    [parent.framebuffer authorizeClientWithAuditToken:&backboardIdentity];
                    parent.framebufferAuthorizedIdentifier = job.identifier;
                    [NSNotificationCenter.defaultCenter postNotificationName:LSBHostFramebufferDidChangeNotification object:nil];
                }
            }
            LSBChildStatusSetParentPID(job.childStatus, parent ? parent.guestPID : old ? LSBChildStatusGetParentPID(old.childStatus) : getpid());
            error = prepareGuestTaskCapture(job);
            if (error) { replyError(message, error); finishJob(job, error); return; }
            LSBBootstrapMetadata metadata = { .guestPID = job.guestPID, .parentPID = LSBChildStatusGetParentPID(job.childStatus),
                .launchdTankPort = job.simulatorLaunchd ? LSBLaunchdHandshakeTankPort(job.launchdHandshake) : MACH_PORT_NULL,
                .ports = &job.request->ports, .executionRole = job.executionRole, .taskCapture = job.taskCapture,
                .nativeProductionStatus = job.executionRole == LSB_EXECUTION_SIMULATOR ? LSBHostNativeProductionStatus() : LSB_NATIVE_PRODUCTION_UNKNOWN };
            error = LSBTransportReplyBootstrapWithMetadata(message, 0, pid, identifier, runtimeRoot.fileSystemRepresentation,
                &job.request->plan, job.request->fds, job.statusFD, job.wakeReadFD, &metadata);
            disposeMessage(message);
            // Don't keep pipe writers alive while waiting for READY; the child now owns copies.
            for (unsigned i = 0; i < job.request->plan.fdCount; ++i) {
                if (job.request->fds[i] >= 0) close(job.request->fds[i]);
                job.request->fds[i] = -1;
            }
            fprintf(stderr, "[broker] check-in request=%llu pid=%d error=%d\n", identifier, pid, error);
            if (error) finishJob(job, error);
            return;
        }
        case LSB_REQUEST_READY: {
            if (!sender || sender.identifier != message->requestID || sender.phase != JobDispatched) {
                replyError(message, ESRCH); return;
            }
            int error = message->error;
            audit_token_t expectedAudit = sender.audit;
            BOOL nativeService = sender.executionRole == LSB_EXECUTION_NATIVE_METAL_SERVICE;
            if (memcmp(&message->audit, &expectedAudit, sizeof(expectedAudit))) { replyError(message, EACCES); return; }
            if (!error && sender.executionRole == LSB_EXECUTION_SIMULATOR &&
                (sender.taskCaptureError || !MACH_PORT_VALID(sender.taskPort))) error = EPROTO;
            stopGuestTaskCapture(sender);
            if (MACH_PORT_VALID(message->nativeServicePort) && (!nativeService || error)) error = EPROTO;
            if (!error && nativeService && !MACH_PORT_VALID(message->nativeServicePort)) error = EPROTO;
            if (!error && nativeService) {
                mach_port_type_t type = 0;
                if (mach_port_type(mach_task_self(), message->nativeServicePort, &type) || !(type & MACH_PORT_TYPE_SEND)) error = EPIPE;
            }
            if (!error && nativeService) {
                // Receive owns one send reference. Move it into the helper before disposing READY and advancing the launch lane.
                sender.nativeServicePort = message->nativeServicePort;
                message->nativeServicePort = MACH_PORT_NULL;
            }
            LSBJob *parent = findJob(sender.parentIdentifier);
            LSBJob *old = findJob(sender.replacesIdentifier);
            LSBChildExecUpdate execution = {0};
            BOOL preparedExec = NO;
            if (sender.replacesIdentifier && (!old || old.phase != JobRunning || old.replacementIdentifier != sender.identifier)) error = ESRCH;
            if (!error && old) {
                parent = findJob(old.parentIdentifier);
                if (parent && !old.parentStatusPending) {
                    error = LSBChildStatusPrepareExec(parent.childStatus, old.parentTicket, pid, &execution);
                    preparedExec = !error;
                } else if (old.parentStatusPending) {
                    LSBPendingAdoption *pending = pendingAdoption(old.identifier);
                    if (!pending) error = ESRCH;
                    else if (pending.observation.execGeneration == UINT32_MAX) error = EOVERFLOW;
                }
            }
            // The child is still blocked in READY: capture its real group before it can run and exit.
            if (!error && parent && !old) {
                pid_t group = getpgid(pid);
                error = group < 0 ? errno : LSBChildStatusPublishRunning(parent.childStatus, sender.parentTicket, sender.guestPID, group);
            }
            int replyError = LSBTransportReply(message, message->error ? 0 : error, pid, sender.identifier, NULL, NULL, NULL);
            disposeMessage(message);
            if (error || replyError) {
                if (preparedExec) LSBChildStatusCancelExec(parent.childStatus, &execution);
                finishJob(sender, error ? error : replyError);
                return;
            }
            sender.phase = JobRunning;
            if (old) {
                // This main-queue transaction owns Replacing until publication; exit callbacks cannot interleave it.
                // READY can release the child first: its subsequent owner-resolution RPC is also handled on this serial main queue,
                // so it cannot observe the candidate until this commit and retirement finish. A failed READY still preserves the old job.
                if (preparedExec) error = LSBChildStatusCommitExec(parent.childStatus, &execution);
                if (error) {
                    signalJob(sender, SIGKILL);
                    finishJob(sender, error);
                    return;
                }
                sender.parentIdentifier = old.parentIdentifier;
                sender.executed = YES;
                sender.parentTicket = old.parentTicket;
                sender.parentStatusPending = old.parentStatusPending;
                if (old.parentStatusPending) {
                    LSBPendingAdoption *pending = pendingAdoption(old.identifier);
                    LSBChildObservation observation = pending.observation;
                    observation.nativePID = pid;
                    ++observation.execGeneration;
                    pending.observation = observation;
                    pending.childIdentifier = sender.identifier;
                    old.parentStatusPending = NO;
                }
                old.retiredForExec = YES;
                sender.replacesIdentifier = 0;
                old.replacementIdentifier = 0;
                old.parentIdentifier = 0;
                for (LSBJob *child in jobs) if (child.parentIdentifier == old.identifier) child.parentIdentifier = sender.identifier;
                if (parent) wakeParent(parent);
                fprintf(stderr, "[broker] exec committed guest=%d native=%d -> %d\n", sender.guestPID, old.pid, pid);
            }
            if (sender.request->object) {
                error = LSBTransportReply(sender.request, 0, sender.guestPID, sender.identifier, NULL, NULL, NULL);
                // Once committed, a dead old RPC peer cannot roll the replacement back.
                if (error && !old) { finishJob(sender, error); return; }
            }
            disposeMessage(sender.request);
            sender.request = NULL;
            if (old) {
                int retireError = signalJob(old, SIGKILL);
                if (retireError && retireError != ESRCH) fprintf(stderr, "[broker] retire old exec instance pid=%d: %s\n", old.pid, strerror(retireError));
                finishJob(old, 0);
            }
            fprintf(stderr, "[broker] spawn ready guest=%d native=%d request=%llu\n", sender.guestPID, pid, sender.identifier);
            startingJob = nil;
            if (nativeService) for (LSBJob *owner in jobs) {
                if (owner.phase == JobRunning && owner.springBoardMetalServiceIdentifier == sender.identifier &&
                    owner.springBoardMetalPending && !owner.springBoardMetalError) registerReplacementSpringBoardMetal(owner, sender);
            }
            startNextJob();
            return;
        }
        default: replyError(message, EINVAL); return;
    }
}

static int prepareRuntimeRoot(void) {
    NSFileManager *manager = NSFileManager.defaultManager;
    LSBRuntimeStorage *storage = LSBRuntimeStorage.sharedStorage;
    if (storage.error) {
        fprintf(stderr, "[broker] Runtime storage unavailable: %s\n", storage.error.localizedDescription.UTF8String);
        return [storage.error.domain isEqualToString:NSPOSIXErrorDomain] ? (int)storage.error.code : EACCES;
    }
    runtimeRoot = storage.runtimeRootURL.path;
    fprintf(stderr, "[broker] App Group=%s storage=%s\n", storage.appGroupIdentifier.UTF8String, storage.storageURL.fileSystemRepresentation);
    BOOL directory = NO;
    if (![manager fileExistsAtPath:runtimeRoot isDirectory:&directory] || !directory) return ENOENT;
    char canonical[PATH_MAX];
    if (!realpath(runtimeRoot.fileSystemRepresentation, canonical)) return errno;
    runtimeRoot = [NSString stringWithUTF8String:canonical];
    if (strlen(runtimeRoot.fileSystemRepresentation) >= LSB_PATH_BYTES) return ENAMETOOLONG;
    fprintf(stderr, "[broker] RuntimeRoot=%s\n", runtimeRoot.fileSystemRepresentation);
    return 0;
}

static int initialExecutionRole(int argc, const char *argv[], uint32_t *role) {
    if (!role || argc < 1 || !argv || !argv[0]) return EINVAL;
    *role = LSB_EXECUTION_SIMULATOR;
    if (argc > 1 && !strcmp(argv[1], "--native-metal-probe")) {
        if (argc != 2) return EINVAL;
        *role = LSB_EXECUTION_NATIVE_METAL_PROBE;
    }
    return 0;
}

static int initialPlan(LSBReceivedMessage *message, int argc, const char *argv[], const char *envp[]) {
    initialTerminalPending = NO;
    initialNativeConfigdPending = NO;
    LSBSpawnPlan *plan = &message->plan;
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) message->fds[i] = -1;
    uint32_t executionRole;
    int roleError = initialExecutionRole(argc, argv, &executionRole);
    if (roleError) return roleError;
    BOOL nativeProbe = executionRole == LSB_EXECUTION_NATIVE_METAL_PROBE;
    if (argc > 1 && !strcmp(argv[1], "--launchd"))
        return argc == 2 ? LSBPrepareLaunchdPlan(message, runtimeRoot.fileSystemRepresentation, envp) : EINVAL;
    if (argc > 1 && !strcmp(argv[1], "--launchd-df"))
        return argc == 2 ? LSBPrepareLaunchdDFPlan(message, runtimeRoot.fileSystemRepresentation, envp) : EINVAL;
    if (argc > 1 && !strcmp(argv[1], "--launchd-backboardd")) {
        enableIndigo = YES;
        return argc == 2 ? LSBPrepareLaunchdBackboardPlan(message, runtimeRoot.fileSystemRepresentation, envp) : EINVAL;
    }
    if (argc == 1 || (argc > 1 && !strcmp(argv[1], "--launchd-services"))) {
        // Icon launches boot the graphical session. Its fixed launchd-owned terminal job supplies the Fish tab.
        const char *defaults[] = {"com.apple.notifyd", "com.apple.lsd", "com.apple.runningboardd", "com.apple.containermanagerd",
                                  "com.apple.fontservicesd", "com.apple.containermanagerd.system", "com.apple.distnoted.xpc.daemon",
                                  "com.apple.configd_sim", "com.apple.sleepd", "com.apple.systemstatusd", "com.apple.SpringBoard",
                                  "com.apple.iconservices.iconservicesagent", "com.apple.TextInput.kbd", "com.apple.assistantd",
                                  "com.apple.PosterBoard", "com.apple.DragUI.druid", NULL};
        enableIndigo = YES;
        const char *const *labels = argc > 2 ? argv + 2 : defaults;
        int error = LSBPrepareLaunchdServicesPlan(message, runtimeRoot.fileSystemRepresentation, envp, labels);
        if (!error) for (const char *const *label = labels; *label; ++label)
            if (!strcmp(*label, "com.apple.configd_sim")) initialNativeConfigdPending = YES;
        enableSpringBoardMetal = NO;
        return error ? error : configuredSpringBoardMetal(plan, labels, &enableSpringBoardMetal);
    }
    if (argc > 1 && !strcmp(argv[1], "--launchd-lifecycle"))
        return argc == 2 ? LSBPrepareLaunchdLifecyclePlan(message, runtimeRoot.fileSystemRepresentation, envp) : EINVAL;
    plan->version = LSB_PLAN_VERSION;
    const char *defaultArgs[] = {"/bin/fish", "-i", "-N", "-P", "-C",
        "status job-control none; set -g fish_use_posix_spawn 1; set -g fish_greeting; function fish_prompt; printf 'fish> '; end", NULL};
    const char *nativeArgs[] = { "/__LiveSimBooterNativeMetalProbe", NULL };
    BOOL terminal = argc > 1 && !strcmp(argv[1], "--terminal");
    int firstArgument = terminal && argc > 1 ? 2 : 1;
    const char *const *arguments = nativeProbe ? nativeArgs : argc > firstArgument ? argv + firstArgument : defaultArgs;
    int count = nativeProbe ? 1 : argc > firstArgument ? argc - firstArgument : 6;
    int error = LSBPlanAddString(plan, arguments[0], &plan->path);
    if (!error) error = LSBPlanAddString(plan, runtimeRoot.fileSystemRepresentation, &plan->cwd);
    for (int i = 0; !error && i < count; ++i) {
        if (plan->argc == LSB_MAX_VECTOR) return E2BIG;
        error = LSBPlanAddString(plan, arguments[i], &plan->argv[plan->argc++]);
    }
    for (const char **item = nativeProbe ? NULL : envp; !error && item && *item; ++item) {
        if (terminal && (!strncmp(*item, "TERM=", 5) || !strncmp(*item, "PATH=", 5) || !strncmp(*item, "HOME=", 5) ||
                         !strncmp(*item, LSB_FAKE_PTY_ENV "=", sizeof(LSB_FAKE_PTY_ENV)))) continue;
        if (plan->envc == LSB_MAX_VECTOR) return E2BIG;
        error = LSBPlanAddString(plan, *item, &plan->envp[plan->envc++]);
    }
    plan->creationMask = 022;
    sigprocmask(0, NULL, (sigset_t *)&plan->signalMask);
    if (terminal && !error) {
        NSString *home = [runtimeRoot stringByAppendingPathComponent:@"home/mobile"];
        NSError *directoryError = nil;
        if (![NSFileManager.defaultManager createDirectoryAtPath:home withIntermediateDirectories:YES attributes:nil error:&directoryError])
            error = EIO;
        NSArray<NSString *> *environment = @[@"TERM=xterm-256color", [@"HOME=" stringByAppendingString:home],
            [NSString stringWithFormat:@"PATH=%@/bin:%@/usr/bin", runtimeRoot, runtimeRoot]];
        for (NSString *entry in environment) {
            if (error) break;
            if (plan->envc == LSB_MAX_VECTOR) { error = E2BIG; break; }
            error = LSBPlanAddString(plan, entry.UTF8String, &plan->envp[plan->envc++]);
        }
        if (!error) hostTerminal = [[LSBTerminalSession alloc] initWithDirectory:runtimeRoot.stringByDeletingLastPathComponent error:&error];
        if (!error) error = [hostTerminal attachToMessage:message];
        if (error) { [hostTerminal close]; hostTerminal = nil; }
        else initialTerminalPending = YES;
    }
    for (int number = 0; !terminal && !error && number < 3; ++number) {
        int flags = fcntl(number, F_GETFD);
        if (flags < 0) continue;
        int duplicate = fcntl(number, F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0) return errno;
        unsigned index = plan->fdCount++;
        plan->fds[index] = (LSBInheritedFD){number, (uint32_t)flags};
        message->fds[index] = duplicate;
    }
    return error ? error : LSBValidateSpawnPlan(plan);
}

static LSBJob *enqueueNativeMetalHelper(BOOL publicAccess, const char *name, int *result) {
    const char *arguments[] = { "LiveSimBooter", "--native-metal-probe", NULL };
    LSBReceivedMessage *request = calloc(1, sizeof(*request));
    if (!request) { *result = ENOMEM; return nil; }
    LSBNativeMetalControl *control = NULL;
    int readOnlyFD = -1;
    int error = initialPlan(request, 2, arguments, NULL);
    if (!error) error = LSBPlanAddString(&request->plan, "/__LiveSimBooterNativeMetalService", &request->plan.path);
    if (!error) error = LSBPlanAddString(&request->plan, name, &request->plan.argv[0]);
    if (!error) error = LSBNativeMetalControlCreate(runtimeRoot.stringByDeletingLastPathComponent.fileSystemRepresentation, &control, &readOnlyFD);
    if (!error && publicAccess) error = LSBNativeMetalControlPublishPublic(control);
    if (!error) {
        unsigned index = request->plan.fdCount++;
        request->plan.fds[index] = (LSBInheritedFD){3, 0};
        request->fds[index] = readOnlyFD;
        readOnlyFD = -1;
        error = LSBValidateSpawnPlan(&request->plan);
    }
    if (error) {
        LSBNativeMetalControlUnmap(control);
        if (readOnlyFD >= 0) close(readOnlyFD);
        disposeMessage(request);
        *result = error;
        return nil;
    }
    LSBJob *helper = enqueueJob(request, nil, LSB_EXECUTION_NATIVE_METAL_SERVICE);
    if (helper) helper.nativeMetalControl = control;
    else LSBNativeMetalControlUnmap(control);
    *result = helper ? 0 : EIO;
    return helper;
}

static int enqueueInitialJob(LSBReceivedMessage *initial, uint32_t executionRole) {
    if (!enableIndigo) { enqueueJob(initial, nil, executionRole); return 0; }
    // Only host-selected graphics jobs provision services. Each helper has its own control page, endpoint and process-reference namespace.
    // Install every dependency before submitting an extension, including synchronous launch-error paths.
    ++finishingDepth;
    BOOL nativeConfigdSelected = initialNativeConfigdPending;
    int error = 0, springBoardError = 0;
    LSBJob *helper = enqueueNativeMetalHelper(NO, "Metal.backboardd", &error);
    LSBJob *springBoard = helper && enableSpringBoardMetal ? enqueueNativeMetalHelper(NO, "Metal.SpringBoard", &springBoardError) : nil;
    LSBJob *publicHelper = helper ? enqueueNativeMetalHelper(YES, "Metal.public", &error) : nil;
    LSBJob *launchd = publicHelper ? enqueueJob(initial, nil, executionRole) : nil;
    if (!publicHelper) disposeMessage(initial);
    if (launchd) {
        launchd.nativeConfigdSelected = nativeConfigdSelected;
        launchd.nativeMetalServiceIdentifier = helper.identifier;
        launchd.publicMetalServiceIdentifier = publicHelper.identifier;
        launchd.springBoardMetalSelected = enableSpringBoardMetal;
        launchd.springBoardMetalServiceIdentifier = springBoard.identifier;
        launchd.springBoardMetalError = springBoardError;
    } else {
        if (helper) finishJob(helper, ECANCELED);
        if (springBoard) finishJob(springBoard, ECANCELED);
        if (publicHelper) finishJob(publicHelper, ECANCELED);
    }
    --finishingDepth;
    startNextJob();
    return launchd ? 0 : error ? error : EIO;
}

// Keep owned bytes, not pointers into the original main stack or mutable process environment.
static int snapshotRebootInputs(int argc, const char *argv[], const char *envp[]) {
    if (argc < 1 || argc > LSB_MAX_VECTOR) return E2BIG;
    NSMutableArray<NSData *> *arguments = [NSMutableArray new], *environment = [NSMutableArray new];
    size_t total = 0;
    for (unsigned vector = 0; vector < 2; ++vector) {
        const char *const *items = vector ? envp : argv;
        NSMutableArray<NSData *> *destination = vector ? environment : arguments;
        for (unsigned index = 0; items && (vector ? items[index] != NULL : index < (unsigned)argc); ++index) {
            if (index == LSB_MAX_VECTOR || !items[index]) return E2BIG;
            size_t length = strnlen(items[index], LSB_STRING_BYTES);
            if (length == LSB_STRING_BYTES || total + length + 1 > LSB_STRING_BYTES) return E2BIG;
            total += length + 1;
            [destination addObject:[NSData dataWithBytes:items[index] length:length + 1]];
        }
    }
    rebootArguments = arguments;
    rebootEnvironment = environment;
    return 0;
}

static void completeUserspaceReboot(uint64_t generation) {
    if (!sessionDraining || generation != rebootGeneration || finishingDepth || rebootPendingDeaths.count) return;
    for (LSBRunningBoardMonitor *monitor in rebootDeathMonitors) LSBRunningBoardMonitorCancel(monitor);
    rebootDeathMonitors = nil;
    for (dispatch_source_t source in rebootExitSources) dispatch_source_cancel(source);
    rebootExitSources = nil;
    if (rebootDrainFailed || launchLaneFailed || jobs.count) {
        fprintf(stderr, "[broker] Simulator reboot stopped: old session could not be fully retired; reopen host\n");
        finishDeviceChange(EIO);
        return;
    }
    NSString *data = [runtimeRoot.stringByDeletingLastPathComponent stringByAppendingPathComponent:@LSB_DATA_DIRECTORY];
    int queueError = LSBQuarantineSessionControlQueue(data.fileSystemRepresentation);
    if (queueError) {
        rebootDrainFailed = YES;
        fprintf(stderr, "[broker] Could not preserve the old launchctl queue: %s; reopen host\n", strerror(queueError));
        finishDeviceChange(queueError);
        return;
    }
    if (pendingDeviceType) {
        int deviceError = LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(runtimeRoot, pendingDeviceCapabilities);
        if (deviceError) { rebootDrainFailed = YES; finishDeviceChange(deviceError); return; }
        sessionDeviceType = pendingDeviceType;
        LSBSaveConfiguredGuestDeviceType(sessionDeviceType);
    }
    LSBLaunchdConfigurationSetDeviceType(sessionDeviceType);
    const char *arguments[LSB_MAX_VECTOR + 1] = {0}, *environment[LSB_MAX_VECTOR + 1] = {0};
    for (NSUInteger i = 0; i < rebootArguments.count; ++i) arguments[i] = rebootArguments[i].bytes;
    for (NSUInteger i = 0; i < rebootEnvironment.count; ++i) environment[i] = rebootEnvironment[i].bytes;
    LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
    int error = initial ? 0 : ENOMEM;
    uint32_t role = LSB_EXECUTION_SIMULATOR;
    enableIndigo = enableSpringBoardMetal = NO;
    if (!error) error = initialExecutionRole((int)rebootArguments.count, arguments, &role);
    if (!error) error = initialPlan(initial, (int)rebootArguments.count, arguments, environment);
    if (error) {
        if (initial) disposeMessage(initial);
        rebootDrainFailed = YES;
        fprintf(stderr, "[broker] Fresh simulator plan failed: %s; reopen host\n", strerror(error));
        finishDeviceChange(error);
        return;
    }
    // The old session's tracked processes have exited. New requests still require the new generation's full audit identity.
    memset(retiredIdentities, 0, sizeof(retiredIdentities));
    retiredIdentityCount = 0;
    retiredIdentitiesFull = NO;
    sessionDraining = NO;
    fprintf(stderr, "[broker] Starting fresh simulator session after userspace reboot\n");
    error = enqueueInitialJob(initial, role);
    if (error) fprintf(stderr, "[broker] Fresh simulator service setup failed: %s\n", strerror(error));
    finishDeviceChange(error);
}

static void beginUserspaceReboot(LSBJob *exitedLaunchd) {
    if (sessionDraining) return;
    sessionDraining = YES;
    rebootDrainFailed = NO;
    uint64_t generation = ++rebootGeneration;
    rebootPendingDeaths = [NSMutableSet new];
    rebootDeathMonitors = [NSMutableArray new];
    rebootExitSources = [NSMutableArray new];
    NSArray<LSBJob *> *oldJobs = [jobs copy];
    // Transfer existing death monitors out of finishJob's recursive cleanup, preserving already queued death evidence.
    // Removing a job or accepting a signal does not prove exit. Never overlap services from two generations.
    for (LSBJob *job in oldJobs) {
        if (job == exitedLaunchd || job.phase == JobQueued) continue;
        if (!job.pid || !job.audit.val[5]) { rebootDrainFailed = YES; continue; }
        NSNumber *identifier = @(job.identifier);
        [rebootPendingDeaths addObject:identifier];
        if (job.runningBoardMonitor) {
            [rebootDeathMonitors addObject:job.runningBoardMonitor];
            job.runningBoardMonitor = nil;
            continue;
        }
        if (job.exitMonitor) {
            [rebootExitSources addObject:job.exitMonitor];
            job.exitMonitor = nil;
            continue;
        }
        LSBRunningBoardMonitor *monitor = nil;
        audit_token_t audit = job.audit;
        int error = LSBRunningBoardMonitorStart(&audit, ^(int status, int monitorError) {
            (void)status;
            if (generation != rebootGeneration || !sessionDraining) return;
            if (monitorError) rebootDrainFailed = YES;
            [rebootPendingDeaths removeObject:identifier];
            completeUserspaceReboot(generation);
        }, &monitor);
        if (error) {
            rebootDrainFailed = YES;
            [rebootPendingDeaths removeObject:identifier];
            fprintf(stderr, "[broker] Reboot death monitor pid=%d failed: %s\n", job.pid, strerror(error));
        } else [rebootDeathMonitors addObject:monitor];
    }
    ++finishingDepth;
    for (LSBJob *job in oldJobs) if (job != exitedLaunchd && job.pid > 0) {
        int error = signalJob(job, SIGKILL);
        if (error && error != ESRCH) fprintf(stderr, "[broker] Reboot termination pid=%d failed: %s\n", job.pid, strerror(error));
    }
    for (LSBJob *job in oldJobs) finishJob(job, job == exitedLaunchd ? 0 : ECANCELED);
    if (adoptionRetrySource) dispatch_source_cancel(adoptionRetrySource);
    adoptionRetrySource = nil;
    [pendingAdoptions removeAllObjects];
    [hostTerminal close];
    hostTerminal = nil;
    [NSNotificationCenter.defaultCenter postNotificationName:LSBHostTerminalDidChangeNotification object:nil];
    [NSNotificationCenter.defaultCenter postNotificationName:LSBHostIndigoDidChangeNotification object:nil];
    --finishingDepth;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (generation != rebootGeneration || !sessionDraining || !rebootPendingDeaths.count) return;
        rebootDrainFailed = YES;
        fprintf(stderr, "[broker] Simulator reboot timed out waiting for %lu old processes; reopen host\n",
                (unsigned long)rebootPendingDeaths.count);
        for (LSBRunningBoardMonitor *monitor in rebootDeathMonitors) LSBRunningBoardMonitorCancel(monitor);
        rebootDeathMonitors = nil;
        for (dispatch_source_t source in rebootExitSources) dispatch_source_cancel(source);
        rebootExitSources = nil;
        finishDeviceChange(ETIMEDOUT);
    });
    completeUserspaceReboot(generation);
}

int LSBStartHost(int argc, const char *argv[], const char *envp[]) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return EPERM;
        if (jobs) return EALREADY;
        if (argc < 1 || !argv || !argv[0]) return EINVAL;
        int error = snapshotRebootInputs(argc, argv, envp);
        if (!error) error = prepareRuntimeRoot();
        if (!error) {
            sessionDeviceType = LSBConfiguredGuestDeviceType();
            NSDictionary *capabilities = nil;
            error = LSBCopyGuestDeviceCapabilities(runtimeRoot, sessionDeviceType, &capabilities);
            // Preserve command-line and legacy setups without a profile. Selecting another device always requires its staged original.
            if (error == ENOENT && sessionDeviceType == LSBDefaultGuestDeviceType()) error = 0;
            if (!error && capabilities) error = LSBInstallGuestDeviceCapabilitiesWithNativeHaptics(runtimeRoot, capabilities);
            LSBLaunchdConfigurationSetDeviceType(sessionDeviceType);
        }
        if (error) { fprintf(stderr, "[broker] Startup failed: %s\n", strerror(error)); return error; }
        LSBReceivedMessage *initial = calloc(1, sizeof(*initial));
        if (!initial) return ENOMEM;
        uint32_t executionRole = LSB_EXECUTION_SIMULATOR;
        error = initialPlan(initial, argc, argv, envp);
        if (!error) error = initialExecutionRole(argc, argv, &executionRole);
        if (error) { disposeMessage(initial); fprintf(stderr, "[broker] Invalid initial plan: %s\n", strerror(error)); return error; }
        mach_port_t service = MACH_PORT_NULL;
        char serviceName[LSB_BROKER_SERVICE_BYTES];
        error = LSBStoreBrokerServiceName(LSBRuntimeStorage.sharedStorage.appGroupIdentifier.UTF8String, serviceName);
        if (error) { disposeMessage(initial); return error; }
        kern_return_t status = bootstrap_check_in(bootstrap_port, serviceName, &service);
        if (status) { disposeMessage(initial); fprintf(stderr, "[broker] bootstrap_check_in: %d\n", status); return EIO; }
        jobs = [NSMutableArray new];
        hostTerminal.signalHandler = ^int(pid_t foreground, int number) { return signalTerminal(foreground, number); };
        if (hostTerminal) [NSNotificationCenter.defaultCenter postNotificationName:LSBHostTerminalDidChangeNotification object:nil];
        [hostTerminal start];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            unsigned receiveFailures = 0;
            for (;;) {
                LSBReceivedMessage *message = calloc(1, sizeof(*message));
                if (!message) abort();
                int receiveError = LSBTransportReceive(service, message);
                if (receiveError) {
                    free(message);
                    // Report the first failed receive in a burst, then at most once per second under a malformed-message flood.
                    if (!(receiveFailures++ % 100)) fprintf(stderr, "[broker] receive failed: error=%d port=%u\n", receiveError, service);
                    mach_port_type_t type = 0;
                    if (mach_port_type(mach_task_self(), service, &type) || !(type & MACH_PORT_TYPE_RECEIVE)) {
                        fprintf(stderr, "[broker] Receive right is no longer valid; stopping receiver\n");
                        return;
                    }
                    usleep(10000);
                    continue;
                }
                receiveFailures = 0;
                dispatch_async(dispatch_get_main_queue(), ^{ @autoreleasepool { handleMessage(message); } });
            }
        });
        error = enqueueInitialJob(initial, executionRole);
        if (error) fprintf(stderr, "[broker] Initial service setup failed: %s\n", strerror(error));
        return error;
    }
}
