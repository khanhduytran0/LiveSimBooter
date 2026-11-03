#include "ForkBashResume.h"
#include "ForkBashImports.h"
#include "ForkTrialWire.h"
#include <fcntl.h>
#include <setjmp.h>
#include <stdlib.h>

extern pid_t LSBTrialBashGetpid(void);
extern int LSBTrialBashExecve(const char *, char *const [], char *const []);
LSBTrialBashCalls LSBTrialObservedCalls;
LSBForkContext LSBTrialObserverContext, LSBTrialGetpidContext;
static bool forkCommitted;

// Offsets from llvm-objdump on the Bash UUID checked by forkTrialBashImage. Pin both direct callers as
// well as the fork return and execve stub so a different Bash invocation cannot commit this trial.
enum { BashShellExecReturn = 0x18540, BashExecReturn = 0x19570, BashTopLevel = 0xa72e0, BashLastStatus = 0xa6d7c, BashExitShell = 0x6cb0 };

_Static_assert(sizeof(sigjmp_buf) == 0xa73a4 - BashTopLevel, "Bash top_level ends at top_level_mask");

static bool stackContains(uintptr_t bottom, size_t size, uintptr_t address, size_t bytes) {
    return address >= bottom && address - bottom <= size && bytes <= size - (address - bottom);
}

static unsigned bashForkCaller(uintptr_t header, const LSBForkContext *caller, uintptr_t bottom, size_t size) {
    static const struct { uint32_t offset, word; } instructions[] = {
        {BashLateChildReturn - 4, 0x940034c3}, {BashLateChildReturn, 0x340001c0}, {BashShellExecReturn - 4, 0x940003fe},
        {BashEarlyChildReturn - 4, 0x9400376a}, {BashEarlyChildReturn, 0x34000e00}, {0x17a98, 0x710002df}, {0x17b14, 0x9400081c},
        {BashSubshellReturn - 4, 0x94003f0d}, {BashSubshellReturn, 0xaa0003f4}, {0x15a50, 0x340173f4}, {0x188e0, 0x9400004c}, {0x188e4, 0x9401c2ef},
        {0x6cb0, 0xa9be4ff4}, {0x3a150, 0x10368c80}, {0x3a15c, 0x94013deb}, {0x4fe80, 0xb9000113}, {0x4fe88, 0x97ffa8af},
        {BashSubstitutionReturn - 4, 0x97ffd8dc}, {BashSubstitutionReturn, 0xaa0003f4}, {0x2f318, 0x34000254}, {0x2f37c, 0x9401683a},
        {0x2f778, 0x94007eba}, {0x2f6d8, 0x97ffda2f}, {0x2f808, 0x94016726},
        {BashForkReturn - 4, 0x94018f84}, {BashForkReturn, 0xaa0003f3}, {BashForkReturn + 4, 0x36f802f3}, {0x2576c, 0x340001b3},
        {BashExecReturn - 4, 0x9401bfca}, {BashExecStub, 0xd503201f}, {BashExecStub + 4, 0x58096790}, {BashExecStub + 8, 0xd61f0200}
    };
    uintptr_t lr;
    if (!forkTrialBashImage(header) || !caller || caller->pc != header + BashForkReturn || (caller->sp & 15) ||
        !stackContains(bottom, size, caller->sp, 0x60) || caller->fp != caller->sp + 0x50 ||
        !forkTrialRead(caller->fp + sizeof(lr), &lr, sizeof(lr))) return 0;
    for (size_t i = 0; i < sizeof(instructions) / sizeof(instructions[0]); ++i) {
        uint32_t word;
        if (!forkTrialRead(header + instructions[i].offset, &word, sizeof(word)) || word != instructions[i].word) return 0;
    }
    if (lr == header + BashSubshellReturn || lr == header + BashSubstitutionReturn || lr == header + BashEarlyChildReturn || lr == header + BashLateChildReturn)
        return (unsigned)(lr - header);
    return 0;
}

static const char *bashBranchName(unsigned site) {
    switch (site) {
        case BashSubshellReturn: return "subshell";
        case BashSubstitutionReturn: return "substitution";
        case BashEarlyChildReturn: return "early";
        case BashLateChildReturn: return "late";
        default: return "unknown";
    }
}

