#import "../Host/GuestApplicationInstaller.m"
#include <assert.h>

@interface InstallerDefaults : NSUserDefaults
@property(nonatomic) NSMutableDictionary *values;
@end
@implementation InstallerDefaults
- (instancetype)init { if ((self = [super init])) _values = [NSMutableDictionary new]; return self; }
- (id)objectForKey:(NSString *)key { return self.values[key]; }
- (void)setObject:(id)value forKey:(NSString *)key { self.values[key] = value; }
- (void)removeObjectForKey:(NSString *)key { [self.values removeObjectForKey:key]; }
- (BOOL)synchronize { return YES; }
@end

@interface InstallFixture : NSObject
@property(nonatomic) NSString *base, *root, *queuePath, *source;
@property(nonatomic) InstallerDefaults *defaults;
@property(nonatomic) LSBGuestApplicationInstaller *model;
@property(nonatomic) NSDictionary *receiptOverrides;
@property(nonatomic) int queue, worker;
- (NSString *)awaitRequest;
- (void)complete:(NSString *)identifier result:(NSDictionary *)result exit:(int)exit timedOut:(BOOL)timedOut;
@end
static void pumpUntil(BOOL (^ready)(void)) {
    NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 5;
    while (!ready()) {
        assert(NSProcessInfo.processInfo.systemUptime < deadline);
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
}
static void makeDirectory(NSString *path) {
    assert([NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES
        attributes:@{NSFilePosixPermissions: @0700} error:NULL]);
}
static void writeJSON(int directory, NSString *name, NSDictionary *object) {
    assert(!writeFile(directory, name, [NSJSONSerialization dataWithJSONObject:object options:0 error:NULL], 0600, NULL));
}
@implementation InstallFixture
- (instancetype)init {
    if (!(self = [super init])) return nil;
    char temporary[] = "/tmp/LSB-IPA-client.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    _base = @(canonical);
    _root = [_base stringByAppendingPathComponent:@"data"];
    _queuePath = [_root stringByAppendingPathComponent:@"var/run/launchctl-control"];
    makeDirectory(_queuePath);
    _source = [_base stringByAppendingPathComponent:@"original.ipa"];
    assert([[@"original archive bytes" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:_source atomically:NO]);
    _queue = openQueue(_root);
    assert(_queue >= 0);
    _worker = openat(_queue, "worker.lock", O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    assert(_worker >= 0 && !flock(_worker, LOCK_EX | LOCK_NB));
    _defaults = [InstallerDefaults new];
    _model = [[LSBGuestApplicationInstaller alloc] initWithDataRoot:[NSURL fileURLWithPath:_root] defaults:_defaults timeout:0.3];
    return self;
}
- (void)dealloc {
    assert(!_model.pending);
    close(_worker);
    close(_queue);
    assert([NSFileManager.defaultManager removeItemAtPath:_base error:NULL]);
}
- (NSString *)awaitRequest {
    pumpUntil(^BOOL { return absent(self.queue, @"request") == EEXIST || !self.model.pending; });
    if (!self.model.pending) fprintf(stderr, "Unexpected submission failure: %s\n", self.model.statusText.UTF8String);
    assert(self.model.pending);
    NSData *request = nil;
    assert(!readFile(_queue, @"request", 16384, &request));
    NSArray *arguments = [[[NSString alloc] initWithData:request encoding:NSUTF8StringEncoding] componentsSeparatedByString:@"\0"];
    assert(arguments.count == 4 && validIdentifier(arguments[0]) && [arguments[1] isEqual:InstallCommand] && ![arguments[3] length]);
    NSString *identifier = arguments[0];
    assert([arguments[2] isEqual:stagedPath(_root, identifier)]);
    assert([[NSData dataWithContentsOfFile:arguments[2]] isEqual:[NSData dataWithContentsOfFile:_source]]);
    assert(self.defaults.values.count == 1);
    assert(!renameatx_np(_queue, "request", _queue, "active", RENAME_EXCL));
    return identifier;
}
- (void)complete:(NSString *)identifier result:(NSDictionary *)result exit:(int)exit timedOut:(BOOL)timedOut {
    NSMutableData *log = [NSMutableData dataWithData:[@"ordinary helper output\nLSB_INSTALL_RESULT " dataUsingEncoding:NSUTF8StringEncoding]];
    [log appendData:[NSJSONSerialization dataWithJSONObject:result options:0 error:NULL]];
    [log appendData:[@"\n" dataUsingEncoding:NSUTF8StringEncoding]];
    assert(!writeFile(_queue, [identifier stringByAppendingString:@".log"], log, 0644, NULL));
    NSMutableDictionary *receipt = [@{@"id": identifier, @"pid": @123, @"spawn_error": @0, @"wait_error": @0,
        @"status": @(exit << 8), @"exit": @(exit), @"signal": @0, @"timed_out": timedOut ? @YES : @NO} mutableCopy];
    if (_receiptOverrides) [receipt addEntriesFromDictionary:_receiptOverrides];
    writeJSON(_queue, [identifier stringByAppendingString:@".json"], receipt);
    assert(!unlinkat(_queue, "active", 0));
}
@end
static NSDictionary *successFor(InstallFixture *fixture) {
    NSString *bundle = [fixture.root stringByAppendingPathComponent:@"Containers/Bundle/Application/test-container/Test.app"];
    return @{@"version": @1, @"success": @YES, @"bundleIdentifier": @"com.example.Test", @"bundleURL": [NSURL fileURLWithPath:bundle].absoluteString};
}
static void begin(InstallFixture *fixture) {
    [fixture.model installIPAFromURL:[NSURL fileURLWithPath:fixture.source]];
    assert(fixture.model.pending);
}
static void finish(InstallFixture *fixture) { pumpUntil(^BOOL { return !fixture.model.pending; }); }
static void assertPreservedOriginal(InstallFixture *fixture) {
    assert([[NSString stringWithContentsOfFile:fixture.source encoding:NSUTF8StringEncoding error:NULL] isEqual:@"original archive bytes"]);
}
static void successfulInstallation(void) {
    InstallFixture *fixture = [InstallFixture new];
    begin(fixture);
    NSString *identifier = [fixture awaitRequest];
    [fixture complete:identifier result:successFor(fixture) exit:0 timedOut:NO];
    finish(fixture);
    assert(!fixture.model.outcomeUnknown && [fixture.model.statusText containsString:@"com.example.Test"]);
    assert(absent(fixture.queue, @"client.lock") == 0 && fixture.defaults.values.count == 0);
    assert(![NSFileManager.defaultManager fileExistsAtPath:stagedPath(fixture.root, identifier)]);
    assertPreservedOriginal(fixture);
}
static void rejectedInstallation(void) {
    InstallFixture *fixture = [InstallFixture new];
    begin(fixture);
    NSString *identifier = [fixture awaitRequest];
    [fixture complete:identifier result:@{@"version": @1, @"success": @NO, @"errorDomain": @"GuestInstall", @"errorCode": @7,
        @"errorDescription": @"Rejected test archive"} exit:1 timedOut:NO];
    finish(fixture);
    assert(!fixture.model.outcomeUnknown && [fixture.model.statusText containsString:@"Rejected test archive"]);
    assert(![NSFileManager.defaultManager fileExistsAtPath:stagedPath(fixture.root, identifier)]);
    assertPreservedOriginal(fixture);
}
static void unknownThenLateReadback(void) {
    InstallFixture *fixture = [InstallFixture new];
    begin(fixture);
    NSString *identifier = [fixture awaitRequest];
    finish(fixture);
    assert(fixture.model.outcomeUnknown && fixture.defaults.values.count == 1);
    assert(absent(fixture.queue, @"active") == EEXIST && absent(fixture.queue, @"client.lock") == EEXIST);
    assert([NSFileManager.defaultManager fileExistsAtPath:stagedPath(fixture.root, identifier)]);
    // A fresh host controller recovers the exact ID. Neither a new import attempt nor Refresh republishes it.
    fixture.model = [[LSBGuestApplicationInstaller alloc] initWithDataRoot:[NSURL fileURLWithPath:fixture.root]
        defaults:fixture.defaults timeout:0.3];
    assert(fixture.model.outcomeUnknown);
    [fixture.model installIPAFromURL:[NSURL fileURLWithPath:fixture.source]];
    assert(!fixture.model.pending && absent(fixture.queue, @"request") == 0);
    [fixture.model refresh];
    finish(fixture);
    assert(fixture.model.outcomeUnknown && absent(fixture.queue, @"request") == 0);
    [fixture complete:identifier result:successFor(fixture) exit:0 timedOut:NO];
    [fixture.model refresh];
    finish(fixture);
    assert(!fixture.model.outcomeUnknown && fixture.defaults.values.count == 0);
    assert(absent(fixture.queue, @"request") == 0 && absent(fixture.queue, @"client.lock") == 0);
}
static void malformedAndTimedOutResults(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        @autoreleasepool {
            InstallFixture *fixture = [InstallFixture new];
            begin(fixture);
            NSString *identifier = [fixture awaitRequest];
            NSMutableDictionary *result = [successFor(fixture) mutableCopy];
            if (mode == 0) result[@"success"] = @1;
            if (mode == 1) [result removeObjectForKey:@"bundleURL"];
            if (mode == 2) result[@"bundleURL"] = @"file:///Applications/Native.app";
            [fixture complete:identifier result:result exit:0 timedOut:mode == 3];
            finish(fixture);
            assert(fixture.model.outcomeUnknown && fixture.defaults.values.count == 1);
            assert([NSFileManager.defaultManager fileExistsAtPath:stagedPath(fixture.root, identifier)]);
            assert(absent(fixture.queue, @"client.lock") == EEXIST);
        }
    }
}
static void occupiedQueueAndSourceSymlink(void) {
    InstallFixture *fixture = [InstallFixture new];
    assert(!mkdirat(fixture.queue, "client.lock", 0700));
    int foreign = openDirectory(fixture.queue, "client.lock", NO);
    writeJSON(foreign, @"owner.json", @{@"id": @"foreign-cli", @"arguments": @[@"print", @"user/501"]});
    close(foreign);
    begin(fixture);
    finish(fixture);
    assert(!fixture.model.outcomeUnknown && absent(fixture.queue, @"client.lock") == EEXIST && absent(fixture.queue, @"request") == 0);
    int imports = openImports(fixture.root, NO);
    assert(imports >= 0);
    close(imports);
    assert([NSFileManager.defaultManager contentsOfDirectoryAtPath:[fixture.root stringByAppendingPathComponent:@"Library/LiveSimBooter/Imports"]
        error:NULL].count == 0);
    assert(!unlinkat(fixture.queue, "client.lock/owner.json", 0) && !unlinkat(fixture.queue, "client.lock", AT_REMOVEDIR));
    NSString *alias = [fixture.base stringByAppendingPathComponent:@"alias.ipa"];
    assert(!symlink(fixture.source.fileSystemRepresentation, alias.fileSystemRepresentation));
    [fixture.model installIPAFromURL:[NSURL fileURLWithPath:alias]];
    finish(fixture);
    assert(!fixture.model.outcomeUnknown && absent(fixture.queue, @"request") == 0);
    assertPreservedOriginal(fixture);
}
static void overflowingReceiptRemainsUnknown(void) {
    InstallFixture *fixture = [InstallFixture new];
    begin(fixture);
    NSString *identifier = [fixture awaitRequest];
    // This used to truncate to exit 0. A malformed success receipt must never remove staging or release the lock.
    fixture.receiptOverrides = @{@"exit": @(UINT64_C(0x100000000))};
    [fixture complete:identifier result:successFor(fixture) exit:0 timedOut:NO];
    finish(fixture);
    assert(fixture.model.outcomeUnknown && fixture.defaults.values.count == 1);
    assert([NSFileManager.defaultManager fileExistsAtPath:stagedPath(fixture.root, identifier)]);
    assert(absent(fixture.queue, @"client.lock") == EEXIST && absent(fixture.queue, @"request") == 0);
    assertPreservedOriginal(fixture);
}
static void noWorkerAndUnsafeReceipt(void) {
    InstallFixture *fixture = [InstallFixture new];
    assert(!flock(fixture.worker, LOCK_UN));
    begin(fixture);
    finish(fixture);
    assert(!fixture.model.outcomeUnknown && absent(fixture.queue, @"request") == 0);
    assert(!flock(fixture.worker, LOCK_EX | LOCK_NB));
    begin(fixture);
    NSString *identifier = [fixture awaitRequest];
    NSString *name = [identifier stringByAppendingString:@".json"];
    assert(!symlinkat(fixture.source.fileSystemRepresentation, fixture.queue, name.UTF8String));
    finish(fixture);
    assert(fixture.model.outcomeUnknown && absent(fixture.queue, @"active") == EEXIST && absent(fixture.queue, @"client.lock") == EEXIST);
    assertPreservedOriginal(fixture);
}
static void cleanupIsIdempotent(void) {
    InstallFixture *fixture = [InstallFixture new];
    begin(fixture);
    NSString *identifier = [fixture awaitRequest];
    finish(fixture);
    NSDictionary *record = fixture.defaults.values.allValues.firstObject;
    assert(!cleanupStage(fixture.root, record) && !cleanupStage(fixture.root, record));
    NSString *path = stagedPath(fixture.root, identifier);
    assert(!releaseLock(fixture.queue, identifier, path) && !releaseLock(fixture.queue, identifier, path));
    [fixture complete:identifier result:successFor(fixture) exit:0 timedOut:NO];
    [fixture.model refresh];
    finish(fixture);
    assert(!fixture.model.outcomeUnknown && fixture.defaults.values.count == 0);
}
static void partialCleanupRetry(void) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        @autoreleasepool {
            InstallFixture *fixture = [InstallFixture new];
            begin(fixture);
            NSString *identifier = [fixture awaitRequest];
            NSString *blockedDirectory = mode ? [fixture.queuePath stringByAppendingPathComponent:@"client.lock"] :
                [stagedPath(fixture.root, identifier) stringByDeletingLastPathComponent];
            int blocked = openDirectory(AT_FDCWD, blockedDirectory.fileSystemRepresentation, NO);
            assert(blocked >= 0);
            assert(!writeFile(blocked, @"preserve-me", [NSData data], 0600, NULL));
            [fixture complete:identifier result:successFor(fixture) exit:0 timedOut:NO];
            finish(fixture);
            assert(fixture.model.outcomeUnknown && fixture.defaults.values.count == 1);
            assert([fixture.model.statusText containsString:@"cleanup failed"]);
            assert(absent(fixture.queue, @"client.lock") == EEXIST);
            // Failed rmdir after marker removal must restore ownership; a failed stage cleanup must keep the lock too.
            int lock = openDirectory(fixture.queue, "client.lock", NO);
            NSData *owner = nil;
            assert(lock >= 0 && !readFile(lock, @"owner.json", 4096, &owner));
            assert([[[NSJSONSerialization JSONObjectWithData:owner options:0 error:NULL] objectForKey:@"id"] isEqual:identifier]);
            close(lock);
            assert(!unlinkat(blocked, "preserve-me", 0));
            close(blocked);
            [fixture.model refresh];
            finish(fixture);
            assert(!fixture.model.outcomeUnknown && fixture.defaults.values.count == 0);
            assert(absent(fixture.queue, @"client.lock") == 0 && absent(fixture.queue, @"request") == 0);
            assert(![NSFileManager.defaultManager fileExistsAtPath:stagedPath(fixture.root, identifier)]);
            assertPreservedOriginal(fixture);
        }
    }
}
int main(void) {
    @autoreleasepool {
        successfulInstallation();
        rejectedInstallation();
        unknownThenLateReadback();
        malformedAndTimedOutResults();
        overflowingReceiptRemainsUnknown();
        occupiedQueueAndSourceSymlink();
        noWorkerAndUnsafeReceipt();
        cleanupIsIdempotent();
        partialCleanupRetry();
        puts("GuestApplicationInstaller tests passed");
    }
    return 0;
}
