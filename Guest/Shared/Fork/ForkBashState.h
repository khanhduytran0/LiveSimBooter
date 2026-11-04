#pragma once

#include "../PrivateMachVM.h"
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include "../../../ProcessSupport/FakePTY.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Keep the interactive terminal usable. The same receipts remain available for device diagnostics.
static inline bool forkTrialTraceEnabled(void) {
    const char *trace = getenv("LSB_FORK_SNAPSHOT_TRACE");
    return trace ? !strcmp(trace, "1") : !getenv(LSB_FAKE_PTY_ENV);
}

__attribute__((format(printf, 1, 2))) static inline void forkTrialTrace(const char *format, ...) {
    if (!forkTrialTraceEnabled()) return;
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

enum {
    BashGetpidCall = 0x257a0, BashGetpidStub = 0x89608, BashGetpidLazyHelper = 0x89ecc, BashGetpidSlot = 0x9c280,
    BashShellVariables = 0xa6808, BashSubshellReturn = 0x15a4c, BashSubstitutionReturn = 0x2f310,
    BashEarlyChildReturn = 0x178d8, BashLateChildReturn = 0x18374, BashForkReturn = 0x2570c
};

// Procursus 1900 Bash 5.2.21. otool -tvV identifies make_child's fork, execute_disk_command's call
// and execute_command_internal's cm_subshell call. nm and the chained bind table identify the variables and getpid slot.
enum {
    ProcursusBashForkReturn = 0x237fc, ProcursusBashLateChildReturn = 0x165c4, ProcursusBashSubshellReturn = 0xeb28,
    ProcursusBashGetpidStub = 0x84064, ProcursusBashGetpidSlot = 0xa0270, ProcursusBashShellVariables = 0xa6c78
};

static inline bool forkTrialRead(uintptr_t address, void *buffer, size_t size) {
    vm_size_t copied = 0;
    return vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)buffer, &copied) == KERN_SUCCESS && copied == size;
}

static inline kern_return_t forkTrialLeaf(uintptr_t address, size_t length, vm_region_submap_info_data_64_t *info) {
    mach_vm_address_t start = address;
    mach_vm_size_t size = 0;
    natural_t depth = UINT32_MAX;
    mach_msg_type_number_t words = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &start, &size, &depth, (vm_region_recurse_info_t)info, &words);
    if (kr != KERN_SUCCESS) return kr;
    if (words < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info->is_submap) return KERN_FAILURE;
    if (start > address || address - start >= size || length > size - (address - start)) return KERN_INVALID_ADDRESS;
    return KERN_SUCCESS;
}

static inline bool forkTrialImageHasUUID(uintptr_t header, const uint8_t uuid[16]) {
    struct mach_header_64 image;
    if (header > UINTPTR_MAX - BashShellVariables - sizeof(uintptr_t) || !forkTrialRead(header, &image, sizeof(image)) || image.magic != MH_MAGIC_64 ||
        image.cputype != CPU_TYPE_ARM64 || image.cpusubtype != CPU_SUBTYPE_ARM64_ALL || image.filetype != MH_EXECUTE || image.sizeofcmds > 0x4000) return false;
    uintptr_t cursor = header + sizeof(image), end = cursor + image.sizeofcmds;
    for (unsigned i = 0; i < image.ncmds && end - cursor >= sizeof(struct load_command); ++i) {
        struct load_command load;
        if (!forkTrialRead(cursor, &load, sizeof(load)) || load.cmdsize < sizeof(load) || load.cmdsize > end - cursor) return false;
        if (load.cmd == LC_UUID) {
            struct uuid_command id;
            return load.cmdsize == sizeof(id) && forkTrialRead(cursor, &id, sizeof(id)) && !memcmp(id.uuid, uuid, sizeof(id.uuid));
        }
        cursor += load.cmdsize;
    }
    return false;
}

// llvm-objdump --indirect-symbols and dyld_info -exports on Bash 3.2.57 identify the _getpid stub/slot and _shell_variables.
// make_child's first child-side BL targets a NOP/LDR-literal/BR stub whose literal displacement selects BashGetpidSlot.
static inline bool forkTrialBashImage(uintptr_t header) {
    static const uint8_t uuid[16] = {0x95, 0xda, 0x63, 0xf7, 0xde, 0xfa, 0x33, 0x94, 0xa3, 0x3f, 0x37, 0x4d, 0x41, 0xf1, 0x19, 0x0c};
    const uint32_t expectedStub[] = {0xd503201f, 0x580963b0, 0xd61f0200};
    uint32_t call, stub[3];
    return forkTrialImageHasUUID(header, uuid) && forkTrialRead(header + BashGetpidCall, &call, sizeof(call)) && call == 0x94018f9a &&
           forkTrialRead(header + BashGetpidStub, stub, sizeof(stub)) && !memcmp(stub, expectedStub, sizeof(stub));
}

static inline bool forkTrialProcursusBashImage(uintptr_t header) {
    static const uint8_t uuid[16] = {0x05, 0x79, 0x98, 0x0b, 0x28, 0x5b, 0x3e, 0x49, 0xa3, 0xb1, 0xff, 0x5e, 0xa2, 0x09, 0x9c, 0x97};
    const uint32_t expectedStub[] = {0x900000f0, 0xf9413a10, 0xd61f0200};
    uint32_t call, stub[3];
    return forkTrialImageHasUUID(header, uuid) && forkTrialRead(header + ProcursusBashForkReturn - 4, &call, sizeof(call)) && call == 0x940181d9 &&
           forkTrialRead(header + ProcursusBashGetpidStub, stub, sizeof(stub)) && !memcmp(stub, expectedStub, sizeof(stub));
}

