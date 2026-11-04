// Inherited function pointers can refer to a loose simulator support library at its parent's slide.
// Give the observer's own copy a second mapping there, so code and data aliases still use the child's broker and TLS state.
#pragma once
#include "../PrivateMachVM.h"
#include "../../../ProcessSupport/GuestExecutablePath.h"
#include <mach-o/dyld.h>
#include <stdint.h>

enum { TrialMaxSupportImages = 16 };
typedef struct { uint64_t header; uint8_t uuid[16]; } TrialSupportImage;
typedef struct { uint32_t count, reserved; TrialSupportImage images[TrialMaxSupportImages]; } TrialSupportImages;

static inline const uint8_t *trialImageUUID(const struct mach_header_64 *mh) {
    if (!mh || mh->magic != MH_MAGIC_64) return NULL;
    const uint8_t *p = (const void *)(mh + 1);
    size_t left = mh->sizeofcmds;
    for (unsigned i = 0; i < mh->ncmds; ++i) {
        const struct load_command *lc = (const void *)p;
        if (left < sizeof(*lc) || lc->cmdsize < sizeof(*lc) || lc->cmdsize > left) return NULL;
        if (lc->cmd == LC_UUID && lc->cmdsize == sizeof(struct uuid_command)) return ((const struct uuid_command *)p)->uuid;
        left -= lc->cmdsize;
        p += lc->cmdsize;
    }
    return NULL;
}

static inline bool trialRuntimeDylib(uint32_t index, const char *root) {
    const struct mach_header *h = _dyld_get_image_header(index);
    if (!h || h->magic != MH_MAGIC_64 || h->filetype != MH_DYLIB || (h->flags & MH_DYLIB_IN_CACHE)) return false;
    const char *path = _dyld_get_image_name(index);
    char resolved[PATH_MAX];
    return path && realpath(path, resolved) && LSBGuestPathInsideRoot(resolved, root);
}

static inline int trialCaptureSupportImages(const char *root, TrialSupportImages *images) {
    *images = (TrialSupportImages){};
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; ++i) {
        if (!trialRuntimeDylib(i, root)) continue;
        const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(i);
        const uint8_t *uuid = trialImageUUID(mh);
        if (!uuid) return ENOEXEC;
        if (images->count == TrialMaxSupportImages) return E2BIG;
        TrialSupportImage *entry = &images->images[images->count++];
        entry->header = (uintptr_t)mh;
        memcpy(entry->uuid, uuid, sizeof(entry->uuid));
    }
    return images->count ? 0 : ENOENT;
}

static inline int trialAliasImage(uint32_t index, uint64_t target) {
    const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(index);
    if ((uintptr_t)mh == target) return 0;
    intptr_t slide = _dyld_get_image_vmaddr_slide(index);
    if (slide < 0 || !target || target % vm_page_size) return EINVAL;
    const uint8_t *p = (const void *)(mh + 1);
    size_t left = mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds; ++i) {
        const struct load_command *lc = (const void *)p;
        if (left < sizeof(*lc) || lc->cmdsize < sizeof(*lc) || lc->cmdsize > left) return ENOEXEC;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const void *)p;
            if (lc->cmdsize < sizeof(*seg)) return ENOEXEC;
            if (seg->vmsize && strncmp(seg->segname, SEG_PAGEZERO, sizeof(seg->segname))) {
                uint64_t src, dst;
                if (__builtin_add_overflow(seg->vmaddr, (uint64_t)slide, &src) || src < (uintptr_t)mh ||
                    __builtin_add_overflow(target, src - (uintptr_t)mh, &dst) || seg->vmsize > UINT64_MAX - dst) return ERANGE;
                vm_prot_t current = 0, maximum = 0;
                mach_vm_address_t at = dst;
                kern_return_t kr = mach_vm_remap(mach_task_self(), &at, seg->vmsize, 0, VM_FLAGS_FIXED, mach_task_self(), src, FALSE,
                                                  &current, &maximum, VM_INHERIT_NONE);
                if (kr) return kr == KERN_NO_SPACE || kr == KERN_INVALID_ADDRESS ? EADDRINUSE : EACCES;
                if (at != dst || ((seg->initprot & VM_PROT_EXECUTE) && !(current & VM_PROT_EXECUTE))) return EACCES;
            }
        }
        left -= lc->cmdsize;
        p += lc->cmdsize;
    }
    return left ? ENOEXEC : 0;
}

static inline int trialAliasSupportImages(const char *root, const TrialSupportImages *images, uint64_t *unavailable) {
    if (!root || !images->count || images->count > TrialMaxSupportImages || images->reserved) return EINVAL;
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < images->count; ++i) {
        uint32_t found = count;
        for (uint32_t j = 0; j < count; ++j) {
            if (!trialRuntimeDylib(j, root)) continue;
            const uint8_t *uuid = trialImageUUID((const void *)_dyld_get_image_header(j));
            if (uuid && !memcmp(uuid, images->images[i].uuid, 16)) {
                if (found != count) return ENOEXEC;
                found = j;
            }
        }
        if (found == count) return ENOENT;
        *unavailable = images->images[i].header;
        int error = trialAliasImage(found, images->images[i].header);
        if (error) return error;
    }
    return 0;
}
