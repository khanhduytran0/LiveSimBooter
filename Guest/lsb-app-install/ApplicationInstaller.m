#import "ApplicationInstallation.h"
#import "../../ProcessSupport/RuntimeLayout.h"

#include <TargetConditionals.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if !TARGET_OS_SIMULATOR && !defined(LSB_APPLICATION_INSTALLER_TESTING)
#error This installer is only for the simulator guest.
#endif

extern uint32_t dyld_get_active_platform(void);

static id fail(NSError **error, int code, NSString *description) {
    if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey: description}];
    return nil;
}

static BOOL validString(id value) {
    return [value isKindOfClass:NSString.class] && [value length] && [value canBeConvertedToEncoding:NSUTF8StringEncoding] &&
        [value rangeOfString:@"\0"].location == NSNotFound && [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] < PATH_MAX;
}

static NSString *canonicalPath(NSString *path, NSError **error) {
    if (!validString(path) || !path.isAbsolutePath) return fail(error, EINVAL, @"An absolute UTF-8 path is required.");
    char resolved[PATH_MAX];
    if (!realpath(path.fileSystemRepresentation, resolved)) return fail(error, errno, @"The requested path could not be resolved.");
    return [NSFileManager.defaultManager stringWithFileSystemRepresentation:resolved length:strlen(resolved)];
}

static BOOL directoryExists(NSString *path) {
    struct stat metadata;
    return !stat(path.fileSystemRepresentation, &metadata) && S_ISDIR(metadata.st_mode);
}

static NSString *guestDataRoot(NSError **error) {
    const char *root = getenv("DYLD_ROOT_PATH"), *simulator = getenv("SIMULATOR_ROOT"), *data = getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY");
    if (dyld_get_active_platform() != 7 || !root || !simulator || !data)
        return fail(error, EINVAL, @"Installation requires the simulator guest and configured runtime/data roots.");
    NSString *runtime = canonicalPath([NSString stringWithUTF8String:root], error);
    NSString *simulatorRoot = canonicalPath([NSString stringWithUTF8String:simulator], error);
    NSString *dataRoot = canonicalPath([NSString stringWithUTF8String:data], error);
    if (!runtime || !simulatorRoot || !dataRoot) return nil;
    NSString *expectedData = [runtime.stringByDeletingLastPathComponent stringByAppendingPathComponent:@LSB_DATA_DIRECTORY];
    if (![runtime isEqual:simulatorRoot] || ![runtime.lastPathComponent isEqual:@LSB_RUNTIME_DIRECTORY] ||
        ![runtime.stringByDeletingLastPathComponent.lastPathComponent isEqual:@LSB_RUNTIME_BUILD] ||
        ![dataRoot isEqual:expectedData] || !directoryExists(runtime) || !directoryExists(dataRoot))
        return fail(error, EINVAL, @"Installation requires matching 23A343 RuntimeRoot and sibling data directories.");
    return dataRoot;
}

static BOOL importIdentifier(NSString *identifier) {
    if (identifier.length != 32) return NO;
    const char *bytes = identifier.UTF8String;
    if (!bytes || strlen(bytes) != 32) return NO;
    for (unsigned i = 0; i < 32; ++i)
        if (!((bytes[i] >= '0' && bytes[i] <= '9') || (bytes[i] >= 'a' && bytes[i] <= 'f'))) return NO;
    return YES;
}

