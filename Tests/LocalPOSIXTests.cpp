#include "../LiveSimBooter/LocalPOSIX.h"
#include "../LiveSimBooter/LocalRuntime.h"
#include "../LiveSimBooter/LazySignatureClient.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

namespace {
using Arguments = std::array<uintptr_t, 8>;
std::function<lsb::SyscallResult(uintptr_t, const Arguments &)> handle;
int localError = 0;
unsigned signatureRetries = 0;
int signatureRetryError = ENOTSUP;
uintptr_t ptr(const void *value) { return reinterpret_cast<uintptr_t>(value); }
}

int LSBLazySignatureRetry(int fd, fsignatures_t *signature) {
    assert(fd == 17 && signature);
    ++signatureRetries;
    return signatureRetryError;
}

namespace lsb {
SyscallResult syscall(uintptr_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    assert(handle);
    return handle(number, {a0, a1, a2, a3, a4, a5, a6, a7});
}
int *errnoAddress() { return &localError; }
void *copy(void *out, const void *in, size_t size) { return std::memcpy(out, in, size); }
void *zero(void *out, size_t size) { return std::memset(out, 0, size); }
size_t length(const char *text) { return std::strlen(text); }
size_t copyString(char *out, const char *in, size_t size) { return ::strlcpy(out, in, size); }
[[noreturn]] void exitProcess(int code) { std::exit(code); }
}

