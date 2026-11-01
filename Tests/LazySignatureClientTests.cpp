#include "../LiveSimBooter/LazySignatureClient.cpp"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
enum class Fault {
    None, SendTimeout, SendInterrupted, SendInvalidDest, ReceiveTimeout, ReceiveInterrupted,
    WrongAudit, WrongNonce, WrongSize, Complex, HostReject, ReplacedAgain, AMFI, Open
};
Fault fault;
int localError;
unsigned sends, receives, registrations, replacements, closed, destroyed, receiveRights;
unsigned returnedEndpointRights, returnedReplyRights;
struct stat source, published;
LSBLazySignatureRequest captured;
audit_token_t expectedAudit = {{1, 2, 3, 4, 5, 6, 7, 8}};
constexpr int sourceFD = 19, replacementFD = 29;
constexpr mach_port_t endpoint = 127, replyPort = 139;
constexpr mach_port_t returnedReply = 149;
const char *path = "/private/runtime/bin/probe";

void reset(Fault next = Fault::None) {
    fault = next;
    localError = EBUSY;
    sends = receives = registrations = replacements = closed = destroyed = receiveRights = 0;
    returnedEndpointRights = returnedReplyRights = 0;
    source = {};
    source.st_dev = 7; source.st_ino = 10; source.st_size = 32768; source.st_mode = S_IFREG | 0755; source.st_uid = 501;
    source.st_mtimespec = {100, 2}; source.st_ctimespec = {101, 3};
    published = source;
    published.st_ino = 11;
    published.st_ctimespec.tv_nsec = 4;
    LSBConfigureLazySignature(endpoint, &expectedAudit);
}

fsignatures_t signatureForTest() {
    fsignatures_t result = {};
    result.fs_blob_start = reinterpret_cast<void *>(16384);
    result.fs_blob_size = 16384;
    return result;
}
}

