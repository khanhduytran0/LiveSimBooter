#pragma once
#include "../Guest/ForkHook.h"
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <string.h>

static inline const char *trialForkImageName(const void *address) {
    Dl_info image;
    if (!dladdr(address, &image) || !image.dli_fname) return "";
    const char *slash = strrchr(image.dli_fname, '/');
    return slash ? slash + 1 : image.dli_fname;
}

static inline int trialReadLocal(uintptr_t address, void *out, size_t size) {
    vm_size_t bytes = 0;
    return vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)out, &bytes) || bytes != size ? EFAULT : 0;
}

// Standalone controls can run without a linked trial shim. Older shims leave 23A343's vfork entry uninterposed.
static inline uintptr_t trialControlForkReturnPC(void) {
    uintptr_t (*originalPC)(bool) = dlsym(RTLD_DEFAULT, "LSBTrialLibcForkReturnPC");
    if (originalPC) return originalPC(false);
    uintptr_t vforkEntry = (uintptr_t)dlsym(RTLD_DEFAULT, "vfork");
    return vforkEntry > 0x8c ? vforkEntry - 0x8c + LSB_FORK_LIBC_RETURN_OFFSET : 0;
}

// Validate against the observer's own 23A343 libc. Symbols can be stripped in the guest cache.
static inline int trialCheckLibcChildBranch(uintptr_t pc, bool isVfork) {
    if (pc <= LSB_FORK_LIBC_RETURN_OFFSET || (pc & 3)) return EPROTO;
    if (strcmp(trialForkImageName((const void *)pc), "libsystem_c.dylib")) return ENOEXEC;
    vm_address_t entry = pc - LSB_FORK_LIBC_RETURN_OFFSET;
    uint32_t instructions[26];
    if (trialReadLocal(entry, instructions, sizeof(instructions))) return EFAULT;
    // fork/vfork+0x18 and +0x64: MOV W0,#flags before v2 prepare and the shared parent/child call respectively.
    uint32_t flags = isVfork ? 0x52800020 : 0x52800000;
    if (instructions[0] != 0xa9be4ff4 || instructions[1] != 0xa9017bfd || instructions[2] != 0x910043fd ||
        instructions[6] != flags || instructions[25] != flags ||
        (instructions[12] & 0xfc000000) != 0x94000000 || instructions[13] != 0xaa0003f3 || instructions[14] != 0x34000100) return ENOEXEC;
    return 0;
}

typedef void (*TrialLibcForkHandler)(unsigned);
typedef struct { TrialLibcForkHandler prepare, parent, child; } TrialLibcForkHandlers;
_Static_assert(sizeof(TrialLibcForkHandlers) == 24, "23A343 libc fork handler slots");

static inline uintptr_t trialForkHandlerSlot(uintptr_t pc, uint32_t adrp, uint32_t ldr) {
    if ((adrp & 0x9f00001f) != 0x90000008 || (ldr & 0xffc003ff) != 0xf9400108) return 0; // ADRP x8; LDR x8,[x8,#imm].
    uint32_t raw = (((adrp >> 5) & 0x7ffff) << 2) | ((adrp >> 29) & 3);
    int32_t pages = (int32_t)(raw ^ 0x100000) - 0x100000;
    return (pc & ~(uintptr_t)4095) + (intptr_t)pages * 4096 + ((ldr >> 10) & 0xfff) * 8;
}

static inline int trialResolveLibcForkHandlers(uintptr_t pc, bool isVfork, TrialLibcForkHandlers *result) {
    // Resolve the v2 table loaded by the selected 23A343 libc entry. The verified zero branch calls child(flags).
    int error = trialCheckLibcChildBranch(pc, isVfork);
    if (error) return error;
    uintptr_t start = pc - LSB_FORK_LIBC_RETURN_OFFSET;
    uint32_t code[24];
    if (trialReadLocal(start, code, sizeof(code))) return EFAULT;
    uintptr_t prepare = trialForkHandlerSlot(start + 0xc, code[3], code[4]);
    uintptr_t parent = trialForkHandlerSlot(start + 0x3c, code[15], code[17]);
    uintptr_t child = trialForkHandlerSlot(start + 0x58, code[22], code[23]);
    if (!prepare || parent != prepare + 8 || child != parent + 8 || trialReadLocal(prepare, result, sizeof(*result))) return ENOEXEC;
    if (strcmp(trialForkImageName((const void *)result->prepare), "libSystem.B.dylib") ||
        strcmp(trialForkImageName((const void *)result->parent), "libSystem.B.dylib") ||
        strcmp(trialForkImageName((const void *)result->child), "libSystem.B.dylib")) return ENOEXEC;
    return 0;
}

#if defined(__arm64__) && !defined(__arm64e__)
// Requires the parent's prepared guest state, a valid single-threaded child and the verified fork or vfork return PC.
// Run libc's zero-return branch on this controller stack. Its stock epilogue returns here, without entering the parent's shim frame.
__attribute__((naked, unused)) static int trialRunLibcChildBranch(uintptr_t pc __attribute__((unused))) {
    __asm__ volatile("stp x20, x19, [sp, #-32]!\n"
                     "stp x29, x30, [sp, #16]\n"
                     "add x29, sp, #16\n"
                     "mov x16, x0\n"
                     "mov x0, #0\n"
                     "br x16");
}
#endif
