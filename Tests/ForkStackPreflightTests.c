// The recorded Bash failure had a vacant SP and an occupied page elsewhere inside its 8 MiB stack.
#define main forkObserverMain
#include "ForkSnapshotChild.c"
#undef main
#include <assert.h>

int LSBTrialReceiveForkFDs(int fd) { (void)fd; return ENOTSUP; }
uintptr_t LSBTrialLibcForkReturnPC(bool isVfork) { (void)isVfork; abort(); } // Only used after preflight.
int LSBTrialResumeTerminalWitness(void) { return 0; }
int LSBTrialRestoreForkSignals(const ForkSignalState *s, void *c, int (*covers)(void *, uint64_t, uint64_t)) { return forkRestoreSignals(s, c, covers); }

int LSBTrialAdoptBash(uintptr_t image, unsigned depth) { (void)image; (void)depth; return ENOTSUP; }

int LSBTransportAcceptFork(const audit_token_t *parent) { (void)parent; return ENOTSUP; }

int LSBTrialResumeBash(uintptr_t header, const LSBForkContext *caller, uintptr_t stackStart, size_t stackSize, int fd, bool dfOnly, bool allowEarly) {
    (void)header; (void)caller; (void)stackStart; (void)stackSize; (void)fd; (void)dfOnly; (void)allowEarly;
    return ENOTSUP;
}

static void testStackSlice(void) {
    uint64_t page = vm_page_size, size = TrialStackHeadroom + 6 * page;
    mach_vm_address_t base = 0;
    assert(!mach_vm_map(mach_task_self(), &base, size, 0, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE,
                        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_COPY));
    assert(!mach_vm_deallocate(mach_task_self(), base + page, size - page));
    volatile uint64_t *otherStack = (void *)(uintptr_t)base;
    *otherStack = 91;
    TrialOffer offer = {.version = TrialWireVersion, .resume = TrialExec, .caller.sp = base + size - 2 * page + 48,
                        .rangeCount = 1, .ranges = {{base, size}}};
    LSBForkWireRange stack;
    assert(!validateOffer(&offer, &stack) && reserveStack(stack, false) == EADDRINUSE);
    uint64_t bottom = trialStackSliceStart(base, offer.caller.sp, page);
    assert(bottom == base + 4 * page && trialStackSliceStart(base, base + page + 48, page) == base);
    offer.flags = TrialSlicedStack;
    offer.fullStackStart = base;
    offer.rangeCount = 2;
    offer.ranges[0] = (LSBForkWireRange){base + page, page}; // E.g. an environment string in an older stack page.
    offer.ranges[1] = (LSBForkWireRange){bottom, size - (bottom - base)};
    assert(!validateOffer(&offer, &stack) && stack.start == bottom && !checkDestinations(&offer, getpid(), false, "slice"));
    offer.ranges[0].start = bottom - page;
    assert(validateOffer(&offer, &stack) == EPROTO); // No offered data, skipped or otherwise, may cover the guard.
    offer.ranges[0].start = base + page;
    offer.resume = TrialDF;
    assert(validateOffer(&offer, &stack) == EPROTO);
    offer.resume = TrialExec;
    offer.flags |= 2;
    assert(validateOffer(&offer, &stack) == EPROTO);
    offer.flags = TrialSlicedStack;
    assert(!validateOffer(&offer, &stack));

    // A substitution child can inherit a reduced stack and offer that entire leaf to its own child.
    // The second copy still requires a guard, even though this offer did not itself slice the stack.
    uint64_t heap = base + size + 2 * page;
    TrialOffer nested = {.version = TrialWireVersion, .resume = TrialShell, .depth = 2, .mainHeader = heap - BashEarlyHeapOffset,
                         .caller.sp = offer.caller.sp, .rangeCount = 3,
                         .ranges = {{base + page, page}, {bottom, size - (bottom - base)}, {heap, BashEarlyHeapBytes}}};
    assert(trialNeedsStackGuard(&nested) && !validateOffer(&nested, &stack) && stack.start == bottom);
    nested.ranges[0].start = bottom - page;
    assert(validateOffer(&nested, &stack) == EPROTO);
    nested.ranges[0].start = base + page;
    nested.depth = 0;
    assert(validateOffer(&nested, &stack) == EPROTO);
    nested.depth = TrialMaxDepth + 1;
    assert(validateOffer(&nested, &stack) == EPROTO);
    assert(!validateOffer(&offer, &stack));

    mach_vm_address_t guard = bottom - page;
    assert(!mach_vm_map(mach_task_self(), &guard, page, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0, FALSE,
                        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_COPY));
    *(volatile int *)(uintptr_t)guard = 17;
    assert(reserveStack(stack, true) == EADDRINUSE && *(volatile int *)(uintptr_t)guard == 17);
    assert(!mach_vm_deallocate(mach_task_self(), guard, page));
    assert(!reserveStack(stack, true) && !releaseStackReservation(stack, true));
    assert(!LSBCheckForkGuard(guard, bottom, TrialStackTag)); // Guard retains VM_PROT_NONE as current and maximum.
    assert(!checkDestinations(&offer, getpid(), false, "slice-before-copy") && *otherStack == 91);
    assert(!mach_vm_deallocate(mach_task_self(), guard, page));
    assert(!mach_vm_deallocate(mach_task_self(), base, page));
}

