// Test ownership changes against isolated storage; never mutate the test process's actual pthread or native cache.
#define LSB_NATIVE_SUPPORT_STATE_TESTING
#include "../LiveSimBooter/NativeSupportState.cpp"

#include <cstdlib>

namespace { uintptr_t testTSD; }
static unsigned checks, locks, unlocks, kernelCalls;
static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void lock(void *address, uint32_t options) {
    check(address == sState.keyLock && options == 0x50000, "use the native key lock and its original options");
    ++locks;
}
static void unlock(void *address) { check(address == sState.keyLock, "unlock the same key lock"); ++unlocks; }
static void destructor(void *) {}
static void otherDestructor(void *) {}
static void kernelInit(const void *functions, const char *const *env, const char *const *apple, const void *programVars) {
    check(*static_cast<const unsigned long *>(functions) == 5 && !env && !apple && !programVars, "kernel callback update skips native apple setup");
    ++kernelCalls;
}

static void memoryRegionTests() {
    struct Region { uintptr_t address, size; vm_prot_t protection; bool submap; };
    Region regions[] = {{0x1000, 0x10000, VM_PROT_READ, true}, {0x2000, 0x1000, VM_PROT_READ | VM_PROT_EXECUTE, false},
                        {0x3000, 0x1000, VM_PROT_READ | VM_PROT_EXECUTE, false}};
    unsigned calls = 0;
    bool shortCount = false;
    kern_return_t queryResult = KERN_SUCCESS;
    auto query = [&](vm_map_read_t, vm_address_t *address, vm_size_t *length, natural_t *depth,
                     vm_region_recurse_info_t output, mach_msg_type_number_t *count) {
        check(calls < 3 && *depth == (calls ? 1 : 0), "descend into the read-only outer cache submap");
        const auto &region = regions[calls++];
        *address = region.address;
        *length = region.size;
        auto info = reinterpret_cast<vm_region_submap_info_data_64_t *>(output);
        info->protection = info->max_protection = region.protection;
        info->is_submap = region.submap;
        *count = shortCount ? VM_REGION_SUBMAP_INFO_V0_COUNT_64 - 1 : VM_REGION_SUBMAP_INFO_V0_COUNT_64;
        return queryResult;
    };
    check(memoryHasUsing(0x2f00, 0x200, VM_PROT_READ | VM_PROT_EXECUTE, query, false) && calls == 3,
          "verify a range spanning executable leaves despite the outer R-- protection");
    calls = 0;
    regions[2].protection = VM_PROT_READ;
    check(!memoryHasUsing(0x2f00, 0x200, VM_PROT_READ | VM_PROT_EXECUTE, query, false), "every leaf must grant requested protection");
    calls = 0;
    regions[2].protection |= VM_PROT_EXECUTE;
    regions[2].address = 0x3100;
    check(!memoryHasUsing(0x2f00, 0x200, VM_PROT_READ | VM_PROT_EXECUTE, query, false), "reject holes between mapped leaves");
    calls = 0;
    shortCount = true;
    check(!memoryHasUsing(0x2f00, 0x200, VM_PROT_READ, query, false) && calls == 1, "reject incomplete VM region metadata");
    calls = 0;
    shortCount = false;
    queryResult = KERN_INVALID_ADDRESS;
    check(!memoryHasUsing(0x2f00, 0x200, VM_PROT_READ, query, false) && calls == 1, "reject failed region query without reading its data");
    calls = 0;
    check(!memoryHasUsing(UINTPTR_MAX - 10, 12, VM_PROT_READ, query, false) && !calls, "reject overflowing requested range");
    check(memoryHasUsing(reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "malloc")), 32, VM_PROT_READ | VM_PROT_EXECUTE,
                         vm_region_recurse_64, false), "recursive query accepts live native executable code");
    check(memoryHasUsing(reinterpret_cast<uintptr_t>(regions), sizeof(regions), VM_PROT_READ | VM_PROT_WRITE,
                         vm_region_recurse_64, false), "recursive query accepts writable native stack storage");
}

