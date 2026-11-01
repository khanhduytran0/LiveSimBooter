#import <Foundation/Foundation.h>
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../ProcessSupport/PrivateXPC.h"
static void testReplyAudit(xpc_object_t response, audit_token_t *audit);
#define xpc_dictionary_get_audit_token testReplyAudit

static int testFcntl(int fd, int command, ...);
static int testFstat(int fd, struct stat *info);
static int testRename(int fromDirectory, const char *from, int toDirectory, const char *to);
static int testClone(int source, int directory, const char *name, uint32_t flags);
static int testUnlink(int directory, const char *name, int flags);
static ssize_t testPwrite(int fd, const void *bytes, size_t size, off_t offset);
static int testFsync(int fd);
#define fcntl testFcntl
#define fstat testFstat
#define renameat testRename
#define fclonefileat testClone
#define unlinkat testUnlink
#define pwrite testPwrite
#define fsync testFsync
#include "../Host/LazySigning.m"
#undef fcntl
#undef fstat
#undef renameat
#undef fclonefileat
#undef unlinkat
#undef pwrite
#undef fsync
#undef xpc_dictionary_get_audit_token

static int signatureError, renameError, cloneError;
static BOOL shortCoverage, failStatAfterRename, renamed;
static unsigned signatureRegistrations;
static unsigned cleanupFailure, backupRemovals;
enum { FixtureSize = 1024, SignatureOffset = 64, SignatureCapacity = 512 };
static audit_token_t replyAudit;
static void testReplyAudit(xpc_object_t response, audit_token_t *audit) { (void)response; *audit = replyAudit; }
static unsigned signatureIOCase, rangeWrites;
static size_t stageBytesWritten;
static BOOL stageCloneAttempted, stageCloneSucceeded, stageSynced;
static int originalDescriptor = -1;
static off_t originalInputOffset;
static NSString *testRoot;

static int testClone(int source, int directory, const char *name, uint32_t flags) {
    assert(flags == CLONE_NOOWNERCOPY);
    BOOL staging = [@(name) hasPrefix:@".lsb-sign-"];
    if (staging) { stageCloneAttempted = YES; assert(source == originalDescriptor); }
    int result;
    if (cloneError) { errno = cloneError; result = -1; }
    else result = fclonefileat(source, directory, name, flags);
    if (staging) stageCloneSucceeded = !result;
    return result;
}

static BOOL isStage(int fd) {
    char path[PATH_MAX];
    return !fcntl(fd, F_GETPATH, path) && [@(path).lastPathComponent hasPrefix:@".lsb-sign-"];
}

static ssize_t testPwrite(int fd, const void *bytes, size_t size, off_t offset) {
    BOOL staging = isStage(fd);
    if (staging && offset >= SignatureOffset && offset < SignatureOffset + SignatureCapacity) {
        ++rangeWrites;
        assert(size <= (size_t)(SignatureOffset + SignatureCapacity - offset));
        if (signatureIOCase == 1 && rangeWrites == 1) { errno = EINTR; return -1; }
        if (signatureIOCase == 2 || (signatureIOCase == 5 && rangeWrites > 1)) return 0;
        if (signatureIOCase == 3 || (signatureIOCase == 4 && rangeWrites > 1)) { errno = EIO; return -1; }
        if (signatureIOCase == 1 || signatureIOCase == 4 || signatureIOCase == 5) size = MIN(size, 3);
    }
    ssize_t result = pwrite(fd, bytes, size, offset);
    if (staging && result > 0) stageBytesWritten += (size_t)result;
    return result;
}

static int testFsync(int fd) {
    assert(isStage(fd));
    assert(lseek(originalDescriptor, 0, SEEK_CUR) == originalInputOffset);
    if (signatureIOCase == 6) { errno = EIO; return -1; }
    int result = fsync(fd);
    if (!result) stageSynced = YES;
    return result;
}

static int testUnlink(int directory, const char *name, int flags) {
    char path[PATH_MAX];
    if (renamed && !fcntl(directory, F_GETPATH, path) && [@(path).lastPathComponent isEqual:@"SigningBackups"]) {
        BOOL manifest = [@(name).pathExtension isEqual:@"plist"];
        if ((!manifest && cleanupFailure == 1) || (manifest && cleanupFailure == 2)) { errno = EACCES; return -1; }
        if (!manifest) {
            struct stat original, source;
            assert(!fstat(originalDescriptor, &source) && !fstatat(directory, name, &original, AT_SYMLINK_NOFOLLOW));
            assert((original.st_mode & 07777) == (source.st_mode & 07777));
            ++backupRemovals;
        }
    }
    return unlinkat(directory, name, flags);
}

