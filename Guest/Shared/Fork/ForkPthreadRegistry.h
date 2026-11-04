#pragma once
#include "ForkTrace.h"
#include "ForkPthreadState.h"
#include "ForkPthreadCacheState.h"
#include "ForkLibcChildBranch.h"
#include "../PrivateMachVM.h"
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint64_t functions[3]; } ForkNativeRegistration;
typedef struct {
    uint64_t pthread;
    uint32_t pthreadLock, handlersLock;
    uint64_t count;
    ForkNativeRegistration small[10];
    uint64_t entries;
} ForkNativeAtFork;
_Static_assert(offsetof(ForkNativeAtFork, entries) == 0x108 && sizeof(ForkNativeRegistration) == 24, "23B50/24A435 atfork registry");
typedef struct { uint64_t object; ForkNativeAtFork info; } ForkPthreadView;
typedef struct { uint64_t entries; uint32_t prefix, count; uint64_t suppressed[ForkPthreadMaxRegistrations][2]; ForkCacheObserver cache; } ForkPthreadStage;

static inline int forkPthreadSingleMain(void) {
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;
    if (!pthread_main_np() || task_threads(mach_task_self(), &threads, &count)) return EBUSY;
    for (unsigned i = 0; i < count; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(*threads));
    return count == 1 ? 0 : EBUSY;
}

static inline uintptr_t forkPthreadPC(uintptr_t pc) {
    __asm__("xpaci %0" : "+r"(pc));
    return pc;
}

// libplatform's _os_once_callout publishes (quiescent_counter << 2) | 1 after initialization.
// Both 23B50 and 24A435 _os_once accept that state before eventually replacing it with UINT64_MAX.
static inline bool forkPthreadOnceComplete(uint64_t once) { return once == UINT64_MAX || (once & 3) == 1; }

// The native pthread_atfork+0x28 loads _os_alloc_once_table, then checks slot 16.
// The guest kernel veneer exposes a different table, so resolve the one native pthread actually uses.
static inline int forkPthreadReadView(ForkPthreadView *view) {
    *view = (ForkPthreadView){};
    uintptr_t start = forkPthreadPC((uintptr_t)&pthread_atfork);
    uint32_t code[17];
    if (trialReadLocal(start, code, sizeof(code))) return EFAULT;
    if (code[0] != 0xd503237f || code[7] != 0xaa0203f3 || code[8] != 0xaa0103f4 || code[9] != 0xaa0003f6 ||
        code[12] != 0xf9408109 || code[13] != 0xb100053f || code[15] != 0xf9408515 || code[16] != 0x910032a0) return ENOTSUP;
    uintptr_t got = trialForkHandlerSlot(start + 0x28, code[10], code[11]), table;
    if (!got || trialReadLocal(got, &table, sizeof(table)) || !table || (table & 7) || table > UINTPTR_MAX - 0x110) return EFAULT;
    struct OnceSlot { uint64_t once; uintptr_t object; } readable;
    const struct OnceSlot *slot = (const void *)(table + 0x100);
    if (trialReadLocal((uintptr_t)slot, &readable, sizeof(readable))) return EFAULT;
    uint64_t once = __atomic_load_n(&slot->once, __ATOMIC_ACQUIRE);
    if (!once) return readable.object ? EAGAIN : 0;
    if (!forkPthreadOnceComplete(once)) return EAGAIN;
    view->object = slot->object;
    if (!view->object) return EAGAIN;
    if (trialReadLocal(view->object, &view->info, sizeof(view->info))) return EFAULT;
    if (view->info.count > ForkPthreadMaxRegistrations) return E2BIG;
    if (view->info.count && (!view->info.entries || view->info.entries > UINTPTR_MAX - view->info.count * sizeof(ForkNativeRegistration))) return EPROTO;
    if (view->info.count && view->info.count <= 10 && view->info.entries != view->object + offsetof(ForkNativeAtFork, small)) return EAGAIN;
    if (view->info.count > 10 && (view->info.entries & (vm_page_size - 1))) return EPROTO;
    return 0;
}

static inline int forkPthreadReadRaw(const ForkPthreadView *view, ForkNativeRegistration raw[ForkPthreadMaxRegistrations]) {
    return view->info.count ? trialReadLocal(view->info.entries, raw, view->info.count * sizeof(*raw)) : 0;
}

