// Retain one native pthread owner while guest libSystem supplies the callbacks for guest allocations and cleanup.
#include "NativeSupportState.h"

#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/task_special_ports.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr size_t kTSDOffset = 224;
constexpr size_t kErrnoOffset = 172;
constexpr unsigned kKernelKeys = 10;
constexpr unsigned kStaticKeys = 256;
constexpr unsigned kTotalKeys = 512;
constexpr unsigned kKernelCodeWords = 128;

// xnu/libsyscall/os/alloc_once.c: 100 pairs. The inspected native 24A435 export occupies exactly 0x640 bytes.
struct AllocOnceEntry { intptr_t once; void *value; };
static_assert(sizeof(AllocOnceEntry) == 16, "Unexpected alloc-once ABI");
AllocOnceEntry sGuestOnce[100];

// Libsystem/init.c's pushed-down callback table, version 2. This does not contain or replace the pthread object itself.
struct PthreadFunctions { unsigned long version; uintptr_t exit; uintptr_t malloc; uintptr_t free; };
static_assert(sizeof(PthreadFunctions) == 32 && offsetof(PthreadFunctions, malloc) == 16, "Unexpected pthread callback ABI");
using KernelInit = void (*)(const void *, const char *const *, const char *const *, const void *);
using Lock = void (*)(void *, uint32_t);
using Unlock = void (*)(void *);

struct State {
    uintptr_t *tsd;
    uintptr_t *exitCallback;
    uintptr_t *mallocCallback;
    uintptr_t *freeCallback;
    uintptr_t *destructors;
    uintptr_t *workqueueCallback;
    uintptr_t *workgroupFunctions;
    uintptr_t initialWorkgroupFunctions;
    uint32_t *maximumStaticKey;
    void *keyLock;
    mach_port_t *bootstrap;
    mach_port_t preparedBootstrap;
    KernelInit kernelInit;
    Lock lock;
    Unlock unlock;
    unsigned char ownedKeys[kStaticKeys];
    bool prepared;
    bool active;
};
State sState;