static int testFcntl(int fd, int command, ...) {
    va_list args;
    va_start(args, command);
    void *argument = va_arg(args, void *);
    va_end(args);
    if (command == F_ADDFILESIGS_RETURN) {
        ++signatureRegistrations;
        char path[PATH_MAX];
        struct stat descriptor, named;
        assert(!fcntl(fd, F_GETPATH, path) && !fstat(fd, &descriptor) && !stat(path, &named));
        assert(descriptor.st_nlink && descriptor.st_dev == named.st_dev && descriptor.st_ino == named.st_ino);
        assert([@(path).lastPathComponent hasPrefix:@".lsb-sign-"]);
        assert(stageSynced && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY);
        fsignatures_t *signature = argument;
        assert(signature->fs_file_start == 0 && (uintptr_t)signature->fs_blob_start == 64 && signature->fs_blob_size == SignatureCapacity);
        if (signatureError) { errno = signatureError; return -1; }
        signature->fs_file_start = shortCoverage ? 63 : SignatureOffset + SignatureCapacity;
        return 0;
    }
    assert(command == F_GETPATH);
    return fcntl(fd, command, argument);
}

static int testFstat(int fd, struct stat *info) {
    if (failStatAfterRename && renamed) { failStatAfterRename = NO; errno = EIO; return -1; }
    int result = fstat(fd, info);
    if (!result && signatureIOCase == 7 && stageSynced && isStage(fd) && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY) ++info->st_ino;
    return result;
}

static int testRename(int fromDirectory, const char *from, int toDirectory, const char *to) {
    if (renameError) { errno = renameError; return -1; }
    int result = renameat(fromDirectory, from, toDirectory, to);
    if (!result) renamed = YES;
    return result;
}

@interface TestCredentials : NSObject
@property (nonatomic, copy) NSString *revision;
@end
@implementation TestCredentials
@end

@interface TestLazySigning : LSBLazySigning
@property NSUInteger calls;
@property int serviceError;
@property NSUInteger mutationOffset;
@property off_t sourceOffset;
@property BOOL changeSize, changeOriginal, invalidateWhileSigning;
@property (copy) NSData *signatureData;
@end
static NSData *validSignature(void) {
    const char name[] = "test";
    uint32_t hashes = sizeof(LSBSignatureCodeDirectory) + sizeof(name) + 64;
    uint32_t cdSize = hashes + 32, reqOffset = 36 + cdSize, cmsOffset = reqOffset + 12, length = cmsOffset + 10;
    NSMutableData *data = [NSMutableData dataWithLength:length];
    LSBSignatureHeader header = {OSSwapHostToBigInt32(0xfade0cc0), OSSwapHostToBigInt32(length), OSSwapHostToBigInt32(3)};
    LSBSignatureIndex indices[] = {{0, OSSwapHostToBigInt32(36)}, {OSSwapHostToBigInt32(2), OSSwapHostToBigInt32(reqOffset)},
        {OSSwapHostToBigInt32(0x10000), OSSwapHostToBigInt32(cmsOffset)}};
    LSBSignatureCodeDirectory cd = {.magic = OSSwapHostToBigInt32(0xfade0c02), .length = OSSwapHostToBigInt32(cdSize),
        .version = OSSwapHostToBigInt32(0x20400), .hashOffset = OSSwapHostToBigInt32(hashes),
        .identifierOffset = OSSwapHostToBigInt32(sizeof(cd)), .specialSlots = OSSwapHostToBigInt32(2), .codeSlots = OSSwapHostToBigInt32(1),
        .codeLimit = OSSwapHostToBigInt32(SignatureOffset), .hashSize = 32, .hashType = 2, .pageShift = 14,
        .executableLimit = OSSwapHostToBigInt64(SignatureOffset)};
    uint8_t *bytes = data.mutableBytes;
    memcpy(bytes, &header, sizeof(header)); memcpy(bytes + sizeof(header), indices, sizeof(indices));
    memcpy(bytes + 36, &cd, sizeof(cd)); memcpy(bytes + 36 + sizeof(cd), name, sizeof(name));
    LSBSignatureHeader requirements = {OSSwapHostToBigInt32(0xfade0c01), OSSwapHostToBigInt32(12), 0};
    LSBSignatureBlob cms = {OSSwapHostToBigInt32(0xfade0b01), OSSwapHostToBigInt32(10)};
    memcpy(bytes + reqOffset, &requirements, sizeof(requirements)); memcpy(bytes + cmsOffset, &cms, sizeof(cms));
    bytes[cmsOffset + sizeof(cms)] = 0x30;
    return data;
}
@implementation TestLazySigning
- (int)signedSignature:(int)source request:(const LSBLazySignatureRequest *)metadata output:(NSData **)output {
    assert(metadata && metadata->sliceOffset == 0 && metadata->blobOffset == SignatureOffset && metadata->blobSize == SignatureCapacity);
    ++_calls;
    *output = nil;
    if (_serviceError) return _serviceError;
    NSMutableData *data = [(_signatureData ?: validSignature()) mutableCopy];
    if (_mutationOffset != NSNotFound && _mutationOffset < data.length) ((uint8_t *)data.mutableBytes)[_mutationOffset] ^= 0xff;
    if (_changeSize) data.length = SignatureCapacity + 1;
    *output = data;
    if (_sourceOffset) assert(lseek(source, _sourceOffset, SEEK_SET) == _sourceOffset);
    originalDescriptor = source;
    originalInputOffset = _sourceOffset;
    if (_changeOriginal) assert(!fchmod(source, 0700));
    if (_invalidateWhileSigning) [self invalidate];
    return 0;
}
@end