// Native dyld is retained but its images are invisible to guest dladdr. Consult its signed cache image text table.
// Callbacks from this native cache can match the observer's existing registrations only.
static inline uintptr_t forkPthreadNativeImage(uintptr_t pc) {
    typedef struct { char magic[16]; uint32_t mappings, mappingCount; uint8_t reserved[112]; uint64_t textOffset, textCount; } CachePrefix;
    typedef struct { uint64_t address, size, fileOffset; uint32_t maximum, current; } CacheMapping;
    typedef struct { uint8_t uuid[16]; uint64_t address; uint32_t size, path; } CacheText;
    _Static_assert(offsetof(CachePrefix, textOffset) == 136 && sizeof(CacheMapping) == 32 && sizeof(CacheText) == 32, "dyld_cache_format.h");
    uint64_t base = 0;
    CachePrefix header;
    CacheMapping first;
    if (__shared_region_check_np(&base) || !base || pc < base || trialReadLocal(base, &header, sizeof(header)) ||
        memcmp(header.magic, "dyld_v1", 7) || !header.mappingCount || header.mappings < sizeof(header) || header.mappings > 16384 ||
        !header.textCount || header.textCount > 16384 || base > UINTPTR_MAX - header.mappings ||
        trialReadLocal(base + header.mappings, &first, sizeof(first)) || first.fileOffset || base < first.address ||
        header.textOffset > first.size || header.textCount * sizeof(CacheText) > first.size - header.textOffset ||
        first.size > UINTPTR_MAX - base) return 0;
    uint64_t slide = base - first.address, unslid = pc - slide;
    CacheText items[64];
    for (size_t i = 0; i < header.textCount; i += 64) {
        size_t n = header.textCount - i < 64 ? header.textCount - i : 64;
        if (trialReadLocal(base + header.textOffset + i * sizeof(CacheText), items, n * sizeof(CacheText))) return 0;
        for (size_t j = 0; j < n; ++j)
            if (unslid >= items[j].address && unslid - items[j].address < items[j].size) return slide + items[j].address;
    }
    return 0;
}

// Validate the actual VM leaf and its owning Mach-O section. Private images must be inherited, and newly registered guest-cache owners need a state plan.
typedef int (*ForkPthreadCovers)(void *, uint64_t, uint64_t);
static inline int forkPthreadImage(uintptr_t pc, uintptr_t taggedImage, uint8_t uuid[16], ForkPthreadCovers covers, void *context) {
    bool cached = (taggedImage & 1) != 0;
    uintptr_t image = taggedImage & ~(uintptr_t)1;
    struct mach_header_64 header;
    if ((pc & 3) || (covers && (covers(context, pc, 4) || covers(context, image, sizeof(header))))) return ENOTSUP;
    if (trialReadLocal(image, &header, sizeof(header))) return EFAULT;
    if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 || (!cached && header.cpusubtype != CPU_SUBTYPE_ARM64_ALL) ||
        (header.filetype != MH_DYLIB && header.filetype != MH_EXECUTE) || ((header.flags & MH_DYLIB_IN_CACHE) != 0) != cached) return ENOTSUP;
    _Alignas(8) uint8_t commands[0x4000];
    if (header.sizeofcmds > sizeof(commands)) return E2BIG;
    if (image > UINTPTR_MAX - sizeof(header) - header.sizeofcmds || (covers && covers(context, image, sizeof(header) + header.sizeofcmds))) return ENOTSUP;
    if (trialReadLocal(image + sizeof(header), commands, header.sizeofcmds)) return EFAULT;
    bool foundUUID = false, foundText = false, executable = false;
    uintptr_t slide = 0;
    size_t off = 0;
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        const struct load_command *load = (const void *)(commands + off);
        if (header.sizeofcmds - off < sizeof(*load) || load->cmdsize < sizeof(*load) || load->cmdsize > header.sizeofcmds - off || (load->cmdsize & 7)) return ENOEXEC;
        if (load->cmd == LC_UUID) {
            const struct uuid_command *id = (const void *)load;
            if (foundUUID || load->cmdsize != sizeof(*id)) return ENOEXEC;
            memcpy(uuid, id->uuid, 16);
            foundUUID = true;
        } else if (load->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const void *)load;
            if (load->cmdsize < sizeof(*segment) || segment->nsects > (load->cmdsize - sizeof(*segment)) / sizeof(struct section_64)) return ENOEXEC;
            if (!strncmp(segment->segname, SEG_TEXT, sizeof(segment->segname))) {
                if (foundText || (!cached && segment->fileoff) || segment->vmaddr > image) return ENOEXEC;
                slide = image - segment->vmaddr;
                foundText = true;
            }
            if (segment->initprot & VM_PROT_EXECUTE) {
                if (!foundText || pc < slide) return ENOTSUP;
                const struct section_64 *sections = (const void *)(segment + 1);
                for (uint32_t j = 0; j < segment->nsects; ++j) {
                    uint64_t address = pc - slide;
                    if (!(sections[j].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS))) continue;
                    if (sections[j].addr < segment->vmaddr || sections[j].size > segment->vmsize ||
                        sections[j].addr - segment->vmaddr > segment->vmsize - sections[j].size) return ENOEXEC;
                    if (address >= sections[j].addr && sections[j].size >= 4 && address - sections[j].addr <= sections[j].size - 4) executable = true;
                }
            }
        }
        off += load->cmdsize;
    }
    if (off != header.sizeofcmds || !foundUUID || !foundText || !executable) return ENOEXEC;
    mach_vm_address_t address = pc;
    mach_vm_size_t size = 0;
    natural_t depth = UINT32_MAX;
    vm_region_submap_info_data_64_t info = {};
    mach_msg_type_number_t words = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t)&info, &words) ||
        words < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info.is_submap || address > pc || size < 4 || pc - address > size - 4 ||
        (info.protection & (VM_PROT_EXECUTE | VM_PROT_WRITE)) != VM_PROT_EXECUTE) return EACCES;
    return 0;
}