static void testAddressLimits(void) {
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    assert(!task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) && count >= TASK_VM_INFO_REV2_COUNT);
    uint64_t page = vm.page_size, above = (vm.max_address / page + 1) * page;
    uint64_t sp = ((uintptr_t)&vm) & ~(page - 1);
    TrialOffer offer = {.version = TrialWireVersion, .caller.sp = sp, .rangeCount = 2, .ranges = {{sp, page}, {above, page}}};
    LSBForkWireRange stack;
    assert(validateOffer(&offer, &stack) == ERANGE);
    offer.skipMask = 2;
    assert(!validateOffer(&offer, &stack)); // VM_INHERIT_NONE consumes no child VA.
    offer.ranges[1].start = UINT64_MAX - page + 1;
    assert(validateOffer(&offer, &stack) == EPROTO);
    if (vm.min_address > page) {
        offer.ranges[0] = (LSBForkWireRange){page, page};
        offer.ranges[1] = (LSBForkWireRange){sp, page};
        offer.skipMask = 0;
        assert(validateOffer(&offer, &stack) == EADDRINUSE);
        offer.skipMask = 1;
        assert(!validateOffer(&offer, &stack));
    }
}

static void testVforkOffer(void) {
    LSBForkWireRange stack;
    uintptr_t base = (uintptr_t)&stack & ~(uintptr_t)(vm_page_size - 1);
    TrialOffer offer = {.version = TrialWireVersion, .resume = TrialSnapshot, .flags = TrialVfork | TrialResumeGuest | TrialRestoreAllocator,
                        .caller.sp = base, .rangeCount = 1, .ranges = {{base, vm_page_size}}, .allocatorMask = 1, .allocator.count = 1,
                        .witness = base, .argc = 1, .argv = base, .envp = base};
    assert(!validateOffer(&offer, &stack));
    offer.resume = TrialDF;
    assert(validateOffer(&offer, &stack) == EPROTO);
    offer.resume = TrialSnapshot;
    offer.flags = TrialVfork | TrialRestoreAllocator;
    assert(validateOffer(&offer, &stack) == EPROTO);
    offer.flags = TrialVfork | TrialResumeGuest;
    assert(validateOffer(&offer, &stack) == EPROTO);
}

int main(void) {
    mach_vm_size_t page = vm_page_size;
    mach_vm_address_t base = 0;
    assert(!mach_vm_map(mach_task_self(), &base, 3 * page, 0, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE,
                        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_COPY));
    assert(!mach_vm_deallocate(mach_task_self(), base, 2 * page));
    TrialOffer offer = {.version = TrialWireVersion, .rangeCount = 1, .caller.sp = base + page, .ranges = {{base, 3 * page}}};
    LSBForkWireRange stack;
    assert(!validateOffer(&offer, &stack));
    assert(checkDestinations(&offer, getpid(), false, "occupied-tail") == EADDRINUSE);
    assert(reserveStack(stack, false) == EADDRINUSE);
    *(volatile int *)(base + 2 * page) = 42; // The failed reservation preserved the existing page.
    assert(!mach_vm_deallocate(mach_task_self(), base + 2 * page, page));
    assert(!checkDestinations(&offer, getpid(), false, "vacant"));
    offer.skipMask = 1;
    assert(validateOffer(&offer, &stack) == EPROTO);
    offer.skipMask = 0;
    offer.bash.variablesAddress = base + 3 * page - 4;
    assert(validateOffer(&offer, &stack) == EPROTO); // Only half the sample would be inherited.
    offer.bash.variablesAddress -= 4;
    assert(!validateOffer(&offer, &stack) && stack.start == base && stack.size == 3 * page);
    assert(!reserveStack(stack, false));
    assert(!LSBCheckForkGuard(base, base + stack.size, TrialStackTag));
    assert(!mach_vm_deallocate(mach_task_self(), base, stack.size));
    testStackSlice();
    testAddressLimits();
    testVforkOffer();
    puts("Fork stack preflight and guarded slice passed");
    return 0;
}
