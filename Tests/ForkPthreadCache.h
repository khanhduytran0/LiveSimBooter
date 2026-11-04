// Included by ForkPthreadRegistry.h after its checked image/section reader.
#pragma once
#include "ForkPthreadCacheState.h"
#include "../ProcessSupport/SlidableCache.h"
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int forkCacheSpanAccess(uintptr_t address, size_t bytes, bool write) {
    if (!bytes || bytes > UINTPTR_MAX - address) return EPROTO;
    uint64_t end = address + bytes;
    for (uint64_t p = address; p < end;) {
        mach_vm_address_t start = p;
        mach_vm_size_t size = 0;
        natural_t depth = UINT32_MAX;
        vm_region_submap_info_data_64_t info = {};
        mach_msg_type_number_t words = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &start, &size, &depth, (vm_region_recurse_info_t)&info, &words) ||
            words < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info.is_submap || start > p || size > UINT64_MAX - start || start + size <= p ||
            !(info.protection & VM_PROT_READ) || (info.protection & VM_PROT_EXECUTE)) return EACCES;
        if (write && (!(info.protection & VM_PROT_WRITE) || info.inheritance != VM_INHERIT_COPY ||
                      info.share_mode == SM_SHARED || info.share_mode == SM_TRUESHARED)) return ENOTSUP;
        p = start + size;
    }
    return 0;
}

static inline int forkCacheAddOwner(ForkCacheCheckpointPlan *plan, const ForkPthreadTarget *fn) {
    uintptr_t image = fn->image & ~UINT64_C(1);
    Dl_info owner;
    uint8_t uuid[16];
    if (!(fn->image & 1) || !dladdr((void *)fn->pc, &owner) || (uintptr_t)owner.dli_fbase != image ||
        forkPthreadImage(fn->pc, fn->image, uuid, NULL, NULL) || memcmp(uuid, fn->uuid, 16)) return ENOTSUP;
    struct mach_header_64 header;
    _Alignas(8) uint8_t commands[16384];
    if (trialReadLocal(image, &header, sizeof(header)) || header.cpusubtype != CPU_SUBTYPE_ARM64_ALL ||
        header.sizeofcmds > sizeof(commands) || trialReadLocal(image + sizeof(header), commands, header.sizeofcmds)) return ENOTSUP;
    size_t off = 0;
    uint64_t slide = 0;
    bool foundText = false;
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        const struct load_command *cmd = (const void *)(commands + off);
        // forkPthreadImage checked this exact header, all load-command and section extents, and the image UUID.
        off += cmd->cmdsize;
        if (cmd->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *seg = (const void *)cmd;
        if (!strncmp(seg->segname, SEG_TEXT, 16)) { slide = image - seg->vmaddr; foundText = true; continue; }
        if (!(seg->maxprot & VM_PROT_WRITE) || !seg->vmsize) continue;
        bool constant = !strncmp(seg->segname, "__DATA_CONST", 16);
        if (!constant && strncmp(seg->segname, "__DATA", 16) && strncmp(seg->segname, "__DATA_DIRTY", 16)) return ENOTSUP;
        if (!foundText || !seg->nsects || seg->vmsize > UINT64_MAX - seg->vmaddr) return ENOEXEC;
        const struct section_64 *sections = (const void *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; ++j) {
            const struct section_64 *s = &sections[j];
            if (!s->size) continue;
            if (strncmp(s->segname, seg->segname, 16) || s->addr < seg->vmaddr || s->size > seg->vmsize ||
                s->addr - seg->vmaddr > seg->vmsize - s->size || slide > UINT64_MAX - s->addr) return ENOEXEC;
            unsigned type = s->flags & SECTION_TYPE;
            if (constant) {
                // Import slots belong to the local dyld. Keep its bindings and compare other constant data exactly.
                if (!strncmp(s->sectname, "__got", 16) && type == S_NON_LAZY_SYMBOL_POINTERS) continue;
                if (strncmp(s->sectname, "__const", 16) || type != S_REGULAR) return ENOTSUP;
            } else if ((strncmp(s->sectname, "__data", 16) && strncmp(s->sectname, "__common", 16) && strncmp(s->sectname, "__bss", 16)) ||
                       (type != S_REGULAR && type != S_ZEROFILL)) return ENOTSUP;
            if (plan->count == ForkCacheMaxSpans || s->size > ForkCacheMaxBytes - plan->used) return E2BIG;
            uintptr_t address = slide + s->addr;
            int error = forkCacheSpanAccess(address, s->size, !constant);
            if (error) return error;
            for (unsigned k = 0; k < plan->count; ++k) {
                ForkCacheSpan prev = plan->spans[k];
                if (address < prev.address + prev.bytes && prev.address < address + s->size) return EPROTO;
            }
            plan->spans[plan->count++] = (ForkCacheSpan){image, address, (uint32_t)s->size, constant ? ForkCacheCompare : ForkCacheCopy};
            plan->used += (uint32_t)s->size;
        }
    }
    return 0;
}