static inline int forkPthreadCapture(ForkPthreadState *state) {
    *state = (ForkPthreadState){};
    ForkPthreadView view;
    ForkNativeRegistration raw[ForkPthreadMaxRegistrations];
    int error = forkPthreadReadView(&view);
    if (!error) error = forkPthreadReadRaw(&view, raw);
    if (error) return error;
    for (unsigned i = 0; i < view.info.count; ++i) for (unsigned k = 0; k < 3; ++k) {
        ForkPthreadTarget *fn = &state->entries[i].functions[k];
        fn->pc = forkPthreadPC(raw[i].functions[k]);
        if (!fn->pc) continue;
        Dl_info image;
        struct mach_header_64 header;
        uintptr_t owner = dladdr((const void *)fn->pc, &image) ? (uintptr_t)image.dli_fbase : 0;
        if (!owner) owner = forkPthreadNativeImage(fn->pc);
        if (!owner || trialReadLocal(owner, &header, sizeof(header))) continue;
        // Header VAs are aligned. Mark cached images for the separate owner-state check when extending the observer's registrations.
        uintptr_t tagged = owner | ((header.flags & MH_DYLIB_IN_CACHE) != 0);
        uint8_t uuid[16];
        if (!forkPthreadImage(fn->pc, tagged, uuid, NULL, NULL)) {
            fn->image = tagged;
            memcpy(fn->uuid, uuid, 16);
        }
    }
    state->count = (uint32_t)view.info.count;
    return 0;
}

// Ordinary fork holds handlersLock. vfork skips user prepare handlers, so recheck its registry only with all peer threads parked.
static inline int forkPthreadCheckPrepared(const ForkPthreadState *state, bool vforkPeersParked) {
    ForkPthreadView view;
    ForkNativeRegistration raw[ForkPthreadMaxRegistrations];
    int error = forkPthreadReadView(&view);
    if (error) return error;
    if (view.info.count != state->count || view.info.pthread != (uintptr_t)pthread_self()) return EAGAIN;
    if (vforkPeersParked ? (!view.info.pthreadLock || view.info.handlersLock) : !view.info.handlersLock) return EAGAIN;
    error = forkPthreadReadRaw(&view, raw);
    if (error) return error;
    for (unsigned i = 0; i < state->count; ++i) for (unsigned k = 0; k < 3; ++k)
        if (state->entries[i].functions[k].pc != forkPthreadPC(raw[i].functions[k])) return EAGAIN;
    return 0;
}

static inline int forkPthreadFinish(ForkPthreadStage *stage) {
    if (!stage->entries || stage->count == stage->prefix) return 0;
    ForkPthreadView view;
    int error = forkPthreadSingleMain();
    if (!error) error = forkPthreadReadView(&view);
    if (error) return error;
    if (view.info.entries != stage->entries || view.info.count != stage->count) return EAGAIN;
    if (view.info.handlersLock && view.info.pthread != (uintptr_t)pthread_self()) return EBUSY;
    ForkNativeRegistration *entries = (void *)(uintptr_t)stage->entries;
    for (unsigned i = stage->prefix; i < stage->count; ++i) if (entries[i].functions[0] || entries[i].functions[1]) return EPROTO;
    for (unsigned i = stage->prefix; i < stage->count; ++i) memcpy(entries[i].functions, stage->suppressed[i], sizeof(stage->suppressed[i]));
    stage->entries = 0;
    return 0;
}

