#pragma once

#include "../Shared/Fork/ForkContext.h"

// The getpid and execve stubs record the unmodified Bash caller's registers before touching a stack.
#define LSB_BASH_GETPID_SP 0
#define LSB_BASH_EXEC_PATH 16
#define LSB_BASH_EXEC_ENVP 32
#define LSB_BASH_EXEC_SP 48

#ifndef __ASSEMBLER__
#include <stdbool.h>

typedef struct {
    uintptr_t getpidSP, getpidLR;
    uintptr_t path, argv, envp, execFP, execSP, execLR;
} LSBTrialBashCalls;

_Static_assert(offsetof(LSBTrialBashCalls, getpidSP) == LSB_BASH_GETPID_SP && offsetof(LSBTrialBashCalls, path) == LSB_BASH_EXEC_PATH &&
               offsetof(LSBTrialBashCalls, envp) == LSB_BASH_EXEC_ENVP && offsetof(LSBTrialBashCalls, execSP) == LSB_BASH_EXEC_SP &&
               sizeof(LSBTrialBashCalls) == 64, "Bash continuation observation layout");

// Run only in the disposable observer, after snapshot installation and import rebinding.
// A return is a pre-commit errno for the parent. General exec commits at the copied child getpid; df-only waits until execve.
int LSBTrialResumeBash(uintptr_t header, const LSBForkContext *caller, uintptr_t stackStart, size_t stackSize, int controlFD,
                       bool dfOnly, bool allowEarly, bool traceEnabled);
#endif
