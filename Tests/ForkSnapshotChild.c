// The observer checks every destination before the parent enters libc's fork handlers.
#include "ForkTrialWire.h"
#include "ForkLibcCheckpoint.h"
#include "ForkGuestResume.h"
#include "ForkSignalState.h"
#include "ForkDescriptorState.h"
#include "../ProcessSupport/ForkDescriptorSlots.h"
#include "../ProcessSupport/ForkRendezvous.h"
#include "ForkAllocator.h"
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
static uint64_t unavailableAddress;

static int validateOffer(const TrialOffer *offer, LSBForkWireRange *stack) {
    *stack = (LSBForkWireRange){};
    if (offer->depth > TrialMaxDepth || (offer->resume == TrialShell) != (offer->depth > 0) ||
        offer->version != TrialWireVersion || offer->resume > TrialShell || (offer->flags & ~(TrialSlicedStack | TrialRestoreAllocator | TrialResumeGuest | TrialVfork)) ||
        ((offer->flags & TrialVfork) && !(offer->flags & TrialResumeGuest)) ||
        ((offer->flags & TrialSlicedStack) && offer->resume < TrialExec) || (!(offer->flags & TrialSlicedStack) && offer->fullStackStart) ||
        !offer->rangeCount || offer->rangeCount > LSB_MAX_FORK_RANGES || (offer->allocatorMask & offer->skipMask) ||
        (offer->rangeCount < 64 && ((offer->skipMask | offer->allocatorMask) >> offer->rangeCount))) return EPROTO;
    if ((offer->flags & TrialResumeGuest) && (!(offer->flags & TrialRestoreAllocator) || !offer->argc || offer->argc > LSB_MAX_VECTOR ||
        !offer->argv || !offer->envp || offer->reserved)) return EPROTO;
    if (offer->flags & TrialRestoreAllocator) {
        if (offer->resume != TrialSnapshot || !offer->allocatorMask || !offer->witness || !offer->allocator.count ||
            offer->allocator.count > ForkAllocatorMaxZones) return EPROTO;
    } else if (offer->allocatorMask || offer->allocator.count || offer->witness) return EPROTO;
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) != KERN_SUCCESS || count < TASK_VM_INFO_REV2_COUNT || vm.page_size <= 0) return EIO;
    uint64_t previousEnd = 0, bytes = 0;
    bool sampledWordCovered = !offer->bash.variablesAddress, heapCovered = offer->resume != TrialShell;
    if (!heapCovered && offer->mainHeader > UINT64_MAX - BashEarlyHeapOffset - BashEarlyHeapBytes) return EPROTO;
    for (unsigned i = 0; i < offer->rangeCount; ++i) {
        LSBForkWireRange r = offer->ranges[i];
        if (!r.start || !r.size || r.start < previousEnd || r.size > UINT64_MAX - r.start || r.size > LSB_MAX_FORK_BYTES - bytes ||
            ((r.start | r.size) & (vm.page_size - 1))) return EPROTO;
        previousEnd = r.start + r.size;
        bytes += r.size;
        if (offer->skipMask & (UINT64_C(1) << i)) continue;
        if (previousEnd > vm.max_address) return ERANGE;
        // The native executable's randomized placement sets this process's VA floor. A fresh observer can have a lower floor.
        if (r.start < vm.min_address) { unavailableAddress = r.start; return EADDRINUSE; }
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
        if (!firstError) unavailableAddress = r.start > start ? r.start : start;
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

static int acceptWithoutWorkers(const audit_token_t *self, const audit_token_t *broker) {
    struct sockaddr_un address;
    int error = LSBForkSocketAddress(getenv("DYLD_ROOT_PATH"), self, &address);
    if (error) return error;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return errno;
    error = trialSocket(fd);
    if (!error && connect(fd, (struct sockaddr *)&address, address.sun_len)) error = errno;
    if (!error) error = LSBForkCheckSocketPeer(fd, broker);
    int32_t result = 0;
    if (!error) error = trialTransfer(fd, &result, sizeof(result), false);
    close(fd);
    return error ? error : result < 0 || result > ELAST ? EPROTO : result;
}

static int runPreparedChildHandlers(uintptr_t pc) {
    TrialLibcForkHandlers handlers;
    int error = trialResolveLibcForkHandlers(pc, false, &handlers);
    if (error) return error;
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count)) return EIO;
    for (unsigned i = 0; i < count; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(*threads));
    forkTrialTrace("fork trial child branch observer_threads=%u\n", count);
    if (count != 1 || !pthread_main_np()) return EBUSY;
    handlers.prepare(0);
    return trialRunLibcChildBranch(pc);
}

