#import "../Host/GuestEntitlementsConversion.h"
#include "../ProcessSupport/GuestEntitlements.h"
#import <Foundation/Foundation.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Apple's codesign --generate-entitlement-der output, independently generated from the expected plist below.
static const unsigned char nestedDER[] =
    "\x70\x62\x02\x01\x01\xb0\x5d\x30\x06\x0c\x01\x61\x01\x01\xff\x30\x23\x0c\x05\x61\x72\x72\x61\x79"
    "\x30\x1a\x01\x01\x00\x02\x01\x11\x0c\x04\x74\x65\x78\x74\xb0\x0c\x30\x0a\x0c\x05\x69\x6e\x6e\x65"
    "\x72\x02\x01\x2a\x30\x0e\x0c\x08\x6e\x65\x67\x61\x74\x69\x76\x65\x02\x02\xff\x7f\x30\x0e\x0c\x08"
    "\x70\x6f\x73\x69\x74\x69\x76\x65\x02\x02\x00\x80\x30\x0e\x0c\x04\x74\x65\x78\x74\x0c\x06\xc3\xa9\xf0\x9d\x84\x9e";

static NSData *blobForDER(NSData *raw) {
    uint32_t size = (uint32_t)raw.length + 8;
    const unsigned char header[] = {0xfa, 0xde, 0x71, 0x72, size >> 24, size >> 16, size >> 8, size};
    NSMutableData *result = [NSMutableData dataWithBytes:header length:sizeof(header)];
    [result appendData:raw];
    return result;
}

static NSDictionary *converted(NSData *blob) {
    void *bytes = NULL; size_t size = 0;
    NSDictionary *decoded = nil;
    int error = LSBCopyXMLFromDEREntitlementsWithDictionary(blob.bytes, blob.length, &bytes, &size, &decoded);
    if (error) fprintf(stderr, "conversion failed: %d (%s)\n", error, strerror(error));
    assert(!error && LSBGuestEntitlementsHasValidBlobHeaderForOperation(bytes, size, 7));
    NSData *xml = [NSData dataWithBytes:(unsigned char *)bytes + 8 length:size - 8];
    free(bytes);
    NSPropertyListFormat format = 0;
    id dictionary = [NSPropertyListSerialization propertyListWithData:xml options:0 format:&format error:NULL];
    assert([dictionary isKindOfClass:NSDictionary.class] && format == NSPropertyListXMLFormat_v1_0);
    assert(decoded && [decoded isEqual:dictionary]);
    return decoded;
}

static void expectError(NSData *blob, int expected) {
    void *bytes = (void *)1; size_t size = 99;
    int error = LSBCopyXMLFromDEREntitlements(blob.bytes, blob.length, &bytes, &size);
    if (error != expected) fprintf(stderr, "conversion error=%d expected=%d\n", error, expected);
    assert(error == expected && !bytes && !size);
    NSDictionary *dictionary = @{ @"stale": @YES };
    bytes = (void *)1; size = 99;
    assert(LSBCopyXMLFromDEREntitlementsWithDictionary(blob.bytes, blob.length, &bytes, &size, &dictionary) == expected);
    assert(!dictionary && !bytes && !size);
}

static void assertImmutableContainers(id value) {
    assert(![value isKindOfClass:NSMutableDictionary.class] && ![value isKindOfClass:NSMutableArray.class]);
    if ([value isKindOfClass:NSDictionary.class]) for (id key in value) assertImmutableContainers(value[key]);
    if ([value isKindOfClass:NSArray.class]) for (id child in value) assertImmutableContainers(child);
}

static NSData *tlv(unsigned char tag, NSData *content) {
    unsigned char header[2 + sizeof(size_t)] = {tag};
    size_t count = 0, size = content.length;
    if (size >= 128) for (size_t n = size; n; n >>= 8) ++count;
    header[1] = count ? 128 | count : size;
    for (size_t i = 0; i < count; ++i) header[2 + i] = size >> (8 * (count - i - 1));
    NSMutableData *result = [NSMutableData dataWithBytes:header length:2 + count];
    [result appendData:content];
    return result;
}

static NSData *singleValueDER(NSData *value) {
    const unsigned char key[] = {0x0c, 1, 'a'}, version[] = {2, 1, 1};
    NSMutableData *entry = [NSMutableData dataWithBytes:key length:sizeof(key)];
    [entry appendData:value];
    NSMutableData *root = [NSMutableData dataWithBytes:version length:sizeof(version)];
    [root appendData:tlv(0xb0, tlv(0x30, entry))];
    return tlv(0x70, root);
}

