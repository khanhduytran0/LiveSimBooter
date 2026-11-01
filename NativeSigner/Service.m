#import "Backend.h"
#import "LazyCodeSigner.h"
#import "../ProcessSupport/NativeSigningProtocol.h"
#import "../ProcessSupport/PrivateXPC.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MaximumPKCS12Size = 16 * 1024 * 1024, MaximumPasswordSize = 4096, MaximumIdentifierSize = 255 };
static audit_token_t authorizedHost;
static xpc_connection_t signingListener, acceptedHost;
static dispatch_queue_t signingQueue;

static BOOL matchingHost(const audit_token_t *audit) { return !memcmp(audit, &authorizedHost, sizeof(*audit)); }

static BOOL requestString(xpc_object_t request, const char *key, size_t maximum, BOOL empty, NSString **value) {
    xpc_object_t object = xpc_dictionary_get_value(request, key);
    if (!object || xpc_get_type(object) != XPC_TYPE_STRING) return NO;
    size_t length = xpc_string_get_length(object);
    const char *bytes = xpc_string_get_string_ptr(object);
    if ((!empty && !length) || length > maximum || strnlen(bytes, maximum + 1) != length) return NO;
    *value = [[NSString alloc] initWithBytes:bytes length:length encoding:NSUTF8StringEncoding];
    return *value != nil;
}

static BOOL sameInput(const struct stat *before, const struct stat *after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino && before->st_size == after->st_size &&
        before->st_mode == after->st_mode && before->st_uid == after->st_uid && before->st_gid == after->st_gid &&
        before->st_gen == after->st_gen && before->st_flags == after->st_flags && before->st_nlink == after->st_nlink &&
        before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec && before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
        before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec && before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
}

