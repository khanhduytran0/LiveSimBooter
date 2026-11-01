#include "../LiveSimBooter/LazySignatureClient.cpp"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
int localError;
unsigned opens, reads, repairs, statCalls, registrations;
bool allAccepted, statRace;
LSBLazySignatureRequest request;
audit_token_t hostAudit = {{1, 2, 3, 4, 5, 6, 7, 8}};
std::vector<ino_t> acceptedInodes;
std::string fixturePath;

int nativeResult(int value) { if (value < 0) localError = errno; return value; }
bool accepted(ino_t inode) { for (ino_t item : acceptedInodes) if (item == inode) return true; return allAccepted; }

void writeBytes(const std::string &path, const std::vector<char> &bytes) {
    int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0700);
    assert(fd >= 0 && ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()) && !::close(fd));
}
std::vector<char> thin() {
    std::vector<char> bytes(32768);
    mach_header_64 header = {MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, MH_DYLIB, 1, sizeof(linkedit_data_command), 0, 0};
    linkedit_data_command code = {LC_CODE_SIGNATURE, sizeof(code), 16384, 16384};
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), &code, sizeof(code));
    return bytes;
}
bool parses(const std::vector<char> &bytes, uint64_t slice = 0) {
    writeBytes(fixturePath, bytes);
    int fd = ::open(fixturePath.c_str(), O_RDONLY);
    assert(fd >= 0);
    fsignatures_t signature = {};
    bool result = readSignature(fd, bytes.size(), signature);
    assert(!::close(fd));
    if (result) assert(signature.fs_file_start == static_cast<off_t>(slice) && address(signature.fs_blob_start) == 16384 && signature.fs_blob_size == 16384);
    return result;
}
}

namespace lsb {
int *errnoAddress() { return &localError; }
void *copy(void *to, const void *from, size_t size) { return std::memcpy(to, from, size); }
void *zero(void *to, size_t size) { return std::memset(to, 0, size); }
[[noreturn]] void exitProcess(int) { std::abort(); }
int open(const char *path, int flags, ...) { ++opens; return nativeResult(::open(path, flags)); }
int close(int fd) { return nativeResult(::close(fd)); }
int fstat(int fd, struct stat *info) { return nativeResult(::fstat(fd, info)); }
int stat(const char *path, struct stat *info) {
    ++statCalls;
    int result = nativeResult(::stat(path, info));
    if (!result && statRace && statCalls == 2) ++info->st_ctimespec.tv_nsec;
    return result;
}
ssize_t pread(int fd, void *bytes, size_t size, off_t offset) {
    ++reads;
    ssize_t result = ::pread(fd, bytes, size, offset);
    if (result < 0) localError = errno;
    return result;
}
int fcntl(int fd, int command, ...) {
    va_list args; va_start(args, command);
    int result;
    if (command == F_GETPATH) result = ::fcntl(fd, command, va_arg(args, char *));
    else if (command == F_SETFL || command == F_SETFD) result = ::fcntl(fd, command, va_arg(args, int));
    else { assert(command == F_GETFL || command == F_GETFD); result = ::fcntl(fd, command); }
    va_end(args);
    return nativeResult(result);
}
SyscallResult syscall(uintptr_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
    if (number == SYS_fcntl) {
        assert(a1 == F_ADDFILESIGS_RETURN);
        ++registrations;
        struct stat info; assert(!::fstat(static_cast<int>(a0), &info));
        if (!accepted(info.st_ino)) return {EACCES, EACCES};
        auto *signature = reinterpret_cast<fsignatures_t *>(a2);
        signature->fs_file_start = static_cast<off_t>(address(signature->fs_blob_start));
        return {0, 0};
    }
    intptr_t result;
    if (number == SYS_lseek) result = ::lseek(static_cast<int>(a0), static_cast<off_t>(a1), static_cast<int>(a2));
    else { assert(number == SYS_dup2); result = ::dup2(static_cast<int>(a0), static_cast<int>(a1)); }
    return {static_cast<uintptr_t>(result), result < 0 ? errno : 0};
}
mach_port_t task_self_trap() { return 1; }
kern_return_t mach_port_allocate(ipc_space_t, mach_port_right_t, mach_port_name_t *port) { *port = 139; return KERN_SUCCESS; }
kern_return_t mach_port_deallocate(ipc_space_t, mach_port_name_t) { assert(false); return KERN_FAILURE; }
int64_t machTrap(int64_t number, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
    assert(number == -19); return KERN_SUCCESS;
}
void mach_msg_destroy(mach_msg_header_t *) {}
mach_msg_return_t mach_msg(mach_msg_header_t *header, mach_msg_option_t options, mach_msg_size_t, mach_msg_size_t,
                           mach_port_name_t, mach_msg_timeout_t, mach_port_name_t) {
    if (options & MACH_SEND_MSG) { request = *reinterpret_cast<LSBLazySignatureRequest *>(header); return MACH_MSG_SUCCESS; }
    // A fixture signer atomically replaces only the requested path. No real code-signing operation or security setting is modified.
    int fd = ::open(request.path, O_RDONLY);
    assert(fd >= 0);
    std::vector<char> bytes(request.file.size);
    assert(::read(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()) && !::close(fd));
    bytes[request.sliceOffset + request.blobOffset] ^= 1;
    std::string temporary = std::string(request.path) + ".signed";
    writeBytes(temporary, bytes);
    assert(!::rename(temporary.c_str(), request.path));
    struct stat published; assert(!::stat(request.path, &published));
    acceptedInodes.push_back(published.st_ino); ++repairs;
    auto *response = reinterpret_cast<LSBLazySignatureResponse *>(header);
    *response = {};
    response->header.msgh_id = LSB_LAZY_SIGNATURE_REPLY_ID; response->header.msgh_size = sizeof(*response);
    response->version = LSB_LAZY_SIGNATURE_VERSION; response->requestID = request.requestID;
    response->file = LSBLazySignatureIdentityFromStat(&published);
    auto *trailer = reinterpret_cast<mach_msg_audit_trailer_t *>(response + 1);
    trailer->msgh_trailer_type = MACH_MSG_TRAILER_FORMAT_0; trailer->msgh_trailer_size = sizeof(*trailer); trailer->msgh_audit = hostAudit;
    return MACH_MSG_SUCCESS;
}
}

