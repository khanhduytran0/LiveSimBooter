#pragma once
#include "ForkBashState.h"
#include <crt_externs.h>
#include <mach-o/nlist.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/wait.h>

// The snapshot is not a registered dyld image in the observer. Resolve its original indirect-symbol table against
// the observer's initialized runtime before resuming expansion. Never enter the parent's dyld lazy binder.
static inline int rebindEarlyBashImports(uintptr_t image) {
    if (!forkTrialBashImage(image)) return ENOEXEC;
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
                if (sections == 2 || strncmp(seg->segname, SEG_DATA, sizeof(seg->segname))) return ENOEXEC;
                pointers[sections++] = section + j;
            }
        }
        remaining -= cmd->cmdsize;
        cmd = (const void *)((const char *)cmd + cmd->cmdsize);
    }
    if (remaining || sections != 2 || !symbols || !dynamic || !linkedit || !text || text > image) return ENOEXEC;
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
            if (index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
            if (index >= symbols->nsyms) return ENOEXEC;
            uint32_t offset = table[index].n_un.n_strx;
            if (offset >= symbols->strsize || !memchr(strings + offset, 0, symbols->strsize - offset)) return ENOEXEC;
            const char *name = strings + offset;
            if (!strcmp(name, "dyld_stub_binder")) continue; // Every lazy slot is populated below.
            if (*name++ != '_') return ENOEXEC;
            const void *address = NULL;
            if (!strcmp(name, "environ")) address = _NSGetEnviron();
            for (size_t k = 0; !address && k < sizeof(local) / sizeof(local[0]); ++k)
                if (!strcmp(name, local[k].name)) address = local[k].address;
            if (!address) address = dlsym(RTLD_DEFAULT, name);
            if (!address) {
                fprintf(stderr, "fork trial missing child import %s\n", name);
                return ENOENT;
            }
            slots[j] = (uintptr_t)address;
        }
    }
    return 0;
}