int main() {
    memoryRegionTests();
    // Original native 24A435 instruction windows. ADRP immediates identify state relative to the supplied code PC.
    uint32_t init[48] = {};
    const uint32_t initSequence[] = {0xf9400408, 0x90334c49, 0xf9001d28, 0xf9400008, 0xf100091f, 0x540000c3,
                                    0x90334c48, 0xa9412809, 0xf9002d09, 0x90334c48, 0xf900250a};
    memcpy(init + 12, initSequence, sizeof(initSequence));
    State decoded = {};
    check(decodePthreadInit(init, 0x18053c254, decoded), "decode verified native initializer callbacks");
    check(reinterpret_cast<uintptr_t>(decoded.exitCallback) == 0x1e6ec4038
          && reinterpret_cast<uintptr_t>(decoded.mallocCallback) == 0x1e6ec4058
          && reinterpret_cast<uintptr_t>(decoded.freeCallback) == 0x1e6ec4048, "derive all three callback globals");
    init[19] ^= 1;
    check(!decodePthreadInit(init, 0x18053c254, decoded), "reject a changed native callback instruction");
    init[19] ^= 1;
    check(decodePthreadInit(init, 0x18053c254 + 0x4200000, decoded)
          && reinterpret_cast<uintptr_t>(decoded.exitCallback) == 0x1eb0c4038, "native cache slide is reflected in decoded globals");

    uint32_t keys[32] = {0x51002808, 0x7103d51f, 0x540003c8, 0xd503237f, 0xa9be4ff4, 0xa9017bfd, 0x910043fd,
        0xaa0103f4, 0xaa0003f3, 0xf0334c40, 0x91001000, 0x52a000a1, 0x95efebd6, 0xf0334c68, 0x91006108,
        0xf8735909, 0xb5000069, 0xaa3403e9, 0xf8335909, 0xf0334c28, 0xb9401909, 0x6b09027f, 0x5400004d,
        0xb9001913, 0xf0334c40, 0x91001000, 0x95efebcc, 0x52800000, 0xa9417bfd, 0xa8c24ff4, 0xd50323ff, 0xd65f03c0};
    check(decodeStaticKeys(keys, 0x18053da28, decoded), "decode native TSD ownership state");
    check(reinterpret_cast<uintptr_t>(decoded.keyLock) == 0x1e6ec8004 && reinterpret_cast<uintptr_t>(decoded.destructors) == 0x1e6ecc018
          && reinterpret_cast<uintptr_t>(decoded.maximumStaticKey) == 0x1e6ec4018, "derive key lock, destructor table and bound");
    keys[25] ^= 0x400;
    check(!decodeStaticKeys(keys, 0x18053da28, decoded), "reject mismatched lock/unlock targets");
    keys[25] ^= 0x400;

    // iPhone16,2 26.1/23B50 uses the reversed, equivalent signed comparison for the maximum registered key.
    uint32_t keys26[] = {0x51002808, 0x7103d51f, 0x540003c8, 0xd503237f, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0103f4,
        0xaa0003f3, 0xd006b160, 0x91005000, 0x52a000a1, 0x948531ff, 0xd006b168, 0x91008108, 0xf8735909,
        0xb5000069, 0xaa3403e9, 0xf8335909, 0xd006b128, 0xb9401909, 0x6b13013f, 0x5400004a, 0xb9001913,
        0xd006b160, 0x91005000, 0x948531f5, 0x52800000, 0xa9417bfd, 0xa8c24ff4, 0xd50323ff, 0xd65f03c0};
    check(decodeStaticKeys(keys26, 0x1dfca2424, decoded) && reinterpret_cast<uintptr_t>(decoded.keyLock) == 0x1ed2d0014
          && reinterpret_cast<uintptr_t>(decoded.destructors) == 0x1ed2d0020
          && reinterpret_cast<uintptr_t>(decoded.maximumStaticKey) == 0x1ed2c8018, "decode original 23B50 static-key state");
    keys26[22] = 0x5400004d;
    check(!decodeStaticKeys(keys26, 0x1dfca2424, decoded), "reject reversed operands without the corresponding branch condition");
    keys26[22] = 0x5400004a;
    keys26[21] = 0x6b09027f;
    check(!decodeStaticKeys(keys26, 0x1dfca2424, decoded), "reject modern operands combined with the old branch condition");

    uint32_t workqueue[32] = {};
    workqueue[0] = 0xf100103f; workqueue[26] = 0xf0334c34; workqueue[27] = 0xf9401288;
    workqueue[28] = 0xb40000e8; workqueue[29] = 0x52800200;
    check(decodeWorkqueue(workqueue, 0x1805410e4, decoded)
          && reinterpret_cast<uintptr_t>(decoded.workqueueCallback) == 0x1e6ec8020, "locate native workqueue ownership guard");
    workqueue[29] = 0;
    check(!decodeWorkqueue(workqueue, 0x1805410e4, decoded), "reject changed workqueue occupied-path result");

    uint32_t workgroup[] = {0xaa0003e8, 0xf0334c29, 0xf9400520, 0xb5000060, 0xf9000528, 0xd65f03c0};
    check(decodeWorkgroup(workgroup, 0x1805410bc, decoded) && reinterpret_cast<uintptr_t>(decoded.workgroupFunctions) == 0x1e6ec8008,
          "locate native libdispatch workgroup callback ownership");
    workgroup[4] ^= 0x400;
    check(!decodeWorkgroup(workgroup, 0x1805410bc, decoded), "reject mismatched workgroup load/store cells");

    uint32_t kernel[kKernelCodeWords] = {0xd503237f, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0203f3, 0xf01c1988, 0xf9002100, 0xf8408c08,
        0xb4000348, 0xf2eb3fa0, 0xaa0803f0, 0xaa0003f1, 0xdac11230, 0xaa1003f1, 0xdac143f1, 0xeb11021f,
        0x54000040, 0xd4388e00, 0xdac123f0, 0xb40001f0, 0xf01c1989, 0x9100e129, 0xaa0803f0, 0xaa0003f1,
        0xdac11230, 0xaa1003f1, 0xdac143f1, 0xeb11021f, 0x54000040, 0xd4388e00, 0xaa0903f1, 0xf2e7e4d1,
        0xdac10230, 0xf9000130, 0xf01c1994, 0x3940a288, 0x37000088, 0x9400003d, 0x52800028, 0x3900a288,
        0xb40006f3, 0xaa1303e0, 0x9400a9f8};
    kernel[95] = 0xa9417bfd; kernel[96] = 0xa8c24ff4; kernel[97] = 0xd65f0fff;
    uintptr_t guard = 0;
    check(decodeKernelGuard(kernel, 0x237f01bdc, guard) && guard == 0x270234028, "locate native mach-init once guard");
    kernel[36] ^= 0x100;
    check(!decodeKernelGuard(kernel, 0x237f01bdc, guard), "reject changed mach-init branch target");
    kernel[36] ^= 0x100;
    kernel[40] ^= 0x01000000;
    check(!decodeKernelGuard(kernel, 0x237f01bdc, guard), "reject inverted NULL-apple branch after modern once guard");

    uint32_t kernel26[kKernelCodeWords] = {0x90299928, 0xf9002100, 0xf8408c08, 0xb4000368, 0xaa0803f0, 0xaa0003f1, 0xf2eb3fb1, 0xdac11230,
        0xaa1003f1, 0xdac143f1, 0xeb11021f, 0x54000040, 0xd4388e00, 0xdac123f0, 0xb4000210, 0x90299929,
        0x9100e129, 0xaa0803f0, 0xaa0003f1, 0xf2eb3fb1, 0xdac11230, 0xaa1003f1, 0xdac143f1, 0xeb11021f,
        0x54000040, 0xd4388e00, 0xaa0903f1, 0xf2e7e4d1, 0xdac10230, 0xf9000130, 0xd503237f, 0xa9be4ff4,
        0xa9017bfd, 0x910043fd, 0x90299933, 0x3940a268, 0x37000088, 0x94000006, 0x52800028, 0x3900a268,
        0xa9417bfd, 0xa8c24ff4, 0xd65f0fff};
    check(decodeKernelGuard(kernel26, 0x22d440b88, guard) && guard == 0x280764028, "decode original 23B50 kernel once guard");
    check(decodeKernelGuard(kernel26, 0x22d440b88 + 0x4200000, guard) && guard == 0x284964028,
          "derive the 23B50 guard using the native cache slide");
    struct Mutation { unsigned index; uint32_t toggle; const char *reason; } mutations[] = {
        {1, 1, "reject callback publication from the wrong argument register"},
        {35, 0x20, "reject a guard load through the wrong base register"},
        {36, 0x01000000, "reject an inverted kernel once-guard test"},
        {36, 0x100, "reject a kernel guard branch that does not skip initialization"},
        {39, 0x400, "reject a kernel guard store differing from the queried byte"},
        {42, 1, "reject an unrecognized return after the 23B50 once guard"},
    };
    for (const auto &mutation : mutations) {
        kernel26[mutation.index] ^= mutation.toggle;
        guard = 0xdeadbeef;
        check(!decodeKernelGuard(kernel26, 0x22d440b88, guard) && guard == 0xdeadbeef, mutation.reason);
        kernel26[mutation.index] ^= mutation.toggle;
    }
    kernel26[4] = 0xaa0003f1; kernel26[5] = 0xaa0803f0;
    check(decodeKernelGuard(kernel26, 0x22d440b88, guard), "callback PAC setup can reorder independent register moves");
    kernel26[30] = 0xd503233f; kernel26[42] = 0xd65f0bff;
    check(decodeKernelGuard(kernel26, 0x22d440b88, guard), "prologue and return authentication key do not affect state discovery");
    kernel26[1] = 0xf9001500;
    check(!decodeKernelGuard(kernel26, 0x22d440b88, guard), "callback storage cannot alias the once-guard byte");

    uint32_t kernel267[kKernelCodeWords] = {0xd503237f, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0203f3, 0xb026cd48, 0xf9002100, 0xf8408c08,
        0xb4000348, 0xf2eb3fa0, 0xaa0803f0, 0xaa0003f1, 0xdac11230, 0xaa1003f1, 0xdac143f1, 0xeb11021f,
        0x54000040, 0xd4388e00, 0xdac123f0, 0xb40001f0, 0xb026cd49, 0x9100e129, 0xaa0803f0, 0xaa0003f1,
        0xdac11230, 0xaa1003f1, 0xdac143f1, 0xeb11021f, 0x54000040, 0xd4388e00, 0xaa0903f1, 0xf2e7e4d1,
        0xdac10230, 0xf9000130, 0xb026cd54, 0x3940a288, 0x37000088, 0x9400003e, 0x52800028, 0x3900a288,
        0xb4000713, 0xaa1303e0, 0x94002adb};
    kernel267[96] = 0xa9417bfd; kernel267[97] = 0xa8c24ff4; kernel267[98] = 0xd65f0fff;
    check(decodeKernelGuard(kernel267, 0x232f4bbac, guard) && guard == 0x2808f4028, "decode original 23H128 kernel once guard");
    const unsigned conflictingRegisters[] = {19, 21};
    for (unsigned reg : conflictingRegisters) {
        kernel267[34] = (kernel267[34] & ~31u) | reg;
        kernel267[35] = (kernel267[35] & ~(31u << 5)) | (reg << 5);
        kernel267[39] = (kernel267[39] & ~(31u << 5)) | (reg << 5);
        guard = 0xdeadbeef;
        check(!decodeKernelGuard(kernel267, 0x232f4bbac, guard) && guard == 0xdeadbeef,
              reg == 19 ? "guard address cannot overwrite the saved apple argument" : "guard address must use a register restored by the epilogue");
    }
    kernel267[34] = 0xb026cd54; kernel267[35] = 0x3940a288; kernel267[39] = 0x3900a288;
    kernel267[40] += 0x20;
    check(!decodeKernelGuard(kernel267, 0x232f4bbac, guard), "reject a NULL-apple branch into the middle of the return epilogue");
    kernel267[97] = 0xa9417bfd; kernel267[98] = 0xa8c24ff4; kernel267[99] = 0xd65f0fff;
    check(decodeKernelGuard(kernel267, 0x232f4bbac, guard), "accept changed apple-parser length when the NULL branch still reaches the return");
    kernel267[4] ^= 0x10000;
    check(!decodeKernelGuard(kernel267, 0x232f4bbac, guard), "NULL branch must test the saved apple argument, not envp");
    kernel267[4] ^= 0x10000;
    kernel267[40] = 0xb4fffff3;
    check(!decodeKernelGuard(kernel267, 0x232f4bbac, guard), "reject backward or out-of-window NULL-apple destinations before reading them");

    uintptr_t tsd[512], destructors[512], exitCallback = 11, mallocCallback = 12, freeCallback = 13, workqueueCallback = 0, workgroupFunctions = 99;
    for (unsigned i = 0; i != 512; ++i) { tsd[i] = 0x100 + i; destructors[i] = 0xabcd000 + i; }
    uint32_t maximum = 40, keyLock = 0;
    mach_port_t bootstrap = 7;
    sState = {};
    sState.tsd = tsd; sState.destructors = destructors; sState.exitCallback = &exitCallback;
    sState.mallocCallback = &mallocCallback; sState.freeCallback = &freeCallback; sState.workqueueCallback = &workqueueCallback;
    sState.workgroupFunctions = &workgroupFunctions; sState.initialWorkgroupFunctions = workgroupFunctions;
    sState.maximumStaticKey = &maximum; sState.keyLock = &keyLock; sState.bootstrap = &bootstrap; sState.preparedBootstrap = 42;
    sState.lock = lock; sState.unlock = unlock; sState.kernelInit = kernelInit; sState.prepared = true;
    testTSD = reinterpret_cast<uintptr_t>(tsd);
    workqueueCallback = 9;
    check(LSBActivateNativeSupportState() == EBUSY && tsd[10] == 0x10a && bootstrap == 7, "occupied workqueue aborts without mutating TLS");
    workqueueCallback = 0;
    workgroupFunctions = 100;
    check(LSBActivateNativeSupportState() == EBUSY && tsd[10] == 0x10a && bootstrap == 7 && workgroupFunctions == 100,
          "unexpected workgroup ownership change aborts without mutation");
    workgroupFunctions = 99;
    check(LSBActivateNativeSupportState() == 0 && bootstrap == 42 && !workgroupFunctions,
          "one-way activation refreshes bootstrap and releases the old native workgroup callback table");
    workgroupFunctions = 101;
    bool reserved = true, cleared = true;
    for (unsigned i = 0; i != 10; ++i) reserved &= tsd[i] == 0x100 + i;
    for (unsigned i = 10; i != 512; ++i) cleared &= tsd[i] == 0;
    check(reserved && cleared, "preserve kernel TSD and clear all old native user objects, including dynamic keys");
    tsd[40] = 0xfeed;
    check(LSBActivateNativeSupportState() == EINVAL && tsd[40] == 0xfeed && workgroupFunctions == 101, "activation cannot clear guest state a second time");
    check(guestKeyInit(9, destructor) == EINVAL && guestKeyInit(256, destructor) == EINVAL, "reserved and dynamic keys are rejected");
    check(guestKeyInit(80, destructor) == 0 && destructors[80] == ~reinterpret_cast<uintptr_t>(destructor) && maximum == 80,
          "first guest registration takes over the native destructor and maximum");
    check(guestKeyInit(80, otherDestructor) == 0 && destructors[80] == ~reinterpret_cast<uintptr_t>(destructor), "duplicate guest registration preserves its first callback");
    check(guestKeyInit(40, nullptr) == 0 && destructors[40] == ~uintptr_t(0) && tsd[40] == 0xfeed, "NULL destructor is registered without clearing live guest TLS");
    check(locks == 3 && unlocks == locks && destructors[300] == 0xabcd000 + 300, "only guest-claimed static keys change, under the native lock");
    PthreadFunctions callbacks = {2, 101, 102, 103};
    guestPthreadInit(&callbacks, nullptr, nullptr, nullptr);
    check(exitCallback == 101 && mallocCallback == 102 && freeCallback == 103 && tsd[40] == 0xfeed, "pthread initialization updates callbacks without resetting pthread/TSD");
    unsigned long kernelVersion = 5;
    guestKernelInit(&kernelVersion, nullptr, nullptr, nullptr);
    check(kernelCalls == 1, "guest callbacks are passed to native guarded kernel initialization");
    const char *pthreadLibrary = "/usr/lib/system/libsystem_pthread.dylib";
    const char *kernelLibrary = "/usr/lib/system/libsystem_kernel.dylib";
    check(LSBOverrideNativeSupportSymbol(pthreadLibrary, "___pthread_init", 7) == reinterpret_cast<uintptr_t>(guestPthreadInit), "override only native pthread initializer");
    check(LSBOverrideNativeSupportSymbol(kernelLibrary, "__os_alloc_once_table", 7) == reinterpret_cast<uintptr_t>(sGuestOnce), "guest gets its own alloc-once table");
    check(LSBOverrideNativeSupportSymbol(kernelLibrary, "___libkernel_voucher_init", 7) == 7, "voucher callback registration still reaches native kernel support");
    check(LSBOverrideNativeSupportSymbol(pthreadLibrary, "_pthread_install_workgroup_functions_np", 7) == 7,
          "native workgroup installer keeps normal guest installation and duplicate rejection");
    check(LSBOverrideNativeSupportSymbol("/usr/lib/system/libsystem_sim_pthread.dylib", "___pthread_init", 7) == 7,
          "do not bypass simulator adapter initialization");
    printf("NativeSupportState: %u checks passed\n", checks);
    return 0;
}
