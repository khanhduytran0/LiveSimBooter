#pragma once

#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <sys/attr.h>
#include <sys/fsgetpath.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace lsb {

int open(const char *path, int flags, ...);
int close(int fd);
ssize_t pread(int fd, void *buffer, size_t size, off_t offset);
ssize_t write(int fd, const void *buffer, size_t size);
void *mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset);
int munmap(void *address, size_t size);
int mprotect(void *address, size_t size, int protection);
int madvise(void *address, size_t size, int advice);
int stat(const char *path, struct stat *result);
int fstat(int fd, struct stat *result);
int fcntl(int fd, int command, ...);
int ioctl(int fd, unsigned long command, ...);
int issetugid();
// dyld supplies its own buffers. Null-buffer allocation is deliberately unsupported across the two allocator runtimes.
char *getcwd(char *buffer, size_t size);
char *realpath(const char *path, char *buffer);
DIR *opendir(const char *path);
int readdir_r(DIR *directory, dirent *entry, dirent **result);
int closedir(DIR *directory);
int proc_regionfilename(int pid, uint64_t address, void *buffer, uint32_t size);
pid_t getpid();
ssize_t fsgetpath(char *buffer, size_t size, fsid_t *filesystem, uint64_t object);
int getattrlistbulk(int fd, void *attributes, void *buffer, size_t size, uint64_t options);
int getattrlist(const char *path, void *attributes, void *buffer, size_t size, unsigned int options);
int getfsstat(struct statfs *buffer, int size, int flags);
int sysctlbyname(const char *name, void *oldValue, size_t *oldSize, void *newValue, size_t newSize);
// Seed only with the actual host AMFI response before cache detachment; subsequent calls require the identical input flags.
void setAMFIPolicyResult(uint64_t inputFlags, int result, uint64_t outputFlags, int error);
int amfi_check_dyld_policy_self(uint64_t inputFlags, uint64_t *outputFlags);
[[noreturn]] void abort_with_payload(uint32_t reasonNamespace, uint64_t code, void *payload, uint32_t size, const char *reason, uint64_t flags);

} // namespace lsb
