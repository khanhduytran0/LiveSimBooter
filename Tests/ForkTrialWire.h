// Disposable guest fork test. This socket belongs to the parent and its direct broker-created child.
#pragma once
#include "../Guest/ForkHook.h"
#include "ForkBashTerminal.h"
#include "ForkAllocatorState.h"
#include "ForkPthreadState.h"
#include "ForkRuntimeImages.h"
#include "../ProcessSupport/SpawnTransport.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

enum { TrialWireVersion = 20, TrialMaxDepth = 4, TrialSlicedStack = 1, TrialRestoreAllocator = 4, TrialResumeGuest = 8, TrialVfork = 16,
       TrialStackHeadroom = 256 * 1024, TrialWitnessSize = 128 };
typedef enum { TrialSnapshot, TrialDF, TrialExec, TrialShell } TrialMode;

static inline uint64_t trialStackSliceStart(uint64_t bottom, uint64_t sp, uint64_t page) {
    uint64_t spPage = sp & ~(page - 1);
    return spPage - bottom > TrialStackHeadroom ? spPage - TrialStackHeadroom : bottom;
}

typedef struct {
    audit_token_t audit;
    int32_t logicalPID;
    uint32_t version;
} TrialReady;

typedef struct { int32_t error; uint32_t phase; uint64_t address; uint32_t pthreadPrefix, reserved; } TrialPreflight;
_Static_assert(sizeof(TrialPreflight) == 24, "fork preflight receipt");
typedef struct {
    uint64_t parentGetpid, variablesAddress, expectedFirstWord;
} TrialBashState;

typedef struct {
    audit_token_t audit;
    uint32_t version, rangeCount;
    uint64_t mainHeader, skipMask;
    uint32_t resume, flags;
    uint64_t depth; // First copied shell is depth 1. Ordinary modes never install a nested provider.
    uint64_t fullStackStart; // Sliced offers only. All live caller frames up to the end of the original leaf are still included.
    uint64_t closeOnExec[LSB_MAX_FDS / 64]; // Only pipe/bitmap FDs explicitly inherited while bootstrapping the early child.
    TrialTerminalAlias terminal;
    TrialBashState bash;
    ForkAllocatorState allocator;
    uint64_t allocatorMask, witness;
    ForkPthreadState pthreads;
    TrialSupportImages support;
    uint64_t argv, envp, guard;
    uint32_t argc, reserved;
    LSBForkContext caller;
    LSBForkWireRange ranges[LSB_MAX_FORK_RANGES]; // Same sorted ranges as the broker offer. Bit i marks VM_INHERIT_NONE.
} TrialOffer;

// A nested fork can offer its parent's reduced stack as a full VM leaf. Always guard the shell stack at every generation.
static inline bool trialNeedsStackGuard(const TrialOffer *offer) { return offer->resume == TrialShell || (offer->flags & TrialSlicedStack); }

_Static_assert(LSB_MAX_FORK_RANGES <= 64 && sizeof(TrialReady) == 40 && LSB_MAX_FDS % 64 == 0 &&
               sizeof(TrialOffer) == 416 + sizeof(ForkPthreadState) + sizeof(TrialSupportImages) + LSB_MAX_FDS / 8 + 16 * LSB_MAX_FORK_RANGES, "fork trial handshake size");

static inline int trialAllocatorCovers(void *context, uint64_t address, uint64_t bytes) {
    const TrialOffer *offer = context;
    if (!bytes || address > UINT64_MAX - bytes) return EINVAL;
    uint64_t limit = address + bytes;
    for (unsigned i = 0; i < offer->rangeCount && address < limit; ++i) {
        LSBForkWireRange r = offer->ranges[i];
        if (r.start > address) break;
        if (offer->allocatorMask & (UINT64_C(1) << i)) {
            if (r.start + r.size > address) address = r.start + r.size;
        }
    }
    return address >= limit ? 0 : EAGAIN;
}

static inline int trialSnapshotCovers(void *context, uint64_t address, uint64_t bytes) {
    const TrialOffer *offer = context;
    if (!address || !bytes || address > UINT64_MAX - bytes) return EINVAL;
    uint64_t limit = address + bytes;
    for (unsigned i = 0; i < offer->rangeCount && address < limit; ++i) {
        LSBForkWireRange r = offer->ranges[i];
        if (r.start > address) break;
        if (!(offer->skipMask & (UINT64_C(1) << i)) && r.start + r.size > address) address = r.start + r.size;
    }
    return address >= limit ? 0 : EAGAIN;
}

__attribute__((visibility("default"))) int LSBTrialReceiveForkFDs(int socket);
__attribute__((visibility("default"))) int LSBTrialResumeTerminalWitness(void);

// Trial shim entry used by the observer once its authenticated snapshot and import bindings are installed.
__attribute__((visibility("default"))) int LSBTrialAdoptBash(uintptr_t image, unsigned depth);

static inline int trialToken(audit_token_t *token) {
    mach_msg_type_number_t words = TASK_AUDIT_TOKEN_COUNT;
    return task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)token, &words) == KERN_SUCCESS && words == TASK_AUDIT_TOKEN_COUNT ? 0 : EIO;
}

static inline int trialSocket(int fd) {
    int enabled = 1;
    struct timeval timeout = {.tv_sec = 20};
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) ||
           setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ? errno : 0;
}

static inline int trialTransfer(int fd, void *buffer, size_t size, bool send) {
    char *cursor = buffer;
    while (size) {
        ssize_t n = send ? write(fd, cursor, size) : read(fd, cursor, size);
        if (n == -1 && errno == EINTR) continue;
        if (n <= 0) return n ? errno : EPIPE;
        cursor += n;
        size -= (size_t)n;
    }
    return 0;
}