static int prepareGuestResume(TrialOffer *offer, uintptr_t libcPC, ForkPthreadStage *registry, TrialGuardPage *guard, const ForkCachePacket *packet) {
    int error = trialCheckVector(offer, offer->argv, (int)offer->argc);
    if (!error) error = trialCheckVector(offer, offer->envp, -1);
    forkTrialTrace("fork trial startup vectors argc=%u argv=%#llx envp=%#llx errno=%d\n", offer->argc, offer->argv, offer->envp, error);
    if (!error) error = trialPrepareGuard(offer->guard, libcPC, guard);
    if (!error) error = forkPthreadStage(&offer->pthreads, trialSnapshotCovers, offer, registry, packet);
    forkTrialTrace("fork trial atfork parent=%u prefix=%u staged=%u errno=%d\n", offer->pthreads.count, registry->prefix, registry->count, error);
    return error;
}

static int verifyInheritedAllocation(TrialOffer *offer) {
    int error = trialAllocatorCovers(offer, offer->witness, TrialWitnessSize);
    unsigned char *original = malloc(64);
    if (!original) return ENOMEM;
    memset(original, 0x6b, 64);
    if (!error) error = forkAllocatorPromote(&offer->allocator, trialAllocatorCovers, offer);
    if (error) return error;
    unsigned char *block = (void *)(uintptr_t)offer->witness;
    malloc_zone_t *owner = malloc_zone_from_ptr(block);
    if (owner != malloc_default_zone() || malloc_size(block) < TrialWitnessSize || malloc_size(original) < 64 ||
        malloc_zone_from_ptr(original) == owner || original[0] != 0x6b) return EINVAL;
    free(original);
    for (unsigned i = 0; i < TrialWitnessSize; ++i) if (block[i] != (unsigned char)(i + 1)) return EIO;
    memset(block, 0x5a, TrialWitnessSize);
    unsigned char *resized = realloc(block, 4096);
    if (!resized) return ENOMEM;
    bool intact = true;
    for (unsigned i = 0; i < TrialWitnessSize; ++i) if (resized[i] != 0x5a) intact = false;
    free(resized);
    return intact ? 0 : EIO;
}

