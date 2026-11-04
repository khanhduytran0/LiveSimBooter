#import "ApplicationRegistrationPlan.h"
#import "LaunchServicesRegistration.h"
#import "LaunchServicesReadProtocol.h"

#include <TargetConditionals.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !TARGET_OS_SIMULATOR
#error This registration tool is only for the simulator guest.
#endif

extern uint32_t dyld_get_active_platform(void);

static void timedOut(int signalNumber) {
    (void)signalNumber;
    static const char message[] = "[app-registration] timeout: current registration or verification exceeded 30 seconds\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void reportError(NSString *path, NSError *error) {
    fprintf(stderr, "[app-registration] FAIL path=%s domain=%s error=%ld detail=%s\n", path.UTF8String,
            error.domain.UTF8String ?: "unknown", (long)error.code, error.localizedDescription.UTF8String ?: "unknown");
}

static BOOL guestRegistrationServiceReady(NSError **error) {
    // An offline LS database may still open locally. Require a reply from the selected simulator's live lsd before using it.
    id<LSBMachServiceConnectionInitializer> allocated = (id)[NSXPCConnection alloc];
    NSXPCConnection *connection = (id)[allocated initWithMachServiceName:@"com.apple.lsd.mapdb" options:0];
    connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(LSBLaunchServicesReadProtocol)];
    [connection resume];
    __block NSError *failure = nil;
    __block BOOL replied = NO;
    id<LSBLaunchServicesReadProtocol> proxy = [connection synchronousRemoteObjectProxyWithErrorHandler:^(NSError *cause) { failure = cause; }];
    [proxy getServerStatusWithCompletionHandler:^(uint32_t status) { (void)status; replied = YES; }];
    [connection invalidate];
    if (!replied || failure) {
        if (error) *error = failure ?: [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOTCONN userInfo:nil];
        return NO;
    }
    return YES;
}

static NSDictionary<NSString *, NSURL *> *guestApplications(NSError **error) {
    __block NSError *failure = nil;
    LSEnumerator *enumerator = [LSEnumerator enumeratorForApplicationProxiesWithOptions:0];
    if (!enumerator) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
        return nil;
    }
    enumerator.errorHandler = ^(NSError *cause) { failure = cause; };
    NSMutableDictionary *records = [NSMutableDictionary dictionary];
    for (LSApplicationProxy *app in enumerator) {
        NSString *identifier = app.bundleIdentifier;
        NSURL *url = app.bundleURL;
        if (!identifier.length || !url.isFileURL || (records[identifier] && ![records[identifier] isEqual:url])) {
            failure = [NSError errorWithDomain:NSPOSIXErrorDomain code:EPROTO userInfo:nil];
            break;
        }
        records[identifier] = url;
    }
    if (failure) { if (error) *error = failure; return nil; }
    return records;
}

