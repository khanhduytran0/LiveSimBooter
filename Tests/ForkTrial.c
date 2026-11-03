// Built into the acceptance-test shim only. Prepare the offer before libc enters its atfork critical section.
#include "ForkTrialWire.h"
#include "ForkBashState.h"
#include "ForkBashEarlyHeap.h"
#include "../Guest/ChildWait.h"
#include "../Guest/ForkContext.h"
#include "../Guest/ProcessIdentity.h"
#include "../Guest/PrivateMachVM.h"
#include "../ProcessSupport/GuestImagePlacement.h"
#include <crt_externs.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <uuid/uuid.h>

extern int LSBPosixSpawn(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const [], char *const []);

static const char option[] = "LSB_FORK_SNAPSHOT_PROBE";
static const char helper[] = LSB_FORK_BOOTSTRAP_EXECUTABLE;

// An observer's registered executable remains the bootstrap. The copied image is not on dyld's image list.
static uintptr_t resumedBash;
static unsigned resumedDepth;
static int prepareResumedBash(LSBForkAttempt *attempt, const LSBForkContext *caller);

int LSBTrialAdoptBash(uintptr_t image, unsigned depth) {
    uintptr_t observer = (uintptr_t)_NSGetMachExecuteHeader();
    if (!depth || depth > TrialMaxDepth || resumedBash || observer < LSB_GUEST_MAIN_IMAGE_CAPACITY || image != observer - LSB_GUEST_MAIN_IMAGE_CAPACITY)
        return EINVAL;
    int error = bashEarlyAdoptHeap(image, image + BashEarlyHeapOffset);
    if (!error) error = LSBInstallForkProvider(prepareResumedBash);
    if (!error) { resumedBash = image; resumedDepth = depth; }
    return error;
}

static int forkTrialMode(void) {
    const char *value = getenv(option);
    if (value && !strcmp(value, "1")) return TrialSnapshot;
    if (value && !strcmp(value, "df")) return TrialDF;
    if (value && !strcmp(value, "early")) return TrialShell;
    if (value && !strcmp(value, "exec")) return TrialExec;
    return LSB_FORK_NOT_HANDLED;
}

bool LSBForkTrialEnabled(void) {
    int mode = forkTrialMode();
    // Descendants inherit the environment. Only the pinned shell uses this foreground continuation.
    if (mode == TrialExec || mode == TrialShell) {
        uintptr_t image = (uintptr_t)_NSGetMachExecuteHeader();
        if (!forkTrialBashImage(image)) return false;
        if (mode == TrialShell && !bashEarlyHeapBase()) {
            int error = bashEarlyInstallHeap(image);
            if (error) fprintf(stderr, "fork trial shell allocator unavailable errno=%d\n", error);
        }
        return true;
    }
    return mode != LSB_FORK_NOT_HANDLED;
}

// An internal socket must not fill a hole in the caller's standard descriptors.
static int prepareControlFD(int *fd) {
    if (*fd >= 3) return fcntl(*fd, F_SETFD, FD_CLOEXEC) < 0 ? errno : 0;
    int moved = fcntl(*fd, F_DUPFD_CLOEXEC, 3);
    if (moved < 0) return errno;
    close(*fd);
    *fd = moved;
    return 0;
}

static int spawnObserver(LSBForkAttempt *attempt, const TrialOffer *offer) {
    char *environment[LSB_MAX_VECTOR + 1];
    size_t count = 0;
    for (char **entry = *_NSGetEnviron(); *entry; ++entry) {
        if (!strncmp(*entry, option, sizeof(option) - 1) && (*entry)[sizeof(option) - 1] == '=') continue;
        if (count == LSB_MAX_VECTOR) return E2BIG;
        environment[count++] = *entry;
    }
    environment[count] = NULL;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return errno;
    int error = prepareControlFD(&sockets[0]);
    if (!error) error = prepareControlFD(&sockets[1]);
    if (error) {
        close(sockets[0]);
        close(sockets[1]);
        return error;
    }
    attempt->socket = sockets[0];
    error = trialSocket(sockets[0]);
    posix_spawn_file_actions_t actions;
    if (!error && !(error = posix_spawn_file_actions_init(&actions))) {
        error = posix_spawn_file_actions_addinherit_np(&actions, sockets[1]);
        for (unsigned fd = 0; !error && fd < LSB_MAX_FDS; ++fd) {
            if (offer->closeOnExec[fd / 64] & (UINT64_C(1) << (fd % 64))) error = posix_spawn_file_actions_addinherit_np(&actions, (int)fd);
        }
        char descriptor[16];
        snprintf(descriptor, sizeof(descriptor), "%d", sockets[1]);
        char *args[] = {(char *)helper, descriptor, NULL};
        pid_t child = 0;
        if (!error) error = LSBPosixSpawn(&child, helper, &actions, NULL, args, environment);
        if (!error) attempt->child = child;
        posix_spawn_file_actions_destroy(&actions);
    }
    close(sockets[1]);
    return error;
}

