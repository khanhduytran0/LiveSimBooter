// The observer checks every destination before the parent enters libc's fork handlers.
#include "ForkTrialWire.h"
#include "ForkBashImports.h"
#include "ForkBashChildImports.h"
#include "ForkBashEarlyHeap.h"
#include "ForkBashResume.h"
#include "ForkBashState.h"
#include <fcntl.h>
#include "../ProcessSupport/ForkImageGuard.h"
#include <limits.h>
#include <mach-o/ldsyms.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int proc_pidinfo(int pid, int flavor, uint64_t address, void *buffer, int size);
// XNU sys/proc_info.h, PROC_PIDREGIONPATH. libproc's proc_regionfilename discards the returned region bounds.
enum { TrialRegionPathFlavor = 31 };
typedef struct { uint64_t start, size; char path[PATH_MAX]; } TrialRegionPath;
_Static_assert(sizeof(TrialRegionPath) == 16 + PATH_MAX, "process region path ABI");
enum { TrialStackTag = VM_MEMORY_APPLICATION_SPECIFIC_15 };

static int validateOffer(const TrialOffer *offer, LSBForkWireRange *stack) {
    *stack = (LSBForkWireRange){};
    if (offer->depth > TrialMaxDepth || (offer->resume == TrialShell) != (offer->depth > 0) ||
        offer->version != TrialWireVersion || offer->resume > TrialShell || (offer->flags & ~TrialSlicedStack) ||
        (offer->flags && offer->resume < TrialExec) || (!offer->flags && offer->fullStackStart) || !offer->rangeCount || offer->rangeCount > LSB_MAX_FORK_RANGES ||
        (offer->rangeCount < 64 && (offer->skipMask >> offer->rangeCount))) return EPROTO;
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) != KERN_SUCCESS || count < TASK_VM_INFO_REV2_COUNT || vm.page_size <= 0) return EIO;
    uint64_t previousEnd = 0, bytes = 0;
    bool sampledWordCovered = !offer->bash.variablesAddress, heapCovered = offer->resume != TrialShell;
    if (!heapCovered && offer->mainHeader > UINT64_MAX - BashEarlyHeapOffset - BashEarlyHeapBytes) return EPROTO;
    for (unsigned i = 0; i < offer->rangeCount; ++i) {
        LSBForkWireRange r = offer->ranges[i];
        if (!r.size || r.start < previousEnd || r.size > LSB_MAX_FORK_BYTES - bytes || ((r.start | r.size) & (vm.page_size - 1))) return EPROTO;
        if (r.start < vm.min_address || r.start >= vm.max_address || r.size > vm.max_address - r.start) return ERANGE;
        previousEnd = r.start + r.size;
        bytes += r.size;
        if (offer->skipMask & (UINT64_C(1) << i)) continue;
        if (!heapCovered && r.start == offer->mainHeader + BashEarlyHeapOffset && r.size == BashEarlyHeapBytes) heapCovered = true;
        if (offer->caller.sp >= r.start && offer->caller.sp < previousEnd) *stack = r;
        if (offer->bash.variablesAddress >= r.start && offer->bash.variablesAddress <= previousEnd - sizeof(uint64_t)) sampledWordCovered = true;
    }
    if (!stack->size || !sampledWordCovered || !heapCovered) return EPROTO;
    uint64_t page = (uint64_t)vm.page_size;
    if (offer->flags & TrialSlicedStack) {
        if ((offer->fullStackStart & (page - 1)) || offer->fullStackStart > stack->start || offer->fullStackStart < vm.min_address ||
            trialStackSliceStart(offer->fullStackStart, offer->caller.sp, page) != stack->start) return EPROTO;
    }
    if (trialNeedsStackGuard(offer)) {
        if (stack->start - vm.min_address < page) return EPROTO;
        uint64_t guard = stack->start - page;
        for (unsigned i = 0; i < offer->rangeCount; ++i) {
            LSBForkWireRange r = offer->ranges[i];
            if (guard >= r.start && guard - r.start < r.size) return EPROTO;
        }
    }
    return 0;
}

