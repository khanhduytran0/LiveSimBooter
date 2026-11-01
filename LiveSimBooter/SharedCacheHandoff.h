#pragma once

#include "MainThreadBootstrap.h"
#include <stddef.h>
#include <stdint.h>

struct mach_header;
struct SimSyscallHelpers;

struct LSBDyldSimLaunch {
    using Main = int (*)(int, const char *const[], const char *const[], const char *const[]);
    using Entry = Main (*)(int, const char *const[], const char *const[], const char *const[], const mach_header *, const mach_header *,
                          uintptr_t, const SimSyscallHelpers *, uintptr_t *);

    Entry entry;
    int argc;
    const char *const *argv;
    const char *const *envp;
    const char *const *apple;
    const mach_header *executable;
    const mach_header *dyld;
    uintptr_t slide;
    const SimSyscallHelpers *helpers;
    lsb::MainThreadBootstrap thread;
    // Zero preserves the native stack. Nonzero is a guarded, 16-byte-aligned stack owned for the process lifetime.
    uintptr_t stackTop = 0;
    // The entire validated cache span, including gaps, becomes owned PROT_NONE memory before dyld maps its segments.
    uintptr_t cacheAddress = 0;
    size_t cacheSize = 0;
    // Retain native mappings for a disjoint ordinary low cache or a validated slidable reservation; false selects legacy detachment.
    bool retainHostCache = true;
};

// Returns an errno before entry with host TLS restored. Successful handoff never returns to host code.
int LSBDetachHostCacheAndEnter(const LSBDyldSimLaunch *launch);