// The host owns a unique staged import. Walk every component without following symlinks, and retain this FD through completion.
static NSURL *openImport(NSString *source, NSString *dataRoot, int *descriptor, struct stat *snapshot, NSError **error) {
    *descriptor = -1;
    NSString *canonical = canonicalPath(source, error);
    if (!canonical) return nil;
    NSString *prefix = [[dataRoot stringByAppendingPathComponent:@"Library/LiveSimBooter/Imports"] stringByAppendingString:@"/"];
    if (![canonical hasPrefix:prefix]) return fail(error, EPERM, @"The archive is outside this simulator's staged imports.");
    NSArray<NSString *> *tail = [[canonical substringFromIndex:prefix.length] componentsSeparatedByString:@"/"];
    if (tail.count != 2 || !importIdentifier(tail[0]) || ![tail[1] isEqual:@"payload.ipa"])
        return fail(error, EINVAL, @"The archive must be Imports/<32 lowercase hex digits>/payload.ipa.");
    // Permit only the filesystem's root alias (for example /var → /private/var), not aliases within the import tree.
    NSString *expectedSuffix = [@"/Library/LiveSimBooter/Imports/" stringByAppendingPathComponent:[tail componentsJoinedByString:@"/"]];
    if (![source hasSuffix:expectedSuffix]) return fail(error, ELOOP, @"The staged archive path must not contain aliases or traversal components.");
    NSString *lexicalData = [source substringToIndex:source.length - expectedSuffix.length];
    if (![canonicalPath(lexicalData, error) isEqual:dataRoot])
        return fail(error, ELOOP, @"The staged archive path must not contain aliases or traversal components.");
    int fd = open(dataRoot.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return fail(error, errno, @"The simulator data directory could not be opened.");
    NSArray<NSString *> *parts = @[@"Library", @"LiveSimBooter", @"Imports", tail[0], @"payload.ipa"];
    for (NSUInteger i = 0; i < parts.count; ++i) {
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (i + 1 < parts.count) flags |= O_DIRECTORY;
        int next = openat(fd, parts[i].fileSystemRepresentation, flags);
        int code = next < 0 ? errno : 0;
        close(fd);
        if (next < 0) return fail(error, code, @"The staged archive path is not a readable file without symlinks.");
        fd = next;
    }
    int code = fstat(fd, snapshot) ? errno : 0;
    if (!code && (!S_ISREG(snapshot->st_mode) || snapshot->st_size <= 0)) code = EINVAL;
    if (code) { close(fd); return fail(error, code, @"The staged archive must be a nonempty regular file."); }
    *descriptor = fd;
    return [NSURL fileURLWithPath:canonical];
}

static BOOL sameSource(int descriptor, NSURL *url, const struct stat *snapshot) {
    struct stat current, named;
    if (fstat(descriptor, &current) || lstat(url.fileSystemRepresentation, &named)) return NO;
    return S_ISREG(named.st_mode) && current.st_dev == named.st_dev && current.st_ino == named.st_ino &&
        current.st_dev == snapshot->st_dev && current.st_ino == snapshot->st_ino && current.st_size == snapshot->st_size &&
        current.st_mtimespec.tv_sec == snapshot->st_mtimespec.tv_sec && current.st_mtimespec.tv_nsec == snapshot->st_mtimespec.tv_nsec &&
        current.st_ctimespec.tv_sec == snapshot->st_ctimespec.tv_sec && current.st_ctimespec.tv_nsec == snapshot->st_ctimespec.tv_nsec;
}

static NSURL *installedURL(NSURL *url, NSString *dataRoot, NSString *relativeRoot, NSError **error) {
    if (![url isKindOfClass:NSURL.class] || !url.isFileURL) return fail(error, EIO, @"LaunchServices did not return an installed file URL.");
    NSString *canonical = canonicalPath(url.path, error);
    if (!canonical) return nil;
    NSString *prefix = [[dataRoot stringByAppendingPathComponent:relativeRoot] stringByAppendingString:@"/"];
    if (![canonical hasPrefix:prefix] || !directoryExists(canonical))
        return fail(error, EPERM, @"LaunchServices returned a container outside the selected simulator data root.");
    return [NSURL fileURLWithPath:canonical isDirectory:YES];
}

static NSDictionary *inspectApplication(NSString *identifier, NSString *dataRoot, NSError **error) {
    if (!validString(identifier)) return fail(error, EINVAL, @"The installation did not return a valid bundle identifier.");
    LSApplicationRecord *record = [[LSApplicationRecord alloc] initWithBundleIdentifier:identifier allowPlaceholder:NO error:error];
    if (!record) return error && *error ? nil : fail(error, ENOENT, @"The installed application has no LaunchServices record.");
    if (![record.bundleIdentifier isEqual:identifier]) return fail(error, EIO, @"The installed record has a different bundle identifier.");
    NSURL *bundle = installedURL(record.URL, dataRoot, @"Containers/Bundle/Application", error);
    if (!bundle) return nil;
    NSMutableDictionary *result = [@{@"bundleIdentifier": identifier, @"bundleURL": bundle.absoluteString} mutableCopy];
    if (record.dataContainerURL) {
        NSURL *data = installedURL(record.dataContainerURL, dataRoot, @"Containers/Data/Application", error);
        if (!data) return nil;
        result[@"dataURL"] = data.absoluteString;
    }
    return result;
}

static NSDictionary *installApplication(NSString *source, NSString *dataRoot, NSError **error) {
    struct stat snapshot;
    int descriptor = -1;
    NSURL *url = openImport(source, dataRoot, &descriptor, &snapshot, error);
    if (!url) return nil;
    @try {
        MIInstallOptions *options = [MIInstallOptions new];
        options.installTargetType = 1;
        options.developerInstall = YES;
        options.userInitiated = YES;
        dispatch_group_t group = dispatch_group_create();
        __block NSString *identifier = nil;
        __block NSError *installationError = nil;
        dispatch_group_enter(group);
        // Same typed call as CoreSimulatorBridge. The control worker imposes the outer timeout; this call is never replayed.
        [IXAppInstallCoordinator installApplication:url consumeSource:NO options:options completion:^(NSString *bundleID, NSError *failure) {
            identifier = [bundleID copy];
            installationError = failure;
            dispatch_group_leave(group);
        }];
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
        if (installationError) { if (error) *error = installationError; return nil; }
        if (!sameSource(descriptor, url, &snapshot))
            return fail(error, ESTALE, @"The staged archive changed during installation; inspect the installed state before taking further action.");
        return inspectApplication(identifier, dataRoot, error);
    } @finally { close(descriptor); }
}

static void reportErrorChain(NSError *error) {
    // Only normal NSError fields and the standard underlying chain; do not dump arbitrary userInfo objects or credentials.
    for (unsigned depth = 0; depth < 6 && [error isKindOfClass:NSError.class]; ++depth) {
        NSString *description = [error.localizedDescription stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"];
        fprintf(stderr, "[app-install] error-depth=%u domain=%.256s code=%ld description=%.2048s\n", depth,
                error.domain.UTF8String ?: "unknown", (long)error.code, description.UTF8String ?: "unknown");
        id underlying = error.userInfo[NSUnderlyingErrorKey];
        error = [underlying isKindOfClass:NSError.class] ? underlying : nil;
    }
}

static int reportResult(NSDictionary *installed, NSError *error) {
    NSMutableDictionary *result = installed ? [installed mutableCopy] : [NSMutableDictionary dictionary];
    result[@"version"] = @1;
    result[@"success"] = installed ? @YES : @NO;
    if (!installed) {
        if (!error) error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:@{NSLocalizedDescriptionKey: @"Installation failed."}];
        result[@"errorDomain"] = error.domain;
        result[@"errorCode"] = @(error.code);
        result[@"errorDescription"] = error.localizedDescription;
        reportErrorChain(error);
    }
    NSData *json = [NSJSONSerialization dataWithJSONObject:result options:0 error:NULL];
    if (!json) return 1;
    printf("LSB_INSTALL_RESULT ");
    if (fwrite(json.bytes, 1, json.length, stdout) != json.length || putchar('\n') == EOF || fflush(stdout)) return 1;
    return installed ? 0 : 1;
}

int main(int argc, const char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool {
        @try {
            NSError *error = nil;
            NSString *dataRoot = guestDataRoot(&error);
            if (!dataRoot) return reportResult(nil, error);
            NSDictionary *installed = nil;
            if (argc == 2) installed = installApplication([NSString stringWithUTF8String:argv[1]], dataRoot, &error);
            else if (argc == 3 && !strcmp(argv[1], "--inspect"))
                installed = inspectApplication([NSString stringWithUTF8String:argv[2]], dataRoot, &error);
            else fail(&error, EINVAL, @"Usage: lsb-app-install /absolute/staged/payload.ipa or lsb-app-install --inspect <bundle-id>");
            return reportResult(installed, error);
        } @catch (NSException *exception) {
            NSError *error = [NSError errorWithDomain:@"LSBApplicationInstallerException" code:1
                userInfo:@{NSLocalizedDescriptionKey: exception.reason ?: exception.name}];
            return reportResult(nil, error);
        }
    }
}