// region_recurse returns the containing or following leaf. One query therefore finds the first overlap even across a hole.
static int checkDestinations(const TrialOffer *offer, pid_t nativePID, bool imageGuard, const char *phase) {
    int firstError = 0;
    for (unsigned i = 0; i < offer->rangeCount; ++i) {
        LSBForkWireRange r = offer->ranges[i];
        uint64_t end = r.start + r.size;
        if (offer->skipMask & (UINT64_C(1) << i)) continue;
        if (imageGuard && r.start >= offer->mainHeader && end <= (uintptr_t)&_mh_execute_header) continue;
        mach_vm_address_t start = r.start;
        mach_vm_size_t size = 0;
        natural_t depth = UINT32_MAX;
        vm_region_submap_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &start, &size, &depth, (vm_region_recurse_info_t)&info, &count);
        if (kr == KERN_INVALID_ADDRESS) continue;
        if (kr != KERN_SUCCESS || count < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info.is_submap || !size || size > UINT64_MAX - start || start + size <= r.start) {
            forkTrialTrace("fork trial phase=%s range[%u]=0x%llx..0x%llx query_kr=%d words=%u\n", phase, i, r.start, end, kr, count);
            return EIO;
        }
        if (start >= end) continue;
        firstError = EADDRINUSE;
        if (!forkTrialTraceEnabled()) continue;
        uint64_t lo = start > r.start ? start : r.start, hi = start + size < end ? start + size : end;
        TrialRegionPath file = {};
        int n = proc_pidinfo(nativePID, TrialRegionPathFlavor, lo, &file, sizeof(file));
        file.path[sizeof(file.path) - 1] = 0;
        bool covers = n == sizeof(file) && file.start <= lo && hi - file.start <= file.size && file.path[0];
        pthread_t self = pthread_self();
        uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
        size_t stackSize = pthread_get_stacksize_np(self);
        uintptr_t bottom = stackSize <= top ? top - stackSize : top;
        bool stackOverlap = bottom < top && r.start < top && end > bottom;
        bool leafOnStack = bottom < top && lo < top && hi > bottom;
        bool adjacentGuard = bottom < top && start + size == bottom && info.protection == VM_PROT_NONE;
        forkTrialTrace("fork trial phase=%s range[%u]=0x%llx..0x%llx overlap=0x%llx..0x%llx child_leaf=0x%llx..0x%llx "
                "tag=%u prot=%d/%d pthread_stack=0x%lx..0x%lx offer_hits_stack=%d overlap_hits_stack=%d adjacent_noaccess=%d path=%s\n",
                phase, i, r.start, end, lo, hi, start, start + size, info.user_tag, info.protection, info.max_protection,
                bottom, top, stackOverlap, leafOnStack, adjacentGuard, covers ? file.path : "(unavailable)");
    }
    return firstError;
}