int main(int argc, char **argv) {
    bool reserved = argc == 3 && !strcmp(argv[2], LSB_FORK_RECEIVE_ARG);
    if (argc != 2 && !reserved) return 1;
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
    uint32_t phase = 1;
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
        if (!preflight) { phase = 2; preflight = checkDestinations(&offer, ready.audit.val[5], true, "preflight"); }
        if (!preflight) { phase = 3; unavailableAddress = stack.start; preflight = reserveStack(stack, trialNeedsStackGuard(&offer)); }
        forkTrialTrace("fork trial observer=%d preflight_errno=%d stack=0x%llx..0x%llx sliced=%d\n",
                ready.logicalPID, preflight, stack.start, stack.start + stack.size, sliced);
    }
    if (!preflight && (offer.flags & TrialResumeGuest)) {
        phase = 4;
        preflight = trialAliasSupportImages(getenv("DYLD_ROOT_PATH"), &offer.support, &unavailableAddress);
        forkTrialTrace("fork trial bootstrap aliases=%u errno=%d\n", offer.support.count, preflight);
    }
    uint32_t prefix = 0;
    bool isVfork = (offer.flags & TrialVfork) != 0;
    if (!preflight && (offer.flags & TrialResumeGuest)) {
        phase = 5;
        preflight = forkPthreadPrefix(&offer.pthreads, &prefix);
        if (!preflight && isVfork) {
            ForkCacheCheckpointPlan plan;
            preflight = forkCachePlan(&offer.pthreads, prefix, &plan);
            // vfork skips the registered prepare handlers that normally protect these owners' mutable cache data.
            if (!preflight && plan.count) preflight = ENOTSUP;
        }
    }
    TrialPreflight report = {.error = preflight, .phase = phase, .address = preflight && phase != 5 ? unavailableAddress : 0, .pthreadPrefix = prefix};
    if (trialTransfer(controlFD, &report, sizeof(report), true) || preflight) return 1;
    audit_token_t broker = {{0}};
    if ((offer.flags & TrialRestoreAllocator) && trialTransfer(controlFD, &broker, sizeof(broker), false)) return 1;
    uint8_t checkpoint = 0;
    if (trialTransfer(controlFD, &checkpoint, sizeof(checkpoint), false) || checkpoint != LSB_FORK_CHECKPOINT) return 1;
    LSBForkContext libc = {};
    if (offer.resume == TrialSnapshot && trialTransfer(controlFD, &libc, sizeof(libc), false)) return 1;
    ForkSignalState signals = {};
    ForkCachePacket cachePacket = {};
    if (offer.flags & TrialResumeGuest) {
        const char *text = getenv(LSB_FORK_ANCHOR_ENV);
        int anchor, length = 0;
        unsigned device;
        unsigned long long inode;
        if (!reserved || !text || sscanf(text, "%d/%x/%llx%n", &anchor, &device, &inode, &length) != 3 || text[length] ||
            anchor < LSB_MAX_FDS || anchor == controlFD) error = EPROTO;
        if (!error) error = forkPthreadSingleMain();
        if (!error) error = LSBForkReleaseDescriptorSlots(anchor, (dev_t)device, (ino_t)inode);
        if (!error) error = LSBTrialReceiveForkFDs(controlFD);
        if (!error) error = forkDescriptorBytes(controlFD, &signals, sizeof(signals), false);
        if (!error) error = forkDescriptorBytes(controlFD, &cachePacket.plan, sizeof(cachePacket.plan), false);
        if (!error && (cachePacket.plan.used > sizeof(cachePacket.bytes) || cachePacket.plan.count > ForkCacheMaxSpans)) error = EPROTO;
        if (!error) error = forkDescriptorBytes(controlFD, cachePacket.bytes, cachePacket.plan.used, false);
    }
    if (!error && guarded) {
        error = releaseStackReservation(stack, trialNeedsStackGuard(&offer));
        if (!error) error = LSBReleaseForkImageGuard(bootstrap, offer.mainHeader);
    }
    // The broker checks vacancy again while both tasks are suspended and installs without overwriting mappings.
    int32_t accepted = error;
    if (!accepted) accepted = offer.flags & TrialRestoreAllocator ? acceptWithoutWorkers(&ready.audit, &broker) : LSBTransportAcceptFork(&offer.audit);
    if (!accepted && !offer.resume && !(offer.flags & TrialRestoreAllocator))
        reportForkBashState(offer.mainHeader, offer.bash.parentGetpid, offer.bash.variablesAddress, offer.bash.expectedFirstWord, ready.logicalPID);
    if (!error && accepted == EADDRINUSE) checkDestinations(&offer, ready.audit.val[5], false, "after-rejection");
    if (guarded) forkTrialTrace("fork trial bootstrap=%p inherited_main=0x%llx guard_release_errno=%d\n",
                         (const void *)&_mh_execute_header, offer.mainHeader, error);
    int32_t result = accepted ? accepted : ENOTSUP;
    if (!accepted && offer.resume == TrialSnapshot) {
        int frameError = trialValidateLibcCheckpoint(&libc, &offer.caller, stack, isVfork);
        forkTrialTrace("fork trial libc_pc=%#llx libc_sp=%#llx frame_errno=%d\n", libc.pc, libc.sp, frameError);
        if (frameError) result = frameError;
        else if (offer.flags & TrialRestoreAllocator) {
            int allocatorError = verifyInheritedAllocation(&offer);
            forkTrialTrace("fork trial inherited_zones=%u realloc_free_errno=%d\n", offer.allocator.count, allocatorError);
            if (!allocatorError && (offer.flags & TrialResumeGuest)) {
                TrialGuardPage guard = {};
                ForkPthreadStage registry = {};
                int resumeError = prepareGuestResume(&offer, libc.pc, &registry, &guard, &cachePacket);
                TrialLibcForkHandlers handlers;
                if (!resumeError) resumeError = trialResolveLibcForkHandlers(libc.pc, isVfork, &handlers);
                if (!resumeError) resumeError = LSBTrialRestoreForkSignals(&signals, &offer, trialSnapshotCovers);
                if (!resumeError) resumeError = trialRestoreProgramVars(&offer);
                forkTrialTrace("fork trial application state ready errno=%d\n", resumeError);
                if (!resumeError) {
                    handlers.prepare(isVfork);
                    resumeError = forkCacheApplyPrepared(&registry.cache, &cachePacket);
                    if (!resumeError) resumeError = forkPthreadFinish(&registry);
                    if (!resumeError) {
                        // A child callback may wait for its parent. Publish the successful fork before dispatching it.
                        int32_t readyToRun = 0;
                        if (forkDescriptorBytes(controlFD, &readyToRun, sizeof(readyToRun), true)) _exit(126);
                        close(controlFD);
                        if (trialRunLibcChildBranch(libc.pc)) _exit(126);
                        trialReturnToGuest(&offer, guard);
                    }
                    handlers.parent(isVfork);
                }
                result = resumeError;
            } else if (!allocatorError) {
                int handlerError = runPreparedChildHandlers(libc.pc);
                forkTrialTrace("fork trial prepared observer libc_child_errno=%d\n", handlerError);
                if (handlerError) result = handlerError;
            } else result = allocatorError;
        }
    }
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
