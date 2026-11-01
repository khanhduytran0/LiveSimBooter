#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import "../NativeSigner/SecuritySigningPrivate.h"
#include <sys/clonefile.h>
static BOOL forceWWDRIssuer;
static unsigned publicCertificateAdds;
static OSStatus publicCertificateAddStatus;
static int forcedCloneError;
static unsigned scratchCloneCalls;
static BOOL failScratchSigning;
static OSStatus testPublicCertificateAdd(CFDictionaryRef query, CFTypeRef *result);
static CFDataRef testIssuerSequence(SecCertificateRef certificate) CF_RETURNS_RETAINED;
static int testClone(int source, int directory, const char *path, uint32_t flags);
static OSStatus testScratchSigning(SecCodeSignerRef signer, SecStaticCodeRef code, SecCSFlags flags, CFErrorRef *error);
#define SecItemAdd testPublicCertificateAdd
#define SecCertificateCopyNormalizedIssuerSequence testIssuerSequence
#define fclonefileat testClone
#define SecCodeSignerAddSignatureWithErrors testScratchSigning
#import "../NativeSigner/LazyCodeSigner.m"
#undef SecItemAdd
#undef SecCertificateCopyNormalizedIssuerSequence
#undef fclonefileat
#undef SecCodeSignerAddSignatureWithErrors
#include <assert.h>

static int testClone(int source, int directory, const char *path, uint32_t flags) {
    ++scratchCloneCalls;
    assert(flags == CLONE_NOOWNERCOPY);
    if (forcedCloneError) { errno = forcedCloneError; return -1; }
    return fclonefileat(source, directory, path, flags);
}

static OSStatus testScratchSigning(SecCodeSignerRef signer, SecStaticCodeRef code, SecCSFlags flags, CFErrorRef *error) {
    return failScratchSigning ? errSecParam : SecCodeSignerAddSignatureWithErrors(signer, code, flags, error);
}

