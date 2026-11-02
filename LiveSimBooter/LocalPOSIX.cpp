#include "LocalPOSIX.h"
#include "LocalRuntime.h"
#include "LazySignatureClient.h"

#include <cerrno>
#include <cstdarg>
#include <limits.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

// Kernel ABI references: apple-oss-distributions/xnu bsd/kern/syscalls.master,
// bsd/sys/proc_info{,_private}.h, and bsd/sys/dirent.h. These wrappers do not call host libSystem.
namespace lsb {
namespace {

bool amfiPolicyReady = false;
uint64_t amfiInput = 0;
uint64_t amfiOutput = 0;
int amfiResult = -1;
int amfiError = ENOSYS;

uintptr_t address(const void *value) { return reinterpret_cast<uintptr_t>(value); }

intptr_t result(SyscallResult value)
{
    if (value.error) {
        *errnoAddress() = value.error;
        return -1;
    }
    return static_cast<intptr_t>(value.value);
}

int fail(int error)
{
    *errnoAddress() = error;
    return -1;
}

int pathForFile(const char *path, char *buffer)
{
    int fd = lsb::open(path, O_EVTONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int status = lsb::fcntl(fd, F_GETPATH, buffer);
    int savedError = *errnoAddress();
    lsb::close(fd);
    if (status < 0)
        *errnoAddress() = savedError;
    return status;
}

// DIR is opaque to dyld_sim; only these three callbacks ever inspect this allocation.
struct Directory {
    int fd;
    unsigned lock;
    size_t offset;
    size_t available;
    bool ended;
    alignas(8) char entries[32768];
};

class DirectoryLock {
public:
    explicit DirectoryLock(Directory *directory) : value(directory->lock)
    {
        while (__atomic_exchange_n(&value, 1, __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(&value, __ATOMIC_RELAXED)) {}
        }
    }
    ~DirectoryLock() { __atomic_store_n(&value, 0, __ATOMIC_RELEASE); }
private:
    unsigned &value;
};

} // namespace

int open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
    }
    return static_cast<int>(result(syscall(SYS_open, address(path), flags, mode)));
}

int close(int fd) { return static_cast<int>(result(syscall(SYS_close, fd))); }
ssize_t pread(int fd, void *buffer, size_t size, off_t offset) { return result(syscall(SYS_pread, fd, address(buffer), size, offset)); }
ssize_t write(int fd, const void *buffer, size_t size) { return result(syscall(SYS_write, fd, address(buffer), size)); }

void *mmap(void *location, size_t size, int protection, int flags, int fd, off_t offset)
{
    // Request modern mmap validation (including nonzero size and aligned file offsets) from the kernel.
    return reinterpret_cast<void *>(result(syscall(SYS_mmap, address(location), size, protection, flags | MAP_UNIX03, fd, offset)));
}

int munmap(void *location, size_t size) { return static_cast<int>(result(syscall(SYS_munmap, address(location), size))); }
int mprotect(void *location, size_t size, int protection) { return static_cast<int>(result(syscall(SYS_mprotect, address(location), size, protection))); }
int madvise(void *location, size_t size, int advice) { return static_cast<int>(result(syscall(SYS_madvise, address(location), size, advice))); }
int stat(const char *path, struct stat *value) { return static_cast<int>(result(syscall(SYS_stat64, address(path), address(value)))); }
int fstat(int fd, struct stat *value) { return static_cast<int>(result(syscall(SYS_fstat64, fd, address(value)))); }