static bool replaceImport(uintptr_t slot, uintptr_t expected, uintptr_t replacement) {
    vm_region_submap_info_data_64_t info = {};
    uintptr_t target;
    if (forkTrialLeaf(slot, sizeof(target), &info) != KERN_SUCCESS || info.protection != (VM_PROT_READ | VM_PROT_WRITE) ||
        info.inheritance != VM_INHERIT_COPY || !forkTrialRead(slot, &target, sizeof(target)) || target != expected) return false;
    return vm_write(mach_task_self(), slot, (vm_offset_t)&replacement, sizeof(replacement)) == KERN_SUCCESS;
}

// Copy arguments onto observer-owned storage before invoking the guest execve. No inherited allocation is freed.
static struct {
    char path[LSB_PATH_BYTES], strings[LSB_STRING_BYTES];
    char *argv[LSB_MAX_VECTOR + 1], *envp[LSB_MAX_VECTOR + 1];
    size_t used, argc, envc;
} command;

static int copyString(uintptr_t address, char *out, size_t capacity, size_t *used) {
    size_t offset = 0, pageMask = (size_t)getpagesize() - 1;
    while (offset < capacity) {
        uintptr_t cursor;
        if (!address || __builtin_add_overflow(address, offset, &cursor)) return EFAULT;
        size_t count = (pageMask + 1) - (cursor & pageMask);
        if (count > capacity - offset) count = capacity - offset;
        if (count > 256) count = 256;
        if (!forkTrialRead(cursor, out + offset, count)) return EFAULT;
        const char *nul = memchr(out + offset, 0, count);
        if (nul) {
            *used = (size_t)(nul - out) + 1;
            return 0;
        }
        offset += count;
    }
    return E2BIG;
}

static int copyVector(uintptr_t vector, char **out, size_t *count) {
    if (!vector || (vector & (sizeof(uintptr_t) - 1))) return EFAULT;
    for (size_t index = 0; index <= LSB_MAX_VECTOR; ++index) {
        uintptr_t slot, address;
        if (__builtin_add_overflow(vector, index * sizeof(address), &slot) || !forkTrialRead(slot, &address, sizeof(address))) return EFAULT;
        if (!address) {
            out[index] = NULL;
            *count = index;
            return 0;
        }
        if (index == LSB_MAX_VECTOR) return E2BIG;
        out[index] = command.strings + command.used;
        size_t bytes;
        int error = copyString(address, out[index], sizeof(command.strings) - command.used, &bytes);
        if (error) return error;
        command.used += bytes;
    }
    return E2BIG;
}

static int copyBashCommand(bool dfOnly) {
    size_t bytes;
    int error = copyString(LSBTrialObservedCalls.path, command.path, sizeof(command.path), &bytes);
    if (error) return error;
    if (dfOnly && strcmp(command.path, "/bin/df")) {
        const char *root = getenv("DYLD_ROOT_PATH");
        size_t length = root ? strlen(root) : 0;
        if (!length || length + sizeof("/bin/df") != bytes || strncmp(root, command.path, length) ||
            strcmp(command.path + length, "/bin/df")) return ENOTSUP;
    }
    command.used = 0;
    if ((error = copyVector(LSBTrialObservedCalls.argv, command.argv, &command.argc)) ||
        (error = copyVector(LSBTrialObservedCalls.envp, command.envp, &command.envc))) return error;
    if (!command.argc) return EPROTO;
    if (!dfOnly) return 0;
    const char *slash = strrchr(command.argv[0], '/');
    return !strcmp(slash ? slash + 1 : command.argv[0], "df") ? 0 : EPROTO;
}

static int commitFork(int fd) {
    int32_t accepted = 0;
    int error = trialTransfer(fd, &accepted, sizeof(accepted), true);
    if (error) {
        // A partial success word must never be followed by the caller's separate error word.
        shutdown(fd, SHUT_WR);
        return error;
    }
    forkCommitted = true;
    close(fd);
    return 0;
}