namespace lsb {
int stat(const char *, struct stat *) { assert(false); return -1; }
ssize_t pread(int, void *, size_t, off_t) { assert(false); return -1; }
int *errnoAddress() { return &localError; }
void *copy(void *output, const void *input, size_t size) { return std::memcpy(output, input, size); }
void *zero(void *output, size_t size) { return std::memset(output, 0, size); }
[[noreturn]] void exitProcess(int) { std::abort(); }
mach_port_t task_self_trap() { return 1; }
kern_return_t mach_port_allocate(ipc_space_t task, mach_port_right_t right, mach_port_name_t *port) {
    assert(task == 1 && right == MACH_PORT_RIGHT_RECEIVE);
    *port = replyPort;
    ++receiveRights;
    return KERN_SUCCESS;
}
int64_t machTrap(int64_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
    assert(number == -19 && a0 == 1 && a1 == replyPort && a2 == MACH_PORT_RIGHT_RECEIVE && a3 == static_cast<uintptr_t>(-1));
    assert(receiveRights == 1);
    --receiveRights;
    return KERN_SUCCESS;
}
void mach_msg_destroy(mach_msg_header_t *header) {
    if (header->msgh_id == LSB_LAZY_SIGNATURE_REQUEST_ID) {
        assert(!header->msgh_local_port);
        if (MACH_MSGH_BITS_REMOTE(header->msgh_bits) == MACH_MSG_TYPE_MOVE_SEND) {
            assert(header->msgh_remote_port == endpoint && returnedEndpointRights == 1);
            --returnedEndpointRights;
        }
    }
    ++destroyed;
}
kern_return_t mach_port_deallocate(ipc_space_t task, mach_port_name_t port) {
    assert(task == 1 && port == returnedReply && returnedReplyRights == 1);
    --returnedReplyRights;
    return KERN_SUCCESS;
}
mach_msg_return_t mach_msg(mach_msg_header_t *header, mach_msg_option_t options, mach_msg_size_t sendSize, mach_msg_size_t receiveSize,
                           mach_port_name_t receivePort, mach_msg_timeout_t timeout, mach_port_name_t notify) {
    assert(timeout == LSB_LAZY_SIGNATURE_TIMEOUT_MS && notify == MACH_PORT_NULL);
    if (options & MACH_SEND_MSG) {
        ++sends;
        assert(options == (MACH_SEND_MSG | MACH_SEND_TIMEOUT | MACH_SEND_INTERRUPT) && sendSize == sizeof(captured) && !receiveSize && !receivePort);
        assert(header->msgh_bits == MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE));
        assert(header->msgh_remote_port == endpoint && header->msgh_local_port == replyPort && header->msgh_id == LSB_LAZY_SIGNATURE_REQUEST_ID);
        captured = *reinterpret_cast<LSBLazySignatureRequest *>(header);
        assert(captured.version == LSB_LAZY_SIGNATURE_VERSION && !captured.reserved && captured.requestID);
        assert(!std::strcmp(captured.path, path));
        LSBLazySignatureFileIdentity identity = LSBLazySignatureIdentityFromStat(&source);
        assert(LSBLazySignatureIdentitiesEqual(&captured.file, &identity));
        assert(!captured.sliceOffset && captured.blobOffset == 16384 && captured.blobSize == 16384);
        if (fault == Fault::SendTimeout || fault == Fault::SendInterrupted) {
            header->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, MACH_MSG_TYPE_MOVE_SEND_ONCE);
            header->msgh_local_port = returnedReply;
            ++returnedEndpointRights; ++returnedReplyRights;
            return fault == Fault::SendTimeout ? MACH_SEND_TIMED_OUT : MACH_SEND_INTERRUPTED;
        }
        return fault == Fault::SendInvalidDest ? MACH_SEND_INVALID_DEST : MACH_MSG_SUCCESS;
    }
    ++receives;
    assert((options & MACH_RCV_MSG) && (options & MACH_RCV_TIMEOUT) && (options & MACH_RCV_INTERRUPT) && !sendSize && receivePort == replyPort);
    assert(receiveSize >= sizeof(LSBLazySignatureResponse) + sizeof(mach_msg_audit_trailer_t));
    if (fault == Fault::ReceiveTimeout) return MACH_RCV_TIMED_OUT;
    if (fault == Fault::ReceiveInterrupted) return MACH_RCV_INTERRUPTED;
    auto *response = reinterpret_cast<LSBLazySignatureResponse *>(header);
    *response = {};
    response->header.msgh_id = LSB_LAZY_SIGNATURE_REPLY_ID;
    response->header.msgh_size = sizeof(*response) - (fault == Fault::WrongSize ? 4 : 0);
    response->header.msgh_bits = fault == Fault::Complex ? MACH_MSGH_BITS_COMPLEX : 0;
    response->version = LSB_LAZY_SIGNATURE_VERSION;
    response->requestID = captured.requestID + (fault == Fault::WrongNonce);
    response->error = fault == Fault::HostReject ? EPERM : 0;
    response->file = LSBLazySignatureIdentityFromStat(&published);
    auto *trailer = reinterpret_cast<mach_msg_audit_trailer_t *>(response + 1);
    trailer->msgh_trailer_type = MACH_MSG_TRAILER_FORMAT_0;
    trailer->msgh_trailer_size = sizeof(*trailer);
    trailer->msgh_audit = expectedAudit;
    if (fault == Fault::WrongAudit) ++trailer->msgh_audit.val[7];
    return MACH_MSG_SUCCESS;
}
int fstat(int fd, struct stat *info) {
    assert(fd == sourceFD || fd == replacementFD);
    *info = fd == sourceFD ? source : published;
    if (fd == replacementFD && fault == Fault::ReplacedAgain) ++info->st_ino;
    return 0;
}
int fcntl(int fd, int command, ...) {
    va_list arguments;
    va_start(arguments, command);
    int result = 0;
    switch (command) {
        case F_GETPATH:
            assert(fd == sourceFD);
            std::strcpy(va_arg(arguments, char *), path);
            break;
        case F_GETFD: assert(fd == sourceFD); result = FD_CLOEXEC; break;
        case F_GETFL: assert(fd == sourceFD); result = O_RDONLY | O_NONBLOCK; break;
        case F_SETFL: assert(fd == replacementFD && va_arg(arguments, int) == (O_RDONLY | O_NONBLOCK)); break;
        case F_SETFD: assert(fd == sourceFD && replacements == 1 && va_arg(arguments, int) == FD_CLOEXEC); break;
        default: assert(false);
    }
    va_end(arguments);
    return result;
}
int open(const char *input, int flags, ...) {
    assert(!std::strcmp(input, path) && flags == (O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (fault == Fault::Open) { localError = ENOENT; return -1; }
    return replacementFD;
}
int close(int fd) { assert(fd == replacementFD); ++closed; localError = EIO; return 0; }
SyscallResult syscall(uintptr_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
    if (number == SYS_lseek) {
        assert((a0 == sourceFD && !a1 && a2 == SEEK_CUR) || (a0 == replacementFD && a1 == 123 && a2 == SEEK_SET));
        return {123, 0};
    }
    if (number == SYS_fcntl) {
        assert(a0 == replacementFD && a1 == F_ADDFILESIGS_RETURN);
        ++registrations;
        auto *signature = reinterpret_cast<fsignatures_t *>(a2);
        assert(signature->fs_file_start == 0 && address(signature->fs_blob_start) == 16384 && signature->fs_blob_size == 16384);
        if (fault == Fault::AMFI) return {EACCES, EACCES};
        signature->fs_file_start = 16384;
        return {0, 0};
    }
    assert(number == SYS_dup2 && a0 == replacementFD && a1 == sourceFD && registrations == 1);
    ++replacements;
    return {sourceFD, 0};
}
}

int main() {
    reset();
    fsignatures_t signature = signatureForTest();
    assert(!LSBLazySignatureRetry(sourceFD, &signature));
    assert(signature.fs_file_start == 16384 && signature.fs_blob_size == 16384 && localError == EBUSY);
    assert(sends == 1 && receives == 1 && registrations == 1 && replacements == 1 && closed == 1 && destroyed == 1 && !receiveRights);
    uint64_t previousRequest = captured.requestID;

    struct { Fault fault; int error; } failures[] = {
        {Fault::SendTimeout, ETIMEDOUT}, {Fault::SendInterrupted, EINTR}, {Fault::SendInvalidDest, EPIPE},
        {Fault::ReceiveTimeout, ETIMEDOUT}, {Fault::ReceiveInterrupted, EINTR}, {Fault::WrongAudit, EACCES},
        {Fault::WrongNonce, EPROTO}, {Fault::WrongSize, EPROTO}, {Fault::Complex, EPROTO}, {Fault::HostReject, EPERM},
        {Fault::ReplacedAgain, EAGAIN}, {Fault::AMFI, EACCES}, {Fault::Open, ENOENT}
    };
    for (auto failure : failures) {
        reset(failure.fault);
        signature = signatureForTest();
        assert(LSBLazySignatureRetry(sourceFD, &signature) == failure.error && localError == EBUSY);
        assert(signature.fs_file_start == 0 && !replacements && !receiveRights && !returnedEndpointRights && !returnedReplyRights && sends == 1);
        assert(captured.requestID > previousRequest);
        previousRequest = captured.requestID;
        assert(registrations == (failure.fault == Fault::AMFI));
        bool sendFailure = failure.fault == Fault::SendTimeout || failure.fault == Fault::SendInterrupted || failure.fault == Fault::SendInvalidDest;
        assert(receives == !sendFailure);
    }
    reset();
    LSBConfigureLazySignature(MACH_PORT_NULL, &expectedAudit);
    signature = signatureForTest();
    assert(LSBLazySignatureRetry(sourceFD, &signature) == ENOTSUP && !sends);
    audit_token_t missingAudit = expectedAudit;
    missingAudit.val[7] = 0;
    LSBConfigureLazySignature(endpoint, &missingAudit);
    assert(LSBLazySignatureRetry(sourceFD, &signature) == ENOTSUP && !sends);
    reset();
    signature.fs_blob_size = 32769;
    assert(LSBLazySignatureRetry(sourceFD, &signature) == EINVAL && !sends);
    puts("Lazy signature client: authenticated bounded retry, generation validation, AMFI registration before FD replacement, errno and FD semantics passed");
}
