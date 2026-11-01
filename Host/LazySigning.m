#import "LazySigning.h"
#import "SigningIdentity.h"
#include "../ProcessSupport/LazySignatureProtocol.h"
#include "../ProcessSupport/NativeSigningProtocol.h"
#include "../ProcessSupport/PrivateXPC.h"
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <stdatomic.h>
#include <sys/clonefile.h>
#include <unistd.h>

static BOOL within(NSString *path, NSString *root) { return [path hasPrefix:[root stringByAppendingString:@"/"]]; }
static int identity(int fd, LSBLazySignatureFileIdentity *result) {
    struct stat info;
    if (fstat(fd, &info)) return errno;
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 || (info.st_mode & (S_ISUID | S_ISGID))) return EINVAL;
    *result = LSBLazySignatureIdentityFromStat(&info);
    return 0;
}
static NSData *identityData(const LSBLazySignatureFileIdentity *value) { return [NSData dataWithBytes:value length:sizeof(*value)]; }

static int copyDescriptor(int source, int directory, const char *name, BOOL *cloned) {
    if (cloned) *cloned = NO;
    if (!fclonefileat(source, directory, name, CLONE_NOOWNERCOPY)) {
        if (cloned) *cloned = YES;
        return 0;
    }
    int cloneError = errno;
    // Cloning can require source-path/metadata access that an authorized read FD does not grant across app containers.
    // Fall back only for clone availability/permission failures; destination collisions and storage failures remain errors.
    if (cloneError != ENOTSUP && cloneError != EXDEV && cloneError != EINVAL && cloneError != EPERM && cloneError != EACCES) return cloneError;
    fprintf(stderr, "[lazy-sign] clone failed: %s (%d); copying descriptor data\n", strerror(cloneError), cloneError);
    struct stat info;
    if (fstat(source, &info)) return errno;
    if (!S_ISREG(info.st_mode) || info.st_size <= 0) return EINVAL;
    int output = openat(directory, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) return errno;
    // Copy only the regular file's bytes, without xattrs, path lookups or changes to the shared input FD's seek position.
    unsigned char *buffer = malloc(131072);
    int error = buffer ? 0 : ENOMEM;
    for (off_t offset = 0; !error && offset < info.st_size;) {
        size_t requested = (size_t)MIN((off_t)131072, info.st_size - offset);
        ssize_t count = pread(source, buffer, requested, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        size_t written = 0;
        while (written < (size_t)count) {
            ssize_t result = pwrite(output, buffer + written, (size_t)count - written, offset + (off_t)written);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) { error = result < 0 ? errno : EIO; break; }
            written += (size_t)result;
        }
        offset += count;
    }
    free(buffer);
    if (!error && fchmod(output, info.st_mode & 07777)) error = errno;
    close(output);
    if (error) unlinkat(directory, name, 0);
    return error;
}

// Code-signature wire layouts are big-endian. Parse copies so unaligned XPC data never becomes an unchecked struct pointer.
typedef struct { uint32_t magic, length, count; } LSBSignatureHeader;
typedef struct { uint32_t type, offset; } LSBSignatureIndex;
typedef struct { uint32_t magic, length; } LSBSignatureBlob;
typedef struct {
    uint32_t magic, length, version, flags, hashOffset, identifierOffset, specialSlots, codeSlots, codeLimit;
    uint8_t hashSize, hashType, platform, pageShift;
    uint32_t spare2, scatterOffset, teamOffset, spare3;
    uint64_t codeLimit64, executableBase, executableLimit, executableFlags;
} LSBSignatureCodeDirectory;
_Static_assert(sizeof(LSBSignatureHeader) == 12 && sizeof(LSBSignatureIndex) == 8 && sizeof(LSBSignatureBlob) == 8, "signature blob ABI");
_Static_assert(sizeof(LSBSignatureCodeDirectory) == 88 && offsetof(LSBSignatureCodeDirectory, executableFlags) == 80, "CodeDirectory v0x20400 ABI");

