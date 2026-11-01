#import "GuestApplicationInstaller.h"
#import "RuntimeStorage.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

NSNotificationName const LSBGuestApplicationInstallerDidChangeNotification = @"LSBGuestApplicationInstallerDidChange";
static NSString *const PendingPreference = @"LSBPendingIPAInstall";
static NSString *const InstallCommand = @"--lsb-install-ipa";
static const NSUInteger MaximumLogBytes = 16 * 1024 * 1024;

static BOOL validIdentifier(id value) {
    if (![value isKindOfClass:NSString.class] || [value length] != 32) return NO;
    const char *bytes = [value UTF8String];
    if (strlen(bytes) != 32) return NO;
    for (unsigned i = 0; i < 32; ++i) if (!((bytes[i] >= '0' && bytes[i] <= '9') || (bytes[i] >= 'a' && bytes[i] <= 'f'))) return NO;
    return YES;
}
static BOOL integerValue(id value) {
    return [value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) == CFNumberGetTypeID() &&
        !CFNumberIsFloatType((__bridge CFNumberRef)value);
}
static BOOL integerInRange(id value, int minimum, int maximum) {
    return integerValue(value) && [value compare:@(minimum)] != NSOrderedAscending && [value compare:@(maximum)] != NSOrderedDescending;
}
static int openDirectory(int parent, const char *name, BOOL create) {
    if (create && mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
    int descriptor = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return -1;
    struct stat metadata;
    int error = fstat(descriptor, &metadata) ? errno : 0;
    if (!error && (!S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid() || (metadata.st_mode & 0022))) error = EACCES;
    if (error) { close(descriptor); errno = error; return -1; }
    return descriptor;
}
static int descend(int directory, const char *const components[], unsigned count, BOOL create) {
    for (unsigned i = 0; directory >= 0 && i < count; ++i) {
        int next = openDirectory(directory, components[i], create), error = errno;
        close(directory);
        directory = next;
        errno = error;
    }
    return directory;
}
static int openImports(NSString *root, BOOL create) {
    const char *components[] = {"Library", "LiveSimBooter", "Imports"};
    return descend(openDirectory(AT_FDCWD, root.fileSystemRepresentation, NO), components, 3, create);
}
static int openQueue(NSString *root) {
    const char *components[] = {"var", "run", "launchctl-control"};
    return descend(openDirectory(AT_FDCWD, root.fileSystemRepresentation, NO), components, 3, NO);
}
static int regularFile(int descriptor, struct stat *metadata) {
    if (fstat(descriptor, metadata)) return errno;
    if (!S_ISREG(metadata->st_mode) || metadata->st_nlink != 1 || (metadata->st_uid != geteuid() && metadata->st_uid != 0)) return EINVAL;
    return 0;
}
static int readFile(int directory, NSString *name, NSUInteger limit, NSData **output) {
    *output = nil;
    int descriptor = openat(directory, name.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) return errno;
    struct stat metadata;
    int error = regularFile(descriptor, &metadata);
    if (!error && (metadata.st_size < 0 || (uint64_t)metadata.st_size > limit)) error = EFBIG;
    NSMutableData *data = !error ? [NSMutableData dataWithLength:(NSUInteger)metadata.st_size] : nil;
    NSUInteger offset = 0;
    while (!error && offset < data.length) {
        ssize_t count = read(descriptor, (char *)data.mutableBytes + offset, data.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) error = count < 0 ? errno : EIO;
        else offset += (NSUInteger)count;
    }
    struct stat after;
    if (!error && fstat(descriptor, &after)) error = errno;
    if (!error && (metadata.st_size != after.st_size || metadata.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
        metadata.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec)) error = EAGAIN;
    close(descriptor);
    if (!error) *output = data;
    return error;
}
static int writeFile(int directory, NSString *name, NSData *data, mode_t mode, BOOL *created) {
    if (created) *created = NO;
    int descriptor = openat(directory, name.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (descriptor < 0) return errno;
    if (created) *created = YES;
    int error = 0;
    NSUInteger offset = 0;
    while (offset < data.length) {
        ssize_t count = write(descriptor, (const char *)data.bytes + offset, data.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        offset += (NSUInteger)count;
    }
    if (!error && fsync(descriptor)) error = errno;
    if (close(descriptor) && !error) error = errno;
    return error;
}
static int absent(int directory, NSString *name) {
    struct stat metadata;
    if (!fstatat(directory, name.fileSystemRepresentation, &metadata, AT_SYMLINK_NOFOLLOW)) return EEXIST;
    return errno == ENOENT ? 0 : errno;
}
static int workerReady(int queue) {
    int descriptor = openat(queue, "worker.lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) return errno;
    struct stat metadata;
    int error = regularFile(descriptor, &metadata);
    if (!error) {
        if (!flock(descriptor, LOCK_EX | LOCK_NB)) error = ENOTCONN;
        else if (errno != EWOULDBLOCK && errno != EAGAIN) error = errno;
    }
    close(descriptor);
    return error;
}
static NSString *stagedPath(NSString *root, NSString *identifier) {
    return [[root stringByAppendingPathComponent:@"Library/LiveSimBooter/Imports"]
        stringByAppendingPathComponent:[identifier stringByAppendingPathComponent:@"payload.ipa"]];
}
static int releaseLock(int queue, NSString *identifier, NSString *path) {
    int lock = openDirectory(queue, "client.lock", NO);
    if (lock < 0) return errno == ENOENT ? 0 : errno;
    NSData *data = nil;
    int error = readFile(lock, @"owner.json", 4096, &data);
    id owner = !error ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    if (!error && (![owner isKindOfClass:NSDictionary.class] || ![owner[@"id"] isEqual:identifier] ||
        ![owner[@"arguments"] isEqual:@[InstallCommand, path]])) error = EACCES;
    struct stat opened, named;
    if (!error && (fstat(lock, &opened) || fstatat(queue, "client.lock", &named, AT_SYMLINK_NOFOLLOW))) error = errno;
    if (!error && (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)) error = ESTALE;
    if (!error && unlinkat(lock, "owner.json", 0)) error = errno;
    if (!error && unlinkat(queue, "client.lock", AT_REMOVEDIR)) {
        error = errno;
        // Keep the identity recoverable if directory cleanup fails after removing its marker.
        writeFile(lock, @"owner.json", data, 0600, NULL);
    }
    close(lock);
    return error;
}
static void discardNewLock(int queue, int lock) {
    struct stat opened, named;
    if (lock < 0 || fstat(lock, &opened) || fstatat(queue, "client.lock", &named, AT_SYMLINK_NOFOLLOW) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) return;
    // This descriptor belongs to the directory just created by this submission, including an incomplete owner write.
    if (unlinkat(lock, "owner.json", 0) && errno != ENOENT) return;
    unlinkat(queue, "client.lock", AT_REMOVEDIR);
}
static int cleanupStage(NSString *root, NSDictionary *record) {
    NSString *identifier = record[@"id"];
    if (!validIdentifier(identifier)) return EINVAL;
    int imports = openImports(root, NO);
    if (imports < 0) return errno;
    int stage = openDirectory(imports, identifier.UTF8String, NO), error = stage < 0 ? errno : 0;
    if (stage < 0 && error == ENOENT) { close(imports); return 0; }
    struct stat metadata;
    BOOL present = NO;
    if (!error) {
        present = !fstatat(stage, "payload.ipa", &metadata, AT_SYMLINK_NOFOLLOW);
        if (!present && errno != ENOENT) error = errno;
    }
    if (!error && present && (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
        (record[@"inode"] && ((uint64_t)metadata.st_ino != [record[@"inode"] unsignedLongLongValue] ||
         (uint64_t)metadata.st_dev != [record[@"device"] unsignedLongLongValue])))) error = ESTALE;
    if (!error && present && unlinkat(stage, "payload.ipa", 0)) error = errno;
    if (stage >= 0) close(stage);
    if (!error && unlinkat(imports, identifier.UTF8String, AT_REMOVEDIR)) error = errno;
    close(imports);
    return error;
}

static int copyArchive(NSURL *url, NSString *root, NSString *identifier, NSDictionary **record) {
    int imports = openImports(root, YES);
    if (imports < 0) return errno;
    int error = mkdirat(imports, identifier.UTF8String, 0700) ? errno : 0;
    if (!error) *record = @{@"id": identifier, @"root": root};
    int stage = !error ? openDirectory(imports, identifier.UTF8String, NO) : -1;
    if (!error && stage < 0) error = errno;
    close(imports);
    int output = !error ? openat(stage, "payload.ipa", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    if (!error && output < 0) error = errno;
    if (stage >= 0) close(stage);
    if (error) return error;
    BOOL scoped = [url startAccessingSecurityScopedResource];
    __block int copyError = 0;
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSError *coordinationError = nil;
    [coordinator coordinateReadingItemAtURL:url options:0 error:&coordinationError byAccessor:^(NSURL *readURL) {
        int input = open(readURL.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat before, after;
        if (input < 0) { copyError = errno; return; }
        if (fstat(input, &before)) copyError = errno;
        if (!copyError && (!S_ISREG(before.st_mode) || before.st_size <= 0)) copyError = EINVAL;
        char bytes[65536];
        off_t offset = 0;
        while (!copyError && offset < before.st_size) {
            ssize_t count = read(input, bytes, (size_t)MIN((off_t)sizeof(bytes), before.st_size - offset));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { copyError = count < 0 ? errno : EIO; break; }
            ssize_t written = 0;
            while (written < count) {
                ssize_t amount = write(output, bytes + written, (size_t)(count - written));
                if (amount < 0 && errno == EINTR) continue;
                if (amount <= 0) { copyError = amount < 0 ? errno : EIO; break; }
                written += amount;
            }
            offset += count;
        }
        if (!copyError && fstat(input, &after)) copyError = errno;
        if (!copyError && (before.st_size != after.st_size || before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
            before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec)) copyError = EAGAIN;
        close(input);
    }];
    if (scoped) [url stopAccessingSecurityScopedResource];
    error = copyError ?: (coordinationError ? EIO : 0);
    if (!error && fsync(output)) error = errno;
    struct stat metadata = {0};
    if (fstat(output, &metadata) && !error) error = errno;
    if (close(output) && !error) error = errno;
    if (metadata.st_ino) *record = @{@"id": identifier, @"root": root,
        @"device": @((uint64_t)metadata.st_dev), @"inode": @((uint64_t)metadata.st_ino)};
    return error;
}
static NSDictionary *installResult(NSData *log) {
    NSData *prefix = [@"LSB_INSTALL_RESULT " dataUsingEncoding:NSUTF8StringEncoding];
    const unsigned char *bytes = log.bytes;
    NSDictionary *result = nil;
    for (NSUInteger start = 0; start < log.length;) {
        NSUInteger end = start;
        while (end < log.length && bytes[end] != '\n') ++end;
        if (end - start >= prefix.length && !memcmp(bytes + start, prefix.bytes, prefix.length)) {
            if (result) return nil;
            id object = [NSJSONSerialization JSONObjectWithData:[log subdataWithRange:NSMakeRange(start + prefix.length, end - start - prefix.length)]
                options:0 error:NULL];
            if (![object isKindOfClass:NSDictionary.class] || !integerValue(object[@"version"]) || [object[@"version"] integerValue] != 1 ||
                !object[@"success"] || CFGetTypeID((__bridge CFTypeRef)object[@"success"]) != CFBooleanGetTypeID()) return nil;
            for (NSString *key in @[@"bundleIdentifier", @"bundleURL", @"dataURL", @"errorDomain", @"errorDescription"])
                if (object[key] && (![object[key] isKindOfClass:NSString.class] || [object[key] length] > 4096)) return nil;
            if (object[@"errorCode"] && !integerValue(object[@"errorCode"])) return nil;
            if ([object[@"success"] boolValue] && (![object[@"bundleIdentifier"] length] || ![object[@"bundleURL"] length])) return nil;
            result = object;
        }
        start = end + 1;
    }
    return result;
}
static BOOL installedBundleURL(NSString *value, NSString *root) {
    NSURL *url = [NSURL URLWithString:value];
    if (!url.isFileURL || url.query || url.fragment || (url.host.length && ![url.host isEqual:@"localhost"])) return NO;
    NSString *prefix = [[root stringByAppendingPathComponent:@"Containers/Bundle/Application"] stringByAppendingString:@"/"];
    NSString *path = url.path;
    if (![path hasPrefix:prefix] || [path lengthOfBytesUsingEncoding:NSUTF8StringEncoding] != strlen(path.UTF8String)) return NO;
    for (NSString *part in path.pathComponents) if ([part isEqual:@"."] || [part isEqual:@".."]) return NO;
    return YES;
}

@implementation LSBGuestApplicationInstaller {
    NSString *_dataRoot;
    NSString *_preferenceKey;
    NSUserDefaults *_defaults;
    NSDictionary *_record;
    dispatch_queue_t _queue;
    NSTimeInterval _timeout;
}
+ (instancetype)sharedController {
    static LSBGuestApplicationInstaller *controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        controller = [[self alloc] initWithDataRoot:LSBRuntimeStorage.sharedStorage.dataURL defaults:NSUserDefaults.standardUserDefaults timeout:190];
    });
    return controller;
}
- (instancetype)initWithDataRoot:(NSURL *)dataRoot defaults:(NSUserDefaults *)defaults timeout:(NSTimeInterval)timeout {
    if (!(self = [super init])) return nil;
    char canonical[PATH_MAX];
    if (dataRoot.isFileURL && realpath(dataRoot.fileSystemRepresentation, canonical)) _dataRoot = @(canonical);
    _defaults = defaults;
    _preferenceKey = [PendingPreference stringByAppendingFormat:@":%@", _dataRoot ?: @"unavailable"];
    _timeout = MAX(0.05, MIN(timeout, 190));
    _queue = dispatch_queue_create("com.kdt.LiveSimBooter.app-install", DISPATCH_QUEUE_SERIAL);
    id saved = [defaults objectForKey:_preferenceKey];
    if ([saved isKindOfClass:NSDictionary.class] && validIdentifier(saved[@"id"]) && [saved[@"root"] isEqual:_dataRoot] &&
        integerValue(saved[@"device"]) && integerValue(saved[@"inode"]) && [saved[@"inode"] unsignedLongLongValue]) {
        _record = saved;
        _outcomeUnknown = YES;
    }
    _statusText = _outcomeUnknown ? NSLocalizedString(@"The previous installation has no confirmed result. Tap Refresh to check it without repeating it.",
        @"IPA installation outcome is unknown; Refresh only retrieves the existing request.") :
        NSLocalizedString(@"Install an IPA into the running simulator. The original file is preserved.", @"Applications settings instructions.");
    return self;
}
- (void)publish {
    [NSNotificationCenter.defaultCenter postNotificationName:LSBGuestApplicationInstallerDidChangeNotification object:self];
}
- (void)finish:(NSString *)message unknown:(BOOL)unknown {
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_pending = NO;
        self->_outcomeUnknown = unknown;
        self->_statusText = message;
        [self publish];
    });
}
- (void)unknownResult {
    [self finish:NSLocalizedString(@"Installation outcome is unknown. The imported copy is kept. Tap Refresh to check; the request will not be repeated.",
        @"IPA installation timed out or its result could not be verified.") unknown:YES];
}
- (void)pollRecord:(NSDictionary *)record wait:(BOOL)wait {
    NSString *identifier = record[@"id"], *path = stagedPath(_dataRoot, identifier);
    NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + (wait ? _timeout : 0);
    do {
        int queue = openQueue(_dataRoot);
        if (queue < 0) break;
        NSData *statusData = nil, *active = nil;
        int error = readFile(queue, [identifier stringByAppendingString:@".json"], 16384, &statusData);
        NSDictionary *status = !error ? [NSJSONSerialization JSONObjectWithData:statusData options:0 error:NULL] : nil;
        if (!error && (![status isKindOfClass:NSDictionary.class] || ![status[@"id"] isEqual:identifier])) error = EPROTO;
        if (!error && (!integerInRange(status[@"pid"], 0, INT_MAX) || !integerInRange(status[@"spawn_error"], 0, INT_MAX) ||
            !integerInRange(status[@"wait_error"], 0, INT_MAX) || !integerInRange(status[@"status"], 0, INT_MAX) ||
            !integerInRange(status[@"exit"], -1, 255) || !integerInRange(status[@"signal"], 0, NSIG - 1))) error = EPROTO;
        if (!error && (!status[@"timed_out"] || CFGetTypeID((__bridge CFTypeRef)status[@"timed_out"]) != CFBooleanGetTypeID())) error = EPROTO;
        if (!error && ([status[@"wait_error"] intValue] || [status[@"timed_out"] boolValue] || [status[@"signal"] intValue])) error = EINPROGRESS;
        int activeError = !error ? readFile(queue, @"active", 16384, &active) : 0;
        if (!error && !activeError) error = EAGAIN; // The worker publishes status before removing active and closing its operation.
        else if (!error && activeError != ENOENT) error = activeError;
        NSDictionary *result = nil;
        if (!error && ![status[@"spawn_error"] intValue]) {
            NSData *log = nil;
            error = readFile(queue, [identifier stringByAppendingString:@".log"], MaximumLogBytes, &log);
            if (!error) result = installResult(log);
            if (!error && (!result || ([result[@"success"] boolValue] ? [status[@"exit"] intValue] != 0 : [status[@"exit"] intValue] == 0))) error = EPROTO;
            if (!error && [result[@"success"] boolValue] && !installedBundleURL(result[@"bundleURL"], _dataRoot)) error = EPROTO;
        }
        if (!error) {
            BOOL success = [result[@"success"] boolValue];
            NSString *message;
            if (success) message = [NSString stringWithFormat:
                NSLocalizedString(@"Installed %@.", @"IPA installation succeeded; app bundle identifier follows."), result[@"bundleIdentifier"]];
            else message = [NSString stringWithFormat:NSLocalizedString(@"Installation failed: %@", @"IPA installation failure with an error description."),
                result[@"errorDescription"] ?: ([status[@"spawn_error"] intValue] ? @(strerror([status[@"spawn_error"] intValue])) :
                NSLocalizedString(@"The guest installer rejected the application.", @"Fallback guest IPA installation error."))];
            int cleanupError = cleanupStage(_dataRoot, record);
            int lockError = cleanupError ? 0 : releaseLock(queue, identifier, path);
            if (cleanupError || lockError) message = [message stringByAppendingFormat:
                NSLocalizedString(@"\nTemporary import cleanup failed (%d). Tap Refresh to retry cleanup without reinstalling.",
                    @"IPA install completed, but its staging or queue cleanup needs another readback."), cleanupError ?: lockError];
            else {
                [_defaults removeObjectForKey:_preferenceKey];
                [_defaults synchronize];
                _record = nil;
            }
            close(queue);
            [self finish:message unknown:cleanupError || lockError];
            return;
        }
        close(queue);
        if (error != ENOENT && error != EAGAIN) break;
        if (!wait || NSProcessInfo.processInfo.systemUptime >= deadline) break;
        usleep(250000);
    } while (YES);
    [self unknownResult];
}
- (void)installIPAFromURL:(NSURL *)url {
    NSAssert(NSThread.isMainThread, @"Application installation UI state belongs to the main thread");
    if (_pending || _outcomeUnknown || !url.isFileURL) return;
    _pending = YES;
    _statusText = NSLocalizedString(@"Copying IPA…", @"IPA import is copying the selected document into simulator staging.");
    [self publish];
    dispatch_async(_queue, ^{
        NSString *identifier = [[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString];
        NSDictionary *record = nil;
        int queue = self->_dataRoot ? openQueue(self->_dataRoot) : -1;
        int error = queue < 0 ? ENOTCONN : workerReady(queue);
        if (!error) error = copyArchive(url, self->_dataRoot, identifier, &record);
        if (!error) error = workerReady(queue);
        NSString *path = self->_dataRoot ? stagedPath(self->_dataRoot, identifier) : nil;
        BOOL published = NO, temporaryCreated = NO;
        int client = -1;
        NSString *temporary = [NSString stringWithFormat:@".%@.new", identifier];
        if (!error) error = mkdirat(queue, "client.lock", 0700) ? errno : 0;
        if (!error) {
            client = openDirectory(queue, "client.lock", NO);
            error = client < 0 ? errno : writeFile(client, @"owner.json", [NSJSONSerialization dataWithJSONObject:
                @{@"id": identifier, @"arguments": @[InstallCommand, path]} options:0 error:NULL], 0600, NULL);
        }
        for (NSString *name in @[@"request", @"active", [identifier stringByAppendingString:@".json"],
            [identifier stringByAppendingString:@".json.new"], [identifier stringByAppendingString:@".log"], temporary])
            if (!error) error = absent(queue, name);
        if (!error) {
            NSMutableData *payload = [NSMutableData data];
            for (NSString *argument in @[identifier, InstallCommand, path]) {
                [payload appendData:[argument dataUsingEncoding:NSUTF8StringEncoding]];
                unsigned char zero = 0; [payload appendBytes:&zero length:1];
            }
            error = payload.length > 16384 ? E2BIG : writeFile(queue, temporary, payload, 0644, &temporaryCreated);
        }
        if (!error) {
            self->_record = record;
            [self->_defaults setObject:record forKey:self->_preferenceKey];
            if (![self->_defaults synchronize]) error = EIO;
        }
        if (!error) {
            if (linkat(queue, temporary.fileSystemRepresentation, queue, "request", 0)) error = errno;
            else published = YES;
        }
        if (temporaryCreated) unlinkat(queue, temporary.fileSystemRepresentation, 0);
        if (!published) {
            discardNewLock(queue, client);
            if (record) cleanupStage(self->_dataRoot, record);
            [self->_defaults removeObjectForKey:self->_preferenceKey];
            [self->_defaults synchronize];
            self->_record = nil;
            if (client >= 0) close(client);
            if (queue >= 0) close(queue);
            [self finish:[NSString stringWithFormat:NSLocalizedString(@"Could not submit installation: %s (%d).",
                @"IPA request failed before publication: system description and error number."), strerror(error ?: EIO), error ?: EIO] unknown:NO];
            return;
        }
        if (client >= 0) close(client);
        close(queue);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_statusText = NSLocalizedString(@"Installing application…", @"Guest IPA installation is running.");
            [self publish];
        });
        [self pollRecord:record wait:YES];
    });
}
- (void)refresh {
    NSAssert(NSThread.isMainThread, @"Application installation UI state belongs to the main thread");
    if (_pending || !_outcomeUnknown) return;
    _pending = YES;
    _statusText = NSLocalizedString(@"Checking the previous installation…", @"Read back an IPA installation without submitting it again.");
    [self publish];
    dispatch_async(_queue, ^{ if (self->_record) [self pollRecord:self->_record wait:NO]; else [self unknownResult]; });
}
@end