static int signRequest(xpc_object_t request, NSData **output) {
    if (!output) return EINVAL;
    *output = nil;
    if (!request || xpc_get_type(request) != XPC_TYPE_DICTIONARY) return EINVAL;
    xpc_object_t version = xpc_dictionary_get_value(request, "version"), inputValue = xpc_dictionary_get_value(request, "input-fd");
    xpc_object_t p12Value = xpc_dictionary_get_value(request, "pkcs12");
    xpc_object_t sliceValue = xpc_dictionary_get_value(request, "slice-offset"), blobValue = xpc_dictionary_get_value(request, "blob-offset");
    xpc_object_t sizeValue = xpc_dictionary_get_value(request, "blob-size");
    NSString *password = nil, *identifier = nil;
    if (!version || xpc_get_type(version) != XPC_TYPE_UINT64 || xpc_uint64_get_value(version) != LSB_NATIVE_SIGNING_VERSION ||
        !inputValue || xpc_get_type(inputValue) != XPC_TYPE_FD || !p12Value || xpc_get_type(p12Value) != XPC_TYPE_DATA ||
        !sliceValue || xpc_get_type(sliceValue) != XPC_TYPE_UINT64 || !blobValue || xpc_get_type(blobValue) != XPC_TYPE_UINT64 ||
        !sizeValue || xpc_get_type(sizeValue) != XPC_TYPE_UINT64 || !xpc_uint64_get_value(sizeValue) ||
        xpc_uint64_get_value(sizeValue) > LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES ||
        !xpc_data_get_length(p12Value) || xpc_data_get_length(p12Value) > MaximumPKCS12Size ||
        !requestString(request, "password", MaximumPasswordSize, YES, &password) ||
        !requestString(request, "identifier", MaximumIdentifierSize, NO, &identifier)) return EINVAL;
    uint64_t sliceOffset = xpc_uint64_get_value(sliceValue), blobOffset = xpc_uint64_get_value(blobValue), blobSize = xpc_uint64_get_value(sizeValue);
    NSCharacterSet *identifierCharacters = [NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"];
    if ([identifier rangeOfCharacterFromSet:identifierCharacters.invertedSet].location != NSNotFound) return EINVAL;
    NSData *p12 = [NSData dataWithBytes:xpc_data_get_bytes_ptr(p12Value) length:xpc_data_get_length(p12Value)];
    CFArrayRef imported = NULL;
    NSDictionary *options = @{(__bridge id)kSecImportExportPassphrase: password, (__bridge id)kSecImportToMemoryOnly: @YES};
    OSStatus status = SecPKCS12Import((__bridge CFDataRef)p12, (__bridge CFDictionaryRef)options, &imported);
    NSArray *items = CFBridgingRelease(imported);
    if (status || items.count != 1 || ![items.firstObject isKindOfClass:NSDictionary.class]) return EACCES;
    id identityValue = items.firstObject[(__bridge id)kSecImportItemIdentity], chain = items.firstObject[(__bridge id)kSecImportItemCertChain];
    if (!identityValue || CFGetTypeID((__bridge CFTypeRef)identityValue) != SecIdentityGetTypeID() || ![chain isKindOfClass:NSArray.class]) return EACCES;
    for (id certificate in chain) if (CFGetTypeID((__bridge CFTypeRef)certificate) != SecCertificateGetTypeID()) return EACCES;

    int input = xpc_dictionary_dup_fd(request, "input-fd");
    if (input < 0) return EBADF;
    struct stat original = {0}, current = {0};
    int flags = fcntl(input, F_GETFL);
    int error = flags < 0 ? errno : 0;
    if (!error && fstat(input, &original)) error = errno;
    if (!error && (!S_ISREG(original.st_mode) || original.st_size <= 0 || (flags & O_ACCMODE) != O_RDONLY || (flags & O_EVTONLY))) error = EINVAL;
    uint64_t fileSize = (uint64_t)original.st_size;
    if (!error && (sliceOffset > fileSize || blobOffset > fileSize - sliceOffset || blobSize > fileSize - sliceOffset - blobOffset)) error = EINVAL;
    NSData *signature = nil;
    if (!error) {
        NSError *signingError = nil;
        signature = LSBCreateRuntimeSignature(input, sliceOffset, blobOffset, blobSize, (__bridge SecIdentityRef)identityValue,
                                               chain, identifier, &signingError);
        if (!signature) {
            fprintf(stderr, "[native-sign] Security failure domain=%s code=%ld\n", signingError.domain.UTF8String ?: "unknown", (long)signingError.code);
            error = [signingError.domain isEqual:NSPOSIXErrorDomain] && signingError.code > 0 && signingError.code <= ELAST ?
                (int)signingError.code : EACCES;
        }
    }
    if (!error && (![signature isKindOfClass:NSData.class] || !signature.length || signature.length > blobSize ||
                   signature.length > LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES || !signature.bytes)) error = EIO;
    // The signer only reads the original descriptor. Reject a result if its source generation changed while Security ran.
    if (!error && fstat(input, &current)) error = errno;
    if (!error && !sameInput(&original, &current)) error = ESTALE;
    close(input);
    if (!error) *output = signature;
    return error;
}

static void signReply(xpc_object_t request, xpc_object_t reply) {
    NSData *signature = nil;
    int error = signRequest(request, &signature);
    xpc_dictionary_set_uint64(reply, "version", LSB_NATIVE_SIGNING_VERSION);
    xpc_dictionary_set_int64(reply, "error", error);
    if (!error) xpc_dictionary_set_data(reply, "signature", signature.bytes, signature.length);
}

static void handleRequest(xpc_connection_t connection, xpc_object_t request) {
    @autoreleasepool {
        audit_token_t audit = {{0}};
        xpc_dictionary_get_audit_token(request, &audit);
        if (!matchingHost(&audit)) { xpc_connection_cancel(connection); return; }
        xpc_object_t reply = xpc_dictionary_create_reply(request);
        if (!reply) return;
        signReply(request, reply);
        xpc_connection_send_message(connection, reply);
    }
}

__attribute__((visibility("default"))) int LSBNativeSignerPrepare(const audit_token_t *host, mach_port_t *endpoint) {
    if (!host || !host->val[5] || !endpoint || signingListener) return EINVAL;
    *endpoint = MACH_PORT_NULL;
    authorizedHost = *host;
    signingQueue = dispatch_queue_create("LiveSimBooter.NativeSigner", DISPATCH_QUEUE_SERIAL);
    if (!signingQueue) return ENOMEM;
    signingListener = xpc_connection_create(NULL, signingQueue);
    if (!signingListener) return ENOMEM;
    xpc_connection_set_event_handler(signingListener, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_CONNECTION) return;
        xpc_connection_t peer = event;
        audit_token_t audit = {{0}};
        xpc_connection_get_audit_token(peer, &audit);
        if (!matchingHost(&audit) || acceptedHost) {
            xpc_connection_set_event_handler(peer, ^(xpc_object_t ignored) { (void)ignored; });
            xpc_connection_activate(peer);
            xpc_connection_cancel(peer);
            return;
        }
        acceptedHost = peer;
        xpc_connection_set_target_queue(peer, signingQueue);
        __weak xpc_connection_t weakPeer = peer;
        xpc_connection_set_event_handler(peer, ^(xpc_object_t request) {
            xpc_connection_t connection = weakPeer;
            if (!connection) return;
            if (xpc_get_type(request) == XPC_TYPE_DICTIONARY) handleRequest(connection, request);
            else if (xpc_get_type(request) == XPC_TYPE_ERROR) {
                if (acceptedHost == connection) acceptedHost = nil;
                xpc_connection_cancel(connection);
            }
        });
        xpc_connection_activate(peer);
    });
    xpc_connection_activate(signingListener);
    xpc_endpoint_t exported = xpc_endpoint_create(signingListener);
    *endpoint = exported ? xpc_endpoint_copy_listener_port_4sim(exported) : MACH_PORT_NULL;
    if (!MACH_PORT_VALID(*endpoint)) { xpc_connection_cancel(signingListener); signingListener = nil; return EIO; }
    return 0;
}

__attribute__((visibility("default"))) void LSBNativeSignerRun(void) { dispatch_main(); }