static NSData *originalBytes(void) {
    unsigned char bytes[FixtureSize];
    for (unsigned index = 0; index < sizeof(bytes); ++index) bytes[index] = (unsigned char)index;
    return [NSData dataWithBytes:bytes length:sizeof(bytes)];
}

static NSString *createInput(NSString *name) {
    NSString *path = [testRoot stringByAppendingPathComponent:name];
    assert([NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]);
    assert([originalBytes() writeToFile:path atomically:NO]);
    assert(!chmod(path.fileSystemRepresentation, 0755));
    return path;
}

static LSBLazySignatureRequest requestFor(NSString *path) {
    LSBLazySignatureRequest request = {.version = LSB_LAZY_SIGNATURE_VERSION, .requestID = 1, .blobOffset = SignatureOffset, .blobSize = SignatureCapacity};
    struct stat info;
    assert(!stat(path.fileSystemRepresentation, &info));
    request.file = LSBLazySignatureIdentityFromStat(&info);
    assert([path getFileSystemRepresentation:request.path maxLength:sizeof(request.path)]);
    return request;
}

static TestLazySigning *newSigner(void) {
    renamed = NO;
    stageCloneAttempted = stageCloneSucceeded = stageSynced = NO;
    rangeWrites = 0;
    stageBytesWritten = 0;
    originalDescriptor = -1;
    for (NSString *name in @[@"RuntimeRoot", @"data"])
        assert([NSFileManager.defaultManager createDirectoryAtPath:[testRoot stringByAppendingPathComponent:name]
            withIntermediateDirectories:YES attributes:nil error:nil]);
    TestCredentials *credentials = [TestCredentials new];
    credentials.revision = NSUUID.UUID.UUIDString;
    int error = 0;
    TestLazySigning *signer = [[TestLazySigning alloc] initWithRuntimeRoot:[testRoot stringByAppendingPathComponent:@"RuntimeRoot"]
        credentials:(LSBSigningCredentials *)credentials identifier:@"com.example.LiveSimBooter" authorize:^BOOL(const audit_token_t *audit) {
            (void)audit; return YES;
        } error:&error];
    assert(signer && !error && MACH_PORT_VALID(signer.endpoint));
    signer.mutationOffset = NSNotFound;
    return signer;
}

static NSArray<NSString *> *backupFiles(void) {
    NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:[testRoot stringByAppendingPathComponent:@"SigningBackups"] error:nil];
    NSMutableArray<NSString *> *binaries = [NSMutableArray new];
    for (NSString *name in names) {
        if ([name.pathExtension isEqual:@"plist"]) assert([names containsObject:name.stringByDeletingPathExtension]);
        else { assert([names containsObject:[name stringByAppendingPathExtension:@"plist"]]); [binaries addObject:name]; }
    }
    return binaries;
}

static void assertNoStages(NSString *path) {
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:path.stringByDeletingLastPathComponent error:nil])
        assert(![name hasPrefix:@".lsb-sign-"]);
}