namespace {

void testForwardingAndErrors()
{
    char buffer[32];
    const char *path = "/runtime/bin/df";
    uintptr_t expectedNumber = 0;
    Arguments expected = {};
    lsb::SyscallResult response{17, 0};
    unsigned calls = 0;
    handle = [&](uintptr_t number, const Arguments &args) {
        assert(number == expectedNumber && args == expected);
        ++calls;
        return response;
    };
    expectedNumber = SYS_open;
    expected = {ptr(path), O_RDONLY, 0};
    assert(lsb::open(path, O_RDONLY) == 17);
    expected = {ptr(path), O_CREAT | O_WRONLY, 0640};
    assert(lsb::open(path, O_CREAT | O_WRONLY, 0640) == 17);

    expectedNumber = SYS_pread;
    expected = {17, ptr(buffer), sizeof(buffer), 0x123456789abcdef0};
    assert(lsb::pread(17, buffer, sizeof(buffer), 0x123456789abcdef0) == 17);
    expectedNumber = SYS_write;
    expected = {17, ptr(buffer), sizeof(buffer)};
    response = {EACCES, EACCES};
    assert(lsb::write(17, buffer, sizeof(buffer)) == -1 && localError == EACCES);

    expectedNumber = SYS_mmap;
    expected = {0, 16384, PROT_READ, MAP_PRIVATE | MAP_UNIX03, 17, 0x100000000};
    response = {ENOMEM, ENOMEM};
    assert(lsb::mmap(nullptr, 16384, PROT_READ, MAP_PRIVATE, 17, 0x100000000) == MAP_FAILED && localError == ENOMEM);
    response = {0x100000000, 0};
    assert(lsb::mmap(nullptr, 16384, PROT_READ, MAP_PRIVATE, 17, 0x100000000) == reinterpret_cast<void *>(0x100000000));

    expectedNumber = SYS_fcntl;
    expected = {17, F_GETFD};
    response = {0, 0};
    assert(lsb::fcntl(17, F_GETFD) == 0);
    expected = {17, F_SETFD, FD_CLOEXEC};
    assert(lsb::fcntl(17, F_SETFD, FD_CLOEXEC) == 0);
    expected = {17, F_ADDFILESIGS_RETURN, ptr(buffer)};
    assert(lsb::fcntl(17, F_ADDFILESIGS_RETURN, buffer) == 0);
    assert(!signatureRetries);
    response = {EACCES, EACCES};
    assert(lsb::fcntl(17, F_ADDFILESIGS_RETURN, buffer) == -1 && localError == EACCES && signatureRetries == 1);
    signatureRetryError = 0;
    assert(lsb::fcntl(17, F_ADDFILESIGS_RETURN, buffer) == 0 && signatureRetries == 2);
    response = {EFAULT, EFAULT};
    assert(lsb::fcntl(17, F_ADDFILESIGS_RETURN, buffer) == -1 && localError == EFAULT && signatureRetries == 2);
    response = {0, 0};
    expectedNumber = SYS_ioctl;
    expected = {17, FIOCLEX};
    assert(lsb::ioctl(17, FIOCLEX) == 0);
    expected = {17, FIONREAD, ptr(buffer)};
    assert(lsb::ioctl(17, FIONREAD, buffer) == 0);
    assert(calls == 14);

    struct stat info;
    expectedNumber = SYS_stat64;
    expected = {ptr(path), ptr(&info)};
    assert(lsb::stat(path, &info) == 0);
    expectedNumber = SYS_fstat64;
    expected = {17, ptr(&info)};
    assert(lsb::fstat(17, &info) == 0);
    expectedNumber = SYS_getfsstat64;
    expected = {0, 0, MNT_NOWAIT};
    assert(lsb::getfsstat(nullptr, 0, MNT_NOWAIT) == 0);

    size_t oldSize = sizeof(buffer);
    const char *name = "kern.osrelease";
    expectedNumber = SYS_sysctlbyname;
    expected = {ptr(name), std::strlen(name), ptr(buffer), ptr(&oldSize), 0, 0};
    assert(lsb::sysctlbyname(name, buffer, &oldSize, nullptr, 0) == 0);
    uint64_t policy = ~uint64_t(0);
    assert(lsb::amfi_check_dyld_policy_self(1, &policy) == -1 && policy == 0 && localError == ENOSYS);
    lsb::setAMFIPolicyResult(1, 0, 0x25, 0);
    assert(lsb::amfi_check_dyld_policy_self(1, &policy) == 0 && policy == 0x25);
    assert(lsb::amfi_check_dyld_policy_self(0, &policy) == -1 && policy == 0 && localError == ENOSYS);
    lsb::setAMFIPolicyResult(1, -1, ~uint64_t(0), EPERM);
    assert(lsb::amfi_check_dyld_policy_self(1, &policy) == -1 && policy == 0 && localError == EPERM);
}

void testPathResolution()
{
    const char *resolved = "/private/runtime/bin/df";
    int pathError = 0;
    unsigned opens = 0;
    unsigned closes = 0;
    handle = [&](uintptr_t number, const Arguments &args) -> lsb::SyscallResult {
        if (number == SYS_open) {
            assert(args[1] == (O_EVTONLY | O_CLOEXEC));
            ++opens;
            return {23, 0};
        }
        if (number == SYS_fcntl) {
            assert(args[0] == 23 && args[1] == F_GETPATH);
            if (pathError)
                return {static_cast<uintptr_t>(pathError), pathError};
            std::strcpy(reinterpret_cast<char *>(args[2]), resolved);
            return {0, 0};
        }
        assert(number == SYS_close && args[0] == 23);
        ++closes;
        return {EBADF, EBADF}; // cleanup must not overwrite the path error
    };
    char path[PATH_MAX];
    assert(lsb::realpath("./bin/link-to-df", path) == path && std::strcmp(path, resolved) == 0);
    assert(lsb::getcwd(path, sizeof(path)) == path && std::strcmp(path, resolved) == 0);
    char small[4] = {'x', 'x', 'x', 'x'};
    assert(lsb::getcwd(small, sizeof(small)) == nullptr && localError == ERANGE && small[0] == 'x');
    pathError = EACCES;
    assert(lsb::realpath("inaccessible", path) == nullptr && localError == EACCES);
    assert(opens == 4 && closes == 4);
    assert(lsb::getcwd(nullptr, 0) == nullptr && localError == EINVAL);
}

size_t addEntry(char *buffer, ino_t inode, unsigned char type, const char *name)
{
    size_t nameSize = std::strlen(name);
    size_t recordSize = (offsetof(dirent, d_name) + nameSize + 1 + 7) & ~size_t(7);
    std::memset(buffer, 0, recordSize);
    auto entry = reinterpret_cast<dirent *>(buffer);
    entry->d_ino = inode;
    entry->d_type = type;
    entry->d_namlen = nameSize;
    entry->d_reclen = recordSize;
    std::memcpy(entry->d_name, name, nameSize + 1);
    return recordSize;
}

void testDirectoryIteration()
{
    unsigned reads = 0;
    bool malformed = false;
    int directoryError = 0;
    void *allocation = nullptr;
    handle = [&](uintptr_t number, const Arguments &args) -> lsb::SyscallResult {
        if (number == SYS_open) {
            assert(args[1] == (O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            return {31, 0};
        }
        if (number == SYS_mmap) {
            allocation = ::mmap(nullptr, args[1], PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            assert(allocation != MAP_FAILED);
            return {ptr(allocation), 0};
        }
        if (number == SYS_getdirentries64) {
            assert(args[0] == 31);
            ++reads;
            if (directoryError)
                return {static_cast<uintptr_t>(directoryError), directoryError};
            if (reads > 1)
                return {0, 0};
            auto buffer = reinterpret_cast<char *>(args[1]);
            size_t size = addEntry(buffer, 0, DT_REG, "deleted");
            size += addEntry(buffer + size, 9, DT_WHT, "whiteout");
            size += addEntry(buffer + size, 10, DT_REG, "dyld_sim");
            size += addEntry(buffer + size, 11, DT_DIR, "system");
            if (malformed)
                reinterpret_cast<dirent *>(buffer)->d_reclen = 0;
            return {size, 0};
        }
        if (number == SYS_close) {
            assert(args[0] == 31);
            return {0, 0};
        }
        assert(number == SYS_munmap && args[0] == ptr(allocation));
        assert(::munmap(allocation, args[1]) == 0);
        allocation = nullptr;
        return {0, 0};
    };
    DIR *directory = lsb::opendir("/runtime/usr/lib");
    assert(directory);
    dirent entry;
    dirent *output = nullptr;
    assert(lsb::readdir_r(directory, &entry, &output) == 0 && output == &entry && std::strcmp(entry.d_name, "dyld_sim") == 0);
    assert(lsb::readdir_r(directory, &entry, &output) == 0 && output == &entry && std::strcmp(entry.d_name, "system") == 0);
    assert(lsb::readdir_r(directory, &entry, &output) == 0 && !output);
    assert(lsb::readdir_r(directory, &entry, &output) == 0 && !output && reads == 2);
    assert(lsb::closedir(directory) == 0 && !allocation);

    malformed = true;
    reads = 0;
    directory = lsb::opendir("/runtime/usr/lib");
    assert(lsb::readdir_r(directory, &entry, &output) == EIO && !output);
    assert(lsb::closedir(directory) == 0);

    directoryError = EACCES;
    directory = lsb::opendir("/runtime/usr/lib");
    assert(lsb::readdir_r(directory, &entry, &output) == EACCES && !output);
    assert(lsb::closedir(directory) == 0);
}

void testRegionFilename()
{
    int queryError = 0;
    const char *path = "/runtime/usr/lib/dyld_sim";
    handle = [&](uintptr_t number, const Arguments &args) -> lsb::SyscallResult {
        assert(number == SYS_proc_info && args[0] == 2 && args[1] == 42 && args[2] == 31 && args[3] == 0x100004000);
        assert(args[5] == PATH_MAX + 16);
        if (queryError)
            return {static_cast<uintptr_t>(queryError), queryError};
        std::strcpy(reinterpret_cast<char *>(args[4]) + 16, path);
        return {args[5], 0};
    };
    char buffer[PATH_MAX];
    assert(lsb::proc_regionfilename(42, 0x100004000, buffer, sizeof(buffer)) == static_cast<int>(std::strlen(path)));
    assert(std::strcmp(buffer, path) == 0);
    queryError = EPERM;
    assert(lsb::proc_regionfilename(42, 0x100004000, buffer, sizeof(buffer)) == 0 && localError == EPERM);
    assert(lsb::proc_regionfilename(42, 0x100004000, buffer, 4) == 0 && localError == ENOMEM);
}

} // namespace

int main()
{
    testForwardingAndErrors();
    testPathResolution();
    testDirectoryIteration();
    testRegionFilename();
    std::puts("LocalPOSIXTests passed");
}
