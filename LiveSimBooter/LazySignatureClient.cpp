#include "LazySignatureClient.h"
#include "LocalMach.h"
#include "LocalPOSIX.h"
#include "LocalRuntime.h"
#include "../ProcessSupport/LazySignatureProtocol.h"

#include <errno.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

mach_port_t signerEndpoint;
audit_token_t signerAudit;
uint64_t nextRequestID;

uintptr_t address(const void *pointer) { return reinterpret_cast<uintptr_t>(pointer); }

bool sameAudit(const audit_token_t &left, const audit_token_t &right) {
    for (unsigned index = 0; index < sizeof(left.val) / sizeof(left.val[0]); ++index)
        if (left.val[index] != right.val[index]) return false;
    return true;
}

int exchange(LSBLazySignatureRequest &request, LSBLazySignatureResponse &response) {
    mach_port_t replyPort = MACH_PORT_NULL;
    if (lsb::mach_port_allocate(lsb::task_self_trap(), MACH_PORT_RIGHT_RECEIVE, &replyPort)) return ENOMEM;
    request.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    request.header.msgh_size = sizeof(request);
    request.header.msgh_remote_port = signerEndpoint;
    request.header.msgh_local_port = replyPort;
    request.header.msgh_id = LSB_LAZY_SIGNATURE_REQUEST_ID;
    mach_msg_return_t result = lsb::mach_msg(&request.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT | MACH_SEND_INTERRUPT, sizeof(request), 0,
                                            MACH_PORT_NULL, LSB_LAZY_SIGNATURE_TIMEOUT_MS, MACH_PORT_NULL);
    if (result) {
        // A failed send may pseudo-receive COPY_SEND/MAKE_SEND_ONCE as new MOVE rights. mach_msg_destroy omits the local port.
        mach_msg_type_name_t localType = MACH_MSGH_BITS_LOCAL(request.header.msgh_bits);
        if (MACH_PORT_VALID(request.header.msgh_local_port) &&
            (localType == MACH_MSG_TYPE_MOVE_SEND || localType == MACH_MSG_TYPE_MOVE_SEND_ONCE))
            (void)lsb::mach_port_deallocate(lsb::task_self_trap(), request.header.msgh_local_port);
        request.header.msgh_local_port = MACH_PORT_NULL;
        lsb::mach_msg_destroy(&request.header);
    }
    struct {
        LSBLazySignatureResponse response;
        mach_msg_audit_trailer_t trailer;
    } incoming;
    lsb::zero(&incoming, sizeof(incoming));
    if (!result) {
        mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_INTERRUPT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
            MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
        result = lsb::mach_msg(&incoming.response.header, options, 0, sizeof(incoming), replyPort,
                              LSB_LAZY_SIGNATURE_TIMEOUT_MS, MACH_PORT_NULL);
    }
    // A fresh receive right per call makes late replies harmless after timeout; never resend an uncertain signing operation.
    (void)lsb::machTrap(-19, lsb::task_self_trap(), replyPort, MACH_PORT_RIGHT_RECEIVE, static_cast<uintptr_t>(-1));
    if (result == MACH_SEND_TIMED_OUT || result == MACH_RCV_TIMED_OUT) return ETIMEDOUT;
    if (result == MACH_SEND_INTERRUPTED || result == MACH_RCV_INTERRUPTED) return EINTR;
    if (result) return EPIPE;
    auto &header = incoming.response.header;
    int error = 0;
    if (header.msgh_size != sizeof(LSBLazySignatureResponse) || header.msgh_id != LSB_LAZY_SIGNATURE_REPLY_ID ||
        (header.msgh_bits & MACH_MSGH_BITS_COMPLEX) || MACH_PORT_VALID(header.msgh_remote_port) || MACH_PORT_VALID(header.msgh_voucher_port) ||
        incoming.response.version != LSB_LAZY_SIGNATURE_VERSION || incoming.response.requestID != request.requestID ||
        incoming.response.error < 0) error = EPROTO;
    else if (incoming.trailer.msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0 ||
             incoming.trailer.msgh_trailer_size < sizeof(mach_msg_audit_trailer_t) || !sameAudit(incoming.trailer.msgh_audit, signerAudit)) error = EACCES;
    if (!error) lsb::copy(&response, &incoming.response, sizeof(response));
    lsb::mach_msg_destroy(&header);
    return error;
}