static void testConversion(void) {
    NSData *blob = blobForDER([NSData dataWithBytes:nestedDER length:sizeof(nestedDER) - 1]);
    NSDictionary *expected = @{ @"a": @YES, @"array": @[@NO, @17, @"text", @{@"inner": @42}],
        @"negative": @(-129), @"positive": @128, @"text": @"é𝄞" };
    NSDictionary *nested = converted(blob);
    assert([nested isEqual:expected]);
    assertImmutableContainers(nested);
    assert(CFGetTypeID((__bridge CFTypeRef)nested[@"a"]) == CFBooleanGetTypeID());
    assert(CFGetTypeID((__bridge CFTypeRef)nested[@"positive"]) == CFNumberGetTypeID());
    assert(CFGetTypeID((__bridge CFTypeRef)nested[@"array"][0]) == CFBooleanGetTypeID());
    NSDictionary *snapshot;
    @autoreleasepool {
        NSMutableData *input = [blob mutableCopy];
        void *xml = NULL; size_t xmlSize = 0;
        assert(!LSBCopyXMLFromDEREntitlementsWithDictionary(input.bytes, input.length, &xml, &xmlSize, &snapshot));
        memset(input.mutableBytes, 0, input.length);
        memset(xml, 0, xmlSize); free(xml);
    }
    assert([snapshot isEqual:expected]); // Owned decoded values survive source/XML destruction and the converter's autorelease pool.
    assertImmutableContainers(snapshot);
    void *legacyXML = NULL, *currentXML = NULL; size_t legacySize = 0, currentSize = 0;
    assert(!LSBCopyXMLFromDEREntitlements(blob.bytes, blob.length, &legacyXML, &legacySize));
    assert(!LSBCopyXMLFromDEREntitlementsWithDictionary(blob.bytes, blob.length, &currentXML, &currentSize, NULL));
    assert(legacySize == currentSize && !memcmp(legacyXML, currentXML, legacySize));
    free(legacyXML); free(currentXML);
    const unsigned char empty[] = {0x70, 5, 2, 1, 1, 0xb0, 0};
    assert([converted(blobForDER([NSData dataWithBytes:empty length:sizeof(empty)])) isEqual:@{}]);
    const unsigned char unicode[] = {0x70, 0x1a, 2, 1, 1, 0xb0, 0x15,
        0x30, 8, 0x0c, 3, 0xee, 0x80, 0x80, 1, 1, 0xff, 0x30, 9, 0x0c, 4, 0xf0, 0x90, 0x80, 0x80, 1, 1, 0xff};
    NSDictionary *unicodeResult = converted(blobForDER([NSData dataWithBytes:unicode length:sizeof(unicode)]));
    NSString *bmpKey = [[NSString alloc] initWithBytes:"\xee\x80\x80" length:3 encoding:NSUTF8StringEncoding];
    NSString *supplementaryKey = [[NSString alloc] initWithBytes:"\xf0\x90\x80\x80" length:4 encoding:NSUTF8StringEncoding];
    NSDictionary *unicodeExpected = @{bmpKey: @YES, supplementaryKey: @YES};
    assert([unicodeResult isEqual:unicodeExpected]);
    const unsigned char minimum[] = {2, 8, 0x80, 0, 0, 0, 0, 0, 0, 0}, maximum[] = {2, 8, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    assert([converted(blobForDER(singleValueDER([NSData dataWithBytes:minimum length:sizeof(minimum)]))) isEqual:@{@"a": @(INT64_MIN)}]);
    assert([converted(blobForDER(singleValueDER([NSData dataWithBytes:maximum length:sizeof(maximum)]))) isEqual:@{@"a": @(INT64_MAX)}]);
    const unsigned char string[] = {0x0c, 5, '<', '&', '>', '"', '\''};
    assert([converted(blobForDER(singleValueDER([NSData dataWithBytes:string length:sizeof(string)]))) isEqual:@{@"a": @"<&>\"'"}]);

    for (size_t i = 0; i < blob.length; ++i) expectError([blob subdataWithRange:NSMakeRange(0, i)], EINVAL);
    NSMutableData *invalid = [blob mutableCopy]; ((unsigned char *)invalid.mutableBytes)[3] = 0x71; expectError(invalid, EINVAL);
    invalid = [blob mutableCopy]; ((unsigned char *)invalid.mutableBytes)[8] = 0x31; expectError(invalid, EINVAL);
    // This is valid DER UTF-8 but cannot be represented as an XPC entitlement string. Reject rather than truncate at NUL.
    const unsigned char nulValue[] = {0x0c, 3, 'a', 0, 'b'};
    expectError(blobForDER(singleValueDER([NSData dataWithBytes:nulValue length:sizeof(nulValue)])), EINVAL);
    void *bytes = (void *)1; size_t size = 99;
    assert(LSBCopyXMLFromDEREntitlements(blob.bytes, blob.length, NULL, &size) == EINVAL && !size);
    assert(LSBCopyXMLFromDEREntitlements(blob.bytes, blob.length, &bytes, NULL) == EINVAL && !bytes);
    snapshot = expected; size = 99;
    assert(LSBCopyXMLFromDEREntitlementsWithDictionary(blob.bytes, blob.length, NULL, &size, &snapshot) == EINVAL && !size && !snapshot);
    snapshot = expected; bytes = (void *)1;
    assert(LSBCopyXMLFromDEREntitlementsWithDictionary(blob.bytes, blob.length, &bytes, NULL, &snapshot) == EINVAL && !bytes && !snapshot);

    // Small DER can expand past the XML transport cap because markup characters require escaping.
    NSMutableData *large = [NSMutableData dataWithLength:220000]; memset(large.mutableBytes, '&', large.length);
    NSData *largeBlob = blobForDER(singleValueDER(tlv(0x0c, large)));
    assert(largeBlob.length < LSB_GUEST_ENTITLEMENTS_MAX_BLOB_SIZE);
    expectError(largeBlob, E2BIG);
}

// XML is read only in this test to independently compare every original entitlement with the production DER-derived snapshot.
static NSDictionary *originalXML(const char *path) {
    NSData *file = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
    assert(file.length >= sizeof(struct mach_header_64));
    const unsigned char *bytes = file.bytes;
    struct mach_header_64 header; memcpy(&header, bytes, sizeof(header));
    assert(header.magic == MH_MAGIC_64 && header.sizeofcmds <= file.length - sizeof(header));
    size_t position = sizeof(header), end = position + header.sizeofcmds;
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        struct load_command command; assert(end - position >= sizeof(command)); memcpy(&command, bytes + position, sizeof(command));
        assert(command.cmdsize >= sizeof(command) && command.cmdsize <= end - position);
        if (command.cmd == LC_SEGMENT_64) {
            struct segment_command_64 segment; assert(command.cmdsize >= sizeof(segment)); memcpy(&segment, bytes + position, sizeof(segment));
            assert(segment.nsects <= (command.cmdsize - sizeof(segment)) / sizeof(struct section_64));
            for (uint32_t j = 0; j < segment.nsects; ++j) {
                struct section_64 section; memcpy(&section, bytes + position + sizeof(segment) + j * sizeof(section), sizeof(section));
                if (strncmp(section.segname, "__TEXT", 16) || strncmp(section.sectname, "__entitlements", 16)) continue;
                assert(section.offset <= file.length && section.size <= file.length - section.offset);
                NSData *xml = [NSData dataWithBytes:bytes + section.offset length:section.size];
                NSDictionary *result = [NSPropertyListSerialization propertyListWithData:xml options:0 format:NULL error:NULL];
                assert([result isKindOfClass:NSDictionary.class]);
                return result;
            }
        }
        position += command.cmdsize;
    }
    assert(!"missing original __entitlements test oracle");
    return nil;
}

