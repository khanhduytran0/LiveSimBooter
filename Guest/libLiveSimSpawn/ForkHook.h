#pragma once

#include "../Shared/Fork/ForkContext.h"
#include <stdbool.h>
#include <sys/types.h>

enum { LSB_FORK_NOT_HANDLED = -1, LSB_FORK_CHECKPOINT = 0x46, LSB_FORK_LIBC_RETURN_OFFSET = 0x34 };

// The provider stages a direct guest child before libc enters its atfork handlers. On return the shim
// owns any supplied descriptor and child. NOT_HANDLED with an empty attempt forwards to native fork.
typedef struct {
    int socket;
    pid_t child;
    bool captureLibcContext;
    bool isVfork; // Input to the built-in snapshot provider. External providers handle only ordinary fork.
    void *state;
    int (*checkPrepared)(void *); // Called with libSystem's prepare locks held. Must not allocate or acquire those locks.
    int (*sendPrepared)(void *, int); // Additional checkpoint payload on the private socket.
    void (*endCheckpoint)(void *); // Balance any holds from sendPrepared, also on failure.
    void (*release)(void *); // Called after libc returns, including on failed attempts.
} LSBForkAttempt;
typedef int (*LSBForkPrepare)(LSBForkAttempt *attempt, const LSBForkContext *caller);

// The provider must stay loaded for the process lifetime. Install from an initializer, before using fork.
// On its private socket the child reads a checkpoint byte and replies with one int32 errno. Zero commits
// the fork and may be sent only when the child's continuation and all inherited state are ready.
// When captureLibcContext is set, the checkpoint byte is followed by the registers at libc's prepared __fork call.
__attribute__((visibility("default"))) int LSBInstallForkProvider(LSBForkPrepare prepare);

// The shim's own imports resolve to the original libc entries even when both public symbols are interposed.
__attribute__((visibility("default"))) uintptr_t LSBTrialLibcForkReturnPC(bool isVfork);