static BOOL signatureString(const uint8_t *bytes, uint32_t offset, uint32_t end) {
    return offset >= sizeof(LSBSignatureCodeDirectory) && offset < end && memchr(bytes + offset, 0, end - offset);
}
static int validateSignature(NSData *signature, const LSBLazySignatureRequest *request) {
    const NSUInteger tableSize = sizeof(LSBSignatureHeader) + 3 * sizeof(LSBSignatureIndex);
    if (![signature isKindOfClass:NSData.class] || signature.length < tableSize || signature.length > request->blobSize ||
        signature.length > LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES) return ENOEXEC;
    const uint8_t *bytes = signature.bytes;
    LSBSignatureHeader header;
    memcpy(&header, bytes, sizeof(header));
    if (OSSwapBigToHostInt32(header.magic) != 0xfade0cc0 || OSSwapBigToHostInt32(header.length) != signature.length ||
        OSSwapBigToHostInt32(header.count) != 3) return ENOEXEC;
    uint32_t offsets[3] = {0}, lengths[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        LSBSignatureIndex index;
        memcpy(&index, bytes + sizeof(header) + i * sizeof(index), sizeof(index));
        uint32_t kind = OSSwapBigToHostInt32(index.type), offset = OSSwapBigToHostInt32(index.offset);
        unsigned slot = kind == 0 ? 0 : kind == 2 ? 1 : kind == 0x10000 ? 2 : 3;
        if (slot == 3 || offsets[slot] || offset < tableSize || offset > signature.length - sizeof(LSBSignatureBlob)) return ENOEXEC;
        LSBSignatureBlob blob;
        memcpy(&blob, bytes + offset, sizeof(blob));
        uint32_t length = OSSwapBigToHostInt32(blob.length), magic = OSSwapBigToHostInt32(blob.magic);
        uint32_t expected = slot == 0 ? 0xfade0c02 : slot == 1 ? 0xfade0c01 : 0xfade0b01;
        if (magic != expected || length < (slot == 0 ? sizeof(LSBSignatureCodeDirectory) : slot == 1 ? 12 : 9) ||
            length > signature.length - offset) return ENOEXEC;
        offsets[slot] = offset; lengths[slot] = length;
    }
    for (unsigned i = 0; i < 3; ++i) for (unsigned j = i + 1; j < 3; ++j)
        if (offsets[i] < offsets[j] + lengths[j] && offsets[j] < offsets[i] + lengths[i]) return ENOEXEC;
    LSBSignatureCodeDirectory cd;
    memcpy(&cd, bytes + offsets[0], sizeof(cd));
    uint32_t hashes = OSSwapBigToHostInt32(cd.hashOffset), slots = OSSwapBigToHostInt32(cd.codeSlots);
    uint64_t codeLimit = OSSwapBigToHostInt32(cd.codeLimit), executableBase = OSSwapBigToHostInt64(cd.executableBase);
    uint64_t executableLimit = OSSwapBigToHostInt64(cd.executableLimit);
    if (OSSwapBigToHostInt32(cd.version) != 0x20400 || cd.flags || cd.hashSize != 32 || cd.hashType != 2 || cd.platform || cd.pageShift != 14 ||
        cd.spare2 || cd.scatterOffset || cd.spare3 || cd.codeLimit64 || cd.executableFlags || !codeLimit || codeLimit != request->blobOffset ||
        OSSwapBigToHostInt32(cd.specialSlots) != 2 || slots != (codeLimit + 16383) / 16384 || hashes < sizeof(cd) + 64 ||
        hashes > lengths[0] || (uint64_t)slots * 32 != lengths[0] - hashes || executableBase > codeLimit ||
        executableLimit > codeLimit - executableBase) return ENOEXEC;
    uint32_t stringsEnd = hashes - 64, team = OSSwapBigToHostInt32(cd.teamOffset);
    if (!signatureString(bytes + offsets[0], OSSwapBigToHostInt32(cd.identifierOffset), stringsEnd) ||
        (team && !signatureString(bytes + offsets[0], team, stringsEnd))) return ENOEXEC;
    return 0;
}