static void testStock(const char *path) {
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    int fd = open(path, O_RDONLY | O_CLOEXEC); assert(fd >= 0);
    unsigned char *bytes = NULL; size_t size = 0;
    assert(!LSBReadMachODEREntitlements(fd, &bytes, &size)); close(fd);
    uint64_t readEnd = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    NSData *blob = [NSData dataWithBytes:bytes length:size]; free(bytes);
    NSDictionary *result = converted(blob);
    uint64_t conversionEnd = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    NSDictionary *expected = originalXML(path);
    if (![result isEqual:expected]) {
        for (NSString *key in expected) if (![result[key] isEqual:expected[key]]) fprintf(stderr, "mismatching key: %s\n", key.UTF8String);
    }
    assert([result isEqual:expected]);
    printf("stock DER->XML: %s (%lu keys, %zu bytes DER; read %.3f ms, conversion %.3f ms)\n", path,
        (unsigned long)result.count, size - 8, (readEnd - start) / 1e6, (conversionEnd - readEnd) / 1e6);
}

int main(int argc, const char **argv) {
    alarm(30);
    @autoreleasepool {
        testConversion();
        for (int i = 1; i < argc; ++i) testStock(argv[i]);
    }
    puts("GuestEntitlementsConversion: native CoreEntitlements, complete plist equality, nested types, framing and XML expansion bounds passed");
    return 0;
}
