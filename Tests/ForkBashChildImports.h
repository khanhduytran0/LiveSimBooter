#pragma once
#include "ForkBashState.h"
#include "ForkProcursusBashGOT.h"
#include <limits.h>
#include <crt_externs.h>
#include <mach-o/nlist.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/wait.h>

// Release 5.2.21 has two-level binds to bootstrap dylibs the observer itself does not link. Keep the handles
// alive for the copied shell's lifetime. Resolve the system imports against the guest's initialized libraries.
static inline int loadProcursusBashImports(void **handles) {
    const char *data = getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY");
    if (!data || data[0] != '/') return ENOENT;
    const char *packages[] = {"libiosexec.1.dylib", "libreadline.8.dylib"};
    for (size_t i = 0; i < sizeof(packages) / sizeof(packages[0]); ++i) {
        char path[PATH_MAX];
        int length = snprintf(path, sizeof(path), "%s/var/jb/usr/lib/%s", data, packages[i]);
        if (length < 0 || (size_t)length >= sizeof(path)) return ENAMETOOLONG;
        handles[1 + i] = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
        if (!handles[1 + i]) {
            forkTrialTrace("fork trial cannot load %s: %s\n", path, dlerror());
            return ENOENT;
        }
    }
    handles[5] = dlopen("/usr/lib/libiconv.2.dylib", RTLD_LAZY | RTLD_LOCAL);
    handles[6] = RTLD_DEFAULT;
    handles[7] = dlopen("/usr/lib/libcharset.1.dylib", RTLD_LAZY | RTLD_LOCAL);
    return handles[5] && handles[7] ? 0 : ENOENT;
}