int retry(int fd, fsignatures_t *signature, LSBLazySignatureFileIdentity *published = nullptr) {
    if (!MACH_PORT_VALID(signerEndpoint)) return ENOTSUP;
    if (!signature || signature->fs_file_start < 0 || !signature->fs_blob_size) return EINVAL;
    LSBLazySignatureRequest request;
    lsb::zero(&request, sizeof(request));
    request.version = LSB_LAZY_SIGNATURE_VERSION;
    request.requestID = __atomic_add_fetch(&nextRequestID, 1, __ATOMIC_RELAXED);
    if (!request.requestID) return EOVERFLOW;
    struct stat info;
    if (lsb::fstat(fd, &info)) return *lsb::errnoAddress();
    if (!S_ISREG(info.st_mode) || info.st_size <= 0) return EINVAL;
    LSBLazySignatureFileIdentity original = LSBLazySignatureIdentityFromStat(&info);
    lsb::copy(&request.file, &original, sizeof(original));
    request.sliceOffset = static_cast<uint64_t>(signature->fs_file_start);
    request.blobOffset = address(signature->fs_blob_start);
    request.blobSize = signature->fs_blob_size;
    if (request.sliceOffset > request.file.size || request.blobOffset > request.file.size - request.sliceOffset ||
        request.blobSize > request.file.size - request.sliceOffset - request.blobOffset) return EINVAL;
    if (lsb::fcntl(fd, F_GETPATH, request.path)) return *lsb::errnoAddress();
    if (request.path[0] != '/' || request.path[sizeof(request.path) - 1]) return EINVAL;
    int descriptorFlags = lsb::fcntl(fd, F_GETFD), statusFlags = lsb::fcntl(fd, F_GETFL);
    if (descriptorFlags < 0 || statusFlags < 0) return *lsb::errnoAddress();
    if ((statusFlags & O_ACCMODE) != O_RDONLY || (statusFlags & O_EVTONLY)) return EACCES;
    lsb::SyscallResult offset = lsb::syscall(SYS_lseek, fd, 0, SEEK_CUR);
    if (offset.error) return offset.error;
    LSBLazySignatureResponse response;
    lsb::zero(&response, sizeof(response));
    int error = exchange(request, response);
    if (error || response.error) return error ? error : response.error;
    if (response.file.size != request.file.size || response.file.mode != request.file.mode || response.file.owner != request.file.owner) return EPROTO;
    int replacement = lsb::open(request.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (replacement < 0) return *lsb::errnoAddress();
    if (lsb::fstat(replacement, &info)) error = *lsb::errnoAddress();
    LSBLazySignatureFileIdentity identity = LSBLazySignatureIdentityFromStat(&info);
    if (!error && !LSBLazySignatureIdentitiesEqual(&identity, &response.file)) error = EAGAIN;
    // Only register the new descriptor. If Security or AMFI rejects it, the caller retains its original open file unchanged.
    fsignatures_t registered;
    lsb::zero(&registered, sizeof(registered));
    // F_ADDFILESIGS_RETURN uses only the original three-field ABI, even when the SDK adds F_ADDFILESIGS_INFO fields.
    registered.fs_file_start = signature->fs_file_start;
    registered.fs_blob_start = signature->fs_blob_start;
    registered.fs_blob_size = signature->fs_blob_size;
    if (!error) {
        lsb::SyscallResult result = lsb::syscall(SYS_fcntl, replacement, F_ADDFILESIGS_RETURN, address(&registered));
        error = result.error;
        if (!error && (registered.fs_file_start < 0 || static_cast<uint64_t>(registered.fs_file_start) < request.blobOffset)) error = EACCES;
    }
    if (!error && lsb::fcntl(replacement, F_SETFL, statusFlags)) error = *lsb::errnoAddress();
    if (!error) error = lsb::syscall(SYS_lseek, replacement, offset.value, SEEK_SET).error;
    if (!error) error = lsb::syscall(SYS_dup2, replacement, fd).error;
    if (!error && lsb::fcntl(fd, F_SETFD, descriptorFlags)) {
        // dup2 committed the replacement. Failing to restore close-on-exec would silently change the caller's descriptor contract.
        lsb::exitProcess(126);
    }
    lsb::close(replacement);
    if (!error) {
        signature->fs_file_start = registered.fs_file_start;
        if (published) lsb::copy(published, &response.file, sizeof(*published));
    }
    return error;
}

bool contains(uint64_t size, uint64_t offset, uint64_t length) { return offset <= size && length <= size - offset; }

bool readFile(int fd, uint64_t fileSize, uint64_t offset, void *output, size_t size) {
    if (!contains(fileSize, offset, size)) return false;
    return lsb::pread(fd, output, size, static_cast<off_t>(offset)) == static_cast<ssize_t>(size);
}

struct CacheSignatureHeader {
    char magic[16];
    uint32_t mappingOffset, mappingCount, imagesOffset, imagesCount;
    uint64_t dyldBase, signatureOffset, signatureSize;
};
static_assert(sizeof(CacheSignatureHeader) == 56 && offsetof(CacheSignatureHeader, signatureOffset) == 40);

bool readSignature(int fd, uint64_t fileSize, fsignatures_t &signature) {
    mach_header_64 header;
    if (!readFile(fd, fileSize, 0, &header, sizeof(header))) return false;
    static constexpr char cacheMagic[] = "dyld_v1   arm64";
    bool isCache = true;
    for (unsigned index = 0; index < sizeof(cacheMagic); ++index)
        if (reinterpret_cast<const char *>(&header)[index] != cacheMagic[index]) isCache = false;
    if (isCache) {
        CacheSignatureHeader cache;
        if (!readFile(fd, fileSize, 0, &cache, sizeof(cache)) || cache.mappingOffset < sizeof(cache) || !cache.mappingCount ||
            !contains(fileSize, cache.mappingOffset, uint64_t(cache.mappingCount) * 32) || !cache.signatureSize ||
            cache.signatureOffset < sizeof(cache) || !contains(fileSize, cache.signatureOffset, cache.signatureSize)) return false;
        signature.fs_blob_start = reinterpret_cast<void *>(cache.signatureOffset);
        signature.fs_blob_size = static_cast<size_t>(cache.signatureSize);
        return true;
    }
    uint64_t sliceOffset = 0, sliceSize = fileSize;
    if (header.magic != MH_MAGIC_64) {
        bool fat64 = header.magic == FAT_MAGIC_64 || header.magic == FAT_CIGAM_64;
        if (!fat64 && header.magic != FAT_MAGIC && header.magic != FAT_CIGAM) return false;
        bool swapped = header.magic == FAT_CIGAM || header.magic == FAT_CIGAM_64;
        auto swap32 = [swapped](uint32_t value) { return swapped ? __builtin_bswap32(value) : value; };
        auto swap64 = [swapped](uint64_t value) { return swapped ? __builtin_bswap64(value) : value; };
        fat_header fat;
        if (!readFile(fd, fileSize, 0, &fat, sizeof(fat))) return false;
        uint32_t count = swap32(fat.nfat_arch);
        size_t recordSize = fat64 ? sizeof(fat_arch_64) : sizeof(fat_arch);
        if (!count || count > 128 || !contains(fileSize, sizeof(fat), uint64_t(count) * recordSize)) return false;
        bool found = false;
        for (uint32_t index = 0; index < count; ++index) {
            uint64_t offset, size;
            uint32_t cpu, subtype, alignment;
            uint64_t at = sizeof(fat) + uint64_t(index) * recordSize;
            if (fat64) {
                fat_arch_64 arch;
                if (!readFile(fd, fileSize, at, &arch, sizeof(arch)) || arch.reserved) return false;
                cpu = swap32(arch.cputype); subtype = swap32(arch.cpusubtype); alignment = swap32(arch.align);
                offset = swap64(arch.offset); size = swap64(arch.size);
            } else {
                fat_arch arch;
                if (!readFile(fd, fileSize, at, &arch, sizeof(arch))) return false;
                cpu = swap32(arch.cputype); subtype = swap32(arch.cpusubtype); alignment = swap32(arch.align);
                offset = swap32(arch.offset); size = swap32(arch.size);
            }
            if (alignment > 63 || (offset & ((uint64_t(1) << alignment) - 1)) || !contains(fileSize, offset, size) ||
                offset < sizeof(fat) + uint64_t(count) * recordSize || size < sizeof(header)) return false;
            if (cpu != CPU_TYPE_ARM64 || subtype != CPU_SUBTYPE_ARM64_ALL) continue;
            if (found) return false;
            found = true; sliceOffset = offset; sliceSize = size;
        }
        if (!found || !readFile(fd, fileSize, sliceOffset, &header, sizeof(header))) return false;
    }
    if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 || header.cpusubtype != CPU_SUBTYPE_ARM64_ALL ||
        (header.filetype != MH_EXECUTE && header.filetype != MH_DYLIB && header.filetype != MH_DYLINKER) ||
        !header.ncmds || header.ncmds > 4096 || header.sizeofcmds > 1024 * 1024 || header.ncmds > header.sizeofcmds / sizeof(load_command) ||
        !contains(sliceSize, sizeof(header), header.sizeofcmds)) return false;
    uint64_t cursor = sizeof(header), end = cursor + header.sizeofcmds;
    bool found = false;
    for (uint32_t index = 0; index < header.ncmds; ++index) {
        load_command command;
        if (!contains(end, cursor, sizeof(command)) || !readFile(fd, fileSize, sliceOffset + cursor, &command, sizeof(command)) ||
            command.cmdsize < sizeof(command) || (command.cmdsize & 7) || !contains(end, cursor, command.cmdsize)) return false;
        if (command.cmd == LC_CODE_SIGNATURE) {
            linkedit_data_command code;
            if (found || command.cmdsize != sizeof(code) || !readFile(fd, fileSize, sliceOffset + cursor, &code, sizeof(code)) ||
                !code.datasize || code.dataoff < end || !contains(sliceSize, code.dataoff, code.datasize)) return false;
            found = true;
            signature.fs_file_start = static_cast<off_t>(sliceOffset);
            signature.fs_blob_start = reinterpret_cast<void *>(uintptr_t(code.dataoff));
            signature.fs_blob_size = code.datasize;
        }
        cursor += command.cmdsize;
    }
    return found && cursor == end;
}