int LSBTrialResumeBash(uintptr_t header, const LSBForkContext *caller, uintptr_t stackStart, size_t stackSize, int controlFD, bool dfOnly, bool allowEarly) {
    unsigned site = bashForkCaller(header, caller, stackStart, stackSize);
    if (!site || ((!allowEarly || dfOnly) && site != BashLateChildReturn) ||
        stackContains(stackStart, stackSize, (uintptr_t)&header, sizeof(header))) return ENOTSUP;
    // Rebinding occurs after all imports have been translated to child-local addresses. Only edit the copied Bash's slots.
    if (!replaceImport(header + BashGetpidSlot, (uintptr_t)&getpid, (uintptr_t)&LSBTrialBashGetpid) ||
        !replaceImport(header + BashExecSlot, (uintptr_t)&execve, (uintptr_t)&LSBTrialBashExecve)) return EPROTO;

    // Bash may redirect stderr after getpid. Keep instrumentation out of the command's output; closed stderr disables it.
    const int traceFD = forkTrialTraceEnabled() ? fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, LSB_MAX_FDS + 1) : -1;
    const LSBTrialBashCalls *calls = &LSBTrialObservedCalls;
    // exit_builtin jumps to Bash's top_level. A grouped shell inherits that jump buffer from before fork,
    // including register state encoded by the parent's libc. Mirror run_one_command (+0x7158) for EXITPROG and ERREXIT.
    // Bash exit_shell runs the usual shutdown with last_command_exit_value. command_substitute sets up its own landing.
    if (site == BashSubshellReturn) {
        switch (sigsetjmp(*(sigjmp_buf *)(header + BashTopLevel), 1)) {
            case 0: break;
            case 3: case 4:
                if (forkCommitted) ((void (*)(int))(header + BashExitShell))(*(const int *)(header + BashLastStatus));
                _exit(126);
            default: _exit(126);
        }
    }
    if (LSBCaptureForkContext(&LSBTrialObserverContext)) LSBResumeForkContext(caller);

    int error = EPROTO;
    if (calls->getpidSP != caller->sp || calls->getpidLR != header + BashGetpidCall + 4) goto failed;
    if (!calls->execLR) {
        if (!dfOnly && (error = commitFork(controlFD))) goto failed;
        if (traceFD >= 0)
            dprintf(traceFD, "fork trial entered Bash child getpid on copied stack child_pid=%d sp=%p lr=%p committed=%d branch=%s\n",
                    getpid(), (void *)calls->getpidSP, (void *)calls->getpidLR, forkCommitted, bashBranchName(site));
        LSBResumeForkContext(&LSBTrialGetpidContext);
    }
    uintptr_t lr;
    if (calls->execLR != header + BashExecReturn || (calls->execSP & 15) ||
        !stackContains(stackStart, stackSize, calls->execSP, sizeof(lr)) || calls->execFP < calls->execSP ||
        !stackContains(stackStart, stackSize, calls->execFP, 2 * sizeof(lr)) || !forkTrialRead(calls->execFP + sizeof(lr), &lr, sizeof(lr)) ||
        lr != header + BashShellExecReturn) goto failed;
    error = copyBashCommand(dfOnly);
    if (error) goto failed;
    if (traceFD >= 0)
        dprintf(traceFD, "fork trial Bash child_pid=%d fork_sp=%p fork_pc=%p getpid_sp=%p getpid_lr=%p exec_sp=%p exec_lr=%p path=%s argc=%zu envc=%zu\n",
                getpid(), (void *)caller->sp, (void *)caller->pc, (void *)calls->getpidSP, (void *)calls->getpidLR,
                (void *)calls->execSP, (void *)calls->execLR, command.path, command.argc, command.envc);
    if (dfOnly && (error = commitFork(controlFD))) goto failed;
    if (traceFD >= 0) close(traceFD);
    // Let the guest execve resolve Bash's path with its normal containment, access and image checks.
    execve(command.path, command.argv, command.envp);
    error = errno;
    dprintf(STDERR_FILENO, "bash: %s: %s\n", command.path, strerror(error));
    // shell_execve +0x434 on this Bash returns 127 for ENOENT, 126 for other exec errors.
    // Interpreting a script after ENOEXEC would re-enter Bash with unsnapshotted shell state.
    _exit(error == ENOENT ? 127 : 126);

failed:
    if (traceFD >= 0) {
        dprintf(traceFD, "fork trial Bash continuation error=%d committed=%d\n", error, forkCommitted);
        close(traceFD);
    }
    if (forkCommitted) _exit(126);
    return error;
}
