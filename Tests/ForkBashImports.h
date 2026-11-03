#pragma once

#include "ForkBashTraps.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>

extern uintptr_t __stack_chk_guard;
enum { BashExecSlot = 0x9c188, BashExecStub = 0x89494 };

static BashTrapReleases inheritedTrapReleases;

__attribute__((noinline)) static void forkTrialBashFree(void *pointer) {
    if (!consumeBashTrapRelease(&inheritedTrapReleases, (uintptr_t)__builtin_return_address(0), (uintptr_t)pointer)) free(pointer);
}

// A failed dup2 branches through Bash's redirection_error and inherited stdio/atexit state, then exit(1) at +0x1854c.
// Only TrialExec reaches this import. Its fork is already committed when the child calls it. Use the child's libc for
// the error report and termination, so its parent observes a child exit through waitpid even if a descriptor disappeared.
static inline int forkTrialBashDup2(int source, int destination) {
    int result = dup2(source, destination);
    if (result >= 0) return result;
    int error = errno;
    dprintf(STDERR_FILENO, "bash: %d: %s\n", source, strerror(error));
    _exit(1);
}

// llvm-objdump --indirect-symbols on this Bash UUID. The child may have different import addresses
// or unresolved lazy bindings to a dyld which only knows the parent's image list. Resolve the
// imports on make_child → execute_command_internal → shell_execve in the initialized observer.
static inline int rebindBashImports(uintptr_t header, bool allowTraps) {
    if (!forkTrialBashImage(header)) return ENOEXEC;
    int error = checkBashTraps(header, allowTraps, &inheritedTrapReleases);
    if (error) return error;
    static const uint32_t offsets[] = {0x9c010, 0x9c168, BashExecSlot, 0x9c1a0, BashFreeSlot, BashGetpidSlot, 0x9c308, 0x9c478, 0x9c488};
    const uintptr_t targets[] = {(uintptr_t)&__stack_chk_guard, (uintptr_t)&forkTrialBashDup2, (uintptr_t)&execve, (uintptr_t)&fcntl,
                                 inheritedTrapReleases.count ? (uintptr_t)&forkTrialBashFree : (uintptr_t)&free,
                                 (uintptr_t)&getpid, (uintptr_t)&malloc, (uintptr_t)&sigaction, (uintptr_t)&sigprocmask};
    _Static_assert(sizeof(offsets) / sizeof(offsets[0]) == sizeof(targets) / sizeof(targets[0]), "Bash import slots");
    vm_region_submap_info_data_64_t info = {};
    size_t count = sizeof(offsets) / sizeof(offsets[0]);
    if (forkTrialLeaf(header + offsets[0], offsets[count - 1] - offsets[0] + sizeof(uintptr_t), &info) != KERN_SUCCESS ||
        info.protection != (VM_PROT_READ | VM_PROT_WRITE) || info.inheritance != VM_INHERIT_COPY) return EPERM;
    for (size_t i = 0; i < count; ++i) {
        if (vm_write(mach_task_self(), header + offsets[i], (vm_offset_t)&targets[i], sizeof(targets[i])) != KERN_SUCCESS) return EFAULT;
    }
    return 0;
}