int main() {
    char root[] = "/tmp/LiveSimBooter-preflight.XXXXXX";
    assert(mkdtemp(root));
    fixturePath = std::string(root) + "/fixture";
    auto bytes = thin();
    assert(parses(bytes));
    for (unsigned fault = 0; fault < 8; ++fault) {
        bytes = thin();
        auto *header = reinterpret_cast<mach_header_64 *>(bytes.data());
        auto *code = reinterpret_cast<linkedit_data_command *>(bytes.data() + sizeof(*header));
        switch (fault) {
            case 0: header->cpusubtype = CPU_SUBTYPE_ARM64E; break;
            case 1: header->ncmds = 4097; break;
            case 2: header->sizeofcmds = UINT32_MAX; break;
            case 3: code->cmdsize = 8; break;
            case 4: code->dataoff = 8; break;
            case 5: code->datasize = 16385; break;
            case 6: bytes.resize(20); break;
            case 7: header->filetype = MH_BUNDLE; break;
        }
        assert(!parses(bytes));
    }
    bytes = thin();
    auto *header = reinterpret_cast<mach_header_64 *>(bytes.data());
    header->ncmds = 2; header->sizeofcmds = 32;
    std::memcpy(bytes.data() + sizeof(*header) + 16, bytes.data() + sizeof(*header), 16);
    assert(!parses(bytes));
    for (bool wide : {false, true}) for (bool swapped : {false, true}) {
        auto swap32 = [swapped](uint32_t x) { return swapped ? __builtin_bswap32(x) : x; };
        auto swap64 = [swapped](uint64_t x) { return swapped ? __builtin_bswap64(x) : x; };
        bytes.assign(65536, 0);
        fat_header fat = {swap32(wide ? FAT_MAGIC_64 : FAT_MAGIC), swap32(1)};
        std::memcpy(bytes.data(), &fat, sizeof(fat));
        if (wide) {
            fat_arch_64 arch = {static_cast<cpu_type_t>(swap32(CPU_TYPE_ARM64)), 0, swap64(16384), swap64(32768), swap32(14), 0};
            std::memcpy(bytes.data() + sizeof(fat), &arch, sizeof(arch));
        } else {
            fat_arch arch = {static_cast<cpu_type_t>(swap32(CPU_TYPE_ARM64)), 0, swap32(16384), swap32(32768), swap32(14)};
            std::memcpy(bytes.data() + sizeof(fat), &arch, sizeof(arch));
        }
        auto slice = thin(); std::memcpy(bytes.data() + 16384, slice.data(), slice.size());
        assert(parses(bytes, 16384));
        std::memset(bytes.data() + sizeof(fat) + 8, 0xff, wide ? 8 : 4); // Invalid slice offset/range.
        assert(!parses(bytes));
        reinterpret_cast<fat_header *>(bytes.data())->nfat_arch = swap32(129);
        assert(!parses(bytes));
        bytes.resize(sizeof(fat)); assert(!parses(bytes));
    }
    bytes.assign(32768, 0);
    CacheSignatureHeader cache = {}; std::memcpy(cache.magic, "dyld_v1   arm64", 16);
    cache.mappingOffset = sizeof(cache); cache.mappingCount = 1; cache.signatureOffset = 16384; cache.signatureSize = 16384;
    std::memcpy(bytes.data(), &cache, sizeof(cache)); assert(parses(bytes));
    reinterpret_cast<CacheSignatureHeader *>(bytes.data())->signatureSize = UINT64_MAX; assert(!parses(bytes));

    writeBytes(fixturePath, thin());
    struct stat original, seen, actual; assert(!::stat(fixturePath.c_str(), &original));
    unsigned beforeOpens = opens, beforeReads = reads;
    localError = EBUSY;
    assert(!LSBLazySignatureDyldStat(fixturePath.c_str(), &seen) && localError == EBUSY);
    assert(seen.st_ino == original.st_ino && opens == beforeOpens && reads == beforeReads && !repairs && !registrations);
    LSBConfigureLazySignature(127, &hostAudit);
    allAccepted = true;
    assert(!LSBLazySignatureDyldStat(fixturePath.c_str(), &seen) && localError == EBUSY && seen.st_ino == original.st_ino && !repairs);
    allAccepted = false;
    std::string symlink = std::string(root) + "/symlink", hardlink = std::string(root) + "/hardlink";
    assert(!::symlink(fixturePath.c_str(), symlink.c_str()) && !::link(fixturePath.c_str(), hardlink.c_str()));
    assert(!LSBLazySignatureDyldStat(symlink.c_str(), &seen) && localError == EBUSY && repairs == 1);
    assert(!::stat(fixturePath.c_str(), &actual) && actual.st_ino != original.st_ino && seen.st_ino == actual.st_ino);
    assert(seen.st_mtimespec.tv_sec == actual.st_mtimespec.tv_sec && seen.st_mtimespec.tv_nsec == actual.st_mtimespec.tv_nsec);
    assert(!LSBLazySignatureDyldStat(fixturePath.c_str(), &seen) && seen.st_ino == actual.st_ino && repairs == 1);
    assert(!::stat(hardlink.c_str(), &seen) && seen.st_ino == original.st_ino); // Atomic replacement physically splits the old hardlink.
    assert(!LSBLazySignatureDyldStat(hardlink.c_str(), &seen) && repairs == 2);
    assert(!::stat(hardlink.c_str(), &actual) && seen.st_ino == actual.st_ino && actual.st_ino != original.st_ino);
    acceptedInodes.clear(); statRace = true; statCalls = 0;
    assert(LSBLazySignatureDyldStat(fixturePath.c_str(), &seen) == -1 && localError == EAGAIN);
    statRace = false;
    assert(!::unlink(symlink.c_str()) && !::unlink(hardlink.c_str()) && !::unlink(fixturePath.c_str()));
    assert(LSBLazySignatureDyldStat(fixturePath.c_str(), &seen) == -1 && localError == ENOENT);
    assert(!::rmdir(root));
    puts("Lazy signature preflight: bounded thin/fat/cache parser, disabled fast path, honest repaired/symlink/hardlink identities and race rejection passed");
}