static inline int forkCachePlan(const ForkPthreadState *state, uint32_t prefix, ForkCacheCheckpointPlan *plan) {
    *plan = (ForkCacheCheckpointPlan){};
    if (prefix > state->count || state->count > ForkPthreadMaxRegistrations || state->reserved) return EPROTO;
    uint64_t seen[ForkCacheMaxOwners];
    unsigned n = 0;
    for (unsigned i = prefix; i < state->count; ++i) for (unsigned k = 0; k < 3; ++k) {
        const ForkPthreadTarget *fn = &state->entries[i].functions[k];
        if (!fn->pc || !(fn->image & 1)) continue;
        for (unsigned j = 0; j < prefix; ++j) for (unsigned l = 0; l < 3; ++l)
            if (state->entries[j].functions[l].image == fn->image) return ENOTSUP; // Active observer registration shares the owner.
        unsigned j = 0;
        while (j < n && seen[j] != fn->image) ++j;
        if (j < n) continue;
        if (n == ForkCacheMaxOwners) return E2BIG;
        seen[n++] = fn->image;
        int error = forkCacheAddOwner(plan, fn);
        if (error) return error;
    }
    return 0;
}

static inline int forkCacheCapturePrepared(const ForkCacheCheckpointPlan *plan, ForkCachePacket *packet) {
    // The caller built the local plan before taking libc locks. Clear unused payload bytes before putting the fixed record on the wire.
    memset(packet, 0, sizeof(*packet));
    packet->plan = *plan;
    if (plan->count > ForkCacheMaxSpans || plan->used > ForkCacheMaxBytes) return EPROTO;
    size_t cursor = 0;
    for (unsigned i = 0; i < plan->count; ++i) {
        ForkCacheSpan s = plan->spans[i];
        if (!s.bytes || s.bytes > plan->used - cursor || (s.kind != ForkCacheCopy && s.kind != ForkCacheCompare)) return EPROTO;
        if (trialReadLocal(s.address, packet->bytes + cursor, s.bytes)) return EFAULT;
        cursor += s.bytes;
    }
    return cursor == plan->used ? 0 : EPROTO;
}

extern const void *_dyld_get_shared_cache_range(size_t *);
static inline int forkCacheBackingFD(LSBSlidableDataHeader *header, int *fd) {
    *fd = -1;
    size_t cacheLength = 0;
    uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&cacheLength);
    uint8_t uuid[16];
    if (!base || cacheLength < LSB_NATIVE_SUPPORT_CACHE_SPAN || trialReadLocal(base + 88, uuid, sizeof(uuid))) return ENOTSUP;
    const char *root = getenv("DYLD_ROOT_PATH");
    char name[LSB_SLIDABLE_DATA_NAME_CAPACITY], path[PATH_MAX];
    LSBSlidableDataFileName(uuid, base, name);
    int length = root ? snprintf(path, sizeof(path), "%s/System/Library/Caches/com.apple.dyld/%s", root, name) : -1;
    if (length < 0 || (size_t)length >= sizeof(path)) return EINVAL;
    int file = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0) return errno;
    struct stat st;
    if (fstat(file, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0222) || pread(file, header, sizeof(*header), 0) != sizeof(*header) ||
        header->actualBase != base || !LSBValidateSlidableDataHeaderWithProfile(header, st.st_size, uuid, LSB_SLIDABLE_CACHE_PREFERRED_BASE,
                                                                              LSB_NATIVE_SUPPORT_CACHE_SPAN, LSBSlidableCacheNativeSupport)) {
        close(file); return ENOTSUP;
    }
    *fd = file;
    return 0;
}