typedef struct {
    LSBForkWireRange range;
    bool skip;
} TrialSource;

static int comparePages(const void *left, const void *right) {
    uint64_t a = ((const TrialSource *)left)->range.start, b = ((const TrialSource *)right)->range.start;
    return (a > b) - (a < b);
}

typedef struct {
    TrialSource sources[LSB_MAX_FORK_RANGES];
    size_t count, bytes, skipped;
} TrialPlan;

typedef struct {
    uint64_t start, end;
    vm_region_submap_info_data_64_t info;
} TrialLeaf;

static int readLeaf(uint64_t address, TrialLeaf *leaf) {
    mach_vm_address_t start = address;
    mach_vm_size_t size = 0;
    natural_t depth = UINT32_MAX;
    mach_msg_type_number_t words = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &start, &size, &depth, (vm_region_recurse_info_t)&leaf->info, &words);
    if (kr != KERN_SUCCESS || start > address || size > UINT64_MAX - start || address - start >= size) return EFAULT;
    if (words < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || leaf->info.is_submap || leaf->info.inheritance > VM_INHERIT_NONE) return ENOTSUP;
    leaf->start = start;
    leaf->end = start + size;
    return 0;
}

static int addSpan(TrialPlan *plan, const char *name, uint64_t start, uint64_t end) {
    // The broker accepts intervals within a single leaf; segment boundaries and VM leaf boundaries can differ.
    while (start < end) {
        TrialLeaf leaf = {};
        int error = readLeaf(start, &leaf);
        if (error) return error;
        uint64_t limit = leaf.end < end ? leaf.end : end;
        uint64_t length = limit - start;
        if (plan->count == LSB_MAX_FORK_RANGES || length > LSB_MAX_FORK_BYTES - plan->bytes) return E2BIG;
        plan->sources[plan->count++] = (TrialSource){{start, length}, leaf.info.inheritance == VM_INHERIT_NONE};
        plan->bytes += length;
        if (leaf.info.inheritance == VM_INHERIT_NONE) plan->skipped += length;
        forkTrialTrace("fork trial source=%s 0x%llx..0x%llx prot=%d/%d inherit=%d\n",
                name, start, limit, leaf.info.protection, leaf.info.max_protection, leaf.info.inheritance);
        start = limit;
    }
    return 0;
}

static bool contains(const TrialPlan *plan, uint64_t address) {
    for (size_t i = 0; i < plan->count; ++i) {
        LSBForkWireRange range = plan->sources[i].range;
        if (address >= range.start && address - range.start < range.size) return true;
    }
    return false;
}

static int findExecutable(uintptr_t callerPC, const struct mach_header_64 **image, intptr_t *slide) {
    char path[PATH_MAX];
    uint32_t bytes = sizeof(path);
    if (_NSGetExecutablePath(path, &bytes)) return ENAMETOOLONG;
    struct stat executable;
    bool haveStat = !stat(path, &executable);
    uint32_t number = _dyld_image_count();
    for (uint32_t i = 0; i < number; ++i) {
        const struct mach_header_64 *candidate = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!candidate || candidate->magic != MH_MAGIC_64 || candidate->filetype != MH_EXECUTE) continue;
        const char *name = _dyld_get_image_name(i);
        struct stat file;
        if (!name || (strcmp(name, path) && (!haveStat || stat(name, &file) || file.st_dev != executable.st_dev || file.st_ino != executable.st_ino))) continue;
        if (*image) return ENOEXEC;
        *image = candidate;
        *slide = _dyld_get_image_vmaddr_slide(i);
    }
    if (!*image) return ENOEXEC;
    Dl_info info;
    if (!dladdr((const void *)callerPC, &info) || !info.dli_fbase) return EFAULT;
    const struct mach_header *callerImage = info.dli_fbase;
    if (callerImage->filetype == MH_EXECUTE && (const void *)*image != info.dli_fbase) return ENOEXEC;
    forkTrialTrace("fork trial main=%s header=%p type=%u caller_image=%s parent_getpid_interposer=%p\n",
            path, (const void *)*image, (*image)->filetype, info.dli_fname, (void *)&LSBGetPID);
    return 0;
}

