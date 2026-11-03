#pragma once

#include "ForkContext.h"
#include <sys/types.h>

enum { LSB_FORK_NOT_HANDLED = -1, LSB_FORK_CHECKPOINT = 0x46 };

// The provider stages a direct guest child before libc enters its atfork handlers. On return the shim
// owns any supplied descriptor and child. NOT_HANDLED with an empty attempt forwards to native fork.
typedef struct { int socket; pid_t child; } LSBForkAttempt;
typedef int (*LSBForkPrepare)(LSBForkAttempt *attempt, const LSBForkContext *caller);

// The provider must stay loaded for the process lifetime. Install from an initializer, before using fork.
// On its private socket the child reads a checkpoint byte and replies with one int32 errno. Zero commits
// the fork and may be sent only when the child's continuation and all inherited state are ready.
__attribute__((visibility("default"))) int LSBInstallForkProvider(LSBForkPrepare prepare);