static inline int forkCachePristine(const LSBSlidableDataHeader *header, int fd, const ForkCacheSpan *span, const uint8_t *current, const uint8_t *source) {
    if (!memcmp(current, source, span->bytes)) return 0;
    for (unsigned i = 0; i < header->mappingCount; ++i) {
        const LSBSlidableDataMapping *m = &header->mappings[i];
        uint64_t start = header->actualBase + (m->cacheAddress - header->preferredBase);
        if (span->address < start || span->bytes > m->size || span->address - start > m->size - span->bytes) continue;
        uint64_t offset = m->backingFileOffset + (span->address - start);
        uint8_t initial[512];
        for (uint32_t p = 0; p < span->bytes;) {
            size_t n = span->bytes - p < sizeof(initial) ? span->bytes - p : sizeof(initial);
            if (pread(fd, initial, n, (off_t)(offset + p)) != (ssize_t)n) return EIO;
            // Preserve any locally initialized word (including pointers and lock storage) unless already identical to the checkpoint.
            for (size_t j = 0; j < n;) {
                size_t word = n - j < sizeof(uint64_t) ? n - j : sizeof(uint64_t);
                if (memcmp(current + p + j, initial + j, word) && memcmp(current + p + j, source + p + j, word)) return EBUSY;
                j += word;
            }
            p += (uint32_t)n;
        }
        return 0;
    }
    return ERANGE;
}

static inline int forkCachePreflight(const ForkPthreadState *state, unsigned prefix, const ForkCachePacket *packet, ForkCacheObserver *observer) {
    ForkCacheCheckpointPlan local;
    int error = forkCachePlan(state, prefix, &local);
    if (error) return error;
    if (!packet) return local.count ? ENOTSUP : 0;
    if (memcmp(&local, &packet->plan, sizeof(local))) return EPROTO;
    if ((error = forkCacheCapturePrepared(&local, &observer->original)) || !local.count) return error;
    LSBSlidableDataHeader backing;
    int fd = -1;
    error = forkCacheBackingFD(&backing, &fd);
    for (unsigned i = 0, cursor = 0; !error && i < local.count; ++i) {
        ForkCacheSpan s = local.spans[i];
        const uint8_t *current = observer->original.bytes + cursor, *source = packet->bytes + cursor;
        if (s.kind == ForkCacheCompare) error = memcmp(current, source, s.bytes) ? ENOTSUP : 0;
        else error = forkCachePristine(&backing, fd, &s, current, source);
        if (error) fprintf(stderr, "fork trial cache owner=%#llx span=%#llx size=%u kind=%u preflight_errno=%d\n", s.image, s.address, s.bytes, s.kind, error);
        cursor += s.bytes;
    }
    if (fd >= 0) close(fd);
    return error;
}

static inline int forkCacheApplyPrepared(const ForkCacheObserver *observer, const ForkCachePacket *packet) {
    if (!packet) return observer->original.plan.count ? EINVAL : 0;
    if (memcmp(&observer->original.plan, &packet->plan, sizeof(packet->plan))) return EPROTO;
    // All ranges and current protections must still match after the observer's own prepare. Only then update its private CoW mappings.
    for (unsigned i = 0, cursor = 0; i < packet->plan.count; ++i) {
        ForkCacheSpan s = packet->plan.spans[i];
        int error = forkCacheSpanAccess(s.address, s.bytes, s.kind == ForkCacheCopy);
        if (error || memcmp((const void *)(uintptr_t)s.address, observer->original.bytes + cursor, s.bytes)) return error ? error : EBUSY;
        cursor += s.bytes;
    }
    for (unsigned i = 0, cursor = 0; i < packet->plan.count; ++i) {
        ForkCacheSpan s = packet->plan.spans[i];
        if (s.kind == ForkCacheCopy) memcpy((void *)(uintptr_t)s.address, packet->bytes + cursor, s.bytes);
        cursor += s.bytes;
    }
    return 0;
}