static int addBashVariablePage(TrialPlan *plan, uintptr_t header, size_t pageSize, TrialBashState *bash) {
    uintptr_t address;
    uint64_t firstWord;
    if (!readForkBashVariables(header, &address, &firstWord)) return 0;
    uint64_t page = address & ~(uint64_t)(pageSize - 1);
    TrialLeaf leaf = {};
    int error = readLeaf(address, &leaf);
    if (error) return error;
    if (leaf.info.inheritance != VM_INHERIT_COPY || leaf.info.protection != (VM_PROT_READ | VM_PROT_WRITE) ||
        page < leaf.start || pageSize > leaf.end - page || contains(plan, address)) return 0;
    error = addSpan(plan, "bash-variable-context-page", page, page + pageSize);
    if (!error) {
        bash->variablesAddress = address;
        bash->expectedFirstWord = firstWord;
    }
    return error;
}

#include "ForkBashCommand.h"
#include "ForkBashPipeline.h"

static int inventory(TrialPlan *plan, const struct mach_header_64 *image, intptr_t slide, TrialOffer *offer) {
    const LSBForkContext *caller = &offer->caller;
    uintptr_t callerPC = caller->pc, callerSP = caller->sp;
    const uintptr_t page = (uintptr_t)getpagesize();
    bool foundUUID = false;
    if (slide < 0 || !page || (page & (page - 1))) return ENOEXEC;
    const char *cmd = (const char *)(image + 1);
    size_t remaining = image->sizeofcmds;
    for (uint32_t i = 0; i < image->ncmds; ++i) {
        const struct load_command *load = (const struct load_command *)cmd;
        if (remaining < sizeof(*load) || load->cmdsize < sizeof(*load) || load->cmdsize > remaining) return ENOEXEC;
        if (load->cmd == LC_UUID) {
            if (load->cmdsize != sizeof(struct uuid_command) || foundUUID) return ENOEXEC;
            uuid_string_t uuid;
            uuid_unparse_lower(((const struct uuid_command *)cmd)->uuid, uuid);
            forkTrialTrace("fork trial main_uuid=%s\n", uuid);
            foundUUID = true;
        }
        if (load->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)cmd;
            if (load->cmdsize < sizeof(*seg)) return ENOEXEC;
            char name[sizeof(seg->segname) + 1] = {};
            memcpy(name, seg->segname, sizeof(seg->segname));
            if (!strcmp(name, SEG_PAGEZERO)) {
                forkTrialTrace("fork trial source=__PAGEZERO excluded (unmapped)\n");
            } else if (seg->vmsize) {
                uint64_t start, end;
                if (__builtin_add_overflow(seg->vmaddr, (uint64_t)slide, &start) || (start & (page - 1)) ||
                    __builtin_add_overflow(start, seg->vmsize, &end) || __builtin_add_overflow(end, page - 1, &end)) return ENOEXEC;
                int error = addSpan(plan, name, start, end & ~(page - 1));
                if (error) return error;
            }
        }
        remaining -= load->cmdsize;
        cmd += load->cmdsize;
    }
    if (remaining || !foundUUID || !contains(plan, (uintptr_t)image)) return ENOEXEC;
    if (!contains(plan, callerPC)) {
        uint64_t start = callerPC & ~(page - 1);
        if (start > UINT64_MAX - page) return EOVERFLOW;
        int error = addSpan(plan, "fork-call-site", start, start + page);
        if (error) return error;
    }
    TrialLeaf stack = {};
    int error = readLeaf(callerSP, &stack);
    if (error) return error;
    if (!(stack.info.protection & VM_PROT_WRITE)) return EACCES;
    uint64_t start = stack.start;
    if (offer->flags & TrialSlicedStack) {
        if (offer->resume < TrialExec || stack.info.inheritance != VM_INHERIT_COPY || ((stack.start | stack.end) & (page - 1))) return ENOTSUP;
        offer->fullStackStart = start;
        start = trialStackSliceStart(start, callerSP, page);
        if (start < page) return ERANGE;
    }
    error = addSpan(plan, start == stack.start ? "calling-stack" : "calling-stack-slice", start, stack.end);
    if (!error && offer->resume == TrialShell) {
        uintptr_t heap = bashEarlyHeapBase();
        if (!heap || heap != (uintptr_t)image + BashEarlyHeapOffset) return ENOTSUP;
        error = addSpan(plan, "bash-heap", heap, heap + BashEarlyHeapBytes);
    }
    if (!error) error = addBashVariablePage(plan, (uintptr_t)image, page, &offer->bash);
    if (!error && offer->resume == TrialShell) error = addBashShellCommand(plan, offer, page);
    else if (!error && offer->resume != TrialSnapshot) error = addBashCommandPages(plan, (uintptr_t)image, caller, page, offer->resume);
    if (error) return error;
    if (trialNeedsStackGuard(offer) && (start < page || contains(plan, start - page))) return ENOTSUP;
    qsort(plan->sources, plan->count, sizeof(*plan->sources), comparePages);
    for (size_t i = 1; i < plan->count; ++i) {
        LSBForkWireRange previous = plan->sources[i-1].range;
        if (previous.start + previous.size > plan->sources[i].range.start) return EINVAL;
    }
    return 0;
}

