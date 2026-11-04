// The observer was spawned, which resets caught dispositions. Restore the caller's mask and actions before child callbacks run.
#pragma once
#include <errno.h>
#include <signal.h>
#include <mach/mach.h>
#include <string.h>
#include <stdint.h>

typedef struct { uint64_t handler; sigset_t mask; int32_t flags; } ForkSignalAction;
typedef struct { ForkSignalAction actions[NSIG]; sigset_t mask; uint32_t reserved; } ForkSignalState;
_Static_assert(sizeof(ForkSignalAction) == 16 && sizeof(ForkSignalState) == NSIG * 16 + 8, "Darwin arm64 signal state");

// The attached terminal represents default stop actions with a process-local handler. Use the guest's public dispositions
// on both sides; an observer must keep its own handler when inheriting SIG_DFL. Pin imports before donor prepare takes dyld locks.
#if defined(LSB_GUEST_RUNTIME)
#include "../../libLiveSimSpawn/ChildWait.h"
static __typeof__(&sigaction) const volatile forkSignalDisposition = LSBSigaction;
#else
static __typeof__(&sigaction) const volatile forkSignalDisposition = sigaction;
#endif
static __typeof__(&sigprocmask) const volatile forkSignalMask = sigprocmask;

__attribute__((visibility("default"))) int LSBTrialRestoreForkSignals(const ForkSignalState *, void *, int (*)(void *, uint64_t, uint64_t));

static inline int forkReadSignalState(ForkSignalState *state) {
    *state = (ForkSignalState){};
    if (forkSignalMask(0, NULL, &state->mask)) return errno;
    for (int i = 1; i < NSIG; ++i) {
        if (i == SIGKILL || i == SIGSTOP) continue;
        struct sigaction action = {};
        if (forkSignalDisposition(i, NULL, &action)) return errno;
        state->actions[i] = (ForkSignalAction){(uintptr_t)action.sa_handler, action.sa_mask, action.sa_flags};
    }
    return 0;
}

static inline int forkSetSignal(int number, const ForkSignalAction *entry) {
    struct sigaction action = {.sa_mask = entry->mask, .sa_flags = entry->flags};
    action.sa_handler = (void (*)(int))(uintptr_t)entry->handler;
    return forkSignalDisposition(number, &action, NULL) ? errno : 0;
}

static inline int forkRestoreSignals(const ForkSignalState *state, void *context, int (*covers)(void *, uint64_t, uint64_t)) {
    ForkSignalAction empty = {};
    if (!covers || state->reserved || memcmp(&state->actions[0], &empty, sizeof(empty)) || memcmp(&state->actions[SIGKILL], &empty, sizeof(empty)) ||
        memcmp(&state->actions[SIGSTOP], &empty, sizeof(empty))) return EPROTO;
    ForkSignalState local;
    int error = forkReadSignalState(&local);
    if (error) return error;
    for (int i = 1; i < NSIG; ++i) {
        uint64_t handler = state->actions[i].handler;
        if (handler == local.actions[i].handler || handler == (uintptr_t)SIG_DFL || handler == (uintptr_t)SIG_IGN) continue;
        if ((handler & 3) || covers(context, handler, sizeof(uint32_t))) return ENOTSUP;
        vm_address_t address = handler;
        vm_size_t size = 0;
        natural_t depth = UINT32_MAX;
        vm_region_submap_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t)&info, &count) ||
            count < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info.is_submap || address > handler || size < sizeof(uint32_t) ||
            handler - address > size - sizeof(uint32_t) || !(info.protection & VM_PROT_EXECUTE)) return EACCES;
    }
    // In the single-thread observer, catchable asynchronous signals must not observe a partially installed table.
    sigset_t all;
    sigfillset(&all);
    if (forkSignalMask(SIG_SETMASK, &all, NULL)) return errno;
    int next = 1;
    for (; next < NSIG; ++next) {
        if (!memcmp(&state->actions[next], &local.actions[next], sizeof(empty))) continue;
        error = forkSetSignal(next, &state->actions[next]);
        if (error) break;
    }
    if (!error && forkSignalMask(SIG_SETMASK, &state->mask, NULL)) error = errno;
    if (error) {
        while (--next > 0) {
            if (memcmp(&state->actions[next], &local.actions[next], sizeof(empty)) && forkSetSignal(next, &local.actions[next])) error = EIO;
        }
        if (forkSignalMask(SIG_SETMASK, &local.mask, NULL)) error = EIO;
    }
    return error;
}