// The snapshot is not a registered dyld image in the observer. Resolve its original indirect-symbol table against
// the observer's initialized runtime before resuming expansion. Never enter the parent's dyld lazy binder.
static inline int rebindBashSymbolPointers(uintptr_t image, bool procursus) {
    const struct mach_header_64 *header = (const void *)image;
    const struct load_command *cmd = (const void *)(header + 1);
    const struct symtab_command *symbols = NULL;
    const struct dysymtab_command *dynamic = NULL;
    const struct segment_command_64 *linkedit = NULL;
    const struct section_64 *pointers[2] = {};
    uintptr_t text = 0;
    size_t remaining = header->sizeofcmds, sections = 0;
    for (unsigned i = 0; i < header->ncmds; ++i) {
        if (remaining < sizeof(*cmd) || cmd->cmdsize < sizeof(*cmd) || cmd->cmdsize > remaining) return ENOEXEC;
        if (cmd->cmd == LC_SYMTAB && cmd->cmdsize == sizeof(*symbols)) symbols = (const void *)cmd;
        if (cmd->cmd == LC_DYSYMTAB && cmd->cmdsize == sizeof(*dynamic)) dynamic = (const void *)cmd;
        if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const void *)cmd;
            if (cmd->cmdsize < sizeof(*seg) || seg->nsects > (cmd->cmdsize - sizeof(*seg)) / sizeof(struct section_64)) return ENOEXEC;
            if (!strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname))) text = seg->vmaddr;
            if (!strncmp(seg->segname, SEG_LINKEDIT, sizeof(seg->segname))) linkedit = seg;
            const struct section_64 *section = (const void *)(seg + 1);
            for (unsigned j = 0; j < seg->nsects; ++j) {
                unsigned type = section[j].flags & SECTION_TYPE;
                if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) continue;
                if (sections == 2 || strncmp(seg->segname, procursus ? "__DATA_CONST" : SEG_DATA, sizeof(seg->segname))) return ENOEXEC;
                if (procursus && (type != S_NON_LAZY_SYMBOL_POINTERS || strncmp(section[j].sectname, "__got", sizeof(section[j].sectname)) ||
                                  section[j].addr != text + ProcursusBashGOT || section[j].size != ProcursusBashGOTBytes)) return ENOEXEC;
                pointers[sections++] = section + j;
            }
        }
        remaining -= cmd->cmdsize;
        cmd = (const void *)((const char *)cmd + cmd->cmdsize);
    }
    if (remaining || sections != (procursus ? 1u : 2u) || !symbols || !dynamic || !linkedit || !text || text > image) return ENOEXEC;
    uint64_t fileEnd = linkedit->fileoff + linkedit->filesize;
    if (fileEnd < linkedit->fileoff || symbols->symoff < linkedit->fileoff || symbols->stroff < linkedit->fileoff ||
        dynamic->indirectsymoff < linkedit->fileoff || symbols->symoff + (uint64_t)symbols->nsyms * sizeof(struct nlist_64) > fileEnd ||
        (uint64_t)symbols->stroff + symbols->strsize > fileEnd || dynamic->indirectsymoff + (uint64_t)dynamic->nindirectsyms * sizeof(uint32_t) > fileEnd)
        return ENOEXEC;
    uintptr_t slide = image - text;
    uintptr_t file = slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *table = (const void *)(file + symbols->symoff);
    const char *strings = (const char *)(file + symbols->stroff);
    const uint32_t *indices = (const void *)(file + dynamic->indirectsymoff);
    // References from the helper go through dyld interposition, including guest PID, executable-access, kill, wait and exec.
    // dlsym alone can resolve the underlying libSystem implementation for these imports.
    static const struct { const char *name; const void *address; } local[] = {
        {"read", &read}, {"write", &write}, {"access", &access}, {"close", &close}, {"dup2", &dup2}, {"execve", &execve},
        {"getpid", &getpid}, {"getppid", &getppid}, {"getpgrp", &getpgrp}, {"setpgid", &setpgid},
        {"kill", &kill}, {"killpg", &killpg}, {"raise", &raise}, {"sigaction", &sigaction},
        {"waitpid", &waitpid}, {"fork", &fork}, {"stat", &stat}, {"lstat", &lstat}, {"fstat", &fstat}, {"fcntl", &fcntl}, {"open", &open},
        {"umask", &umask}, {"isatty", &isatty}, {"ttyname", &ttyname}, {"ioctl", &ioctl}, {"tcgetattr", &tcgetattr}, {"tcsetattr", &tcsetattr},
        {"tcgetpgrp", &tcgetpgrp}, {"tcsetpgrp", &tcsetpgrp}, {"dlopen", &dlopen}, {"exit", &exit}, {"fopen", &fopen}, {"unlink", &unlink}
    };
    void *handles[8] = {}; // LC_LOAD_DYLIB ordinals in the pinned Procursus executable.
    if (procursus) {
        int error = loadProcursusBashImports(handles);
        if (error) return error;
    }
    for (size_t i = 0; i < sections; ++i) {
        const struct section_64 *s = pointers[i];
        size_t count = s->size / sizeof(uintptr_t);
        vm_region_submap_info_data_64_t info = {};
        if (s->size % sizeof(uintptr_t) || s->reserved1 > dynamic->nindirectsyms || count > dynamic->nindirectsyms - s->reserved1 ||
            forkTrialLeaf(slide + s->addr, s->size, &info) != KERN_SUCCESS || info.protection != (VM_PROT_READ | VM_PROT_WRITE) ||
            info.inheritance != VM_INHERIT_COPY) return ENOEXEC;
        uintptr_t *slots = (void *)(slide + s->addr);
        for (size_t j = 0; j < count; ++j) {
            uint32_t index = indices[s->reserved1 + j];
            if (index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) {
                if (procursus) return ENOEXEC; // Every slot in this GOT is an external bind.
                continue;
            }
            if (index >= symbols->nsyms) return ENOEXEC;
            uint32_t offset = table[index].n_un.n_strx;
            if (offset >= symbols->strsize || !memchr(strings + offset, 0, symbols->strsize - offset)) return ENOEXEC;
            const char *name = strings + offset;
            if (!strcmp(name, "dyld_stub_binder")) continue; // Every lazy slot is populated below.
            if (*name++ != '_') return ENOEXEC;
            const void *address = NULL;
            if (!strcmp(name, "environ")) address = _NSGetEnviron();
            if (procursus && !strcmp(name, "ie_execve")) address = &execve;
            if (procursus && !strcmp(name, "faccessat")) address = &faccessat;
            for (size_t k = 0; !address && k < sizeof(local) / sizeof(local[0]); ++k)
                if (!strcmp(name, local[k].name)) address = local[k].address;
            unsigned ordinal = GET_LIBRARY_ORDINAL(table[index].n_desc);
            if (procursus && (ordinal >= sizeof(handles) / sizeof(handles[0]) || !handles[ordinal])) return ENOEXEC;
            if (!address) address = dlsym(procursus ? handles[ordinal] : RTLD_DEFAULT, name);
            if (!address) {
                fprintf(stderr, "fork trial missing child import %s\n", name);
                return ENOENT;
            }
            slots[j] = (uintptr_t)address;
        }
    }
    if (procursus) {
        // dyld_info -fixups lists exactly one external bind outside __got, a readline completion callback in __data.
        vm_region_submap_info_data_64_t info = {};
        uintptr_t slot = image + 0xa46f8;
        if (forkTrialLeaf(slot, sizeof(uintptr_t), &info) != KERN_SUCCESS || info.protection != (VM_PROT_READ | VM_PROT_WRITE) ||
            info.inheritance != VM_INHERIT_COPY) return ENOEXEC;
        *(uintptr_t *)slot = *(const uintptr_t *)(image + 0xa05a0); // __got._rl_complete
    }
    return 0;
}

static inline int rebindEarlyBashImports(uintptr_t image) {
    if (!forkTrialProcursusBashImage(image)) return forkTrialBashImage(image) ? rebindBashSymbolPointers(image, false) : ENOEXEC;
    vm_prot_t previous;
    int error = beginProcursusBashGOT(image, &previous);
    if (error) return error;
    error = rebindBashSymbolPointers(image, true);
    int restore = endProcursusBashGOT(image, previous);
    return error ? error : restore;
}