static int reserveStack(LSBForkWireRange stack, bool guardStack) {
    // One non-overwriting mapping covers both the copied stack and its permanent lower guard. No read-then-replace race.
    uint64_t guardSize = guardStack ? vm_page_size : 0;
    mach_vm_address_t address = stack.start - guardSize;
    kern_return_t kr = mach_vm_map(mach_task_self(), &address, stack.size + guardSize, 0, VM_FLAGS_FIXED | VM_MAKE_TAG((unsigned)TrialStackTag),
                                   MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
    if (kr == KERN_NO_SPACE) return EADDRINUSE;
    return kr == KERN_SUCCESS && address == stack.start - guardSize ? 0 : EIO;
}

static int releaseStackReservation(LSBForkWireRange stack, bool guardStack) {
    uint64_t guardSize = guardStack ? vm_page_size : 0;
    int error = LSBCheckForkGuard(stack.start - guardSize, stack.start + stack.size, TrialStackTag);
    if (error) return error;
    return mach_vm_deallocate(mach_task_self(), stack.start, stack.size) == KERN_SUCCESS ? 0 : EIO;
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    char *end;
    long value = strtol(argv[1], &end, 10);
    if (!*argv[1] || *end || value < 0 || value > INT_MAX) return 1;
    int controlFD = (int)value;
    if (fcntl(controlFD, F_SETFD, FD_CLOEXEC) == -1) return 1;
    TrialReady ready = {.logicalPID = getpid(), .version = TrialWireVersion};
    int error = trialToken(&ready.audit);
    if (!error) error = trialSocket(controlFD);
    if (!error) error = trialTransfer(controlFD, &ready, sizeof(ready), true);
    TrialOffer offer = {};
    if (!error) error = trialTransfer(controlFD, &offer, sizeof(offer), false);
    if (error) return 1;
    LSBForkWireRange stack = {};
    int32_t preflight = validateOffer(&offer, &stack);
    const char *native = getenv("LSB_NATIVE_SUPPORT");
    bool guarded = native && !strcmp(native, "1");
    bool sliced = (offer.flags & TrialSlicedStack) != 0;
    if (!preflight && (sliced || offer.resume == TrialShell) && !guarded) preflight = ENOTSUP;
    for (unsigned fd = 0; !preflight && fd < LSB_MAX_FDS; ++fd) {
        if (!(offer.closeOnExec[fd / 64] & (UINT64_C(1) << (fd % 64)))) continue;
        if (offer.resume != TrialShell || fd == (unsigned)controlFD) preflight = EPROTO;
        else if (fcntl((int)fd, F_SETFD, FD_CLOEXEC) < 0) preflight = errno;
    }
    if (!preflight && offer.resume != TrialShell && offer.terminal.target) preflight = EPROTO;
    if (!preflight) preflight = trialRestoreTerminal(offer.terminal, controlFD, ttyname_r);
    uintptr_t bootstrap = (uintptr_t)&_mh_execute_header;
    if (guarded && !preflight) {
        if (bootstrap < LSB_GUEST_MAIN_IMAGE_CAPACITY || offer.mainHeader != bootstrap - LSB_GUEST_MAIN_IMAGE_CAPACITY) preflight = EPROTO;
        else if (LSBCheckForkGuard(offer.mainHeader, bootstrap, LSB_FORK_IMAGE_GUARD_TAG)) preflight = EPROTO;
        if (!preflight) preflight = checkDestinations(&offer, ready.audit.val[5], true, "preflight");
        if (!preflight) preflight = reserveStack(stack, trialNeedsStackGuard(&offer));
        forkTrialTrace("fork trial observer=%d preflight_errno=%d stack=0x%llx..0x%llx sliced=%d\n",
                ready.logicalPID, preflight, stack.start, stack.start + stack.size, sliced);
    }
    if (trialTransfer(controlFD, &preflight, sizeof(preflight), true) || preflight) return 1;
    uint8_t checkpoint = 0;
    if (trialTransfer(controlFD, &checkpoint, sizeof(checkpoint), false) || checkpoint != LSB_FORK_CHECKPOINT) return 1;
    if (guarded) {
        error = releaseStackReservation(stack, trialNeedsStackGuard(&offer));
        if (!error) error = LSBReleaseForkImageGuard(bootstrap, offer.mainHeader);
    }
    // The broker checks vacancy again while both tasks are suspended and installs without overwriting mappings.
    int32_t accepted = error ? error : LSBTransportAcceptFork(&offer.audit);
    if (!accepted && !offer.resume) reportForkBashState(offer.mainHeader, offer.bash.parentGetpid, offer.bash.variablesAddress, offer.bash.expectedFirstWord, ready.logicalPID);
    if (!error && accepted == EADDRINUSE) checkDestinations(&offer, ready.audit.val[5], false, "after-rejection");
    if (guarded) forkTrialTrace("fork trial bootstrap=%p inherited_main=0x%llx guard_release_errno=%d\n",
                         (const void *)&_mh_execute_header, offer.mainHeader, error);
    int32_t result = accepted ? accepted : ENOTSUP;
    if (!accepted && offer.resume && guarded) {
        if (offer.resume == TrialShell) {
            result = rebindEarlyBashImports(offer.mainHeader);
            if (!result) result = LSBTrialAdoptBash(offer.mainHeader, (unsigned)offer.depth);
        } else result = rebindBashImports(offer.mainHeader, offer.resume == TrialExec);
        if (!result) result = LSBTrialResumeBash(offer.mainHeader, &offer.caller, stack.start, stack.size, controlFD, offer.resume == TrialDF, offer.resume == TrialShell);
    }
    forkTrialTrace("fork trial syscall observer=%d snapshot_errno=%d fork_errno=%d complete=0\n", ready.logicalPID, accepted, result);
    return trialTransfer(controlFD, &result, sizeof(result), true) != 0;
}