static OSStatus testPublicCertificateAdd(CFDictionaryRef query, CFTypeRef *result) {
    assert(!result && CFDictionaryGetValue(query, kSecClass) == kSecClassCertificate);
    assert(CFDictionaryGetValue(query, kSecAttrAccessible) == kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    SecCertificateRef certificate = (SecCertificateRef)CFDictionaryGetValue(query, kSecValueRef);
    SecCertificateRef expected = copyWWDRG3(NULL);
    assert(expected && CFEqual(certificate, expected)); CFRelease(expected);
    ++publicCertificateAdds;
    return publicCertificateAddStatus;
}

static CFDataRef testIssuerSequence(SecCertificateRef certificate) {
    return forceWWDRIssuer ? SecCertificateCopyNormalizedSubjectSequence(certificate) : SecCertificateCopyNormalizedIssuerSequence(certificate);
}

static NSURL *writeFixture(NSData *data, NSString *directory, NSString *name) {
    NSURL *url = [NSURL fileURLWithPath:[directory stringByAppendingPathComponent:name]];
    assert([data writeToURL:url atomically:NO]);
    return url;
}

static NSMutableData *thinFixture(uint32_t type, uint32_t signatureSize) {
    struct mach_header_64 header = {.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = type, .ncmds = 2,
        .sizeofcmds = sizeof(struct segment_command_64) + sizeof(struct linkedit_data_command)};
    struct segment_command_64 text = {.cmd = LC_SEGMENT_64, .cmdsize = sizeof(text), .vmaddr = 0x100000000, .vmsize = SignPageSize,
        .filesize = SignPageSize, .maxprot = VM_PROT_READ | VM_PROT_EXECUTE, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    memcpy(text.segname, "__TEXT", 6);
    struct linkedit_data_command signature = {.cmd = LC_CODE_SIGNATURE, .cmdsize = sizeof(signature), .dataoff = SignPageSize, .datasize = signatureSize};
    NSMutableData *data = [NSMutableData dataWithLength:SignPageSize + signatureSize];
    uint8_t *bytes = data.mutableBytes;
    memcpy(bytes, &header, sizeof(header)); memcpy(bytes + sizeof(header), &text, sizeof(text));
    memcpy(bytes + sizeof(header) + sizeof(text), &signature, sizeof(signature));
    return data;
}

static void scratchInputTests(NSString *directory) {
    NSData *contents = thinFixture(MH_EXECUTE, SignPageSize);
    NSURL *url = writeFixture(contents, directory, @"readonly-original");
    assert(!chmod(url.fileSystemRepresentation, 0444));
    int input = open(url.fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    struct stat before, after;
    assert(input >= 0 && !fstat(input, &before) && lseek(input, 117, SEEK_SET) == 117);
    for (unsigned mode = 0; mode < 2; ++mode) {
        forcedCloneError = mode ? EPERM : 0;
        scratchCloneCalls = 0;
        NSString *path = [directory stringByAppendingPathComponent:[NSString stringWithFormat:@"scratch-%u", mode]];
        NSError *error = nil;
        assert(prepareScratch(input, contents.length, path.fileSystemRepresentation, &error) && !error && scratchCloneCalls == 1);
        assert(!fstat(input, &after) && sameInput(&before, &after) && lseek(input, 0, SEEK_CUR) == 117);
        assert([[NSData dataWithContentsOfURL:url] isEqual:contents] && [[NSData dataWithContentsOfFile:path] isEqual:contents]);
        struct stat scratch;
        assert(!stat(path.fileSystemRepresentation, &scratch) && (scratch.st_mode & 07777) == 0600 && scratch.st_ino != before.st_ino);
        int writable = open(path.fileSystemRepresentation, O_WRONLY);
        assert(writable >= 0 && pwrite(writable, "X", 1, 0) == 1 && !close(writable));
        assert([[NSData dataWithContentsOfURL:url] isEqual:contents]);
        assert(!unlink(path.fileSystemRepresentation));
    }
    forcedCloneError = 0;
    close(input);
}

static void structuralTests(void) {
    NSError *certificateError = nil;
    SecCertificateRef intermediate = copyWWDRG3(&certificateError);
    assert(intermediate && !certificateError);
    NSArray *originalChain = @[(__bridge id)intermediate];
    assert(chainWithPublicIntermediate(intermediate, originalChain, &certificateError) == originalChain && !publicCertificateAdds);
    forceWWDRIssuer = YES;
    NSArray *expanded = chainWithPublicIntermediate(intermediate, @[], &certificateError);
    assert(expanded.count == 1 && publicCertificateAdds == 1 && CFEqual((__bridge CFTypeRef)expanded[0], intermediate));
    publicCertificateAddStatus = errSecDuplicateItem;
    assert(chainWithPublicIntermediate(intermediate, expanded, &certificateError).count == 1 && publicCertificateAdds == 2);
    publicCertificateAddStatus = errSecNotAvailable;
    assert(!chainWithPublicIntermediate(intermediate, expanded, &certificateError) && certificateError.code == errSecNotAvailable);
    publicCertificateAddStatus = 0; forceWWDRIssuer = NO;
    CFRelease(intermediate);
    NSString *directory = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil]);
    scratchInputTests(directory);
    NSURL *url = writeFixture(thinFixture(MH_EXECUTE, SignPageSize), directory, @"thin");
    int fd = open(url.fileSystemRepresentation, O_RDWR);
    NSError *error = nil;
    NSArray<LSBSigningSlice *> *slices = readSlices(fd, 2 * SignPageSize, NO, &error);
    assert(slices.count == 1 && !error && slices[0].fileType == MH_EXECUTE && slices[0].signatureOffset == SignPageSize);
    assert(matchingSlice(slices, 0, SignPageSize, SignPageSize) == 0);
    assert(matchingSlice(slices, 1, SignPageSize, SignPageSize) == NSNotFound);
    assert(matchingSlice(slices, 0, SignPageSize + 1, SignPageSize) == NSNotFound);
    assert(matchingSlice(slices, 0, SignPageSize, SignPageSize - 1) == NSNotFound);
    NSData *name = [@"com.example.LazySign" dataUsingEncoding:NSUTF8StringEncoding], *team = [@"TESTTEAM00" dataUsingEncoding:NSUTF8StringEncoding];
    CodeDirectory template = {.magic = OSSwapHostToBigInt32(CodeDirectoryMagic), .version = OSSwapHostToBigInt32(0x20400),
        .identifierOffset = OSSwapHostToBigInt32(sizeof(CodeDirectory)), .hashSize = 32, .hashType = 2, .specialSlots = OSSwapHostToBigInt32(2),
        .teamOffset = OSSwapHostToBigInt32((uint32_t)(sizeof(CodeDirectory) + name.length + 1))};
    template.hashOffset = OSSwapHostToBigInt32((uint32_t)(sizeof(template) + name.length + 1 + team.length + 1 + 64));
    NSMutableData *metadata = [NSMutableData dataWithBytes:&template length:sizeof(template)];
    uint8_t zero = 0; [metadata appendData:name]; [metadata appendBytes:&zero length:1]; [metadata appendData:team]; [metadata appendBytes:&zero length:1];
    [metadata increaseLengthBy:64];
    uint32_t requirements[] = {OSSwapHostToBigInt32(RequirementsMagic), OSSwapHostToBigInt32(12), 0};
    NSData *requirementData = [NSData dataWithBytes:requirements length:sizeof(requirements)];
    NSDictionary *slots = @{@0: metadata, @2: requirementData};
    NSData *directoryData = rebuiltDirectory(fd, slices[0], slots, @"com.example.LazySign", &error);
    assert(directoryData && !error);
    CodeDirectory rebuilt; [directoryData getBytes:&rebuilt length:sizeof(rebuilt)];
    assert(OSSwapBigToHostInt32(rebuilt.version) == 0x20400 && !rebuilt.flags && !rebuilt.executableFlags && !rebuilt.scatterOffset);
    assert(rebuilt.pageShift == 14 && OSSwapBigToHostInt32(rebuilt.codeSlots) == 1);
    NSData *before = [NSData dataWithContentsOfURL:url];
    uint8_t expected[32]; CC_SHA256(before.bytes, SignPageSize, expected);
    assert(!memcmp((const uint8_t *)directoryData.bytes + OSSwapBigToHostInt32(rebuilt.hashOffset), expected, sizeof(expected)));
    NSData *signature = buildSignature(directoryData, requirementData, [NSData dataWithBytes:"CMS" length:3], SignPageSize, &error);
    assert(signature && writeSignature(fd, slices[0], signature, &error));
    NSData *after = [NSData dataWithContentsOfURL:url];
    assert(before.length == after.length && !memcmp(before.bytes, after.bytes, SignPageSize));
    assert(signatureSlots(fd, slices[0], &error).count == 3);
    // iOS Security emits CodeDirectory+CMS without a requirements blob. Rebuild an empty set and seal its actual bytes.
    uint32_t pairHeader[] = {OSSwapHostToBigInt32(SuperblobMagic), OSSwapHostToBigInt32((uint32_t)(28 + directoryData.length + 11)),
        OSSwapHostToBigInt32(2), 0, OSSwapHostToBigInt32(28), OSSwapHostToBigInt32(0x10000),
        OSSwapHostToBigInt32((uint32_t)(28 + directoryData.length))};
    NSMutableData *pair = [NSMutableData dataWithBytes:pairHeader length:sizeof(pairHeader)];
    [pair appendData:directoryData];
    uint32_t cmsHeader[] = {OSSwapHostToBigInt32(CMSMagic), OSSwapHostToBigInt32(11)};
    [pair appendBytes:cmsHeader length:sizeof(cmsHeader)]; [pair appendBytes:"CMS" length:3];
    assert(writeSignature(fd, slices[0], pair, &error));
    NSDictionary *pairSlots = signatureSlots(fd, slices[0], &error);
    assert(pairSlots.count == 3 && [pairSlots[@2] isEqualToData:requirementData]);
    NSData *pairDirectory = rebuiltDirectory(fd, slices[0], pairSlots, @"com.example.LazySign", &error);
    assert(pairDirectory);
    uint32_t pairHashes = be32((const uint8_t *)pairDirectory.bytes + 16);
    CC_SHA256(requirementData.bytes, (CC_LONG)requirementData.length, expected);
    assert(!memcmp((const uint8_t *)pairDirectory.bytes + pairHashes - 64, expected, sizeof(expected)));
    // Only the two known blobs are accepted; an entitlement slot cannot masquerade as an optional requirements omission.
    ((uint32_t *)pair.mutableBytes)[5] = OSSwapHostToBigInt32(5);
    assert(writeSignature(fd, slices[0], pair, &error));
    assert(!signatureSlots(fd, slices[0], &error) && error.code == EFTYPE);
    ((uint32_t *)pair.mutableBytes)[5] = OSSwapHostToBigInt32(0);
    assert(writeSignature(fd, slices[0], pair, &error));
    assert(!signatureSlots(fd, slices[0], &error) && error.code == EFTYPE);
    error = nil;
    assert(!buildSignature(directoryData, requirementData, [NSMutableData dataWithLength:1024], 64, &error) && error.code == ENOSPC);
    error = nil;
    assert(!rebuiltDirectory(fd, slices[0], slots, @"wrong.identifier", &error) && error.code == EFTYPE);
    close(fd);

    // Selected arm64 fat slice retains its original offset and leaves the unrelated architecture outside the signing set.
    NSMutableData *fat = [NSMutableData dataWithLength:5 * SignPageSize];
    uint32_t fatHeader[] = {OSSwapHostToBigInt32(FAT_MAGIC), OSSwapHostToBigInt32(2), OSSwapHostToBigInt32(CPU_TYPE_X86_64), 0,
        OSSwapHostToBigInt32(SignPageSize), OSSwapHostToBigInt32(SignPageSize), OSSwapHostToBigInt32(14), OSSwapHostToBigInt32(CPU_TYPE_ARM64), 0,
        OSSwapHostToBigInt32(3 * SignPageSize), OSSwapHostToBigInt32(2 * SignPageSize), OSSwapHostToBigInt32(14)};
    memcpy(fat.mutableBytes, fatHeader, sizeof(fatHeader));
    memcpy((uint8_t *)fat.mutableBytes + 3 * SignPageSize, before.bytes, before.length);
    url = writeFixture(fat, directory, @"fat"); fd = open(url.fileSystemRepresentation, O_RDONLY); error = nil;
    slices = readSlices(fd, fat.length, NO, &error); assert(slices.count == 1 && slices[0].offset == 3 * SignPageSize); close(fd);
    assert(matchingSlice(slices, 3 * SignPageSize, SignPageSize, SignPageSize) == 0 && matchingSlice(slices, 0, SignPageSize, SignPageSize) == NSNotFound);
    ((uint32_t *)fat.mutableBytes)[9] = OSSwapHostToBigInt32(SignPageSize);
    url = writeFixture(fat, directory, @"fat-overlap"); fd = open(url.fileSystemRepresentation, O_RDONLY);
    assert(!readSlices(fd, fat.length, NO, &error)); close(fd);

    // DSC signature allocation remains governed by its original header; only the disposable Security scratch parser uses the actual tail.
    NSMutableData *cache = [NSMutableData dataWithLength:2 * SignPageSize];
    uint8_t *cacheBytes = cache.mutableBytes; memcpy(cacheBytes, "dyld_v1   arm64\0", 16);
    uint32_t mappingOffset = 400, mappingCount = 1, platform = 7, flags = 0x200;
    uint64_t signatureOffset = SignPageSize, signatureCapacity = SignPageSize;
    memcpy(cacheBytes + 16, &mappingOffset, 4); memcpy(cacheBytes + 20, &mappingCount, 4);
    memcpy(cacheBytes + 40, &signatureOffset, 8); memcpy(cacheBytes + 48, &signatureCapacity, 8);
    memcpy(cacheBytes + 216, &platform, 4); memcpy(cacheBytes + 220, &flags, 4);
    uint64_t mapping[] = {0x110000000, SignPageSize, 0}; memcpy(cacheBytes + 400, mapping, sizeof(mapping));
    uint32_t prot = VM_PROT_READ | VM_PROT_EXECUTE; memcpy(cacheBytes + 424, &prot, 4); memcpy(cacheBytes + 428, &prot, 4);
    url = writeFixture(cache, directory, @"cache"); fd = open(url.fileSystemRepresentation, O_RDONLY); error = nil;
    slices = readSlices(fd, cache.length, NO, &error); assert(slices.count == 1 && slices[0].cache && !error); close(fd);
    signatureCapacity = 3 * SignPageSize; memcpy(cacheBytes + 48, &signatureCapacity, 8);
    url = writeFixture(cache, directory, @"cache-invalid"); fd = open(url.fileSystemRepresentation, O_RDONLY);
    assert(!readSlices(fd, cache.length, NO, &error)); error = nil;
    assert(readSlices(fd, cache.length, YES, &error).count == 1 && !error); close(fd);
    assert([NSFileManager.defaultManager removeItemAtPath:directory error:nil]);
    puts("LazyCodeSigner: thin/fat/cache bounds, read-only clone/copy scratch, unchanged input/offset, 16K hashes and capacity rejection passed");
}

static NSSet<NSString *> *scratchArtifacts(void) {
    NSMutableSet *result = [NSMutableSet new];
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:NSTemporaryDirectory() error:nil])
        if ([name hasPrefix:@".lsb-security-sign."]) [result addObject:name];
    return result;
}

