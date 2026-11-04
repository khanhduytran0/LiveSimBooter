// Procursus Bash 5.2.21 has one __DATA_CONST,__got at +0xa0000, as reported by llvm-objdump --macho --indirect-symbols.
#pragma once
#include "ForkBashState.h"
#include <errno.h>

enum { ProcursusBashGOT = 0xa0000, ProcursusBashGOTBytes = 0xb50 };

static inline int beginProcursusBashGOT(uintptr_t image, vm_prot_t *previous) {
    vm_region_submap_info_data_64_t info = {};
    uintptr_t start = image + ProcursusBashGOT;
    if (!forkTrialProcursusBashImage(image) || forkTrialLeaf(start, ProcursusBashGOTBytes, &info) != KERN_SUCCESS ||
        info.inheritance != VM_INHERIT_COPY || (info.protection != VM_PROT_READ && info.protection != (VM_PROT_READ | VM_PROT_WRITE))) return ENOEXEC;
    *previous = info.protection;
    if (*previous & VM_PROT_WRITE) return 0;
    return mach_vm_protect(mach_task_self(), start, ProcursusBashGOTBytes, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) == KERN_SUCCESS ? 0 : EACCES;
}

static inline int endProcursusBashGOT(uintptr_t image, vm_prot_t previous) {
    if (previous & VM_PROT_WRITE) return 0;
    return mach_vm_protect(mach_task_self(), image + ProcursusBashGOT, ProcursusBashGOTBytes, FALSE, previous) == KERN_SUCCESS ? 0 : EACCES;
}