// Rebuild the entire plan when the stack changes: argv/env may independently reach pages that only the full stack covered before.
static int buildOffer(TrialPlan *plan, TrialOffer *offer, intptr_t slide, uint32_t flags) {
    *plan = (TrialPlan){};
    offer->flags = flags;
    offer->fullStackStart = 0;
    offer->skipMask = 0;
    memset(offer->closeOnExec, 0, sizeof(offer->closeOnExec));
    offer->terminal = (TrialTerminalAlias){};
    offer->bash = (TrialBashState){.parentGetpid = (uintptr_t)&LSBGetPID};
    int error = inventory(plan, (const struct mach_header_64 *)offer->mainHeader, slide, offer);
    if (error) return error;
    offer->rangeCount = (uint32_t)plan->count;
    memset(offer->ranges, 0, sizeof(offer->ranges));
    for (size_t i = 0; i < plan->count; ++i) {
        offer->ranges[i] = plan->sources[i].range;
        if (plan->sources[i].skip) offer->skipMask |= UINT64_C(1) << i;
    }
    return 0;
}

// Only the acceptance build records which logical child Bash actually reaps. This may run inside its SIGCHLD handler.
static _Atomic(pid_t) expectedWait[LSB_MAX_FDS];
static _Atomic(unsigned) tracedChildren;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "trial wait bookkeeping must be signal-safe");
void LSBForkTrialDidCommit(pid_t child) {
    if (!forkTrialTraceEnabled()) return;
    // Count before publishing the PID: a SIGCHLD handler may reap it immediately after the slot becomes visible.
    atomic_fetch_add(&tracedChildren, 1);
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) {
        pid_t empty = 0;
        if (atomic_compare_exchange_strong(&expectedWait[i], &empty, child)) return;
    }
    atomic_fetch_sub(&tracedChildren, 1);
}

void LSBForkTrialDidWait(pid_t child, int status) {
    if (!atomic_load(&tracedChildren)) return;
    bool reaped = WIFEXITED(status) || WIFSIGNALED(status);
    unsigned i = 0;
    for (; i < LSB_MAX_FDS; ++i) {
        pid_t expected = atomic_load(&expectedWait[i]);
        if (expected != child) continue;
        if (reaped) {
            if (!atomic_compare_exchange_strong(&expectedWait[i], &expected, 0)) continue;
            atomic_fetch_sub(&tracedChildren, 1);
        }
        break;
    }
    if (i == LSB_MAX_FDS) return;
    char message[] = "fork trial parent wait child=0x00000000 status=0x00000000\n";
    const size_t childOffset = sizeof("fork trial parent wait child=0x") - 1, statusOffset = sizeof(message) - 10;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) {
        unsigned shift = 28 - 4 * i;
        message[childOffset + i] = hex[((uint32_t)child >> shift) & 15];
        message[statusOffset + i] = hex[((uint32_t)status >> shift) & 15];
    }
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
}