static inline void forkPthreadReport(const char *role, const ForkPthreadState *state) {
    if (forkTrialTraceSetting() != ForkTraceVerbose) return;
    for (unsigned i = 0; i < state->count; ++i) for (unsigned k = 0; k < 3; ++k) {
        const ForkPthreadTarget *fn = &state->entries[i].functions[k];
        if (!fn->pc) continue;
        uint64_t base = fn->image & ~UINT64_C(1);
        uint64_t uuid[2];
        memcpy(uuid, fn->uuid, sizeof(uuid));
        fprintf(stderr, "fork trial atfork %s[%u,%u] pc=%#llx base=%#llx offset=%#llx cache=%u uuid=%016llx:%016llx\n",
                role, i, k, fn->pc, base, base && fn->pc >= base ? fn->pc - base : 0, (unsigned)(fn->image & 1), uuid[0], uuid[1]);
    }
}

#include "ForkPthreadCache.h"

static inline int forkPthreadPrefix(const ForkPthreadState *parent, uint32_t *prefix) {
    *prefix = 0;
    if (parent->reserved || parent->count > ForkPthreadMaxRegistrations) return EPROTO;
    ForkPthreadState local;
    int error = forkPthreadSingleMain();
    if (!error) error = forkPthreadCapture(&local);
    if (error) return error;
    *prefix = local.count;
    forkPthreadReport("parent", parent);
    forkPthreadReport("observer", &local);
    if (local.count > parent->count || memcmp(parent->entries, local.entries, local.count * sizeof(local.entries[0]))) return ENOTSUP;
    for (unsigned i = 0; i < local.count; ++i) for (unsigned k = 0; k < 3; ++k) {
        const ForkPthreadTarget *fn = &local.entries[i].functions[k];
        if (fn->pc && !fn->image) return ENOTSUP;
    }
    return 0;
}

// A private, disposable, single-threaded observer only. Register through native pthread so its pointer conventions stay intact. Suppress
// inherited prepares during local prepare and parent handlers on an abandoned observer. Finish restores both just before libc's child branch.
static inline int forkPthreadStage(const ForkPthreadState *parent, ForkPthreadCovers covers, void *context, ForkPthreadStage *stage,
                                   const ForkCachePacket *cache) {
    if (!covers) return EINVAL;
    *stage = (ForkPthreadStage){};
    int error = forkPthreadPrefix(parent, &stage->prefix);
    if (!error) error = forkCachePreflight(parent, stage->prefix, cache, &stage->cache);
    if (error) return error;
    for (unsigned i = stage->prefix; i < parent->count; ++i) for (unsigned k = 0; k < 3; ++k) {
        const ForkPthreadTarget *fn = &parent->entries[i].functions[k];
        if (!fn->pc) continue;
        if (!fn->image) return ENOTSUP;
        bool cached = (fn->image & 1) != 0;
        if (cached) {
            Dl_info owner;
            if (!cache || !dladdr((void *)fn->pc, &owner) || (uintptr_t)owner.dli_fbase != (fn->image & ~UINT64_C(1))) return ENOTSUP;
        }
        uint8_t uuid[16];
        error = forkPthreadImage(fn->pc, fn->image, uuid, cached ? NULL : covers, context);
        if (error || memcmp(uuid, fn->uuid, sizeof(uuid))) return error ? error : ENOEXEC;
    }
    for (unsigned i = stage->prefix; i < parent->count; ++i) {
        const ForkPthreadTarget *fn = parent->entries[i].functions;
        error = pthread_atfork((void (*)(void))(uintptr_t)fn[0].pc, (void (*)(void))(uintptr_t)fn[1].pc, (void (*)(void))(uintptr_t)fn[2].pc);
        if (error) return error; // Native pthread has no deregistration API. Discard this unpublished child on any failure.
    }
    ForkPthreadView view;
    error = forkPthreadReadView(&view);
    if (error) return error;
    if (view.info.count != parent->count) return EAGAIN;
    ForkNativeRegistration *entries = (void *)(uintptr_t)view.info.entries;
    for (unsigned i = stage->prefix; i < parent->count; ++i) for (unsigned k = 0; k < 3; ++k)
        if (forkPthreadPC(entries[i].functions[k]) != parent->entries[i].functions[k].pc) return EPROTO;
    for (unsigned i = stage->prefix; i < parent->count; ++i) {
        memcpy(stage->suppressed[i], entries[i].functions, sizeof(stage->suppressed[i]));
        memset(entries[i].functions, 0, sizeof(stage->suppressed[i]));
    }
    stage->entries = view.info.entries;
    stage->count = parent->count;
    return 0;
}