int fcntl(int fd, int command, ...)
{
    uintptr_t argument = 0;
    va_list arguments;
    va_start(arguments, command);
    switch (command) {
        case F_GETFD: case F_GETFL: case F_GETOWN: case F_FULLFSYNC: case F_BARRIERFSYNC: case F_GETNOSIGPIPE:
        case F_GETPROTECTIONCLASS: case F_GETPROTECTIONLEVEL: case F_GETLEASE:
            break;
        case F_DUPFD: case F_DUPFD_CLOEXEC: case F_SETFD: case F_SETFL: case F_SETOWN: case F_RDAHEAD: case F_NOCACHE:
        case F_NODIRECT: case F_SETPROTECTIONCLASS: case F_SETBACKINGSTORE: case F_SETNOSIGPIPE: case F_SETLEASE: case F_TRANSFEREXTENTS:
            argument = static_cast<uintptr_t>(va_arg(arguments, int));
            break;
        default:
            argument = address(va_arg(arguments, void *));
            break;
    }
    va_end(arguments);
    SyscallResult original = syscall(SYS_fcntl, fd, command, argument);
    if (command == F_ADDFILESIGS_RETURN && original.error && original.error != EFAULT && original.error != EBADF &&
        !LSBLazySignatureRetry(fd, reinterpret_cast<fsignatures_t *>(argument))) return 0;
    return static_cast<int>(result(original));
}

int ioctl(int fd, unsigned long command, ...)
{
    uintptr_t argument = 0;
    if (command != FIOCLEX && command != FIONCLEX) {
        va_list arguments;
        va_start(arguments, command);
        argument = address(va_arg(arguments, void *));
        va_end(arguments);
    }
    return static_cast<int>(result(syscall(SYS_ioctl, fd, command, argument)));
}

int issetugid() { return static_cast<int>(result(syscall(SYS_issetugid))); }

char *getcwd(char *buffer, size_t size)
{
    if (!buffer || !size) {
        fail(EINVAL);
        return nullptr;
    }
    char path[PATH_MAX];
    if (pathForFile(".", path) < 0)
        return nullptr;
    if (length(path) >= size) {
        fail(ERANGE);
        return nullptr;
    }
    copyString(buffer, path, size);
    return buffer;
}

char *realpath(const char *path, char *buffer)
{
    if (!path || !buffer) {
        fail(EINVAL);
        return nullptr;
    }
    // F_GETPATH uses the opened vnode, so the kernel resolves symlinks, '..', relative paths, and filesystem case spelling.
    return pathForFile(path, buffer) < 0 ? nullptr : buffer;
}

DIR *opendir(const char *path)
{
    int fd = lsb::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    auto directory = static_cast<Directory *>(lsb::mmap(nullptr, sizeof(Directory), PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0));
    if (directory == MAP_FAILED) {
        int savedError = *errnoAddress();
        lsb::close(fd);
        *errnoAddress() = savedError;
        return nullptr;
    }
    // Anonymous mapping initializes offset, available, ended and lock to zero.
    directory->fd = fd;
    return reinterpret_cast<DIR *>(directory);
}

int readdir_r(DIR *opaque, dirent *entry, dirent **output)
{
    if (output)
        *output = nullptr;
    if (!opaque || !entry || !output)
        return EINVAL;
    auto directory = reinterpret_cast<Directory *>(opaque);
    DirectoryLock lock(directory);
    for (;;) {
        if (directory->offset == directory->available) {
            if (directory->ended)
                return 0;
            off_t position = 0;
            SyscallResult read = syscall(SYS_getdirentries64, directory->fd, address(directory->entries), sizeof(directory->entries), address(&position));
            if (read.error)
                return read.error;
            if (read.value > sizeof(directory->entries))
                return EIO;
            directory->offset = 0;
            directory->available = read.value;
            if (!read.value) {
                directory->ended = true;
                return 0;
            }
        }
        constexpr size_t headerSize = offsetof(dirent, d_name);
        size_t remaining = directory->available - directory->offset;
        if (remaining < headerSize + 1)
            return EIO;
        auto source = reinterpret_cast<const dirent *>(directory->entries + directory->offset);
        size_t recordSize = source->d_reclen;
        if (recordSize < headerSize + 1 || recordSize > remaining || recordSize > sizeof(dirent) || (recordSize & 3))
            return EIO;
        if (source->d_namlen >= sizeof(entry->d_name) || headerSize + source->d_namlen >= recordSize || source->d_name[source->d_namlen])
            return EIO;
        directory->offset += recordSize;
        if (!source->d_ino || source->d_type == DT_WHT)
            continue;
        zero(entry, sizeof(*entry));
        copy(entry, source, recordSize);
        *output = entry;
        return 0;
    }
}