static void testConfinement(void) {
    TestLazySigning *signer = newSigner();
    for (NSString *name in @[@"outside", @"RuntimeRootOther/bin/test", @"dataOther/bin/test"]) {
        NSString *path = createInput(name);
        LSBLazySignatureRequest request = requestFor(path);
        LSBLazySignatureFileIdentity replacement;
        assert([signer sign:&request replacement:&replacement] == EACCES);
        assert(!signer.calls && [[NSData dataWithContentsOfFile:path] isEqual:originalBytes()]);
    }
    NSString *path = createInput(@"RuntimeRoot/bin/noncanonical"), *link = [testRoot stringByAppendingPathComponent:@"RuntimeRoot/bin/alias"];
    assert(!symlink(path.fileSystemRepresentation, link.fileSystemRepresentation));
    for (NSString *alias in @[link, [testRoot stringByAppendingString:@"/RuntimeRoot/bin/./noncanonical"]]) {
        LSBLazySignatureRequest request = requestFor(alias);
        LSBLazySignatureFileIdentity replacement;
        assert([signer sign:&request replacement:&replacement] == EACCES && !signer.calls);
    }
    [signer invalidate];
}

static void testPublishAndQueuedOldRequests(void) {
    TestLazySigning *signer = newSigner();
    NSString *path = createInput(@"RuntimeRoot/bin/success");
    LSBLazySignatureRequest request = requestFor(path);
    int original = open(path.fileSystemRepresentation, O_RDONLY);
    assert(original >= 0);
    NSUInteger beforeBackups = backupFiles().count;
    // receive() serializes these requests on its signing queue. All queued clients can have captured the old file generation.
    dispatch_queue_t queue = dispatch_queue_create("LiveSimBooter.test-queued-signs", DISPATCH_QUEUE_SERIAL);
    dispatch_group_t group = dispatch_group_create();
    int results[8] = {0};
    LSBLazySignatureFileIdentity replacements[8] = {0};
    int *resultPointer = results;
    LSBLazySignatureFileIdentity *replacementPointer = replacements;
    for (unsigned index = 0; index < 8; ++index) dispatch_group_async(group, queue, ^{
        resultPointer[index] = [signer sign:&request replacement:&replacementPointer[index]];
    });
    assert(!dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    for (unsigned index = 0; index < 8; ++index) {
        if (results[index]) fprintf(stderr, "queued sign %u failed: %s (%d), path=%s\n", index, strerror(results[index]), results[index], path.fileSystemRepresentation);
        assert(!results[index]);
        assert(LSBLazySignatureIdentitiesEqual(&replacements[0], &replacements[index]));
    }
    assert(signer.calls == 1 && backupFiles().count == beforeBackups);
    assert(replacements[0].inode != request.file.inode && replacements[0].mode == request.file.mode);
    unsigned char preserved[FixtureSize];
    assert(pread(original, preserved, sizeof(preserved), 0) == sizeof(preserved));
    assert([[NSData dataWithBytes:preserved length:sizeof(preserved)] isEqual:originalBytes()]);
    close(original);
    NSData *published = [NSData dataWithContentsOfFile:path];
    assert(published.length == FixtureSize && ((const unsigned char *)published.bytes)[SignatureOffset] == 0xfa);
    assertNoStages(path);
    LSBLazySignatureRequest stale = request;
    ++stale.file.inode;
    LSBLazySignatureFileIdentity replacement;
    assert([signer sign:&stale replacement:&replacement] == EAGAIN && signer.calls == 1);
    [signer invalidate];

    signer = newSigner();
    path = createInput(@"data/Containers/Bundle/Application/sample/app");
    request = requestFor(path);
    assert(![signer sign:&request replacement:&replacement] && signer.calls == 1);
    [signer invalidate];
}

static void testFailures(void) {
    for (unsigned mode = 0; mode < 9; ++mode) {
        TestLazySigning *signer = newSigner();
        NSString *path = createInput([NSString stringWithFormat:@"RuntimeRoot/bin/failure-%u", mode]);
        LSBLazySignatureRequest request = requestFor(path);
        LSBLazySignatureFileIdentity replacement;
        NSUInteger beforeBackups = backupFiles().count;
        int expected;
        switch (mode) {
            case 0: signer.serviceError = EPIPE; expected = EPIPE; break;
            case 1: signer.mutationOffset = 0; expected = ENOEXEC; break;
            case 2: signer.mutationOffset = 36 + offsetof(LSBSignatureCodeDirectory, codeLimit); expected = ENOEXEC; break;
            case 3: signer.changeSize = YES; expected = ENOEXEC; break;
            case 4: signatureError = EACCES; expected = EACCES; break;
            case 5: shortCoverage = YES; expected = ENOEXEC; break;
            case 6: signer.changeOriginal = YES; expected = EAGAIN; break;
            case 7: renameError = EIO; expected = EIO; break;
            default: signer.invalidateWhileSigning = YES; expected = EAGAIN; break;
        }
        assert([signer sign:&request replacement:&replacement] == expected);
        assert([[NSData dataWithContentsOfFile:path] isEqual:originalBytes()]);
        assert(backupFiles().count == beforeBackups);
        assertNoStages(path);
        [signer invalidate];
        signatureError = renameError = 0;
        shortCoverage = NO;
    }
    TestLazySigning *signer = newSigner();
    NSString *path = createInput(@"RuntimeRoot/bin/committed-error");
    LSBLazySignatureRequest request = requestFor(path);
    LSBLazySignatureFileIdentity replacement;
    NSUInteger beforeBackups = backupFiles().count;
    failStatAfterRename = YES;
    renamed = NO;
    assert([signer sign:&request replacement:&replacement] == EIO && renamed);
    assert(backupFiles().count == beforeBackups + 1); // Never discard the original after publication, even if reply preparation fails.
    NSString *backupRoot = [testRoot stringByAppendingPathComponent:@"SigningBackups"];
    for (NSString *name in backupFiles()) {
        assert([[NSData dataWithContentsOfFile:[backupRoot stringByAppendingPathComponent:name]] isEqual:originalBytes()]);
        NSDictionary *manifest = [NSDictionary dictionaryWithContentsOfFile:
            [backupRoot stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"plist"]]];
        assert([manifest[@"path"] isEqual:@"RuntimeRoot/bin/committed-error"] && [manifest[@"created"] isKindOfClass:NSDate.class]);
        assert([manifest[@"original-inode"] unsignedLongLongValue] == request.file.inode && [manifest[@"size"] unsignedLongLongValue] == FixtureSize);
        assert([manifest[@"signature-offset"] unsignedLongLongValue] == 64 && [manifest[@"signature-size"] unsignedLongLongValue] == SignatureCapacity);
        assert([manifest[@"signing-identifier"] isEqual:@"com.example.LiveSimBooter"]);
    }
    assert(((const unsigned char *)[NSData dataWithContentsOfFile:path].bytes)[SignatureOffset] == 0xfa);
    assertNoStages(path);
    [signer invalidate];
}