static int signatureReply(xpc_object_t response, const audit_token_t *expected, uint64_t capacity, NSData **output) {
    *output = nil;
    if (!response || xpc_get_type(response) != XPC_TYPE_DICTIONARY) return EPIPE;
    audit_token_t audit = {0};
    xpc_dictionary_get_audit_token(response, &audit);
    xpc_object_t version = xpc_dictionary_get_value(response, "version"), error = xpc_dictionary_get_value(response, "error");
    if (memcmp(&audit, expected, sizeof(audit)) || !version || xpc_get_type(version) != XPC_TYPE_UINT64 ||
        xpc_uint64_get_value(version) != LSB_NATIVE_SIGNING_VERSION || !error || xpc_get_type(error) != XPC_TYPE_INT64 ||
        xpc_dictionary_get_value(response, "output-fd")) return EPROTO;
    int64_t code = xpc_int64_get_value(error);
    xpc_object_t data = xpc_dictionary_get_value(response, "signature");
    if (code < 0 || code > INT_MAX || (code && data)) return EPROTO;
    if (code) return (int)code;
    if (!data || xpc_get_type(data) != XPC_TYPE_DATA) return EPROTO;
    size_t size = xpc_data_get_length(data);
    if (!size || size > capacity || size > LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES) return EPROTO;
    *output = [NSData dataWithBytes:xpc_data_get_bytes_ptr(data) length:size];
    return *output ? 0 : ENOMEM;
}

static int writeSignature(NSData *signature, int destination, uint64_t offset, uint64_t capacity) {
    struct stat output;
    if (fstat(destination, &output)) return errno;
    if (!S_ISREG(output.st_mode) || output.st_size <= 0 || !signature.length || signature.length > capacity ||
        capacity > LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES || offset > (uint64_t)output.st_size || capacity > (uint64_t)output.st_size - offset) return EINVAL;
    unsigned char *buffer = calloc(1, 131072);
    if (!buffer) return ENOMEM;
    int error = 0;
    for (uint64_t copied = 0; copied < capacity;) {
        size_t count = (size_t)MIN(capacity - copied, 131072);
        memset(buffer, 0, count);
        if (copied < signature.length) memcpy(buffer, (const uint8_t *)signature.bytes + copied, (size_t)MIN(count, signature.length - copied));
        size_t written = 0;
        while (written < count) {
            ssize_t result = pwrite(destination, buffer + written, count - written, (off_t)(offset + copied + written));
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) { error = result < 0 ? errno : EIO; break; }
            written += (size_t)result;
        }
        if (error) break;
        copied += count;
    }
    free(buffer);
    return error;
}