static int prepareBashFork(LSBForkAttempt *attempt, const LSBForkContext *caller, TrialMode mode, const struct mach_header_64 *mainImage, intptr_t slide) {
    uintptr_t callerPC = caller->pc, callerSP = caller->sp;
    int error;
    TrialPlan plan = {};
    TrialOffer offer = {.version = TrialWireVersion, .mainHeader = (uintptr_t)mainImage, .caller = *caller, .resume = mode,
                        .depth = mode == TrialShell ? resumedDepth + 1 : 0};
    error = buildOffer(&plan, &offer, slide, 0);
    if (!error) error = trialToken(&offer.audit);
    if (error) { fprintf(stderr, "fork trial inventory errno=%d\n", error); return error; }
    // Only a confirmed collision before the broker offer warrants trying a fresh observer. libc hasn't acquired its atfork locks yet.
    const unsigned maxAttempts = 4;
    for (unsigned candidate = 1; candidate <= maxAttempts; ++candidate) {
        TrialReady ready = {};
        int32_t preflight = 0;
        error = spawnObserver(attempt, &offer);
        if (!error) error = trialTransfer(attempt->socket, &ready, sizeof(ready), false);
        if (!error && ready.version != TrialWireVersion) error = EPROTO;
        if (!error && ready.logicalPID != attempt->child) error = ESRCH;
        if (!error) error = trialTransfer(attempt->socket, &offer, sizeof(offer), true);
        if (!error) error = trialTransfer(attempt->socket, &preflight, sizeof(preflight), false);
        if (error) break; // The caller owns the final attempt's socket and child, including on any failed handshake.
        forkTrialTrace("fork trial candidate=%u parent=%d observer=%d preflight_errno=%d\n", candidate, LSBGuestPID(), attempt->child, preflight);
        if (preflight < 0 || preflight > ELAST) { error = EPROTO; break; }
        if (!preflight) { error = LSBTransportOfferFork(&ready.audit, offer.ranges, offer.rangeCount); break; }
        error = preflight;
        if (error != EADDRINUSE || candidate == maxAttempts) break;
        close(attempt->socket);
        attempt->socket = -1;
        // A rejecting observer exits after replying. The application's SIGCHLD handler may win this exact-PID wait.
        pid_t child;
        do { child = LSBWaitPID(attempt->child, NULL, 0); } while (child < 0 && errno == EINTR);
        if (child < 0 && errno != ECHILD) { error = errno; break; }
        attempt->child = 0;
        if (mode >= TrialExec && !offer.flags) {
            error = buildOffer(&plan, &offer, slide, TrialSlicedStack);
            if (error) break;
        }
    }
    forkTrialTrace("fork trial parent=%d observer=%d caller_pc=0x%lx caller_sp=0x%lx ranges=%zu bytes=%zu inherit_none=%zu prepare_errno=%d\n",
            LSBGuestPID(), attempt->child, callerPC, callerSP, plan.count, plan.bytes, plan.skipped, error);
    return error;
}

int LSBPrepareForkAttempt(LSBForkAttempt *attempt, const LSBForkContext *caller) {
    int mode = forkTrialMode();
    if (mode == LSB_FORK_NOT_HANDLED) return LSB_FORK_NOT_HANDLED;
    if (!caller->pc || !caller->sp) return ENOEXEC;
    const struct mach_header_64 *mainImage = NULL;
    intptr_t slide = 0;
    int error = findExecutable(caller->pc, &mainImage, &slide);
    if (error) { fprintf(stderr, "fork trial executable lookup errno=%d\n", error); return error; }
    return prepareBashFork(attempt, caller, mode, mainImage, slide);
}

static int prepareResumedBash(LSBForkAttempt *attempt, const LSBForkContext *caller) {
    if (!resumedBash || caller->pc != resumedBash + BashForkReturn) return LSB_FORK_NOT_HANDLED;
    if (resumedDepth >= TrialMaxDepth) return ENOTSUP;
    // The pinned PIE's __TEXT vmaddr is 0x100000000. Its signed segments are still mapped at the original guest addresses.
    if (resumedBash < LSB_GUEST_MAIN_IMAGE_OFFSET || !forkTrialBashImage(resumedBash)) return ENOEXEC;
    return prepareBashFork(attempt, caller, TrialShell, (const void *)resumedBash, (intptr_t)(resumedBash - LSB_GUEST_MAIN_IMAGE_OFFSET));
}
