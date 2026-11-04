#pragma once

#include "ForkBashState.h"
#include <errno.h>
#include <signal.h>

enum { BashSignalCount = 32, BashOriginalSignals = 0xa7030, BashTrapList = 0xa71b8, BashSignalModes = 0xa8670,
       BashInitializeTraps = 0x36e48, BashRestoreTrapFreeReturn = 0x381b4, BashFreeStub = 0x89548, BashFreeSlot = 0x9c200 };

typedef struct {
    uintptr_t freeCaller, strings[BashSignalCount];
    unsigned count;
} BashTrapReleases;

// restore_original_signals (+0x380a8) walks signals 1..31. A trapped SIG_IGN goes directly
// to sigaction(SIG_IGN). A command trap restores original_signals[sig] and frees the string at +0x381b0.
// It only compares the string address; its bytes need not be present in the copied heap.
static inline int checkBashTraps(uintptr_t header, bool allowTraps, BashTrapReleases *releases) {
    uint32_t modes[BashSignalCount];
    uintptr_t traps[BashSignalCount], originals[BashSignalCount];
    if (header > UINTPTR_MAX - BashSignalModes - sizeof(modes) || !forkTrialRead(header + BashSignalModes, modes, sizeof(modes)) ||
        !forkTrialRead(header + BashTrapList, traps, sizeof(traps)) || !forkTrialRead(header + BashOriginalSignals, originals, sizeof(originals))) return EFAULT;
    if (releases) *releases = (BashTrapReleases){.freeCaller = header + BashRestoreTrapFreeReturn};
    enum { Trapped = 1, Special = 4, InProgress = 16 };
    bool frees = false;
    for (unsigned sig = 1; sig < BashSignalCount; ++sig) {
        if (!(modes[sig] & (Trapped | Special))) continue;
        // A disposable preflight child can exit before fork commits. It must never run an explicit parent CHLD trap.
        if (originals[sig] > 1 || ((modes[sig] & Trapped) && (!allowTraps || sig == SIGCHLD || (modes[sig] & InProgress)))) {
            fprintf(stderr, "fork trial unsupported Bash signal=%u mode=0x%x original=%p\n", sig, modes[sig], (void *)originals[sig]);
            return ENOTSUP;
        }
        if (!(modes[sig] & Trapped) || traps[sig] < 2 || traps[sig] == header + BashInitializeTraps) continue;
        frees = true;
        if (releases) releases->strings[releases->count++] = traps[sig];
    }
    if (frees) {
        const uint32_t stub[] = {0xd503201f, 0x580965b0, 0xd61f0200}; // ldr x16 references BashFreeSlot.
        uint32_t call, words[3];
        if (!forkTrialRead(header + BashRestoreTrapFreeReturn - 4, &call, sizeof(call)) || call != 0x940144e6 ||
            !forkTrialRead(header + BashFreeStub, words, sizeof(words)) || memcmp(words, stub, sizeof(stub))) return ENOEXEC;
    }
    return 0;
}

static inline bool consumeBashTrapRelease(BashTrapReleases *releases, uintptr_t caller, uintptr_t string) {
    if (caller != releases->freeCaller) return false;
    for (unsigned i = 0; i < releases->count; ++i) {
        if (releases->strings[i] != string) continue;
        releases->strings[i] = releases->strings[--releases->count];
        return true;
    }
    return false;
}
