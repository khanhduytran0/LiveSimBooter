#ifndef LSB_PEER_PROCESS_COMPATIBILITY_H
#define LSB_PEER_PROCESS_COMPATIBILITY_H

#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// Safe before simulator TLS exists: atomic range reads only, no errno, filesystem access or syscalls.
bool LSBPeerProcessCallerEnabled(uintptr_t caller);
// launchd's child lookup uses logical PIDs and must run before any native lookup can match a reused proxy PID.
bool LSBLaunchdProcessTaskName(uintptr_t caller, mach_port_name_t target, int pid, mach_port_name_t *task, kern_return_t *result);

// Call only after the existing wrapper has called the native API. Pass its direct caller, result and errno unchanged.
// Preserve errno except failed POSIX copyout (EFAULT); Mach failure retains its original kern_return and errno.
// A successful cooperative fallback never rewrites native audit/PID fields.
int LSBPeerProcessPIDInfo(uintptr_t caller, int pid, int flavor, uint64_t argument, void *buffer, int size, int originalResult, int originalErrno);
kern_return_t LSBPeerProcessTaskName(uintptr_t caller, mach_port_name_t target, int pid, mach_port_name_t *task, kern_return_t originalResult);
int LSBPeerProcessPIDPath(int pid, void *buffer, uint32_t size);
int LSBPeerSandboxCheck(pid_t pid, const char *operation, int type, ...);
int LSBLaunchdProcessTerminate(pid_t pid, int *signalNumber);

#endif