int main(int argc, const char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *rootBytes = getenv("DYLD_ROOT_PATH"), *simulatorRoot = getenv("SIMULATOR_ROOT");
    if (dyld_get_active_platform() != 7 || !rootBytes || rootBytes[0] != '/' || !simulatorRoot || strcmp(rootBytes, simulatorRoot)) {
        fprintf(stderr, "[app-registration] requires matching simulator DYLD_ROOT_PATH and SIMULATOR_ROOT\n");
        return 2;
    }
    BOOL dpkg = argc == 2 && !strcmp(argv[1], "--dpkg-trigger");
    if (!dpkg && (argc != 3 || strcmp(argv[1], "--manifest"))) {
        fprintf(stderr, "usage: app-registration --manifest <guest-path> | --dpkg-trigger\n");
        return 2;
    }
    struct sigaction action = {0};
    action.sa_handler = timedOut;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) return 2;
    @autoreleasepool {
        @try {
            NSString *root = [NSString stringWithUTF8String:rootBytes];
            NSError *error = nil;
            if (dpkg) {
                NSDictionary *environment = NSProcessInfo.processInfo.environment;
                __block BOOL ready = NO;
                BOOL ok = LSBRefreshDpkgApplications(root, environment[@"SIMULATOR_SHARED_RESOURCES_DIRECTORY"], environment[@"DPKG_ROOT"],
                    ^NSDictionary *(NSError **cause) {
                        alarm(30);
                        if (!ready) ready = guestRegistrationServiceReady(cause);
                        return ready ? guestApplications(cause) : nil;
                    },
                    ^BOOL(NSString *identifier, NSURL *url, BOOL removing, NSError **cause) {
                        alarm(30);
                        LSApplicationProxy *record = [LSApplicationProxy applicationProxyForIdentifier:identifier];
                        NSURL *current = record.bundleURL;
                        fprintf(stderr, "[app-registration] %s identifier=%s url=%s existing=%s installed=%d\n",
                                removing ? "unregister" : "register", identifier.UTF8String, url.path.fileSystemRepresentation,
                                current.path.fileSystemRepresentation ?: "(none)", record.isInstalled);
                        if (current) {
                            NSString *existing = current.path;
                            if ([existing hasPrefix:@"/var/"]) existing = [@"/private" stringByAppendingString:existing];
                            if (!current.isFileURL || ![existing isEqual:url.path]) {
                                if (cause) *cause = [NSError errorWithDomain:NSPOSIXErrorDomain code:EEXIST
                                    userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"%@ already refers to %@", identifier, current]}];
                                return NO;
                            }
                        }
                        LSApplicationWorkspace *workspace = [LSApplicationWorkspace defaultWorkspace];
                        BOOL changed = removing ? [workspace unregisterApplication:url] :
                            [workspace registerApplicationDictionary:@{@"CFBundleIdentifier": identifier, @"Path": url.path}];
                        if (!changed) {
                            NSString *method = removing ? @"unregisterApplication:" : @"registerApplicationDictionary:";
                            if (cause) *cause = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:@{NSLocalizedDescriptionKey:
                                [NSString stringWithFormat:@"guest LSApplicationWorkspace %@ returned NO for %@", method, url.path]}];
                            return NO;
                        }
                        if (removing) return YES;
                        record = [LSApplicationProxy applicationProxyForIdentifier:identifier];
                        return LSBVerifyApplicationRegistration(url, identifier, record.bundleURL, record.bundleIdentifier, record.isInstalled, cause);
                    }, &error);
                alarm(0);
                if (!ok) reportError(@"--dpkg-trigger", error);
                return ok ? 0 : 1;
            }
            NSString *manifestPath = [NSString stringWithUTF8String:argv[2]];
            NSArray<NSString *> *paths = LSBReadApplicationRegistrationManifest(root, manifestPath, &error);
            if (!paths) { reportError(manifestPath ?: @"<invalid>", error); return 2; }
            printf("[app-registration] begin selected=%lu guest-pid=%d\n", (unsigned long)paths.count, getpid());
            alarm(30);
            LSApplicationWorkspace *workspace = [LSApplicationWorkspace defaultWorkspace];
            alarm(0);
            if (!workspace) return 2;
            unsigned registered = 0, already = 0, skipped = 0, failed = 0;
            for (NSString *path in paths) {
                @autoreleasepool {
                    NSString *identifier = nil, *skipReason = nil;
                    NSURL *url = LSBPrepareApplicationRegistration(root, path, &identifier, &skipReason, &error);
                    if (!url) {
                        if (skipReason) {
                            printf("[app-registration] SKIP path=%s reason=%s\n", path.UTF8String, skipReason.UTF8String);
                            ++skipped;
                        } else { reportError(path, error); ++failed; }
                        continue;
                    }
                    printf("[app-registration] CHECK path=%s identifier=%s\n", path.UTF8String, identifier.UTF8String);
                    alarm(30);
                    LSApplicationProxy *existing = [LSApplicationProxy applicationProxyForIdentifier:identifier];
                    BOOL matching = LSBVerifyApplicationRegistration(url, identifier, existing.bundleURL, existing.bundleIdentifier,
                                                                    existing.isInstalled, NULL);
                    alarm(0);
                    if (matching) {
                        printf("[app-registration] ALREADY identifier=%s\n", identifier.UTF8String);
                        ++already;
                        continue;
                    }
                    printf("[app-registration] REGISTER path=%s identifier=%s\n", path.UTF8String, identifier.UTF8String);
                    alarm(30);
                    BOOL success = [workspace registerApplication:url];
                    if (success) {
                        LSApplicationProxy *record = [LSApplicationProxy applicationProxyForIdentifier:identifier];
                        success = LSBVerifyApplicationRegistration(url, identifier, record.bundleURL, record.bundleIdentifier, record.isInstalled, &error);
                    } else {
                        error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO
                            userInfo:@{NSLocalizedDescriptionKey: @"registerApplication: returned NO"}];
                    }
                    alarm(0);
                    if (success) { printf("[app-registration] OK identifier=%s\n", identifier.UTF8String); ++registered; }
                    else { reportError(path, error); ++failed; }
                }
            }
            printf("[app-registration] complete selected=%lu registered=%u already=%u skipped=%u failed=%u\n",
                   (unsigned long)paths.count, registered, already, skipped, failed);
            return failed ? 1 : 0;
        } @catch (NSException *exception) {
            alarm(0);
            fprintf(stderr, "[app-registration] exception %s: %s\n", exception.name.UTF8String, exception.reason.UTF8String);
            return 1;
        }
    }
}