int preflightStat(const char *path, struct stat *output) {
    if (!S_ISREG(output->st_mode) || output->st_size <= 0) return 0;
    int fd = lsb::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    int error = 0;
    struct stat opened;
    LSBLazySignatureFileIdentity initial = LSBLazySignatureIdentityFromStat(output);
    if (!lsb::fstat(fd, &opened)) {
        LSBLazySignatureFileIdentity actual = LSBLazySignatureIdentityFromStat(&opened);
        fsignatures_t signature;
        lsb::zero(&signature, sizeof(signature));
        if (LSBLazySignatureIdentitiesEqual(&initial, &actual) && readSignature(fd, initial.size, signature)) {
            lsb::SyscallResult registered = lsb::syscall(SYS_fcntl, fd, F_ADDFILESIGS_RETURN, address(&signature));
            LSBLazySignatureFileIdentity published;
            if (registered.error && registered.error != EFAULT && registered.error != EBADF && !retry(fd, &signature, &published)) {
                struct stat refreshed;
                if (lsb::stat(path, &refreshed)) error = *lsb::errnoAddress();
                else {
                    LSBLazySignatureFileIdentity refreshedIdentity = LSBLazySignatureIdentityFromStat(&refreshed);
                    if (!LSBLazySignatureIdentitiesEqual(&refreshedIdentity, &published)) error = EAGAIN;
                    else lsb::copy(output, &refreshed, sizeof(*output));
                }
            }
        }
    }
    lsb::close(fd);
    return error;
}

} // namespace