static NSData *inputDigest(int fd, off_t size) {
    CC_SHA256_CTX context;
    assert(CC_SHA256_Init(&context));
    uint8_t page[SignPageSize], hash[CC_SHA256_DIGEST_LENGTH];
    for (off_t position = 0; position < size;) {
        size_t count = (size_t)MIN((off_t)sizeof(page), size - position);
        assert(readBytes(fd, page, count, (uint64_t)position, NULL));
        assert(CC_SHA256_Update(&context, page, (CC_LONG)count));
        position += (off_t)count;
    }
    assert(CC_SHA256_Final(hash, &context));
    return [NSData dataWithBytes:hash length:sizeof(hash)];
}

static BOOL createBlobFixture(NSURL *url, uint64_t sliceOffset, uint64_t blobOffset, uint64_t blobSize, SecIdentityRef identity,
                              NSArray *chain, NSString *identifier, NSString *output, NSError **error) {
    int input = open(url.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat before, after;
    assert(input >= 0 && !fstat(input, &before) && lseek(input, 79, SEEK_SET) == 79);
    NSData *digest = inputDigest(input, before.st_size);
    NSSet *existingScratch = scratchArtifacts();
    NSError *expectedError = nil;
    scratchCloneCalls = 0;
    assert(!LSBCreateRuntimeSignature(input, sliceOffset, blobOffset + 1, blobSize, identity, chain, identifier, &expectedError));
    assert(expectedError.code == ENOEXEC && !scratchCloneCalls && [existingScratch isEqual:scratchArtifacts()]);
    // Force Security to reject after scratch creation, then verify both cleanup and the original read-only descriptor.
    failScratchSigning = YES;
    assert(!LSBCreateRuntimeSignature(input, sliceOffset, blobOffset, blobSize, identity, chain, identifier, &expectedError));
    assert(expectedError && scratchCloneCalls == 1 && [existingScratch isEqual:scratchArtifacts()]);
    failScratchSigning = NO;
    scratchCloneCalls = 0;
    NSData *signature = LSBCreateRuntimeSignature(input, sliceOffset, blobOffset, blobSize, identity, chain, identifier, error);
    assert(!fstat(input, &after) && sameInput(&before, &after) && lseek(input, 0, SEEK_CUR) == 79);
    assert([digest isEqual:inputDigest(input, before.st_size)] && [existingScratch isEqual:scratchArtifacts()]);
    assert(scratchCloneCalls == 1);
    close(input);
    if (!signature) return NO;
    assert(signature.length <= blobSize && signature.length <= MaximumSignatureSize && be32(signature.bytes) == SuperblobMagic);
    assert(be32((const uint8_t *)signature.bytes + 4) == signature.length);
    assert([signature writeToFile:output options:NSDataWritingAtomic error:error]);
    puts("LazyCodeSigner blob: real CMS, unchanged read-only input/mode/offset, one scratch clone and success/failure cleanup passed");
    return YES;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 5 || argc == 8 || argc == 9) {
            NSData *p12 = [NSData dataWithContentsOfFile:@(argv[1])];
            NSMutableDictionary *options = [@{(__bridge id)kSecImportExportPassphrase: @(argv[2]),
                (__bridge id)kSecImportToMemoryOnly: @YES} mutableCopy];
#if TARGET_OS_OSX
            // macOS's legacy CMS backend rejects memory-only SecIdentity keys. The optional real-signing test gets an isolated
            // disposable keychain; normal tests never use a keychain. Do not pass a personal identity to this test helper.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            SecKeychainRef keychain = NULL;
            NSString *keychainPath = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
            const char *password = "ephemeral-test-only";
            assert(!SecKeychainCreate(keychainPath.fileSystemRepresentation, (UInt32)strlen(password), password, false, NULL, &keychain));
            options[(__bridge id)kSecImportExportKeychain] = (__bridge id)keychain;
            [options removeObjectForKey:(__bridge id)kSecImportToMemoryOnly];
#endif
            CFArrayRef items = NULL;
            OSStatus status = SecPKCS12Import((__bridge CFDataRef)p12, (__bridge CFDictionaryRef)options, &items);
            NSDictionary *item = status ? nil : ((__bridge NSArray *)items).firstObject;
            SecIdentityRef identity = (__bridge SecIdentityRef)item[(__bridge id)kSecImportItemIdentity];
            NSError *error = nil;
            NSURL *url = [NSURL fileURLWithPath:@(argv[3])];
            NSArray *chain = item[(__bridge id)kSecImportItemCertChain];
            BOOL ok = identity && (argc == 9 ? createBlobFixture(url, strtoull(argv[5], NULL, 0), strtoull(argv[6], NULL, 0),
                strtoull(argv[7], NULL, 0), identity, chain, @(argv[4]), @(argv[8]), &error) :
                argc == 8 ? LSBSignRuntimeBinarySlice(url, strtoull(argv[5], NULL, 0), strtoull(argv[6], NULL, 0),
                strtoull(argv[7], NULL, 0), identity, chain, @(argv[4]), &error) : LSBSignRuntimeBinary(url, identity, chain, @(argv[4]), &error));
            if (!ok) fprintf(stderr, "LazyCodeSigner: %s\n", error.description.UTF8String);
            if (items) CFRelease(items);
#if TARGET_OS_OSX
            assert(!SecKeychainDelete(keychain)); CFRelease(keychain);
#pragma clang diagnostic pop
#endif
            return ok ? 0 : 1;
        }
        structuralTests();
        return 0;
    }
}
