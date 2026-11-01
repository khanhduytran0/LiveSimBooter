#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <xpc/xpc.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
// Real memory-only PKCS12 parsing and read-only file I/O; the signing core is replaced with a bounded signature-data fixture.
#include "../NativeSigner/Service.m"

typedef enum {
    SignatureValid, SignaturePOSIXFailure, SignatureSecurityFailure, SignatureMissing, SignatureEmpty,
    SignatureTooLargeForSlot, SignatureTooLargeForProtocol, SignatureWrongType, SignatureSourceChanged
} SignatureResult;
static SignatureResult resultKind;
static unsigned signs;
static int sourceWriter = -1, lastInput = -1;

@interface OversizedSignature : NSData @end
@implementation OversizedSignature
- (NSUInteger)length { return LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES + 1; }
- (const void *)bytes { return "unused"; }
@end

NSData *LSBCreateRuntimeSignature(int input, uint64_t sliceOffset, uint64_t blobOffset, uint64_t blobSize, SecIdentityRef identity,
                                 NSArray *chain, NSString *identifier, NSError **error) {
    assert(identity && chain.count && [identifier isEqual:@"com.example.LiveSimBooter"]);
    assert(sliceOffset == 4 && blobOffset == 4 && blobSize == 6);
    assert((fcntl(input, F_GETFL) & O_ACCMODE) == O_RDONLY);
    char original[16];
    assert(pread(input, original, sizeof(original), 0) == sizeof(original) && !memcmp(original, "ORIGINAL-CONTENT", sizeof(original)));
    assert(lseek(input, 0, SEEK_CUR) == 7);
    assert(pwrite(input, "X", 1, 0) == -1 && errno == EBADF); // The service must never hand its core a writable source.
    ++signs;
    lastInput = input;
    switch (resultKind) {
        case SignaturePOSIXFailure:
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOSPC userInfo:nil]; return nil;
        case SignatureSecurityFailure:
            *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:errSecParam userInfo:nil]; return nil;
        case SignatureMissing: return nil;
        case SignatureEmpty: return [NSData data];
        case SignatureTooLargeForSlot: return [NSData dataWithBytes:"TOO LONG" length:8];
        case SignatureTooLargeForProtocol: return [OversizedSignature new];
        case SignatureWrongType: return (NSData *)@"not data";
        case SignatureSourceChanged: assert(!fchmod(sourceWriter, 0640)); break;
        case SignatureValid: break;
    }
    return [NSData dataWithBytes:"SIGNED" length:6];
}

static xpc_object_t request(NSData *p12, int fd) {
    xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(message, "version", LSB_NATIVE_SIGNING_VERSION);
    xpc_dictionary_set_fd(message, "input-fd", fd);
    xpc_dictionary_set_data(message, "pkcs12", p12.bytes, p12.length);
    xpc_dictionary_set_string(message, "password", "fixture-only");
    xpc_dictionary_set_string(message, "identifier", "com.example.LiveSimBooter");
    xpc_dictionary_set_uint64(message, "slice-offset", 4);
    xpc_dictionary_set_uint64(message, "blob-offset", 4);
    xpc_dictionary_set_uint64(message, "blob-size", 6);
    return message;
}