static inline bool readForkBashVariables(uintptr_t header, uintptr_t *address, uint64_t *firstWord) {
    *address = 0;
    *firstWord = 0;
    uintptr_t variables;
    vm_region_submap_info_data_64_t info = {};
    if (!forkTrialBashImage(header) || !forkTrialRead(header + BashShellVariables, &variables, sizeof(variables)) ||
        !variables || (variables & (sizeof(*firstWord) - 1)) || forkTrialLeaf(variables, sizeof(*firstWord), &info) != KERN_SUCCESS ||
        !(info.protection & VM_PROT_READ) || info.inheritance != VM_INHERIT_COPY || !forkTrialRead(variables, firstWord, sizeof(*firstWord))) return false;
    *address = variables;
    return true;
}

static inline bool forkTrialRepairGetpid(uintptr_t header, uintptr_t parentGetpid, uintptr_t copiedTarget, uintptr_t childGetpid) {
    vm_region_submap_info_data_64_t text = {}, slot = {};
    const vm_prot_t rx = VM_PROT_READ | VM_PROT_EXECUTE, rw = VM_PROT_READ | VM_PROT_WRITE;
    if (!parentGetpid || copiedTarget != parentGetpid ||
        forkTrialLeaf(header + BashGetpidStub, 3 * sizeof(uint32_t), &text) != KERN_SUCCESS || text.protection != rx ||
        forkTrialLeaf(header + BashGetpidSlot, sizeof(uintptr_t), &slot) != KERN_SUCCESS || slot.protection != rw ||
        slot.inheritance != VM_INHERIT_COPY) return false;
    if (copiedTarget != childGetpid && vm_write(mach_task_self(), header + BashGetpidSlot, (vm_offset_t)&childGetpid, sizeof(childGetpid)) != KERN_SUCCESS)
        return false;
    uintptr_t rebound;
    return forkTrialRead(header + BashGetpidSlot, &rebound, sizeof(rebound)) && rebound == childGetpid;
}

// Only the observer's successful snapshot path calls this. A nonzero variablesAddress denotes a word in an explicitly included private page.
static inline void reportForkBashState(uintptr_t header, uintptr_t parentGetpid, uintptr_t variablesAddress, uint64_t firstWord, pid_t expectedGuestPID) {
    if (!forkTrialBashImage(header)) return;
    uintptr_t copiedTarget, variables;
    if (!forkTrialRead(header + BashGetpidSlot, &copiedTarget, sizeof(copiedTarget)) ||
        !forkTrialRead(header + BashShellVariables, &variables, sizeof(variables))) return;
    uintptr_t childGetpid = (uintptr_t)&getpid;
    Dl_info childImage = {};
    const char *name = dladdr((void *)childGetpid, &childImage) ? childImage.dli_fname : NULL;
    fprintf(stderr, "fork trial Bash getpid_slot=%p copied_target=%p child_target=%p binding=%s child_image=%s\n", (void *)(header + BashGetpidSlot),
            (void *)copiedTarget, (void *)childGetpid, copiedTarget == childGetpid ? "matches" : copiedTarget == header + BashGetpidLazyHelper ? "lazy" : "differs",
            name ? name : "(unknown)");
    const char *baseName = name ? strrchr(name, '/') : NULL;
    if (baseName && !strcmp(baseName + 1, "libLiveSimSpawn.dylib") && expectedGuestPID > 0 && forkTrialRepairGetpid(header, parentGetpid, copiedTarget, childGetpid)) {
        pid_t (*stub)(void) = (pid_t (*)(void))(header + BashGetpidStub);
        pid_t result = stub(); // Only the three reviewed instructions execute, using the observer's own stack and bound target.
        fprintf(stderr, "fork trial Bash getpid_rebound=%d stub_pid=%d expected_pid=%d matches=%d\n",
                copiedTarget != childGetpid, result, expectedGuestPID, result == expectedGuestPID);
    } else {
        fprintf(stderr, "fork trial Bash getpid_repair=skipped\n");
    }

    vm_region_submap_info_data_64_t info = {};
    kern_return_t kr = forkTrialLeaf(variables, sizeof(firstWord), &info);
    const char *coverage = !variables ? "null" : kr == KERN_SUCCESS ? "mapped" : kr == KERN_INVALID_ADDRESS ? "vacant" : "unknown";
    uint64_t copiedWord;
    const char *sample = "not-offered";
    if (variablesAddress)
        sample = variables == variablesAddress && kr == KERN_SUCCESS && (info.protection & VM_PROT_READ) && info.inheritance == VM_INHERIT_COPY &&
                 forkTrialRead(variables, &copiedWord, sizeof(copiedWord)) && copiedWord == firstWord ? "matches" : "mismatch";
    fprintf(stderr, "fork trial Bash shell_variables=%p child=%s tag=%u prot=%d sample=%s\n", (void *)variables, coverage,
            kr == KERN_SUCCESS ? info.user_tag : 0, kr == KERN_SUCCESS ? info.protection : 0, sample);
}