bool equal(const char *left, const char *right) {
    if (!left || !right) return false;
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

bool adrp(uint32_t instruction, uintptr_t pc, unsigned reg, uintptr_t &address) {
    if ((instruction & 0x9f00001f) != (0x90000000u | reg)) return false;
    int64_t immediate = ((instruction >> 5) & 0x7ffff) << 2 | ((instruction >> 29) & 3);
    if (immediate & 0x100000) immediate -= 0x200000;
    address = (pc & ~uintptr_t(4095)) + immediate * 4096;
    return true;
}

bool addAddress(const uint32_t *code, unsigned index, uintptr_t pc, unsigned reg, uintptr_t &address) {
    if (!adrp(code[index], pc + index * 4, reg, address)) return false;
    uint32_t add = code[index + 1];
    if ((add & 0xffc003ff) != (0x91000000u | (reg << 5) | reg)) return false;
    address += (add >> 10) & 4095;
    return true;
}

bool memoryOperand(uint32_t instruction, uint32_t opcode, unsigned base, unsigned value, unsigned scale, uintptr_t page, uintptr_t &address) {
    if ((instruction & 0xffc003ff) != (opcode | (base << 5) | value)) return false;
    address = page + (((instruction >> 10) & 4095) << scale);
    return true;
}

// Decode semantic instruction sequences, not cache virtual addresses. Fail before handoff if the native ABI differs.
// Provenance: extracted iPhone18,2 24A435 and iPhone16,2 23B50/23H128 libsystem_pthread initializer/key/workqueue exports.
bool decodePthreadInit(const uint32_t *code, uintptr_t pc, State &state) {
    for (unsigned i = 0; i + 11 < 48; ++i) {
        if (code[i] != 0xf9400408 || code[i + 3] != 0xf9400008 || code[i + 4] != 0xf100091f || code[i + 5] != 0x540000c3
            || code[i + 7] != 0xa9412809) continue;
        uintptr_t page, exit, malloc, free;
        if (!adrp(code[i + 1], pc + (i + 1) * 4, 9, page)
            || !memoryOperand(code[i + 2], 0xf9000000, 9, 8, 3, page, exit)
            || !adrp(code[i + 6], pc + (i + 6) * 4, 8, page)
            || !memoryOperand(code[i + 8], 0xf9000000, 8, 9, 3, page, malloc)
            || !adrp(code[i + 9], pc + (i + 9) * 4, 8, page)
            || !memoryOperand(code[i + 10], 0xf9000000, 8, 10, 3, page, free)) return false;
        state.exitCallback = reinterpret_cast<uintptr_t *>(exit);
        state.mallocCallback = reinterpret_cast<uintptr_t *>(malloc);
        state.freeCallback = reinterpret_cast<uintptr_t *>(free);
        return exit != malloc && exit != free && malloc != free;
    }
    return false;
}

bool decodeStaticKeys(const uint32_t *code, uintptr_t pc, State &state) {
    // 23B50 reverses both CMP operands and the signed branch: maximum >= key is equivalent to key <= maximum.
    bool maximumAlreadyCoversKey = (code[21] == 0x6b09027f && code[22] == 0x5400004d)
                               || (code[21] == 0x6b13013f && code[22] == 0x5400004a);
    if (code[0] != 0x51002808 || code[1] != 0x7103d51f || code[2] != 0x540003c8
        || code[11] != 0x52a000a1 || (code[12] & 0xfc000000) != 0x94000000
        || code[15] != 0xf8735909 || code[16] != 0xb5000069 || code[17] != 0xaa3403e9 || code[18] != 0xf8335909
        || !maximumAlreadyCoversKey || (code[26] & 0xfc000000) != 0x94000000) return false;
    uintptr_t lock, unlock, table, page, maximum, storeMaximum;
    if (!addAddress(code, 9, pc, 0, lock) || !addAddress(code, 24, pc, 0, unlock) || lock != unlock
        || !addAddress(code, 13, pc, 8, table) || !adrp(code[19], pc + 76, 8, page)
        || !memoryOperand(code[20], 0xb9400000, 8, 9, 2, page, maximum)
        || !memoryOperand(code[23], 0xb9000000, 8, 19, 2, page, storeMaximum) || maximum != storeMaximum) return false;
    state.keyLock = reinterpret_cast<void *>(lock);
    state.destructors = reinterpret_cast<uintptr_t *>(table);
    state.maximumStaticKey = reinterpret_cast<uint32_t *>(maximum);
    return true;
}

bool decodeWorkqueue(const uint32_t *code, uintptr_t pc, State &state) {
    uintptr_t page, callback;
    if (code[0] != 0xf100103f || code[28] != 0xb40000e8 || code[29] != 0x52800200
        || !adrp(code[26], pc + 104, 20, page) || !memoryOperand(code[27], 0xf9400000, 20, 8, 3, page, callback)) return false;
    state.workqueueCallback = reinterpret_cast<uintptr_t *>(callback);
    return true;
}

bool decodeWorkgroup(const uint32_t *code, uintptr_t pc, State &state) {
    // Native 24A435 pthread_install_workgroup_functions_np traps on a second installation, including guest libdispatch_init.
    uintptr_t page, loaded, stored;
    if (code[0] != 0xaa0003e8 || code[3] != 0xb5000060 || code[5] != 0xd65f03c0
        || !adrp(code[1], pc + 4, 9, page) || !memoryOperand(code[2], 0xf9400000, 9, 0, 3, page, loaded)
        || !memoryOperand(code[4], 0xf9000000, 9, 8, 3, page, stored) || loaded != stored) return false;
    state.workgroupFunctions = reinterpret_cast<uintptr_t *>(loaded);
    return true;
}

// Locate callback publication and the once guard, without depending on the compiler's prologue or PAC conversion sequence.
// Some versions also inspect apple; follow their NULL branch to the return instead of pinning the skipped parser's size.
bool decodeKernelGuard(const uint32_t *code, uintptr_t pc, uintptr_t &guard) {
    bool checksApple = (code[40] & 0xff00001f) == 0xb4000013; // CBZ x19
    unsigned publication = checksApple ? 5 : 0, tail = 40;
    if (checksApple) {
        unsigned skip = (code[40] >> 5) & 0x7ffff;
        if (code[4] != 0xaa0203f3 || skip < 3 || skip > kKernelCodeWords - tail - 3) return false; // x19 preserves apple (x2)
        tail += skip;
    }
    bool returns = code[tail + 2] == 0xd65f0fff || code[tail + 2] == 0xd65f0bff || code[tail + 2] == 0xd65f03c0;
    if (code[tail] != 0xa9417bfd || code[tail + 1] != 0xa8c24ff4 || !returns) return false;
    uintptr_t page, callbacks, decodedGuard, storeGuard;
    unsigned guardRegister = code[34] & 31;
    // The verified epilogue restores x19/x20; the apple path already owns x19 until its NULL check.
    if ((guardRegister != 19 && guardRegister != 20) || (checksApple && guardRegister == 19)) return false;
    if (!adrp(code[publication], pc + publication * 4, 8, page)
        || !memoryOperand(code[publication + 1], 0xf9000000, 8, 0, 3, page, callbacks)
        || code[publication + 2] != 0xf8408c08
        || !adrp(code[34], pc + 136, guardRegister, page)
        || !memoryOperand(code[35], 0x39400000, guardRegister, 8, 0, page, decodedGuard)
        || code[36] != 0x37000088 || (code[37] & 0xfc000000) != 0x94000000 || code[38] != 0x52800028
        || !memoryOperand(code[39], 0x39000000, guardRegister, 8, 0, page, storeGuard)
        || decodedGuard != storeGuard || decodedGuard == callbacks) return false;
    guard = decodedGuard;
    return true;
}

uintptr_t currentTSD() {
#if defined(LSB_NATIVE_SUPPORT_STATE_TESTING)
    extern uintptr_t testTSD;
    return testTSD;
#else
    uintptr_t address;
    __asm volatile("mrs %0, TPIDRRO_EL0" : "=r"(address) : : "memory");
    return address;
#endif
}

[[noreturn]] void invalidHandoff() {
#if defined(LSB_NATIVE_SUPPORT_STATE_TESTING)
    __builtin_trap();
#else
    register uintptr_t x0 __asm("x0") = 126;
    register uintptr_t x16 __asm("x16") = 1;
    __asm volatile("svc #0x80" : "+r"(x0), "+r"(x16) : : "memory", "cc");
    __builtin_trap();
#endif
}

void guestPthreadInit(const PthreadFunctions *functions, const void *, const void *, const void *) {
    if (!sState.active || !functions || functions->version != 2 || !functions->exit || !functions->malloc || !functions->free) invalidHandoff();
    *sState.exitCallback = functions->exit;
    *sState.mallocCallback = functions->malloc;
    *sState.freeCallback = functions->free;
}

void guestPthreadLateInit() {
    // The native owner has already frozen its pthread/JIT configuration. Its late initializer is not repeatable.
    if (!sState.active) invalidHandoff();
}

void guestKernelInit(const void *functions, const char *const *, const char *const *, const void *) {
    if (!sState.active || !functions || *static_cast<const unsigned long *>(functions) != 5) invalidHandoff();
    // Keep the native once-guarded mach state and update guest callbacks. No native dyld/apple initialization is repeated.
    sState.kernelInit(functions, nullptr, nullptr, nullptr);
}

int guestKeyInit(unsigned long key, void (*destructor)(void *)) {
    if (!sState.active) return EINVAL;
    if (key < kKernelKeys || key >= kStaticKeys) return EINVAL;
    sState.lock(sState.keyLock, 0x50000);
    if (!sState.ownedKeys[key]) {
        // Native pthread stores complemented function pointers; zero means unregistered and ~0 means a NULL destructor.
        sState.destructors[key] = ~reinterpret_cast<uintptr_t>(destructor);
        sState.ownedKeys[key] = 1;
    }
    if (*sState.maximumStaticKey < key) *sState.maximumStaticKey = static_cast<uint32_t>(key);
    sState.unlock(sState.keyLock);
    return 0;
}

template <typename Query>
bool memoryHasUsing(uintptr_t address, size_t size, vm_prot_t protection, Query query, bool diagnostics) {
    if (!address || !size || address + size < address) return false;
    constexpr mach_msg_type_number_t requiredCount = VM_REGION_SUBMAP_INFO_V0_COUNT_64;
    static_assert(requiredCount * sizeof(natural_t) >= offsetof(vm_region_submap_info_data_64_t, is_submap) + sizeof(boolean_t));
    const uintptr_t end = address + size;
    uintptr_t cursor = address;
    natural_t depth = 0;
    for (unsigned attempt = 0; attempt != 64; ++attempt) {
        vm_address_t region = cursor;
        vm_size_t length = 0;
        vm_region_submap_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t result = query(mach_task_self(), &region, &length, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
        bool covered = region <= cursor && length && length <= UINTPTR_MAX - region && region + length > cursor;
        bool valid = result == KERN_SUCCESS && count >= requiredCount && covered && depth <= 16;
        if (valid && info.is_submap && depth < 16) { ++depth; continue; }
        if (!valid || info.is_submap || (info.protection & protection) != protection) {
            if (diagnostics)
                fprintf(stderr, "Native support VM address=0x%llx size=0x%llx want=%d cursor=0x%llx kr=%d region=0x%llx size=0x%llx "
                        "protection=%d max=%d submap=%d depth=%u count=%u\n", (unsigned long long)address, (unsigned long long)size,
                        protection, (unsigned long long)cursor, result, (unsigned long long)region, (unsigned long long)length,
                        info.protection, info.max_protection, info.is_submap, depth, count);
            return false;
        }
        cursor = region + length;
        if (cursor >= end) return true;
    }
    if (diagnostics) fprintf(stderr, "Native support VM region limit address=0x%llx size=0x%llx cursor=0x%llx depth=%u\n",
                             (unsigned long long)address, (unsigned long long)size, (unsigned long long)cursor, depth);
    return false;
}

#if !defined(LSB_NATIVE_SUPPORT_STATE_TESTING)
bool memoryHas(uintptr_t address, size_t size, vm_prot_t protection) {
    // vm_region_64 reports the native cache's outer R-- submap. Descend to each leaf before testing actual protections.
    return memoryHasUsing(address, size, protection, vm_region_recurse_64, true);
}

const uint32_t *nativeCode(const char *symbol, const char *image, size_t bytes) {
    void *address = dlsym(RTLD_DEFAULT, symbol);
    Dl_info info = {};
    int located = address ? dladdr(address, &info) : 0;
    bool readable = address && memoryHas(reinterpret_cast<uintptr_t>(address), bytes, VM_PROT_READ | VM_PROT_EXECUTE);
    if (!address || !located || !info.dli_fname || !strstr(info.dli_fname, image) || !readable) {
        fprintf(stderr, "Native support lookup %s address=%p dladdr=%d image=%s RX=%d\n", symbol, address, located,
                info.dli_fname ? info.dli_fname : "<none>", readable);
        return nullptr;
    }
    return static_cast<const uint32_t *>(address);
}
#endif

int fail(char *error, size_t size, int status, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return status;
}

} // namespace

int LSBPrepareNativeSupportState(uintptr_t stackTop, size_t stackSize, char *error, size_t errorSize) {
#if defined(LSB_NATIVE_SUPPORT_STATE_TESTING)
    (void)stackTop; (void)stackSize;
    return fail(error, errorSize, ENOTSUP, "Live native preparation is disabled in the unit-test build");
#else
    if (sState.prepared || sState.active) return fail(error, errorSize, EALREADY, "Native support state was already prepared");
    if (!pthread_main_np() || stackTop != reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()))
        || stackSize != pthread_get_stacksize_np(pthread_self())) return fail(error, errorSize, ENOTSUP, "Native pthread stack bounds differ from guest stack");
    State candidate = {};
    uintptr_t tsd = currentTSD();
    if (tsd < kTSDOffset || (tsd & 15) || !memoryHas(tsd, sizeof(uintptr_t) * kTotalKeys, VM_PROT_READ | VM_PROT_WRITE))
        return fail(error, errorSize, ENOTSUP, "Native pthread TSD storage is unsupported");
    candidate.tsd = reinterpret_cast<uintptr_t *>(tsd);
    if (candidate.tsd[0] != tsd - kTSDOffset || candidate.tsd[1] != tsd - kTSDOffset + kErrnoOffset)
        return fail(error, errorSize, ENOTSUP, "Native pthread TSD prefix is unsupported");

    auto init = nativeCode("__pthread_init", "libsystem_pthread.dylib", 192);
    auto keys = nativeCode("pthread_key_init_np", "libsystem_pthread.dylib", 128);
    auto workqueue = nativeCode("pthread_workqueue_setup", "libsystem_pthread.dylib", 128);
    auto workgroup = nativeCode("pthread_install_workgroup_functions_np", "libsystem_pthread.dylib", 24);
    auto kernel = nativeCode("__libkernel_init", "libsystem_kernel.dylib", kKernelCodeWords * sizeof(uint32_t));
    uintptr_t kernelGuard = 0;
    if (!init || !keys || !workqueue || !workgroup || !kernel) {
        if (error && errorSize) snprintf(error, errorSize, "Native support symbols unavailable: init=%p keys=%p workqueue=%p workgroup=%p kernel=%p",
                                        init, keys, workqueue, workgroup, kernel);
        return ENOTSUP;
    }
    if (!decodePthreadInit(init, reinterpret_cast<uintptr_t>(init), candidate))
        return fail(error, errorSize, ENOTSUP, "Native pthread initializer instructions differ from the validated ABIs");
    if (!decodeStaticKeys(keys, reinterpret_cast<uintptr_t>(keys), candidate))
        return fail(error, errorSize, ENOTSUP, "Native static-key initializer instructions differ from the validated ABIs");
    if (!decodeWorkqueue(workqueue, reinterpret_cast<uintptr_t>(workqueue), candidate))
        return fail(error, errorSize, ENOTSUP, "Native workqueue instructions differ from the validated ABIs");
    if (!decodeWorkgroup(workgroup, reinterpret_cast<uintptr_t>(workgroup), candidate))
        return fail(error, errorSize, ENOTSUP, "Native workgroup instructions differ from the validated ABIs");
    if (!decodeKernelGuard(kernel, reinterpret_cast<uintptr_t>(kernel), kernelGuard))
        return fail(error, errorSize, ENOTSUP, "Native kernel initializer instructions differ from the validated ABIs");

    candidate.kernelInit = reinterpret_cast<KernelInit>(const_cast<uint32_t *>(kernel));
    candidate.lock = reinterpret_cast<Lock>(dlsym(RTLD_DEFAULT, "os_unfair_lock_lock_with_options"));
    candidate.unlock = reinterpret_cast<Unlock>(dlsym(RTLD_DEFAULT, "os_unfair_lock_unlock"));
    candidate.bootstrap = static_cast<mach_port_t *>(dlsym(RTLD_DEFAULT, "bootstrap_port"));
    if (!candidate.lock || !candidate.unlock || !memoryHas(kernelGuard, 1, VM_PROT_READ) || !(*reinterpret_cast<uint8_t *>(kernelGuard) & 1)
        || !memoryHas(reinterpret_cast<uintptr_t>(candidate.exitCallback), 8, VM_PROT_READ | VM_PROT_WRITE)
        || !memoryHas(reinterpret_cast<uintptr_t>(candidate.mallocCallback), 8, VM_PROT_READ | VM_PROT_WRITE)
        || !memoryHas(reinterpret_cast<uintptr_t>(candidate.freeCallback), 8, VM_PROT_READ | VM_PROT_WRITE)
        || !memoryHas(reinterpret_cast<uintptr_t>(candidate.keyLock), 4, VM_PROT_READ | VM_PROT_WRITE)
        || !memoryHas(reinterpret_cast<uintptr_t>(candidate.destructors), kTotalKeys * 8, VM_PROT_READ | VM_PROT_WRITE)
        || !memoryHas(reinterpret_cast<uintptr_t>(candidate.maximumStaticKey), 4, VM_PROT_READ | VM_PROT_WRITE)
        || !memoryHas(reinterpret_cast<uintptr_t>(candidate.workqueueCallback), 8, VM_PROT_READ)
        || !memoryHas(reinterpret_cast<uintptr_t>(candidate.workgroupFunctions), 8, VM_PROT_READ | VM_PROT_WRITE)
        || !memoryHas(reinterpret_cast<uintptr_t>(candidate.bootstrap), 4, VM_PROT_READ | VM_PROT_WRITE))
        return fail(error, errorSize, ENOTSUP, "Native support callback storage is not initialized or writable");
    if (*candidate.workqueueCallback) return fail(error, errorSize, EBUSY, "Native pthread already has workqueue callbacks");
    candidate.initialWorkgroupFunctions = *candidate.workgroupFunctions;
    if (task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &candidate.preparedBootstrap) != KERN_SUCCESS)
        return fail(error, errorSize, EIO, "Cannot read the guest bootstrap port");
    candidate.prepared = true;
    sState = candidate;
    return 0;