static void verifySource(int fd, const struct stat *original) {
    char bytes[16];
    struct stat current;
    assert(pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes) && !memcmp(bytes, "ORIGINAL-CONTENT", sizeof(bytes)));
    assert(!fstat(fd, &current) && sameInput(original, &current));
    assert(lseek(fd, 0, SEEK_CUR) == 7 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    if (lastInput >= 0) assert(fcntl(lastInput, F_GETFD) == -1 && errno == EBADF); // The received duplicate belongs to the service.
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        assert(argc == 2);
        NSString *fixturePath = [NSString stringWithUTF8String:argv[1]];
        NSData *p12 = [NSData dataWithContentsOfFile:fixturePath];
        assert(p12.length);
        char source[PATH_MAX];
        NSString *template = [fixturePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"service-input.XXXXXX"];
        assert([template getFileSystemRepresentation:source maxLength:sizeof(source)]);
        sourceWriter = mkstemp(source);
        assert(sourceWriter >= 0 && write(sourceWriter, "ORIGINAL-CONTENT", 16) == 16);
        int fd = open(source, O_RDONLY | O_CLOEXEC);
        assert(fd >= 0 && lseek(fd, 7, SEEK_SET) == 7);
        struct stat original;
        assert(!fstat(fd, &original));
        NSData *output = nil;
        assert(!signRequest(request(p12, fd), &output) && [output isEqualToData:[NSData dataWithBytes:"SIGNED" length:6]]);
        verifySource(fd, &original);

        xpc_object_t reply = xpc_dictionary_create(NULL, NULL, 0);
        signReply(request(p12, fd), reply);
        xpc_object_t signature = xpc_dictionary_get_value(reply, "signature");
        assert(xpc_get_type(xpc_dictionary_get_value(reply, "version")) == XPC_TYPE_UINT64);
        assert(xpc_dictionary_get_uint64(reply, "version") == LSB_NATIVE_SIGNING_VERSION);
        assert(xpc_get_type(xpc_dictionary_get_value(reply, "error")) == XPC_TYPE_INT64 && !xpc_dictionary_get_int64(reply, "error"));
        assert(signature && xpc_get_type(signature) == XPC_TYPE_DATA && xpc_data_get_length(signature) == 6);
        assert(!memcmp(xpc_data_get_bytes_ptr(signature), "SIGNED", 6) && !xpc_dictionary_get_value(reply, "output-fd"));
        verifySource(fd, &original);

        unsigned before = signs;
        assert(signRequest(NULL, &output) == EINVAL && !output && signs == before);
        assert(signRequest(xpc_string_create("bad"), &output) == EINVAL && !output && signs == before);
        assert(signRequest(request(p12, fd), NULL) == EINVAL && signs == before);
        xpc_object_t message = request(p12, fd);
        xpc_dictionary_set_int64(message, "version", LSB_NATIVE_SIGNING_VERSION);
        assert(signRequest(message, &output) == EINVAL && !output && signs == before);
        for (uint64_t version = 0; version <= 3; ++version) {
            if (version == LSB_NATIVE_SIGNING_VERSION) continue;
            message = request(p12, fd);
            xpc_dictionary_set_uint64(message, "version", version);
            assert(signRequest(message, &output) == EINVAL && !output && signs == before);
        }
        message = request(p12, fd);
        xpc_dictionary_set_string(message, "password", "wrong password");
        assert(signRequest(message, &output) == EACCES && !output && signs == before);
        message = request(p12, sourceWriter);
        assert(signRequest(message, &output) == EINVAL && !output && signs == before);
        message = request(p12, fd);
        xpc_dictionary_set_string(message, "identifier", "../../bad");
        assert(signRequest(message, &output) == EINVAL && !output && signs == before);
        for (const char **key = (const char *[]){"input-fd", "pkcs12", "password", "identifier", "slice-offset", "blob-offset", "blob-size", NULL};
             *key; ++key) {
            message = request(p12, fd);
            xpc_dictionary_set_value(message, *key, NULL);
            assert(signRequest(message, &output) == EINVAL && !output && signs == before);
            xpc_dictionary_set_int64(message, *key, 4);
            assert(signRequest(message, &output) == EINVAL && !output && signs == before);
        }
        for (const char **key = (const char *[]){"slice-offset", "blob-offset", "blob-size", NULL}; *key; ++key) {
            message = request(p12, fd);
            xpc_dictionary_set_uint64(message, *key, UINT64_MAX);
            assert(signRequest(message, &output) == EINVAL && !output && signs == before);
        }
        message = request(p12, fd);
        xpc_dictionary_set_uint64(message, "blob-size", 0);
        assert(signRequest(message, &output) == EINVAL && !output && signs == before);
        message = request(p12, fd);
        xpc_dictionary_set_uint64(message, "blob-size", LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES + 1);
        assert(signRequest(message, &output) == EINVAL && !output && signs == before);

        for (SignatureResult kind = SignaturePOSIXFailure; kind < SignatureSourceChanged; ++kind) {
            resultKind = kind;
            int expected = kind == SignaturePOSIXFailure ? ENOSPC : (kind <= SignatureMissing ? EACCES : EIO);
            output = [NSData dataWithBytes:"stale" length:5];
            assert(signRequest(request(p12, fd), &output) == expected && !output);
            reply = xpc_dictionary_create(NULL, NULL, 0);
            signReply(request(p12, fd), reply);
            assert(xpc_dictionary_get_uint64(reply, "version") == LSB_NATIVE_SIGNING_VERSION);
            assert(xpc_dictionary_get_int64(reply, "error") == expected);
            assert(!xpc_dictionary_get_value(reply, "signature") && !xpc_dictionary_get_value(reply, "output-fd"));
            verifySource(fd, &original);
        }
        resultKind = SignatureSourceChanged;
        assert(signRequest(request(p12, fd), &output) == ESTALE && !output);
        assert(!fchmod(sourceWriter, original.st_mode));
        assert(lseek(fd, 0, SEEK_CUR) == 7);
        resultKind = SignatureValid;

        authorizedHost = (audit_token_t){{1, 2, 3, 4, 5, 6, 7, 8}};
        assert(matchingHost(&authorizedHost));
        for (unsigned index = 0; index < sizeof(authorizedHost.val) / sizeof(authorizedHost.val[0]); ++index) {
            audit_token_t other = authorizedHost;
            ++other.val[index];
            assert(!matchingHost(&other));
        }
        close(fd); close(sourceWriter); unlink(source);
        puts("Native signer service: signature DATA v2, readonly source/offset ownership, generation checks, bounds and authenticated host passed");
    }
    return 0;
}