void LSBConfigureLazySignature(mach_port_t endpoint, const audit_token_t *hostAudit) {
    signerEndpoint = MACH_PORT_NULL;
    lsb::zero(&signerAudit, sizeof(signerAudit));
    if (!MACH_PORT_VALID(endpoint) || !hostAudit || !hostAudit->val[5] || !hostAudit->val[7]) return;
    lsb::copy(&signerAudit, hostAudit, sizeof(signerAudit));
    signerEndpoint = endpoint;
}

int LSBLazySignatureRetry(int fd, fsignatures_t *signature) {
    int saved = *lsb::errnoAddress();
    int error = retry(fd, signature);
    *lsb::errnoAddress() = saved;
    return error;
}

int LSBLazySignatureDyldStat(const char *path, struct stat *info) {
    int result = lsb::stat(path, info);
    if (result || !MACH_PORT_VALID(signerEndpoint)) return result;
    // dyld fd8d0c4d52320ebf64db34f3cb280310d905c5ae: DyldDelegates::mapFileReadOnly records FileID from path stat before open.
    // JustInTimeLoader keeps it after registration; Loader::validateFile and alias dedup compare that persistent inode/mtime.
    // Repair here so its persistent inode/mtime and later descriptor validation all describe the actual published file.
    int saved = *lsb::errnoAddress();
    int error = preflightStat(path, info);
    *lsb::errnoAddress() = error ? error : saved;
    return error ? -1 : 0;
}