#endif
}

uintptr_t LSBOverrideNativeSupportSymbol(const char *library, const char *symbol, uintptr_t nativeAddress) {
    if (!sState.prepared || !library || !symbol) return nativeAddress;
    if (equal(library, "/usr/lib/system/libsystem_pthread.dylib")) {
        if (equal(symbol, "___pthread_init")) return reinterpret_cast<uintptr_t>(guestPthreadInit);
        if (equal(symbol, "___pthread_late_init")) return reinterpret_cast<uintptr_t>(guestPthreadLateInit);
        if (equal(symbol, "_pthread_key_init_np")) return reinterpret_cast<uintptr_t>(guestKeyInit);
    } else if (equal(library, "/usr/lib/system/libsystem_kernel.dylib")) {
        if (equal(symbol, "___libkernel_init")) return reinterpret_cast<uintptr_t>(guestKernelInit);
        if (equal(symbol, "__os_alloc_once_table")) return reinterpret_cast<uintptr_t>(sGuestOnce);
    }
    return nativeAddress;
}

int LSBActivateNativeSupportState() {
    if (!sState.prepared || sState.active || currentTSD() != reinterpret_cast<uintptr_t>(sState.tsd)) return EINVAL;
    if (*sState.workqueueCallback) return EBUSY;
    if (*sState.workgroupFunctions != sState.initialWorkgroupFunctions) return EBUSY;
    *sState.bootstrap = sState.preparedBootstrap;
    // Guest libdispatch installs its own table through the unchanged native API. Future duplicate installs must still trap.
    *sState.workgroupFunctions = 0;
    // Native preparation is over. Forget its user TLS objects without invoking old destructors against guest malloc/TLS.
    // Preserve pthread self/errno, Mach ports, QoS and kernel-reserved slots 0..9. No native cache TLV users run after this point.
    auto slots = reinterpret_cast<volatile uintptr_t *>(sState.tsd);
    for (unsigned key = kKernelKeys; key != kTotalKeys; ++key) slots[key] = 0;
    sState.active = true;
    return 0;
}
