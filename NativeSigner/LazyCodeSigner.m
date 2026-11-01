#import "LazyCodeSigner.h"
#import "SecuritySigningPrivate.h"
#import "AppleWWDRG3.h"
#include "../ProcessSupport/NativeSigningProtocol.h"
#import <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SignPageSize = 16384, MaximumSignatureSize = LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES, CodeDirectoryMagic = 0xfade0c02,
       SuperblobMagic = 0xfade0cc0, RequirementsMagic = 0xfade0c01, CMSMagic = 0xfade0b01 };

// CodeDirectory v0x20400 wire layout, as also used by Tools/sign-sim-executable.py and Tools/sign-dyld-cache.py.
typedef struct {
    uint32_t magic, length, version, flags, hashOffset, identifierOffset, specialSlots, codeSlots, codeLimit;
    uint8_t hashSize, hashType, platform, pageShift;
    uint32_t spare2, scatterOffset, teamOffset, spare3;
    uint64_t codeLimit64, executableBase, executableLimit, executableFlags;
} CodeDirectory;
_Static_assert(sizeof(CodeDirectory) == 88 && offsetof(CodeDirectory, executableFlags) == 80, "CodeDirectory wire layout changed");

@interface LSBSigningSlice : NSObject
@property uint64_t offset, size, signatureOffset, signatureCapacity, executableBase, executableLimit;
@property cpu_subtype_t subtype;
@property uint32_t fileType;
@property BOOL cache;
@end
@implementation LSBSigningSlice
@end

static BOOL failure(NSError **error, NSInteger code, NSString *detail) {
    // These are fixed implementation reasons, never input paths, identity material or passwords.
    fprintf(stderr, "[native-sign] rejected=%s code=%ld\n", detail.UTF8String, (long)code);
    if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey: detail}];
    return NO;
}

static BOOL securityFailure(NSError **error, OSStatus status, NSString *operation) {
    CFStringRef explanation = SecCopyErrorMessageString(status, NULL);
    NSString *detail = [NSString stringWithFormat:@"%@: %@ (%d)", operation, explanation ? (__bridge NSString *)explanation : @"Security error", (int)status];
    if (explanation) CFRelease(explanation);
    if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:status userInfo:@{NSLocalizedDescriptionKey: detail}];
    return NO;
}

static SecCertificateRef copyWWDRG3(NSError **error) CF_RETURNS_RETAINED {
    NSData *data = [[NSData alloc] initWithBase64EncodedString:LSBAppleWWDRG3Base64 options:0];
    uint8_t hash[CC_SHA256_DIGEST_LENGTH];
    if (!data || data.length > UINT32_MAX || !CC_SHA256(data.bytes, (CC_LONG)data.length, hash) ||
        memcmp(hash, LSBAppleWWDRG3SHA256, sizeof(hash))) {
        failure(error, EFTYPE, @"The bundled public signing intermediate is invalid"); return NULL;
    }
    SecCertificateRef certificate = SecCertificateCreateWithData(NULL, (__bridge CFDataRef)data);
    if (!certificate) securityFailure(error, errSecDecode, @"Read public WWDR G3 intermediate");
    return certificate;
}