static void testCopyFallback(void) {
    NSString *sourcePath = createInput(@"unlinked-copy-source");
    int source = open(sourcePath.fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    int directory = open(testRoot.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(source >= 0 && directory >= 0 && !unlink(sourcePath.fileSystemRepresentation));
    assert(lseek(source, 71, SEEK_SET) == 71);
    for (unsigned index = 0; index < 2; ++index) {
        cloneError = index ? EACCES : EPERM;
        NSString *name = [NSString stringWithFormat:@"copied-%u", index];
        BOOL cloned = YES;
        assert(!copyDescriptor(source, directory, name.fileSystemRepresentation, &cloned) && !cloned);
        assert(lseek(source, 0, SEEK_CUR) == 71);
        NSData *copied = [NSData dataWithContentsOfFile:[testRoot stringByAppendingPathComponent:name]];
        assert([copied isEqual:originalBytes()]);
        struct stat copiedInfo;
        assert(!fstatat(directory, name.fileSystemRepresentation, &copiedInfo, AT_SYMLINK_NOFOLLOW) && (copiedInfo.st_mode & 07777) == 0755);
        // Existing destinations remain untouched even when the clone failure itself permits a fallback.
        assert(copyDescriptor(source, directory, name.fileSystemRepresentation, NULL) == EEXIST);
        assert([[NSData dataWithContentsOfFile:[testRoot stringByAppendingPathComponent:name]] isEqual:originalBytes()]);
    }
    for (const int *denied = (int[]){EEXIST, ENOSPC, EROFS, EBADF, 0}; *denied; ++denied) {
        cloneError = *denied;
        assert(copyDescriptor(source, directory, "must-not-exist", NULL) == *denied);
        assert(faccessat(directory, "must-not-exist", F_OK, 0) && errno == ENOENT);
    }
    close(source);
    close(directory);

    // Exercise publication and original backup through the same denied-clone fallback, including a nonzero signer FD offset.
    cloneError = EPERM;
    TestLazySigning *signer = newSigner();
    signer.sourceOffset = 23;
    NSString *path = createInput(@"RuntimeRoot/bin/denied-clone");
    LSBLazySignatureRequest request = requestFor(path);
    LSBLazySignatureFileIdentity replacement;
    NSUInteger backups = backupFiles().count;
    unsigned removals = backupRemovals;
    assert(![signer sign:&request replacement:&replacement]);
    assert(backupFiles().count == backups && backupRemovals == removals + 1 && replacement.inode != request.file.inode);
    for (NSString *name in backupFiles()) {
        struct stat backupInfo;
        NSString *backupPath = [[testRoot stringByAppendingPathComponent:@"SigningBackups"] stringByAppendingPathComponent:name];
        assert(!stat(backupPath.fileSystemRepresentation, &backupInfo) && (backupInfo.st_mode & 07777) == 0755);
    }
    assert(((const unsigned char *)[NSData dataWithContentsOfFile:path].bytes)[SignatureOffset] == 0xfa);
    assertNoStages(path);
    [signer invalidate];
    cloneError = 0;
}

static void testBackupCleanupFailures(void) {
    NSString *backupRoot = [testRoot stringByAppendingPathComponent:@"SigningBackups"];
    for (unsigned mode = 1; mode <= 2; ++mode) {
        TestLazySigning *signer = newSigner();
        NSString *path = createInput([NSString stringWithFormat:@"RuntimeRoot/bin/cleanup-failure-%u", mode]);
        LSBLazySignatureRequest request = requestFor(path);
        LSBLazySignatureFileIdentity replacement, repeated;
        NSSet *before = [NSSet setWithArray:[NSFileManager.defaultManager contentsOfDirectoryAtPath:backupRoot error:nil]];
        int oldDescriptor = open(path.fileSystemRepresentation, O_RDONLY);
        assert(oldDescriptor >= 0);
        cleanupFailure = mode;
        // Signing/publication succeeded even when deleting its temporary backup fails. Do not roll back or make the caller sign it again.
        assert(![signer sign:&request replacement:&replacement] && renamed && replacement.inode != request.file.inode);
        assert(![signer sign:&request replacement:&repeated] && signer.calls == 1);
        assert(LSBLazySignatureIdentitiesEqual(&replacement, &repeated));
        NSData *published = [NSData dataWithContentsOfFile:path];
        assert(published.length == FixtureSize && ((const unsigned char *)published.bytes)[SignatureOffset] == 0xfa);
        unsigned char original[FixtureSize];
        assert(pread(oldDescriptor, original, sizeof(original), 0) == sizeof(original));
        assert([[NSData dataWithBytes:original length:sizeof(original)] isEqual:originalBytes()]);
        close(oldDescriptor);
        NSMutableSet *leftovers = [NSMutableSet setWithArray:[NSFileManager.defaultManager contentsOfDirectoryAtPath:backupRoot error:nil]];
        [leftovers minusSet:before];
        assert(leftovers.count == (mode == 1 ? 2 : 1));
        for (NSString *name in leftovers) {
            NSString *leftover = [backupRoot stringByAppendingPathComponent:name];
            if ([name.pathExtension isEqual:@"plist"]) {
                NSDictionary *manifest = [NSDictionary dictionaryWithContentsOfFile:leftover];
                assert(([manifest[@"path"] isEqual:[NSString stringWithFormat:@"RuntimeRoot/bin/cleanup-failure-%u", mode]]));
            } else {
                assert(mode == 1 && [[NSData dataWithContentsOfFile:leftover] isEqual:originalBytes()]);
            }
            assert([NSFileManager.defaultManager removeItemAtPath:leftover error:nil]);
        }
        assertNoStages(path);
        [signer invalidate];
        cleanupFailure = 0;
    }
}

static void testSignatureSplice(void) {
    for (unsigned mode = 0; mode < 8; ++mode) {
        TestLazySigning *signer = newSigner();
        signer.sourceOffset = 23;
        signer.sourceOffset = 17;
        signatureIOCase = mode;
        NSString *path = createInput([NSString stringWithFormat:@"RuntimeRoot/bin/signature-io-%u", mode]);
        LSBLazySignatureRequest request = requestFor(path);
        LSBLazySignatureFileIdentity replacement;
        NSUInteger backups = backupFiles().count;
        unsigned registrations = signatureRegistrations;
        int expected = mode < 2 ? 0 : mode == 7 ? EAGAIN : EIO;
        assert([signer sign:&request replacement:&replacement] == expected);
        assert(backupFiles().count == backups);
        if (expected) {
            assert([[NSData dataWithContentsOfFile:path] isEqual:originalBytes()]);
            assert(signatureRegistrations == registrations);
        } else {
            assert(stageBytesWritten == (stageCloneSucceeded ? SignatureCapacity : FixtureSize + SignatureCapacity));
            assert(stageSynced && signatureRegistrations == registrations + 1);
            NSMutableData *expectedBytes = [originalBytes() mutableCopy];
            memset((uint8_t *)expectedBytes.mutableBytes + SignatureOffset, 0, SignatureCapacity);
            memcpy((uint8_t *)expectedBytes.mutableBytes + SignatureOffset, validSignature().bytes, validSignature().length);
            assert([[NSData dataWithContentsOfFile:path] isEqual:expectedBytes]);
        }
        assertNoStages(path);
        [signer invalidate];
        signatureIOCase = 0;
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        TestLazySigning *signer = newSigner();
        NSString *path = createInput([NSString stringWithFormat:@"RuntimeRoot/bin/readonly-%u", mode]);
        assert(!chmod(path.fileSystemRepresentation, mode ? 0555 : 0444));
        LSBLazySignatureRequest request = requestFor(path);
        LSBLazySignatureFileIdentity replacement;
        assert(![signer sign:&request replacement:&replacement] && replacement.mode == request.file.mode);
        assert(stageBytesWritten == (stageCloneSucceeded ? SignatureCapacity : FixtureSize + SignatureCapacity));
        [signer invalidate];
    }
    TestLazySigning *signer = newSigner();
    NSString *path = createInput(@"RuntimeRoot/bin/signature-bounds");
    for (unsigned mode = 0; mode < 4; ++mode) {
        LSBLazySignatureRequest request = requestFor(path);
        LSBLazySignatureFileIdentity replacement;
        if (!mode) request.sliceOffset = UINT64_MAX;
        else if (mode == 1) request.blobOffset = UINT64_MAX;
        else if (mode == 2) request.blobSize = UINT64_MAX;
        else request.blobSize = 0;
        assert([signer sign:&request replacement:&replacement] == EINVAL && !signer.calls);
        assert([[NSData dataWithContentsOfFile:path] isEqual:originalBytes()]);
    }
    [signer invalidate];
}

static void setSignatureWord(NSMutableData *data, NSUInteger offset, uint32_t value) {
    assert(offset <= data.length && sizeof(value) <= data.length - offset);
    value = OSSwapHostToBigInt32(value);
    memcpy((uint8_t *)data.mutableBytes + offset, &value, sizeof(value));
}

static void testSignatureValidation(void) {
    for (unsigned mode = 0; mode < 28; ++mode) {
        TestLazySigning *signer = newSigner();
        NSString *path = createInput([NSString stringWithFormat:@"RuntimeRoot/bin/invalid-signature-%u", mode]);
        LSBLazySignatureRequest request = requestFor(path);
        LSBLazySignatureFileIdentity replacement;
        NSMutableData *data = [validSignature() mutableCopy];
        uint32_t reqOffset = (uint32_t)data.length - 22, cmsOffset = (uint32_t)data.length - 10;
        switch (mode) {
            case 0: setSignatureWord(data, 0, 0); break;
            case 1: setSignatureWord(data, 4, (uint32_t)data.length + 1); break;
            case 2: setSignatureWord(data, 8, 4); break;
            case 3: setSignatureWord(data, 12, 7); break;
            case 4: setSignatureWord(data, 20, 0); break; // Duplicate CodeDirectory slot.
            case 5: setSignatureWord(data, 16, 12); break; // Child overlaps the index table.
            case 6: setSignatureWord(data, 16, UINT32_MAX); break;
            case 7: setSignatureWord(data, 36, 0); break;
            case 8: setSignatureWord(data, 40, UINT32_MAX); break;
            case 9: setSignatureWord(data, reqOffset, 0); break;
            case 10: setSignatureWord(data, cmsOffset, 0); break;
            case 11: setSignatureWord(data, cmsOffset + 4, 8); break;
            case 12: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, version), 0x20500); break;
            case 13: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, flags), 2); break;
            case 14: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, codeLimit), SignatureOffset - 1); break;
            case 15: ((uint8_t *)data.mutableBytes)[36 + offsetof(LSBSignatureCodeDirectory, hashType)] = 1; break;
            case 16: ((uint8_t *)data.mutableBytes)[36 + offsetof(LSBSignatureCodeDirectory, hashSize)] = 20; break;
            case 17: ((uint8_t *)data.mutableBytes)[36 + offsetof(LSBSignatureCodeDirectory, pageShift)] = 12; break;
            case 18: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, scatterOffset), 4); break;
            case 19: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, codeLimit64) + 4, 1); break;
            case 20: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, executableFlags) + 4, 1); break;
            case 21: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, hashOffset), UINT32_MAX); break;
            case 22: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, codeSlots), 2); break;
            case 23: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, specialSlots), 5); break;
            case 24: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, identifierOffset), 87); break;
            case 25: setSignatureWord(data, 36 + offsetof(LSBSignatureCodeDirectory, teamOffset), UINT32_MAX); break;
            case 26: ((uint8_t *)data.mutableBytes)[36 + sizeof(LSBSignatureCodeDirectory) + 4] = 1; break;
            default: // Individually bounded/magic-correct requirements still cannot overlap the CodeDirectory hash region.
                setSignatureWord(data, 24, 36 + 100);
                setSignatureWord(data, 36 + 100, 0xfade0c01);
                setSignatureWord(data, 36 + 104, 12);
                break;
        }
        signer.signatureData = data;
        unsigned registrations = signatureRegistrations;
        assert([signer sign:&request replacement:&replacement] == ENOEXEC);
        assert(!stageCloneAttempted && signatureRegistrations == registrations && [[NSData dataWithContentsOfFile:path] isEqual:originalBytes()]);
        [signer invalidate];
    }
}