int closedir(DIR *opaque)
{
    if (!opaque)
        return fail(EINVAL);
    auto directory = reinterpret_cast<Directory *>(opaque);
    int status = lsb::close(directory->fd);
    int savedError = *errnoAddress();
    int unmapStatus = lsb::munmap(directory, sizeof(Directory));
    if (status < 0) {
        *errnoAddress() = savedError;
        return status;
    }
    return unmapStatus;
}

int proc_regionfilename(int pid, uint64_t regionAddress, void *buffer, uint32_t size)
{
    if (!buffer || size < PATH_MAX) {
        fail(!buffer ? EFAULT : ENOMEM);
        return 0;
    }
    // PROC_INFO_CALL_PIDINFO=2, PROC_PIDREGIONPATH=31; the reply is two 64-bit fields followed by MAXPATHLEN bytes.
    alignas(8) unsigned char reply[16 + PATH_MAX];
    zero(reply, sizeof(reply));
    SyscallResult query = syscall(SYS_proc_info, 2, pid, 31, regionAddress, address(reply), sizeof(reply));
    if (query.error) {
        fail(query.error);
        return 0;
    }
    if (query.value != sizeof(reply)) {
        fail(EIO);
        return 0;
    }
    auto path = reinterpret_cast<char *>(reply + 16);
    path[PATH_MAX - 1] = '\0';
    return static_cast<int>(copyString(static_cast<char *>(buffer), path, size));
}

pid_t getpid() { return static_cast<pid_t>(result(syscall(SYS_getpid))); }

ssize_t fsgetpath(char *buffer, size_t size, fsid_t *filesystem, uint64_t object)
{
    return result(syscall(SYS_fsgetpath, address(buffer), size, address(filesystem), object));
}

int getattrlistbulk(int fd, void *attributes, void *buffer, size_t size, uint64_t options)
{
    return static_cast<int>(result(syscall(SYS_getattrlistbulk, fd, address(attributes), address(buffer), size, options)));
}

int getattrlist(const char *path, void *attributes, void *buffer, size_t size, unsigned int options)
{
    return static_cast<int>(result(syscall(SYS_getattrlist, address(path), address(attributes), address(buffer), size, options)));
}

int getfsstat(struct statfs *buffer, int size, int flags) { return static_cast<int>(result(syscall(SYS_getfsstat64, address(buffer), size, flags))); }

int sysctlbyname(const char *name, void *oldValue, size_t *oldSize, void *newValue, size_t newSize)
{
    if (!name)
        return fail(EFAULT);
    return static_cast<int>(result(syscall(SYS_sysctlbyname, address(name), length(name), address(oldValue), address(oldSize), address(newValue), newSize)));
}

void setAMFIPolicyResult(uint64_t inputFlags, int policyResult, uint64_t outputFlags, int error)
{
    amfiInput = inputFlags;
    amfiResult = policyResult;
    amfiOutput = policyResult == 0 ? outputFlags : 0;
    amfiError = error;
    amfiPolicyReady = true;
}

int amfi_check_dyld_policy_self(uint64_t inputFlags, uint64_t *outputFlags)
{
    if (!outputFlags)
        return fail(EINVAL);
    *outputFlags = 0;
    // libamfi's policy-specific request ABI is private. Preserve the real pre-handoff result for the same process and input.
    if (!amfiPolicyReady || inputFlags != amfiInput)
        return fail(ENOSYS);
    *outputFlags = amfiOutput;
    if (amfiResult)
        *errnoAddress() = amfiError;
    return amfiResult;
}

[[noreturn]] void abort_with_payload(uint32_t reasonNamespace, uint64_t code, void *payload, uint32_t size, const char *reason, uint64_t flags)
{
    syscall(SYS_abort_with_payload, reasonNamespace, code, address(payload), size, address(reason), flags);
    // A failed/unsupported abort syscall must never return into unmapped host code.
    exitProcess(127);
}

} // namespace lsb