static NSArray *chainWithPublicIntermediate(SecCertificateRef leaf, NSArray *certificates, NSError **error) {
    SecCertificateRef intermediate = copyWWDRG3(error);
    if (!intermediate) return nil;
    CFDataRef issuer = SecCertificateCopyNormalizedIssuerSequence(leaf);
    CFDataRef subject = SecCertificateCopyNormalizedSubjectSequence(intermediate);
    BOOL matches = issuer && subject && CFEqual(issuer, subject);
    if (issuer) CFRelease(issuer);
    if (subject) CFRelease(subject);
    if (!matches) { CFRelease(intermediate); return certificates; }
    // SecCodeSigner's first CMS pass discovers its chain through the keychain, not the in-memory P12 array.
    // Like LiveContainer, supply Apple's public G3 intermediate. This adds no private key and changes no trust settings.
    // Restrict the addition to a matching leaf issuer; Security still validates the chain and AMFI still validates the final signature.
    NSDictionary *query = @{(__bridge id)kSecClass: (__bridge id)kSecClassCertificate,
        (__bridge id)kSecValueRef: (__bridge id)intermediate, (__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    OSStatus status = SecItemAdd((__bridge CFDictionaryRef)query, NULL);
    if (status && status != errSecDuplicateItem) {
        fprintf(stderr, "[native-sign] Security stage=SecItemAdd-public-WWDR-G3 status=%d\n", (int)status);
        securityFailure(error, status, @"Make public WWDR G3 intermediate available to Security");
        CFRelease(intermediate); return nil;
    }
    NSMutableArray *result = [certificates mutableCopy];
    if (![result containsObject:(__bridge id)intermediate]) [result addObject:(__bridge id)intermediate];
    CFRelease(intermediate);
    return result;
}

static BOOL readBytes(int fd, void *bytes, size_t size, uint64_t offset, NSError **error) {
    size_t done = 0;
    while (done < size) {
        ssize_t count = pread(fd, (uint8_t *)bytes + done, size - done, (off_t)(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return failure(error, count ? errno : EFTYPE, @"Cannot read complete signing input");
        done += (size_t)count;
    }
    return YES;
}

static uint32_t be32(const void *bytes) { uint32_t value; memcpy(&value, bytes, sizeof(value)); return OSSwapBigToHostInt32(value); }
static uint64_t le64(const void *bytes) { uint64_t value; memcpy(&value, bytes, sizeof(value)); return OSSwapLittleToHostInt64(value); }
static uint32_t le32(const void *bytes) { uint32_t value; memcpy(&value, bytes, sizeof(value)); return OSSwapLittleToHostInt32(value); }
static BOOL rangeFits(uint64_t offset, uint64_t size, uint64_t end) { return offset <= end && size <= end - offset; }

static LSBSigningSlice *machoSlice(int fd, uint64_t offset, uint64_t size, NSError **error) {
    struct mach_header_64 header;
    if (size < sizeof(header)) { failure(error, EFTYPE, @"Truncated Mach-O header"); return nil; }
    if (!readBytes(fd, &header, sizeof(header), offset, error)) return nil;
    if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 ||
        (header.filetype != MH_EXECUTE && header.filetype != MH_DYLIB && header.filetype != MH_DYLINKER)) {
        failure(error, ENOTSUP, @"Lazy sign requires an arm64 executable, dylib or dyld"); return nil;
    }
    if (!header.ncmds || header.sizeofcmds > 16 * 1024 * 1024 || !rangeFits(sizeof(header), header.sizeofcmds, size)) {
        failure(error, EFTYPE, @"Invalid Mach-O load-command range"); return nil;
    }
    NSMutableData *commands = [NSMutableData dataWithLength:header.sizeofcmds];
    if (!readBytes(fd, commands.mutableBytes, commands.length, offset + sizeof(header), error)) return nil;
    LSBSigningSlice *slice = [LSBSigningSlice new];
    slice.offset = offset; slice.size = size; slice.subtype = header.cpusubtype; slice.fileType = header.filetype;
    size_t cursor = 0;
    BOOL executable = NO;
    for (uint32_t index = 0; index < header.ncmds; ++index) {
        if (!rangeFits(cursor, sizeof(struct load_command), commands.length)) goto invalid;
        const struct load_command *command = (const void *)((const uint8_t *)commands.bytes + cursor);
        if (command->cmdsize < sizeof(*command) || command->cmdsize % 8 || !rangeFits(cursor, command->cmdsize, commands.length)) goto invalid;
        if (command->cmd == LC_CODE_SIGNATURE) {
            if (slice.signatureCapacity || command->cmdsize != sizeof(struct linkedit_data_command)) goto invalid;
            const struct linkedit_data_command *signature = (const void *)command;
            slice.signatureOffset = signature->dataoff; slice.signatureCapacity = signature->datasize;
        } else if (command->cmd == LC_SEGMENT_64) {
            if (command->cmdsize < sizeof(struct segment_command_64)) goto invalid;
            const struct segment_command_64 *segment = (const void *)command;
            if (!rangeFits(segment->fileoff, segment->filesize, size)) goto invalid;
            if (segment->initprot & VM_PROT_EXECUTE) {
                if (executable || !segment->filesize) goto invalid;
                executable = YES; slice.executableBase = segment->fileoff; slice.executableLimit = segment->filesize;
            }
        }
        cursor += command->cmdsize;
    }
    if (cursor != commands.length || !executable || slice.signatureOffset < sizeof(header) + commands.length ||
        !rangeFits(slice.executableBase, slice.executableLimit, slice.signatureOffset) ||
        slice.signatureCapacity < 12 || slice.signatureCapacity > MaximumSignatureSize ||
        !rangeFits(slice.signatureOffset, slice.signatureCapacity, size)) goto invalid;
    return slice;
invalid:
    failure(error, EFTYPE, @"Malformed Mach-O or missing existing signature allocation");
    return nil;
}

static NSArray<LSBSigningSlice *> *readSlices(int fd, uint64_t size, BOOL scratch, NSError **error) {
    uint8_t header[400] = {0};
    if (size < 32 || !readBytes(fd, header, (size_t)MIN(size, sizeof(header)), 0, error)) return nil;
    if (!memcmp(header, "dyld_v1   arm64\0", 16)) {
        if (size < sizeof(header) || le32(header + 216) != 7 || !(le32(header + 220) & 0x200) || le32(header + 396)) goto unsupported;
        uint32_t mappingsOffset = le32(header + 16), count = le32(header + 20);
        LSBSigningSlice *slice = [LSBSigningSlice new];
        slice.cache = YES; slice.size = size; slice.signatureOffset = le64(header + 40); slice.signatureCapacity = le64(header + 48);
        // Security's DSC writer leaves codeSignatureSize unchanged; only scratch output may use its actual trailing allocation.
        if (scratch && slice.signatureOffset <= size) slice.signatureCapacity = size - slice.signatureOffset;
        if (!slice.signatureOffset || slice.signatureOffset >= UINT32_MAX || slice.signatureOffset % SignPageSize ||
            slice.signatureCapacity < 12 || slice.signatureCapacity > MaximumSignatureSize ||
            !rangeFits(slice.signatureOffset, slice.signatureCapacity, size) || mappingsOffset < sizeof(header) ||
            !count || count > 64 || !rangeFits(mappingsOffset, (uint64_t)count * 32, slice.signatureOffset)) goto unsupported;
        BOOL executable = NO;
        for (uint32_t index = 0; index < count; ++index) {
            uint8_t mapping[32];
            if (!readBytes(fd, mapping, sizeof(mapping), mappingsOffset + index * sizeof(mapping), error)) return nil;
            uint64_t mappingSize = le64(mapping + 8), fileOffset = le64(mapping + 16);
            if (!mappingSize || !rangeFits(fileOffset, mappingSize, slice.signatureOffset)) goto unsupported;
            if (le32(mapping + 28) & VM_PROT_EXECUTE) {
                if (executable || fileOffset) goto unsupported;
                executable = YES; slice.executableLimit = mappingSize;
            }
        }
        if (!executable) goto unsupported;
        return @[slice];
    }
    uint32_t magic = be32(header);
    if (magic == FAT_MAGIC || magic == FAT_MAGIC_64) {
        uint32_t count = be32(header + 4);
        size_t stride = magic == FAT_MAGIC_64 ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
        if (!count || count > 32 || !rangeFits(8, count * stride, size)) goto unsupported;
        NSMutableData *architectures = [NSMutableData dataWithLength:count * stride];
        if (!readBytes(fd, architectures.mutableBytes, architectures.length, 8, error)) return nil;
        NSMutableArray *result = [NSMutableArray new];
        NSMutableArray<NSValue *> *ranges = [NSMutableArray new];
        for (uint32_t index = 0; index < count; ++index) {
            const uint8_t *arch = (const uint8_t *)architectures.bytes + index * stride;
            uint64_t offset = magic == FAT_MAGIC_64 ? ((uint64_t)be32(arch + 8) << 32) | be32(arch + 12) : be32(arch + 8);
            uint64_t length = magic == FAT_MAGIC_64 ? ((uint64_t)be32(arch + 16) << 32) | be32(arch + 20) : be32(arch + 12);
            uint32_t alignment = be32(arch + (magic == FAT_MAGIC_64 ? 24 : 16));
            if (alignment > 31 || (offset & (((uint64_t)1 << alignment) - 1)) ||
                offset < 8 + architectures.length || !length || !rangeFits(offset, length, size)) goto unsupported;
            for (NSValue *value in ranges) {
                NSRange other = value.rangeValue;
                if (offset < NSMaxRange(other) && other.location < offset + length) goto unsupported;
            }
            [ranges addObject:[NSValue valueWithRange:NSMakeRange((NSUInteger)offset, (NSUInteger)length)]];
            if (be32(arch) == CPU_TYPE_ARM64) {
                LSBSigningSlice *slice = machoSlice(fd, offset, length, error);
                if (!slice) return nil;
                if ((uint32_t)slice.subtype != be32(arch + 4)) goto unsupported;
                [result addObject:slice];
            }
        }
        if (!result.count) goto unsupported;
        return result;
    }
    {
        LSBSigningSlice *slice = machoSlice(fd, 0, size, error);
        return slice ? @[slice] : nil;
    }
unsupported:
    failure(error, ENOTSUP, @"Unsupported runtime format, cache layout or signature allocation");
    return nil;
}

static NSDictionary<NSNumber *, NSData *> *signatureSlots(int fd, LSBSigningSlice *slice, NSError **error) {
    uint8_t header[12];
    uint64_t base = slice.offset + slice.signatureOffset;
    if (!readBytes(fd, header, sizeof(header), base, error)) return nil;
    uint32_t length = be32(header + 4), count = be32(header + 8);
    uint32_t tableSize = 12 + count * 8;
    if (be32(header) != SuperblobMagic || (count != 2 && count != 3) || length < tableSize || length > slice.signatureCapacity) {
        fprintf(stderr, "[native-sign] signature-shape magic=0x%x count=%u length=%u capacity=%llu\n",
            be32(header), count, length, (unsigned long long)slice.signatureCapacity);
        failure(error, EFTYPE, @"Security produced an unsupported signature superblob"); return nil;
    }
    NSMutableData *signature = [NSMutableData dataWithLength:length];
    if (!readBytes(fd, signature.mutableBytes, signature.length, base, error)) return nil;
    NSMutableDictionary *result = [NSMutableDictionary new];
    NSMutableArray<NSValue *> *ranges = [NSMutableArray new];
    const uint8_t *bytes = signature.bytes;
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t kind = be32(bytes + 12 + index * 8), offset = be32(bytes + 16 + index * 8);
        uint32_t expected = kind == 0 ? CodeDirectoryMagic : kind == 2 ? RequirementsMagic : kind == 0x10000 ? CMSMagic : 0;
        if (!expected || result[@(kind)] || offset < tableSize || !rangeFits(offset, 8, length)) {
            fprintf(stderr, "[native-sign] unsupported signature-slot index=%u kind=0x%x offset=%u\n", index, kind, offset); goto invalid;
        }
        uint32_t blobLength = be32(bytes + offset + 4);
        if (be32(bytes + offset) != expected || blobLength < 8 || !rangeFits(offset, blobLength, length)) goto invalid;
        NSRange range = NSMakeRange(offset, blobLength);
        for (NSValue *other in ranges) if (NSIntersectionRange(range, other.rangeValue).length) goto invalid;
        [ranges addObject:[NSValue valueWithRange:range]];
        result[@(kind)] = [signature subdataWithRange:range];
    }
    if (!result[@0] || !result[@0x10000]) goto invalid;
    if (!result[@2]) {
        // The iOS Security writer omits the optional requirements slot. Canonicalize to an empty requirements set,
        // whose complete blob is hashed into special slot 2 when rebuilding the final CodeDirectory below.
        uint32_t empty[] = {OSSwapHostToBigInt32(RequirementsMagic), OSSwapHostToBigInt32(12), 0};
        result[@2] = [NSData dataWithBytes:empty length:sizeof(empty)];
    }
    return result;
invalid:
    failure(error, EFTYPE, @"Security produced an unsupported signature (entitlements and alternate CodeDirectories are not allowed)");
    return nil;
}

static NSData *directoryString(NSData *directory, uint32_t offset, uint32_t limit, NSError **error) {
    if (offset < sizeof(CodeDirectory) || offset >= limit || limit > directory.length) {
        failure(error, EFTYPE, @"Invalid CodeDirectory string"); return nil;
    }
    const uint8_t *begin = (const uint8_t *)directory.bytes + offset;
    const uint8_t *end = memchr(begin, 0, limit - offset);
    if (!end) { failure(error, EFTYPE, @"Unterminated CodeDirectory string"); return nil; }
    return [NSData dataWithBytes:begin length:(NSUInteger)(end - begin + 1)];
}

static NSData *rebuiltDirectory(int fd, LSBSigningSlice *slice, NSDictionary *slots, NSString *identifier, NSError **error) {
    NSData *old = slots[@0], *requirements = slots[@2];
    if (old.length < sizeof(CodeDirectory)) { failure(error, EFTYPE, @"Truncated Security CodeDirectory"); return nil; }
    const uint8_t *bytes = old.bytes;
    uint32_t version = be32(bytes + 8), hashes = be32(bytes + 16), special = be32(bytes + 24);
    if (version < 0x20400 || version > 0x20600 || special > 2 || bytes[36] != 32 || bytes[37] != 2 || hashes < special * 32) {
        fprintf(stderr, "[native-sign] directory-shape version=0x%x length=%lu hash-offset=%u hash-size=%u hash-type=%u special=%u identifier-offset=%u team-offset=%u\n",
            version, (unsigned long)old.length, hashes, bytes[36], bytes[37], special, be32(bytes + 20), be32(bytes + 48));
        failure(error, ENOTSUP, @"Unsupported Security CodeDirectory layout"); return nil;
    }
    NSData *name = directoryString(old, be32(bytes + 20), hashes - special * 32, error);
    NSData *team = be32(bytes + 48) ? directoryString(old, be32(bytes + 48), hashes - special * 32, error) : [NSData data];
    NSMutableData *expectedName = [[identifier dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
    uint8_t zero = 0; [expectedName appendBytes:&zero length:1];
    if (!name || !team || ![name isEqualToData:expectedName]) {
        fprintf(stderr, "[native-sign] directory-strings version=0x%x length=%lu hash-offset=%u special=%u identifier-offset=%u team-offset=%u name-valid=%u team-valid=%u match=%u\n",
            version, (unsigned long)old.length, hashes, special, be32(bytes + 20), be32(bytes + 48), name != nil, team != nil, [name isEqualToData:expectedName]);
        failure(error, EFTYPE, @"Security signing identifier or team metadata is invalid"); return nil;
    }
    uint64_t count64 = (slice.signatureOffset + SignPageSize - 1) / SignPageSize;
    uint64_t hashOffset64 = sizeof(CodeDirectory) + name.length + team.length + 64;
    uint64_t length64 = hashOffset64 + count64 * 32;
    if (length64 > MaximumSignatureSize || slice.signatureOffset >= UINT32_MAX) {
        failure(error, EFBIG, @"Runtime signature exceeds the supported contiguous range"); return nil;
    }
    CodeDirectory header = {.magic = OSSwapHostToBigInt32(CodeDirectoryMagic), .length = OSSwapHostToBigInt32((uint32_t)length64),
        .version = OSSwapHostToBigInt32(0x20400), .hashOffset = OSSwapHostToBigInt32((uint32_t)hashOffset64),
        .identifierOffset = OSSwapHostToBigInt32(sizeof(CodeDirectory)), .specialSlots = OSSwapHostToBigInt32(2),
        .codeSlots = OSSwapHostToBigInt32((uint32_t)count64), .codeLimit = OSSwapHostToBigInt32((uint32_t)slice.signatureOffset),
        .hashSize = 32, .hashType = 2, .pageShift = 14,
        .teamOffset = team.length ? OSSwapHostToBigInt32((uint32_t)(sizeof(CodeDirectory) + name.length)) : 0,
        .executableBase = OSSwapHostToBigInt64(slice.executableBase), .executableLimit = OSSwapHostToBigInt64(slice.executableLimit)};
    // No scatter table, entitlements, CS_ADHOC, or CS_EXECSEG_MAIN_BINARY. The original header and load commands stay sealed unchanged.
    NSMutableData *directory = [NSMutableData dataWithCapacity:(NSUInteger)length64];
    [directory appendBytes:&header length:sizeof(header)]; [directory appendData:name]; [directory appendData:team];
    uint8_t hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(requirements.bytes, (CC_LONG)requirements.length, hash); [directory appendBytes:hash length:sizeof(hash)];
    memset(hash, 0, sizeof(hash)); [directory appendBytes:hash length:sizeof(hash)];
    uint8_t page[SignPageSize];
    for (uint64_t index = 0; index < count64; ++index) {
        size_t count = (size_t)MIN((uint64_t)sizeof(page), slice.signatureOffset - index * sizeof(page));
        if (!readBytes(fd, page, count, slice.offset + index * sizeof(page), error)) return nil;
        CC_SHA256(page, (CC_LONG)count, hash); [directory appendBytes:hash length:sizeof(hash)];
    }
    return directory;
}

static NSData *signDirectory(NSData *directory, SecIdentityRef identity, NSArray *certificates, NSError **error) {
    uint8_t hash[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(directory.bytes, (CC_LONG)directory.length, hash);
    NSDictionary *v1 = @{@"cdhashes": @[[NSData dataWithBytes:hash length:20]]};
    NSData *agility = [NSPropertyListSerialization dataWithPropertyList:v1 format:NSPropertyListXMLFormat_v1_0 options:0 error:error];
    if (!agility) return nil;
    // SEC_OID_SHA256 = 192 in Apple's Security CMS ABI; same attributes as Tools/sign-code-directory.m.
    NSDictionary *v2 = @{@192: [NSData dataWithBytes:hash length:sizeof(hash)]};
    CMSEncoderRef encoder = NULL;
    CFDataRef output = NULL;
    SecCertificateRef leaf = NULL;
    const char *stage = "SecIdentityCopyCertificate";
    OSStatus status = SecIdentityCopyCertificate(identity, &leaf);
    NSMutableArray *supporting = [NSMutableArray new];
    if (!status) for (id certificate in certificates)
        if (!CFEqual((__bridge CFTypeRef)certificate, leaf) && ![supporting containsObject:certificate]) [supporting addObject:certificate];
    if (leaf) CFRelease(leaf);
    if (!status) { stage = "CMSEncoderCreate"; status = CMSEncoderCreate(&encoder); }
#define CMS_CALL(call) do { if (!status) { stage = #call; status = (call); } } while (0)
    CMS_CALL(CMSEncoderSetCertificateChainMode(encoder, kCMSCertificateSignerOnly));
    CMS_CALL(CMSEncoderAddSigners(encoder, identity));
    if (supporting.count) CMS_CALL(CMSEncoderAddSupportingCerts(encoder, (__bridge CFArrayRef)supporting));
    CMS_CALL(CMSEncoderSetSignerAlgorithm(encoder, kCMSEncoderDigestAlgorithmSHA256));
    CMS_CALL(CMSEncoderSetHasDetachedContent(encoder, true));
    CMS_CALL(CMSEncoderAddSignedAttributes(encoder, kCMSAttrSigningTime | kCMSAttrAppleCodesigningHashAgility | kCMSAttrAppleCodesigningHashAgilityV2));
    CMS_CALL(CMSEncoderSetSigningTime(encoder, CFAbsoluteTimeGetCurrent()));
    CMS_CALL(CMSEncoderSetAppleCodesigningHashAgility(encoder, (__bridge CFDataRef)agility));
    CMS_CALL(CMSEncoderSetAppleCodesigningHashAgilityV2(encoder, (__bridge CFDictionaryRef)v2));
    CMS_CALL(CMSEncoderUpdateContent(encoder, directory.bytes, directory.length));
    CMS_CALL(CMSEncoderCopyEncodedContent(encoder, &output));
#undef CMS_CALL
    if (encoder) CFRelease(encoder);
    if (status || !output) {
        if (output) CFRelease(output);
        size_t length = strcspn(stage, "(");
        fprintf(stderr, "[native-sign] Security stage=%.*s status=%d\n", (int)length, stage, (int)status);
        securityFailure(error, status ? status : errSecInternalComponent,
            [NSString stringWithFormat:@"Sign runtime CodeDirectory (%.*s)", (int)length, stage]); return nil;
    }
    return CFBridgingRelease(output);
}

static BOOL signScratch(NSURL *url, SecIdentityRef identity, NSString *identifier, NSError **error) {
    // The native Security SPI sequence also used by LiveContainer's codesign/CodeSigner.m at 6b7e28503e97188fca754214948bd4fa4cb92251.
    // Only this disposable scratch copy is passed to its writer; the final output uses the original allocation and bytes.
    NSDictionary *parameters = @{(__bridge id)kSecCodeSignerIdentity: (__bridge id)identity,
        (__bridge id)kSecCodeSignerIdentifier: identifier, (__bridge id)kSecCodeSignerFlags: @0,
        (__bridge id)kSecCodeSignerPageSize: @(SignPageSize), (__bridge id)kSecCodeSignerDigestAlgorithm: @2,
        (__bridge id)kSecCodeSignerRequireTimestamp: @NO, (__bridge id)kSecCodeSignerPreserveMetadata: @0};
    SecCodeSignerRef signer = NULL;
    SecStaticCodeRef code = NULL;
    CFErrorRef detail = NULL;
    const char *stage = "SecCodeSignerCreate";
    OSStatus status = SecCodeSignerCreate((__bridge CFDictionaryRef)parameters, 0, &signer);
    if (!status) {
        stage = "SecStaticCodeCreateWithPathAndAttributes";
        status = SecStaticCodeCreateWithPathAndAttributes((__bridge CFURLRef)url, 0, (__bridge CFDictionaryRef)@{}, &code);
    }
    if (!status) { stage = "SecCodeSignerAddSignatureWithErrors"; status = SecCodeSignerAddSignatureWithErrors(signer, code, 0, &detail); }
    if (code) CFRelease(code);
    if (signer) CFRelease(signer);
    if (status) {
        fprintf(stderr, "[native-sign] Security stage=%s status=%d\n", stage, (int)status);
        if (detail) { if (error) *error = CFBridgingRelease(detail); else CFRelease(detail); return NO; }
        return securityFailure(error, status, @"Security runtime signing");
    }
    if (detail) CFRelease(detail);
    return YES;
}

static NSData *buildSignature(NSData *directory, NSData *requirements, NSData *cms, uint64_t capacity, NSError **error) {
    uint64_t length = 36 + directory.length + requirements.length + 8 + cms.length;
    if (length > capacity || length > UINT32_MAX) {
        failure(error, ENOSPC, @"The new signature does not fit the binary's existing signature allocation"); return nil;
    }
    uint32_t header[] = {OSSwapHostToBigInt32(SuperblobMagic), OSSwapHostToBigInt32((uint32_t)length), OSSwapHostToBigInt32(3),
        0, OSSwapHostToBigInt32(36), OSSwapHostToBigInt32(2), OSSwapHostToBigInt32((uint32_t)(36 + directory.length)),
        OSSwapHostToBigInt32(0x10000), OSSwapHostToBigInt32((uint32_t)(36 + directory.length + requirements.length))};
    NSMutableData *result = [NSMutableData dataWithBytes:header length:sizeof(header)];
    [result appendData:directory]; [result appendData:requirements];
    uint32_t wrapper[] = {OSSwapHostToBigInt32(CMSMagic), OSSwapHostToBigInt32((uint32_t)(8 + cms.length))};
    [result appendBytes:wrapper length:sizeof(wrapper)]; [result appendData:cms];
    return result;
}

static BOOL writeSignature(int fd, LSBSigningSlice *slice, NSData *signature, NSError **error) {
    uint8_t zeros[32768] = {0};
    for (uint64_t position = 0; position < slice.signatureCapacity;) {
        const void *bytes = position < signature.length ? (const uint8_t *)signature.bytes + position : zeros;
        size_t count = (size_t)MIN(position < signature.length ? signature.length - position : sizeof(zeros), slice.signatureCapacity - position);
        ssize_t written = pwrite(fd, bytes, count, (off_t)(slice.offset + slice.signatureOffset + position));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return failure(error, written ? errno : EIO, @"Cannot write the staged runtime signature");
        position += (uint64_t)written;
    }
    return YES;
}

static NSUInteger matchingSlice(NSArray<LSBSigningSlice *> *slices, uint64_t sliceOffset, uint64_t blobOffset, uint64_t blobSize) {
    NSUInteger result = NSNotFound;
    for (NSUInteger index = 0; index < slices.count; ++index) {
        LSBSigningSlice *slice = slices[index];
        if (slice.offset == sliceOffset && slice.signatureOffset == blobOffset && slice.signatureCapacity == blobSize) {
            if (result != NSNotFound) return NSNotFound;
            result = index;
        }
    }
    return result;
}

static BOOL sameInput(const struct stat *before, const struct stat *after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino && before->st_size == after->st_size &&
        before->st_mode == after->st_mode && before->st_uid == after->st_uid && before->st_gid == after->st_gid &&
        before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec && before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
        before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec && before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
}

static BOOL prepareScratch(int input, uint64_t size, const char *path, NSError **error) {
    int scratch = -1;
    BOOL success = NO;
    uint8_t *buffer = NULL;
    if (!fclonefileat(input, AT_FDCWD, path, CLONE_NOOWNERCOPY)) {
        // A cloned 0444 input cannot be opened for writing until its private copy has been made owner-writable.
        scratch = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (scratch < 0 || fchmod(scratch, 0600)) failure(error, errno, @"Cannot prepare signing scratch file");
        else success = YES;
    } else {
        int cloneError = errno;
        if (cloneError != ENOTSUP && cloneError != EXDEV && cloneError != EINVAL && cloneError != ENOSYS &&
            cloneError != EPERM && cloneError != EACCES) return failure(error, cloneError, @"Cannot clone signing scratch file");
        scratch = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (scratch < 0) return failure(error, errno, @"Cannot create signing scratch file");
        buffer = malloc(131072);
        if (!buffer) { failure(error, ENOMEM, @"Cannot allocate signing scratch copy buffer"); goto finish; }
        for (uint64_t offset = 0; offset < size;) {
            size_t count = (size_t)MIN((uint64_t)131072, size - offset);
            if (!readBytes(input, buffer, count, offset, error)) goto finish;
            size_t done = 0;
            while (done < count) {
                ssize_t written = pwrite(scratch, buffer + done, count - done, (off_t)(offset + done));
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { failure(error, written ? errno : EIO, @"Cannot copy signing scratch file"); goto finish; }
                done += (size_t)written;
            }
            offset += count;
        }
        success = YES;
    }
finish:
    free(buffer);
    if (scratch >= 0 && close(scratch) && success) success = failure(error, errno, @"Cannot close signing scratch file");
    return success;
}

static NSArray<NSData *> *createSignatures(int fd, BOOL exactSlice, uint64_t sliceOffset, uint64_t blobOffset, uint64_t blobSize,
                                          SecIdentityRef identity, NSArray *certificates, NSString *identifier,
                                          NSArray<LSBSigningSlice *> **writtenSlices, NSError **error) {
    if (error) *error = nil;
    if (writtenSlices) *writtenSlices = nil;
    if (!identity || CFGetTypeID(identity) != SecIdentityGetTypeID() || !identifier.length ||
        [identifier rangeOfString:@"\0"].location != NSNotFound || ![certificates isKindOfClass:NSArray.class]) {
        failure(error, EINVAL, @"Missing signing identity or identifier"); return nil;
    }
    for (id certificate in certificates) if (CFGetTypeID((__bridge CFTypeRef)certificate) != SecCertificateGetTypeID()) {
        failure(error, EINVAL, @"Invalid certificate chain"); return nil;
    }
    struct stat before, after;
    NSArray<LSBSigningSlice *> *original = nil, *signedSlices = nil;
    NSArray *signingCertificates = nil;
    NSMutableArray<NSData *> *signatures = nil;
    NSMutableArray<LSBSigningSlice *> *outputSlices = nil;
    NSArray<NSData *> *result = nil;
    NSUInteger selected = NSNotFound;
    char *temporary = NULL, *scratchDirectory = NULL;
    BOOL createdDirectory = NO;
    int scratchFD = -1;
    if (fstat(fd, &before)) { failure(error, errno, @"Cannot inspect runtime signing input"); goto finish; }
    if (!S_ISREG(before.st_mode) || before.st_size <= 0) { failure(error, EINVAL, @"Signing requires a regular input file"); goto finish; }
    original = readSlices(fd, (uint64_t)before.st_size, NO, error);
    if (!original) goto finish;
    if (exactSlice) {
        selected = matchingSlice(original, sliceOffset, blobOffset, blobSize);
        if (selected == NSNotFound) { failure(error, ENOEXEC, @"Requested signature allocation does not match the runtime binary"); goto finish; }
    }
    {
        SecCertificateRef leaf = NULL;
        OSStatus status = SecIdentityCopyCertificate(identity, &leaf);
        if (status) { securityFailure(error, status, @"Read imported signing certificate"); goto finish; }
        signingCertificates = chainWithPublicIntermediate(leaf, certificates, error);
        CFRelease(leaf);
        if (!signingCertificates) goto finish;
    }
    scratchDirectory = strdup([[NSTemporaryDirectory() stringByAppendingPathComponent:@".lsb-security-sign.XXXXXX"]
                               fileSystemRepresentation]);
    if (!scratchDirectory) { failure(error, ENOMEM, @"Cannot allocate signing scratch path"); goto finish; }
    if (!mkdtemp(scratchDirectory)) { failure(error, errno, @"Cannot create private signing scratch directory"); goto finish; }
    createdDirectory = YES;
    if (asprintf(&temporary, "%s/binary", scratchDirectory) < 0) { failure(error, ENOMEM, @"Cannot allocate signing scratch file path"); goto finish; }
    if (!prepareScratch(fd, (uint64_t)before.st_size, temporary, error)) goto finish;
    if (!signScratch([NSURL fileURLWithFileSystemRepresentation:temporary isDirectory:NO relativeToURL:nil], identity, identifier, error)) goto finish;
    scratchFD = open(temporary, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat scratchStat;
    if (scratchFD < 0 || fstat(scratchFD, &scratchStat)) { failure(error, errno, @"Cannot read Security signing result"); goto finish; }
    signedSlices = readSlices(scratchFD, (uint64_t)scratchStat.st_size, YES, error);
    if (!signedSlices) goto finish;
    if (signedSlices.count != original.count) { failure(error, EFTYPE, @"Security changed the architecture count"); goto finish; }
    signatures = [NSMutableArray new];
    outputSlices = [NSMutableArray new];
    for (NSUInteger index = 0; index < original.count; ++index) {
        if (exactSlice && index != selected) continue;
        LSBSigningSlice *input = original[index], *signedSlice = signedSlices[index];
        if (input.cache != signedSlice.cache || input.subtype != signedSlice.subtype || input.fileType != signedSlice.fileType) {
            failure(error, EFTYPE, @"Security changed the selected binary architecture"); goto finish;
        }
        NSDictionary *slots = signatureSlots(scratchFD, signedSlice, error);
        NSData *directory = slots ? rebuiltDirectory(fd, input, slots, identifier, error) : nil;
        NSData *cms = directory ? signDirectory(directory, identity, signingCertificates, error) : nil;
        NSData *signature = cms ? buildSignature(directory, slots[@2], cms, input.signatureCapacity, error) : nil;
        if (!signature) goto finish;
        [signatures addObject:signature];
        [outputSlices addObject:input];
    }
    if (fstat(fd, &after)) { failure(error, errno, @"Cannot recheck runtime signing input"); goto finish; }
    if (!sameInput(&before, &after)) { failure(error, ESTALE, @"Runtime signing input changed while creating its signature"); goto finish; }
    result = signatures;
    if (writtenSlices) *writtenSlices = outputSlices;
finish:
    if (scratchFD >= 0) close(scratchFD);
    if (temporary) { unlink(temporary); free(temporary); }
    if (createdDirectory) rmdir(scratchDirectory);
    free(scratchDirectory);
    return result;
}

NSData *LSBCreateRuntimeSignature(int inputFD, uint64_t sliceOffset, uint64_t blobOffset, uint64_t blobSize,
                                SecIdentityRef identity, NSArray *certificates, NSString *identifier, NSError **error) {
    if (error) *error = nil;
    int flags = fcntl(inputFD, F_GETFL);
    if (flags < 0) { failure(error, errno, @"Cannot inspect runtime signing descriptor"); return nil; }
    if ((flags & O_ACCMODE) != O_RDONLY) { failure(error, EINVAL, @"Signature creation requires a read-only input descriptor"); return nil; }
    return createSignatures(inputFD, YES, sliceOffset, blobOffset, blobSize, identity, certificates, identifier, NULL, error).firstObject;
}

static BOOL signRuntimeBinary(NSURL *stagedFile, BOOL exactSlice, uint64_t sliceOffset, uint64_t blobOffset, uint64_t blobSize,
                              SecIdentityRef identity, NSArray *certificates, NSString *identifier, NSError **error) {
    if (error) *error = nil;
    if (!stagedFile.isFileURL) return failure(error, EINVAL, @"Missing signing staging file");
    int fd = open(stagedFile.fileSystemRepresentation, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return failure(error, errno, @"Cannot open the staged runtime file");
    BOOL success = NO;
    struct stat info;
    NSArray<LSBSigningSlice *> *slices = nil;
    NSArray<NSData *> *signatures = nil;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size <= 0) {
        failure(error, EINVAL, @"Signing requires a private regular staging file"); goto finish;
    }
    signatures = createSignatures(fd, exactSlice, sliceOffset, blobOffset, blobSize, identity, certificates, identifier, &slices, error);
    if (!signatures) goto finish;
    // The file-writing wrappers are only for explicit private fixtures/staging files. The native service returns the blob directly.
    for (NSUInteger index = 0; index < slices.count; ++index)
        if (!writeSignature(fd, slices[index], signatures[index], error)) goto finish;
    success = fsync(fd) == 0;
    if (!success) failure(error, errno, @"Cannot flush the staged runtime signature");
finish:
    close(fd);
    return success;
}

BOOL LSBSignRuntimeBinary(NSURL *stagedFile, SecIdentityRef identity, NSArray *certificates, NSString *identifier, NSError **error) {
    return signRuntimeBinary(stagedFile, NO, 0, 0, 0, identity, certificates, identifier, error);
}

BOOL LSBSignRuntimeBinarySlice(NSURL *stagedFile, uint64_t sliceOffset, uint64_t blobOffset, uint64_t blobSize,
                              SecIdentityRef identity, NSArray *certificates, NSString *identifier, NSError **error) {
    return signRuntimeBinary(stagedFile, YES, sliceOffset, blobOffset, blobSize, identity, certificates, identifier, error);
}