@implementation LSBLazySigning {
    NSString *_runtimeRoot, *_dataRoot, *_backupRoot, *_identifier;
    LSBSigningCredentials *_credentials;
    BOOL (^_authorize)(const audit_token_t *);
    dispatch_queue_t _receiverQueue, _signQueue;
    dispatch_source_t _source;
    xpc_connection_t _connection;
    audit_token_t _serviceAudit;
    NSMutableDictionary<NSString *, NSDictionary *> *_published;
    atomic_bool _invalidated;
    atomic_uint _pending;
}
- (instancetype)initWithRuntimeRoot:(NSString *)root credentials:(LSBSigningCredentials *)credentials identifier:(NSString *)identifier
                         authorize:(BOOL (^)(const audit_token_t *))authorize error:(int *)error {
    if (!(self = [super init])) return nil;
    *error = 0;
    if (!credentials || !identifier.length || !authorize) { *error = EINVAL; return nil; }
    char canonical[PATH_MAX];
    if (!realpath(root.fileSystemRepresentation, canonical)) { *error = errno; return nil; }
    _runtimeRoot = @(canonical);
    NSString *data = [_runtimeRoot.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"data"];
    if (!realpath(data.fileSystemRepresentation, canonical)) { *error = errno; return nil; }
    _dataRoot = @(canonical);
    _backupRoot = [_runtimeRoot.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"SigningBackups"];
    _credentials = credentials; _identifier = [identifier copy]; _authorize = [authorize copy];
    _identityRevision = [credentials.revision copy];
    _published = [NSMutableDictionary new];
    _receiverQueue = dispatch_queue_create("LiveSimBooter.lazy-sign.receive", DISPATCH_QUEUE_SERIAL);
    _signQueue = dispatch_queue_create("LiveSimBooter.lazy-sign.work", DISPATCH_QUEUE_SERIAL);
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &_endpoint) ||
        mach_port_insert_right(mach_task_self(), _endpoint, _endpoint, MACH_MSG_TYPE_MAKE_SEND)) {
        *error = EIO; [self invalidate]; return nil;
    }
    mach_port_t receive = _endpoint;
    _source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, receive, 0, _receiverQueue);
    if (!_source) { *error = ENOMEM; [self invalidate]; return nil; }
    __weak LSBLazySigning *weakSelf = self;
    dispatch_source_set_event_handler(_source, ^{ [weakSelf receive]; });
    dispatch_source_set_cancel_handler(_source, ^{ mach_port_mod_refs(mach_task_self(), receive, MACH_PORT_RIGHT_RECEIVE, -1); });
    dispatch_resume(_source);
    return self;
}
- (void)dealloc { [self invalidate]; }
- (void)invalidate {
    if (atomic_exchange(&_invalidated, true)) return;
    if (_source) dispatch_source_cancel(_source);
    else if (MACH_PORT_VALID(_endpoint)) mach_port_mod_refs(mach_task_self(), _endpoint, MACH_PORT_RIGHT_RECEIVE, -1);
    if (MACH_PORT_VALID(_endpoint)) mach_port_deallocate(mach_task_self(), _endpoint);
    _endpoint = MACH_PORT_NULL;
    xpc_connection_t connection = _connection;
    if (connection) xpc_connection_cancel(connection);
}
- (int)connectService:(mach_port_t)port audit:(const audit_token_t *)audit {
    if (!NSThread.isMainThread || !MACH_PORT_VALID(port) || !audit || !audit->val[5] || audit->val[5] > INT_MAX || !audit->val[7] ||
        _connection || atomic_load(&_invalidated)) return EINVAL;
    if (mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1)) return EPIPE;
    xpc_endpoint_t endpoint = xpc_endpoint_create_mach_port_4sim(port);
    if (!endpoint) { mach_port_deallocate(mach_task_self(), port); return EIO; }
    _connection = xpc_connection_create_from_endpoint(endpoint);
    if (!_connection) return ENOMEM;
    _serviceAudit = *audit;
    xpc_connection_set_event_handler(_connection, ^(xpc_object_t event) { (void)event; });
    xpc_connection_activate(_connection);
    return 0;
}
- (void)reply:(LSBLazySignatureRequest *)request error:(int)error file:(const LSBLazySignatureFileIdentity *)file {
    LSBLazySignatureResponse response = {0};
    response.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    response.header.msgh_size = sizeof(response);
    response.header.msgh_remote_port = request->header.msgh_remote_port;
    response.header.msgh_id = LSB_LAZY_SIGNATURE_REPLY_ID;
    response.version = LSB_LAZY_SIGNATURE_VERSION; response.requestID = request->requestID; response.error = error;
    if (!error && file) response.file = *file;
    if (mach_msg(&response.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(response), 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL))
        mach_msg_destroy(&response.header);
    request->header.msgh_remote_port = MACH_PORT_NULL;
}
- (void)receive {
    for (unsigned count = 0; count < 16 && !atomic_load(&_invalidated); ++count) {
        struct { LSBLazySignatureRequest message; mach_msg_max_trailer_t trailer; } buffer = {0};
        kern_return_t result = mach_msg(&buffer.message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT |
            MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
            0, sizeof(buffer), _endpoint, 0, MACH_PORT_NULL);
        if (result == MACH_RCV_TIMED_OUT) break;
        if (result) break;
        LSBLazySignatureRequest *request = &buffer.message;
        if ((request->header.msgh_bits & MACH_MSGH_BITS_COMPLEX) || request->header.msgh_size != sizeof(*request) ||
            request->header.msgh_id != LSB_LAZY_SIGNATURE_REQUEST_ID ||
            MACH_MSGH_BITS_REMOTE(request->header.msgh_bits) != MACH_MSG_TYPE_PORT_SEND_ONCE ||
            MACH_PORT_VALID(request->header.msgh_voucher_port)) {
            mach_msg_destroy(&request->header); continue;
        }
        mach_msg_audit_trailer_t *trailer = (void *)((uint8_t *)&buffer + round_msg(request->header.msgh_size));
        int error = trailer->msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0 || trailer->msgh_trailer_size < sizeof(*trailer) ? EACCES : 0;
        if (!error && (request->version != LSB_LAZY_SIGNATURE_VERSION || request->reserved || !request->requestID ||
            !memchr(request->path, 0, sizeof(request->path)) || request->path[0] != '/' || !request->blobSize ||
            request->sliceOffset > request->file.size || request->blobOffset > request->file.size - request->sliceOffset ||
            request->blobSize > request->file.size - request->sliceOffset - request->blobOffset)) error = EINVAL;
        if (!error && atomic_fetch_add(&_pending, 1) >= 16) { atomic_fetch_sub(&_pending, 1); error = EBUSY; }
        if (error) { [self reply:request error:error file:NULL]; continue; }
        LSBLazySignatureRequest copy = *request;
        audit_token_t audit = trailer->msgh_audit;
        dispatch_async(_signQueue, ^{ @autoreleasepool {
            __block BOOL authorized = NO;
            dispatch_sync(dispatch_get_main_queue(), ^{ authorized = !atomic_load(&self->_invalidated) && self->_authorize(&audit); });
            LSBLazySignatureFileIdentity replacement = {0};
            int result = authorized ? [self sign:&copy replacement:&replacement] : EACCES;
            LSBLazySignatureRequest replyRequest = copy;
            [self reply:&replyRequest error:result file:&replacement];
            atomic_fetch_sub(&self->_pending, 1);
        } });
    }
}
- (int)signedSignature:(int)source request:(const LSBLazySignatureRequest *)metadata output:(NSData **)output {
    *output = nil;
    if (!_connection || atomic_load(&_invalidated)) return EPIPE;
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(request, "version", LSB_NATIVE_SIGNING_VERSION);
    xpc_dictionary_set_uint64(request, "slice-offset", metadata->sliceOffset);
    xpc_dictionary_set_uint64(request, "blob-offset", metadata->blobOffset);
    xpc_dictionary_set_uint64(request, "blob-size", metadata->blobSize);
    xpc_dictionary_set_fd(request, "input-fd", source);
    xpc_dictionary_set_data(request, "pkcs12", _credentials.pkcs12Data.bytes, _credentials.pkcs12Data.length);
    xpc_dictionary_set_string(request, "password", _credentials.password.UTF8String);
    xpc_dictionary_set_string(request, "identifier", _identifier.UTF8String);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block xpc_object_t response;
    xpc_connection_send_message_with_reply(_connection, request, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(xpc_object_t reply) {
        response = reply; dispatch_semaphore_signal(done);
    });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 90 * NSEC_PER_SEC))) {
        xpc_connection_cancel(_connection);
        return ETIMEDOUT;
    }
    return signatureReply(response, &_serviceAudit, metadata->blobSize, output);
}
- (int)sign:(const LSBLazySignatureRequest *)request replacement:(LSBLazySignatureFileIdentity *)replacement {
    char canonical[PATH_MAX], actual[PATH_MAX];
    if (!realpath(request->path, canonical)) return errno;
    NSString *path = @(canonical);
    if (strcmp(canonical, request->path) || (!within(path, _runtimeRoot) && !within(path, _dataRoot))) return EACCES;
    int source = open(canonical, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source < 0) return errno;
    int parent = -1, stage = -1, backup = -1;
    NSData *signature = nil;
    BOOL committed = NO, stageCloned = NO;
    NSString *temporary = [@".lsb-sign-" stringByAppendingString:NSUUID.UUID.UUIDString], *backupName = NSUUID.UUID.UUIDString;
    LSBLazySignatureFileIdentity before = {0}, after = {0};
    int error = identity(source, &before);
    if (!error && (fcntl(source, F_GETPATH, actual) || strcmp(actual, canonical))) error = EAGAIN;
    if (!error && !LSBLazySignatureIdentitiesEqual(&before, &request->file)) {
        NSDictionary *published = _published[path];
        if ([published[@"before"] isEqual:identityData(&request->file)] && [published[@"after"] isEqual:identityData(&before)]) {
            *replacement = before; close(source); return 0;
        }
        error = EAGAIN;
    }
    if (!error && (!request->blobSize || request->sliceOffset > before.size || request->blobOffset > before.size - request->sliceOffset ||
        request->blobSize > before.size - request->sliceOffset - request->blobOffset)) error = EINVAL;
    if (!error && request->blobSize > LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES) error = EINVAL;
    if (!error) error = [self signedSignature:source request:request output:&signature];
    if (!error) error = validateSignature(signature, request);
    uint64_t start = request->sliceOffset + request->blobOffset;
    if (!error) error = identity(source, &after);
    if (!error && !LSBLazySignatureIdentitiesEqual(&before, &after)) error = EAGAIN;
    if (!error) {
        parent = open(path.stringByDeletingLastPathComponent.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (parent < 0) error = errno;
        else if (fcntl(parent, F_GETPATH, actual) || strcmp(actual, path.stringByDeletingLastPathComponent.fileSystemRepresentation)) error = EAGAIN;
        else if (!within(@(actual), _runtimeRoot) && !within(@(actual), _dataRoot) &&
                 ![@(actual) isEqual:_runtimeRoot] && ![@(actual) isEqual:_dataRoot]) error = EACCES;
    }
    if (!error) {
        // The signer supplies only a validated signature blob, never replacement code. Clone the original and write only its existing signature allocation.
        error = copyDescriptor(source, parent, temporary.fileSystemRepresentation, &stageCloned);
        if (error) fprintf(stderr, "[lazy-sign] staging copy failed: %s (%d)\n", strerror(error), error);
    }
    if (!error) {
        int writable = -1;
        LSBLazySignatureFileIdentity stagedIdentity = {0};
        // A cloned 0444/0555 input is not writable yet. Change only this private stage, then verify reopening kept its inode.
        stage = openat(parent, temporary.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (stage < 0 || fchmod(stage, 0600)) error = errno;
        if (!error) error = identity(stage, &stagedIdentity);
        if (!error) {
            writable = openat(parent, temporary.fileSystemRepresentation, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
            error = writable < 0 ? errno : identity(writable, &after);
            if (!error && !LSBLazySignatureIdentitiesEqual(&stagedIdentity, &after)) error = EAGAIN;
        }
        if (stage >= 0) { close(stage); stage = -1; }
        if (writable >= 0) {
            if (!error) error = writeSignature(signature, writable, start, request->blobSize);
            if (!error && fchmod(writable, before.mode & 07777)) error = errno;
            if (!error && fsync(writable)) error = errno;
            if (!error) error = identity(writable, &stagedIdentity);
            if (close(writable) && !error) error = errno;
        }
        if (!error) {
            // Do not retain a writable descriptor across kernel registration or publication. Reopening must select the same completed inode.
            stage = openat(parent, temporary.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            error = stage < 0 ? errno : identity(stage, &after);
            if (!error && !LSBLazySignatureIdentitiesEqual(&stagedIdentity, &after)) error = EAGAIN;
        }
        if (error) fprintf(stderr, "[lazy-sign] signature staging failed: %s (%d)\n", strerror(error), error);
    }
    if (!error) {
        // amfid reopens the vnode's pathname. Register the completed, still-linked sibling before publication;
        // failure removes it and leaves the original untouched.
        fsignatures_t signature = {.fs_file_start = (off_t)request->sliceOffset,
            .fs_blob_start = (void *)(uintptr_t)request->blobOffset, .fs_blob_size = (size_t)request->blobSize};
        if (fcntl(stage, F_ADDFILESIGS_RETURN, &signature)) error = errno;
        else if (signature.fs_file_start < 0 || (uint64_t)signature.fs_file_start < request->blobOffset) error = ENOEXEC;
        if (error) fprintf(stderr, "[lazy-sign] staged signature registration failed: %s (%d)\n", strerror(error), error);
    }
    if (!error && ![NSFileManager.defaultManager createDirectoryAtPath:_backupRoot withIntermediateDirectories:YES
        attributes:@{NSFilePosixPermissions:@0700} error:nil]) error = EIO;
    if (!error) {
        backup = open(_backupRoot.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        error = backup < 0 ? errno : 0;
        if (!error && (fcntl(backup, F_GETPATH, actual) || strcmp(actual, _backupRoot.fileSystemRepresentation))) error = EACCES;
        if (!error) {
            error = copyDescriptor(source, backup, backupName.fileSystemRepresentation, NULL);
            if (error) fprintf(stderr, "[lazy-sign] original backup copy failed: %s (%d)\n", strerror(error), error);
        }
        if (!error) {
            NSString *base = _runtimeRoot.stringByDeletingLastPathComponent;
            NSDictionary *record = @{@"path": [path substringFromIndex:base.length + 1], @"created": NSDate.date,
                @"original-device": @(before.device), @"original-inode": @(before.inode), @"size": @(before.size),
                @"slice-offset": @(request->sliceOffset), @"signature-offset": @(request->blobOffset),
                @"signature-size": @(request->blobSize), @"signing-identifier": _identifier};
            NSData *plist = [NSPropertyListSerialization dataWithPropertyList:record format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
            NSString *recordPath = [_backupRoot stringByAppendingPathComponent:[backupName stringByAppendingPathExtension:@"plist"]];
            if (!plist || ![plist writeToFile:recordPath options:NSDataWritingAtomic error:nil]) error = EIO;
        }
    }
    struct stat current;
    if (!error && (fcntl(parent, F_GETPATH, actual) || strcmp(actual, path.stringByDeletingLastPathComponent.fileSystemRepresentation))) error = EAGAIN;
    if (!error && fstatat(parent, path.lastPathComponent.fileSystemRepresentation, &current, AT_SYMLINK_NOFOLLOW)) error = errno;
    if (!error) {
        after = LSBLazySignatureIdentityFromStat(&current);
        if (!LSBLazySignatureIdentitiesEqual(&before, &after) || atomic_load(&_invalidated)) error = EAGAIN;
    }
    if (!error) {
        if (renameat(parent, temporary.fileSystemRepresentation, parent, path.lastPathComponent.fileSystemRepresentation)) error = errno;
        else committed = YES;
    }
    if (!error) error = identity(stage, replacement);
    if (!error) {
        _published[path] = @{@"before":identityData(&before), @"after":identityData(replacement)};
        // Publication is complete. A cleanup failure must not report a signing failure or restore an inode guests may already have opened.
        int cleanupError = unlinkat(backup, backupName.fileSystemRepresentation, 0) ? errno : 0;
        if (!cleanupError && unlinkat(backup, [backupName stringByAppendingPathExtension:@"plist"].fileSystemRepresentation, 0)) cleanupError = errno;
        const char *publication = stageCloned ? "original clone + signature splice" : "original descriptor copy + signature splice";
        if (cleanupError) fprintf(stderr, "[lazy-sign] signed %s via %s; cleanup of SigningBackups/%s failed: %s (%d)\n",
                                  canonical, publication, backupName.UTF8String, strerror(cleanupError), cleanupError);
        else fprintf(stderr, "[lazy-sign] signed %s via %s; temporary backup removed\n", canonical, publication);
    } else {
        if (parent >= 0) unlinkat(parent, temporary.fileSystemRepresentation, 0);
        if (backup >= 0 && !committed) {
            unlinkat(backup, backupName.fileSystemRepresentation, 0);
            unlinkat(backup, [backupName stringByAppendingPathExtension:@"plist"].fileSystemRepresentation, 0);
        }
        fprintf(stderr, "[lazy-sign] %s failed: %s (%d)\n", canonical, strerror(error), error);
    }
    if (stage >= 0) close(stage);
    if (parent >= 0) close(parent);
    if (backup >= 0) close(backup);
    close(source);
    return error;
}
@end