static xpc_object_t goodSignatureReply(void) {
    xpc_object_t reply = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(reply, "version", LSB_NATIVE_SIGNING_VERSION);
    xpc_dictionary_set_int64(reply, "error", 0);
    NSData *data = validSignature();
    xpc_dictionary_set_data(reply, "signature", data.bytes, data.length);
    return reply;
}

static void testSignatureReplies(void) {
    audit_token_t expected = {{11, 12, 13, 14, 15, 16, 17, 18}};
    replyAudit = expected;
    NSData *signature = nil;
    assert(!signatureReply(goodSignatureReply(), &expected, SignatureCapacity, &signature) && [signature isEqual:validSignature()]);
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    for (unsigned mode = 0; mode < 14; ++mode) {
        xpc_object_t reply = goodSignatureReply();
        uint64_t capacity = SignatureCapacity;
        switch (mode) {
            case 0: ++replyAudit.val[0]; break; // A differing non-PID audit field must also reject the response.
            case 1: xpc_dictionary_set_value(reply, "version", NULL); break;
            case 2: xpc_dictionary_set_int64(reply, "version", LSB_NATIVE_SIGNING_VERSION); break;
            case 3: xpc_dictionary_set_uint64(reply, "version", 1); break;
            case 4: xpc_dictionary_set_uint64(reply, "error", 0); break;
            case 5: xpc_dictionary_set_int64(reply, "error", -1); break;
            case 6: xpc_dictionary_set_int64(reply, "error", INT64_MAX); break;
            case 7: xpc_dictionary_set_int64(reply, "error", EACCES); break; // Error replies cannot carry data.
            case 8: xpc_dictionary_set_string(reply, "signature", "invalid"); break;
            case 9: xpc_dictionary_set_data(reply, "signature", "", 0); break;
            case 10: capacity = validSignature().length - 1; break;
            case 11: xpc_dictionary_set_value(reply, "signature", NULL); break;
            case 12: xpc_dictionary_set_value(reply, "signature", NULL); xpc_dictionary_set_fd(reply, "output-fd", fd); break;
            default: xpc_dictionary_set_fd(reply, "output-fd", fd); break;
        }
        assert(signatureReply(reply, &expected, capacity, &signature) == EPROTO && !signature);
        replyAudit = expected;
    }
    close(fd);
    xpc_object_t failure = goodSignatureReply();
    xpc_dictionary_set_int64(failure, "error", EACCES);
    xpc_dictionary_set_value(failure, "signature", NULL);
    assert(signatureReply(failure, &expected, SignatureCapacity, &signature) == EACCES && !signature);
    assert(signatureReply(xpc_array_create(NULL, 0), &expected, SignatureCapacity, &signature) == EPIPE && !signature);
}

int main(void) {
    @autoreleasepool {
        char directory[] = "/tmp/LiveSimBooter-LazySigning.XXXXXX", canonical[PATH_MAX];
        assert(mkdtemp(directory) && realpath(directory, canonical));
        testRoot = @(canonical);
        testConfinement();
        testPublishAndQueuedOldRequests();
        testFailures();
        testCopyFallback();
        testBackupCleanupFailures();
        testSignatureSplice();
        testSignatureValidation();
        testSignatureReplies();
        assert([NSFileManager.defaultManager removeItemAtPath:testRoot error:nil]);
        puts("LazySigning: path confinement, immutable originals, clone/copy publication, backup cleanup/failure retention and queued retry reuse passed");
    }
    return 0;
}
