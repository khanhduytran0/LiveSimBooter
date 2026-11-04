// Prepare the fork offer before libc takes its atfork locks.
#include "../Shared/Fork/ForkTrialWire.h"
#include "../Shared/Fork/ForkAllocator.h"
#include "../Shared/Fork/ForkPthreadRegistry.h"
#include "../Shared/Fork/ForkDescriptorState.h"
#include "../Shared/Fork/ForkSignalState.h"
#include "../../ProcessSupport/ForkDescriptorSlots.h"
#include "../../ProcessSupport/ForkStackHint.h"
#include "../../ProcessSupport/ForkPolicy.h"
#include "FakePTY.h"
#include "TerminalStopWitness.h"
#include "../Shared/Fork/ForkBashState.h"
#include "../Shared/Fork/ForkBashEarlyHeap.h"
#include "ChildWait.h"
#include "../Shared/Fork/ForkContext.h"
#include "ProcessIdentity.h"
#include "RuntimeDirectories.h"
#include "../../ProcessSupport/GuestExecutablePath.h"
#include "../Shared/PrivateMachVM.h"
#include "../../ProcessSupport/GuestImagePlacement.h"
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
#include <sys/mman.h>
#include <sys/wait.h>
#include <uuid/uuid.h>

extern uintptr_t __stack_chk_guard;
extern int LSBPosixSpawn(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const [], char *const []);

static const char option[] = LSB_FORK_MODE_ENV;
static const char helper[] = LSB_FORK_BOOTSTRAP_EXECUTABLE;

// An observer's registered executable remains the bootstrap. The copied image is not on dyld's image list.
static uintptr_t resumedBash;
static unsigned resumedDepth;
static bool procursusTerminal;
static bool defaultResume;
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

static int forkTrialMode(const char *value) {
    if (!value) {
        if (procursusTerminal) return TrialShell;
        return defaultResume ? TrialSnapshot : LSB_FORK_NOT_HANDLED;
    }
    if (!strcmp(value, "1") || !strcmp(value, "allocator") || !strcmp(value, "resume")) return TrialSnapshot;
    if (!strcmp(value, "df")) return TrialDF;
    if (!strcmp(value, "early")) return TrialShell;
    if (!strcmp(value, "exec")) return TrialExec;
    return LSB_FORK_NOT_HANDLED;
}

bool LSBForkTrialEnabled(void) {
    const char *selected = getenv(option);
    const char *nativeSupport = getenv("LSB_NATIVE_SUPPORT");
    if (!selected && nativeSupport && !strcmp(nativeSupport, "1")) {
        char path[PATH_MAX];
        uint32_t size = sizeof(path);
        if (!_NSGetExecutablePath(path, &size)) defaultResume = LSBUseForkDonorLayout(getenv("DYLD_ROOT_PATH"), path, NULL);
    }
    // Preserve the installed interactive Bash path. Only its pinned image uses the shell-specific allocator.
    if (!selected && getenv(LSB_FAKE_PTY_ENV)) {
        char **argv = *_NSGetArgv();
        const char *path = argv && argv[0] ? argv[0] : "";
        const char *slash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : path;
        if (!strcmp(name, "bash") || !strcmp(name, "-bash"))
            procursusTerminal = forkTrialProcursusBashImage((uintptr_t)_NSGetMachExecuteHeader());
    }
    int mode = forkTrialMode(selected);
    // Descendants inherit the environment. Only the pinned shell uses this foreground continuation.
    if (mode == TrialExec || mode == TrialShell) {
        uintptr_t image = (uintptr_t)_NSGetMachExecuteHeader();
        if (!forkTrialBashImage(image) && !(mode == TrialShell && forkTrialProcursusBashImage(image))) return false;
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

static int spawnObserver(LSBForkAttempt *attempt, const TrialOffer *offer, char *stackHint, bool *earlyCollision) {
    char *environment[LSB_MAX_VECTOR + 1];
    size_t count = 0;
    const bool isolated = (offer->flags & TrialRestoreAllocator) != 0;
    if (isolated) environment[count++] = "LSB_FORK_DEFER_DEBUGGER=1";
    for (char **entry = *_NSGetEnviron(); *entry; ++entry) {
        if (!strncmp(*entry, LSB_FORK_STACK_HINT_ENV "=", sizeof(LSB_FORK_STACK_HINT_ENV))) continue;
        if (!strncmp(*entry, option, sizeof(option) - 1) && (*entry)[sizeof(option) - 1] == '=') continue;
        if (isolated && !strncmp(*entry, "LSB_FORK_DEFER_DEBUGGER=", sizeof("LSB_FORK_DEFER_DEBUGGER=") - 1)) continue;
        if (count == LSB_MAX_VECTOR) return E2BIG;
        environment[count++] = *entry;
    }
    bool hinted = stackHint[0] && count < LSB_MAX_VECTOR;
    if (hinted) environment[count++] = stackHint;
    environment[count] = NULL;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return errno;
    bool transferFDs = (offer->flags & TrialResumeGuest) != 0;
    if (transferFDs) {
        int high = fcntl(sockets[1], F_DUPFD_CLOEXEC, LSB_MAX_FDS + 4);
        if (high < 0) { int error = errno; close(sockets[0]); close(sockets[1]); return error; }
        close(sockets[1]);
        sockets[1] = high;
    }
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
        char *args[] = {(char *)helper, descriptor, transferFDs ? LSB_FORK_RECEIVE_ARG : NULL, NULL};
        pid_t child = 0;
        posix_spawnattr_t attributes;
        bool initialized = false;
        if (!error && transferFDs) {
            error = posix_spawnattr_init(&attributes);
            initialized = error == 0;
            if (!error) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
            for (int fd = 0; !error && fd <= STDERR_FILENO; ++fd)
                error = posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
        }
        if (!error) {
            error = LSBPosixSpawn(&child, helper, &actions, initialized ? &attributes : NULL, args, environment);
            if (error == E2BIG && hinted) {
                // Preserve the existing spawn when its full flag/environment/string budget leaves no room for the optional hint.
                environment[--count] = NULL;
                hinted = false;
                error = LSBPosixSpawn(&child, helper, &actions, initialized ? &attributes : NULL, args, environment);
            }
            if (hinted) *earlyCollision = LSBForkAcknowledgedStackCollision(error, sockets[0]);
        }
        if (initialized) posix_spawnattr_destroy(&attributes);
        if (!error) attempt->child = child;
        posix_spawn_file_actions_destroy(&actions);
    }
    close(sockets[1]);
    return error;
}

typedef struct {
    LSBForkWireRange range;
    bool skip, allocator;
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
    // Keep per-leaf skip bits in the offer. A segment can include VM_INHERIT_NONE spans.
    while (start < end) {
        TrialLeaf leaf = {};
        int error = readLeaf(start, &leaf);
        if (error) return error;
        uint64_t limit = leaf.end < end ? leaf.end : end;
        uint64_t length = limit - start;
        if (plan->count == LSB_MAX_FORK_RANGES || length > LSB_MAX_FORK_BYTES - plan->bytes) return E2BIG;
        plan->sources[plan->count++] = (TrialSource){.range = {start, length}, .skip = leaf.info.inheritance == VM_INHERIT_NONE};
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

static int addAllocatorSpan(void *context, uint64_t start, uint64_t size) {
    TrialPlan *plan = context;
    if (size > UINT64_MAX - start) return EOVERFLOW;
    // The broker walks each leaf again while both tasks are suspended. One contiguous interval can include the rack's guard pages.
    if (!size || size > LSB_MAX_FORK_BYTES - plan->bytes || plan->count == LSB_MAX_FORK_RANGES) return E2BIG;
    for (uint64_t cursor = start; cursor < start + size;) {
        TrialLeaf leaf = {};
        int error = readLeaf(cursor, &leaf);
        if (error) return error;
        if (leaf.info.inheritance != VM_INHERIT_COPY) return ENOTSUP;
        cursor = leaf.end;
    }
    plan->sources[plan->count++] = (TrialSource){.range = {start, size}, .allocator = true};
    plan->bytes += size;
    forkTrialTrace("fork trial scalable-zone span=%#llx..%#llx bytes=%zu total=%zu\n", start, start + size, (size_t)size, plan->bytes);
    return 0;
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
#include "ForkProcursusBashCommand.h"

static int addImage(TrialPlan *plan, const struct mach_header_64 *image, intptr_t slide, uintptr_t page) {
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
            forkTrialTrace("fork trial image=%p uuid=%s\n", (const void *)image, uuid);
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
    return remaining || !contains(plan, (uintptr_t)image) ? ENOEXEC : 0;
}

static int addLooseImages(TrialPlan *plan, uintptr_t callerPC, uintptr_t page) {
    Dl_info info;
    if (!dladdr((const void *)callerPC, &info) || !info.dli_fbase) return EFAULT;
    const LSBGuestRuntimeDirectories *directories = NULL;
    int error = LSBGetRuntimeDirectories(&directories);
    if (error) return error;
    // Keep the observer's own bootstrap libraries. The snapshot also needs mapped libraries from installed apps or packages, including dlopen loads.
    // The shared cache has per-process data and a shared LINKEDIT, which need a separate inheritance plan.
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const struct mach_header_64 *library = (const void *)_dyld_get_image_header(i);
        if (!library || library->magic != MH_MAGIC_64 || library->filetype != MH_DYLIB || (library->flags & MH_DYLIB_IN_CACHE)) continue;
        const char *path = _dyld_get_image_name(i);
        if (library != info.dli_fbase) {
            if (!path) continue;
            if (!LSBGuestPathInBootstrap(directories->runtime, path) && !LSBGuestInstalledApplicationPath(directories->runtime, path)) {
                char resolved[PATH_MAX];
                if (!realpath(path, resolved) || (!LSBGuestPathInBootstrap(directories->runtime, resolved) &&
                    !LSBGuestInstalledApplicationPath(directories->runtime, resolved))) continue;
            }
        }
        error = addImage(plan, library, _dyld_get_image_vmaddr_slide(i), page);
        if (error) return error;
    }
    return contains(plan, callerPC) ? 0 : ENOTSUP;
}

static int addForkVector(TrialPlan *plan, uintptr_t vector, size_t page, int argc) {
    size_t budget = LSB_STRING_BYTES;
    if (!vector || vector % sizeof(uintptr_t)) return EFAULT;
    for (unsigned i = 0; i <= LSB_MAX_VECTOR; ++i) {
        uintptr_t slot, value;
        if (__builtin_add_overflow(vector, i * sizeof(uintptr_t), &slot) || !forkTrialRead(slot, &value, sizeof(value))) return EFAULT;
        if (!value) {
            if (argc >= 0 && (unsigned)argc != i) return EPROTO;
            return addForkBytes(plan, vector, (i + 1) * sizeof(uintptr_t), page);
        }
        if (argc >= 0 && i >= (unsigned)argc) return EPROTO;
        int error = addForkString(plan, value, &budget, page);
        if (error) return error;
    }
    return E2BIG;
}

static int inventory(TrialPlan *plan, const struct mach_header_64 *image, intptr_t slide, TrialOffer *offer) {
    const LSBForkContext *caller = &offer->caller;
    uintptr_t callerPC = caller->pc, callerSP = caller->sp;
    const uintptr_t page = (uintptr_t)getpagesize();
    int error = addImage(plan, image, slide, page);
    if (error) return error;
    if (offer->resume == TrialSnapshot) {
        error = addLooseImages(plan, callerPC, page);
    } else if (!contains(plan, callerPC)) {
        uint64_t start = callerPC & ~(page - 1);
        if (start > UINT64_MAX - page) return EOVERFLOW;
        error = addSpan(plan, "fork-call-site", start, start + page);
    }
    if (error) return error;
    TrialLeaf stack = {};
    error = readLeaf(callerSP, &stack);
    if (error) return error;
    if (!(stack.info.protection & VM_PROT_WRITE)) return EACCES;
    if (offer->flags & TrialResumeGuest) {
        // This checkpoint offers one writable stack leaf. Publish it as pthread's stack only if it is the caller's full stack.
        pthread_t self = pthread_self();
        if (stack.end != (uintptr_t)pthread_get_stackaddr_np(self) || stack.end - stack.start != pthread_get_stacksize_np(self)) return ENOTSUP;
    }
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
    if (!error && !(offer->flags & TrialRestoreAllocator)) error = addBashVariablePage(plan, (uintptr_t)image, page, &offer->bash);
    if (!error && (offer->flags & TrialRestoreAllocator)) error = forkAllocatorCapture(&offer->allocator, addAllocatorSpan, plan);
    if (!error && offer->resume == TrialShell) {
        error = forkTrialProcursusBashImage((uintptr_t)image) ? checkProcursusBashFork(offer) : addBashShellCommand(plan, offer, page);
    }
    else if (!error && offer->resume != TrialSnapshot) error = addBashCommandPages(plan, (uintptr_t)image, caller, page, offer->resume);
    if (!error && (offer->flags & TrialResumeGuest)) {
        offer->argv = (uintptr_t)*_NSGetArgv();
        offer->argc = (uint32_t)*_NSGetArgc();
        offer->envp = (uintptr_t)*_NSGetEnviron();
        offer->guard = __stack_chk_guard;
        const LSBGuestRuntimeDirectories *directories = NULL;
        error = addForkVector(plan, offer->argv, page, (int)offer->argc);
        if (!error) error = addForkVector(plan, offer->envp, page, -1);
        if (!error) error = LSBGetRuntimeDirectories(&directories);
        if (!error) error = trialCaptureSupportImages(directories->runtime, &offer->support);
        if (!error) {
            error = forkPthreadCapture(&offer->pthreads);
            if (error) forkTrialTrace("fork trial cannot capture pthread registrations errno=%d\n", error);
        }
    }
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
    offer->allocatorMask = 0;
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
        if (plan->sources[i].allocator) offer->allocatorMask |= UINT64_C(1) << i;
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

typedef struct {
    TrialOffer offer;
    ForkDescriptorTraps traps;
    ForkDescriptorPeers parked;
    ForkCacheCheckpointPlan cachePlan;
    ForkCachePacket cachePacket;
    int descriptorError;
    ForkDescriptorIssue receiverIssue;
    int receiverError;
    uint64_t missingStart, missingBytes;
    int checkError;
    bool checked;
} AllocatorAttempt;

int LSBTrialRestoreForkSignals(const ForkSignalState *state, void *context, int (*covers)(void *, uint64_t, uint64_t)) {
    return forkRestoreSignals(state, context, covers);
}

int LSBTrialReceiveForkFDs(int socket) {
    ForkDescriptorIssue issue;
    int32_t received = forkDescriptorReceiveReport(socket, LSBGuestChildWakeFD(), LSBGuestFakePTYWatchFD(), 7, &issue);
    int error = forkDescriptorBytes(socket, &received, sizeof(received), true);
    if (!error) error = forkDescriptorBytes(socket, &issue, sizeof(issue), true);
    return error ? error : received;
}

static int sendPreparedDescriptors(void *context, int socket) {
    AllocatorAttempt *state = context;
    state->traps.stopWitnessThread = LSBTerminalStopWitnessThreadID();
    state->descriptorError = forkDescriptorSendParked(socket, LSBNativePID(), LSBGuestChildWakeFD(), LSBGuestFakePTYWatchFD(), &state->traps, &state->parked);
    if (!state->descriptorError) state->descriptorError = forkDescriptorBytes(socket, &state->receiverError, sizeof(int32_t), false);
    if (!state->descriptorError) state->descriptorError = forkDescriptorBytes(socket, &state->receiverIssue, sizeof(state->receiverIssue), false);
    if (!state->descriptorError && !state->receiverError && (state->offer.flags & TrialVfork)) {
        state->checkError = forkPthreadCheckPrepared(&state->offer.pthreads, true);
        if (state->checkError) return state->checkError;
    }
    if (!state->descriptorError && !state->receiverError) {
        ForkSignalState signals;
        state->descriptorError = forkReadSignalState(&signals);
        if (!state->descriptorError) {
            signals.mask = state->parked.savedSignals;
            state->descriptorError = forkDescriptorBytes(socket, &signals, sizeof(signals), true);
        }
        if (!state->descriptorError) state->descriptorError = forkCacheCapturePrepared(&state->cachePlan, &state->cachePacket);
        if (!state->descriptorError) {
            size_t size = offsetof(ForkCachePacket, bytes) + state->cachePacket.plan.used;
            state->descriptorError = forkDescriptorBytes(socket, &state->cachePacket, size, true);
        }
    }
    return state->descriptorError ? state->descriptorError : state->receiverError;
}

static void endDescriptorCheckpoint(void *context) {
    AllocatorAttempt *state = context;
    int error = forkDescriptorUnpark(&state->parked);
    if (error && !state->descriptorError) state->descriptorError = error;
}

static int checkPreparedSpan(void *context, uint64_t start, uint64_t bytes) {
    AllocatorAttempt *state = context;
    int error = trialAllocatorCovers(&state->offer, start, bytes);
    if (error) { state->missingStart = start; state->missingBytes = bytes; }
    return error;
}

static int recheckAllocator(void *context) {
    AllocatorAttempt *state = context;
    state->checked = true;
    state->checkError = forkAllocatorRecheck(&state->offer.allocator, checkPreparedSpan, state);
    if (!state->checkError && (state->offer.flags & TrialResumeGuest) && !(state->offer.flags & TrialVfork))
        state->checkError = forkPthreadCheckPrepared(&state->offer.pthreads, false);
    return state->checkError;
}

static int registerAllocatorOffer(LSBForkAttempt *attempt, const audit_token_t *child, bool *grew) {
    AllocatorAttempt *state = attempt->state;
    audit_token_t broker = {{0}};
    *grew = false;
    int error = LSBTransportOfferForkSocket(child, state->offer.ranges, state->offer.rangeCount, &broker);
    if (error) return error;

    // Registering the offer via guest XPC can allocate a new malloc region. Check before the observer receives its broker token.
    state->missingStart = state->missingBytes = 0;
    _malloc_fork_prepare();
    error = forkAllocatorRecheck(&state->offer.allocator, checkPreparedSpan, state);
    _malloc_fork_parent();
    *grew = error == EAGAIN && state->missingBytes != 0;
    return error ? error : trialTransfer(attempt->socket, &broker, sizeof(broker), true);
}

static int reapRejectedObserver(LSBForkAttempt *attempt) {
    // EOF aborts an observer still waiting for its broker token. The broker releases its offer when the observer exits.
    close(attempt->socket);
    attempt->socket = -1;
    pid_t child;
    do { child = LSBWaitPID(attempt->child, NULL, 0); } while (child < 0 && errno == EINTR);
    if (child < 0 && errno != ECHILD) return errno;
    attempt->child = 0; // The application's SIGCHLD handler can also reap this exact child.
    return 0;
}

static bool retryPlacement(unsigned *failures, bool beforeGuestLaunch) {
    // The native stack probe rejects a candidate before guest dyld startup. A full observer preflight is more expensive.
    return ++*failures < (beforeGuestLaunch ? 8u : 4u);
}

static int receiveObserverReady(const LSBForkAttempt *attempt, TrialReady *ready, int inventoryError) {
    int error = trialTransfer(attempt->socket, ready, sizeof(*ready), false);
    if (error) return error;
    if (ready->version != TrialWireVersion) return EPROTO;
    if (ready->logicalPID != attempt->child) return ESRCH;
    return inventoryError;
}

static void releaseAllocatorTrial(void *context) {
    AllocatorAttempt *state = context;
    TrialOffer *offer = &state->offer;
    if (offer->flags & TrialResumeGuest)
        forkTrialTrace("fork trial receiver errno=%d phase=%d target=%d kind=%d flags=%d threads=%d\n", state->receiverError,
                       state->receiverIssue.phase, state->receiverIssue.target, state->receiverIssue.type,
                       state->receiverIssue.fdFlags, state->receiverIssue.threads);
    if (state->checked)
        forkTrialTrace("fork trial prepared state errno=%d fds_errno=%d missing=%#llx bytes=%llu\n",
                       state->checkError, state->descriptorError, state->missingStart, state->missingBytes);
    unsigned char *witness = (void *)(uintptr_t)offer->witness;
    if (witness) {
        bool intact = true;
        for (unsigned i = 0; i < TrialWitnessSize; ++i) if (witness[i] != (unsigned char)(i + 1)) intact = false;
        forkTrialTrace("fork trial parent allocator witness intact=%d\n", intact);
        free(witness);
    }
    munmap(context, sizeof(*state));
}

static int prepareForkTrial(LSBForkAttempt *attempt, const LSBForkContext *caller, TrialMode mode, unsigned snapshotFlags,
                            const struct mach_header_64 *mainImage, intptr_t slide) {
    forkTrialCaptureTrace();
    attempt->captureLibcContext = mode == TrialSnapshot;
    uintptr_t callerPC = caller->pc, callerSP = caller->sp;
    int error;
    TrialPlan plan = {};
    TrialOffer offer = {.version = TrialWireVersion, .mainHeader = (uintptr_t)mainImage, .caller = *caller, .resume = mode,
                        .depth = mode == TrialShell ? resumedDepth + 1 : 0};
    bool resumeGuest = (snapshotFlags & TrialResumeGuest) != 0;
    if (attempt->isVfork && !resumeGuest) return LSB_FORK_NOT_HANDLED;
    bool restoreAllocator = (snapshotFlags & TrialRestoreAllocator) != 0;
    if (restoreAllocator) {
        offer.flags = snapshotFlags | (attempt->isVfork ? TrialVfork : 0);
        AllocatorAttempt *state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (state == MAP_FAILED) return errno;
        attempt->state = state;
        attempt->release = releaseAllocatorTrial;
        if (resumeGuest) {
            error = forkDescriptorPrepare(&state->traps);
            if (error) return error;
            attempt->sendPrepared = sendPreparedDescriptors;
            attempt->endCheckpoint = endDescriptorCheckpoint;
        }
        unsigned char *witness = malloc(TrialWitnessSize);
        if (!witness) return ENOMEM;
        for (unsigned i = 0; i < TrialWitnessSize; ++i) witness[i] = (unsigned char)(i + 1);
        state->offer.witness = offer.witness = (uintptr_t)witness;
    }
    error = restoreAllocator ? 0 : buildOffer(&plan, &offer, slide, 0);
    if (!error) error = trialToken(&offer.audit);
    if (error) { fprintf(stderr, "fork trial inventory errno=%d\n", error); return error; }
    // Placement and allocator growth have independent bounds. No libc fork callbacks have run at this point.
    unsigned placementFailures = 0;
    bool retriedGrowth = false;
    char stackHint[sizeof(LSB_FORK_STACK_HINT_ENV "=") + LSBForkStackHintLength] = {};
    TrialLeaf required = {};
    if (resumeGuest && !readLeaf(callerSP, &required) && required.info.inheritance == VM_INHERIT_COPY &&
        (required.info.protection & VM_PROT_WRITE) && required.end - required.start <= LSB_MAX_FORK_BYTES)
        snprintf(stackHint, sizeof(stackHint), LSB_FORK_STACK_HINT_ENV "=%016llx:%016llx", required.start, required.end);
    for (unsigned candidate = 1; ; ++candidate) {
        TrialReady ready = {};
        TrialPreflight report = {};
        bool earlyCollision = false;
        error = spawnObserver(attempt, &offer, stackHint, &earlyCollision);
        if (earlyCollision) {
            // READY failed before the broker published a child. Only close this candidate's private control socket.
            forkTrialTrace("fork trial native stack collision candidate=%u parent=%d required=%#llx..%#llx\n", candidate, LSBGuestPID(), required.start, required.end);
            close(attempt->socket);
            attempt->socket = -1;
            if (!retryPlacement(&placementFailures, true)) break;
            continue;
        }
        if (error) break;
        int inventoryError = 0;
        if (restoreAllocator) {
            // Spawn has initialized the donor's XPC connection. Inventory its heap while the child starts guest dyld;
            // registerAllocatorOffer and the locked libc checkpoint still recheck coverage before accepting any mappings.
            inventoryError = buildOffer(&plan, &offer, slide, offer.flags);
            if (!inventoryError) {
                inventoryError = trialAllocatorCovers(&offer, offer.witness, TrialWitnessSize);
                if (inventoryError) forkTrialTrace("fork trial missing allocator witness=%#llx zone=%p default=%#llx errno=%d\n",
                    offer.witness, (void *)malloc_zone_from_ptr((const void *)(uintptr_t)offer.witness), offer.allocator.zones[0], inventoryError);
            }
            if (!inventoryError) {
                ((AllocatorAttempt *)attempt->state)->offer = offer;
                attempt->checkPrepared = recheckAllocator;
            }
        }
        error = receiveObserverReady(attempt, &ready, inventoryError);
        if (!error) error = trialTransfer(attempt->socket, &offer, sizeof(offer), true);
        if (!error) error = trialTransfer(attempt->socket, &report, sizeof(report), false);
        if (error) break; // The caller owns the final attempt's socket and child, including on any failed handshake.
        int preflight = report.error;
        forkTrialTrace("fork trial candidate=%u parent=%d observer=%d preflight_errno=%d phase=%u address=%#llx\n",
                       candidate, LSBGuestPID(), attempt->child, preflight, report.phase, report.address);
        if (preflight < 0 || preflight > ELAST || report.reserved || report.pthreadPrefix > offer.pthreads.count ||
            (!resumeGuest && report.pthreadPrefix)) { error = EPROTO; break; }
        bool grew = false;
        if (!preflight) {
            if (resumeGuest) {
                ForkCacheCheckpointPlan *cache = &((AllocatorAttempt *)attempt->state)->cachePlan;
                error = forkCachePlan(&offer.pthreads, report.pthreadPrefix, cache);
                // vfork bypasses user prepare handlers. A new cache owner's mutable data may contain locks held by an unrelated thread.
                if (!error && attempt->isVfork && cache->count) error = ENOTSUP;
                if (error) break;
            }
            error = restoreAllocator ? registerAllocatorOffer(attempt, &ready.audit, &grew) : LSBTransportOfferFork(&ready.audit, offer.ranges, offer.rangeCount);
            if (!grew) break;
            AllocatorAttempt *state = attempt->state;
            forkTrialTrace("fork trial allocator grew during offer candidate=%u missing=%#llx bytes=%llu\n", candidate, state->missingStart, state->missingBytes);
        } else {
            error = preflight;
            if (error != EADDRINUSE || !retryPlacement(&placementFailures, false)) break;
        }
        int reapError = reapRejectedObserver(attempt);
        if (reapError) { error = reapError; break; }
        if (grew) {
            if (retriedGrowth) break;
            retriedGrowth = true;
        }
        if (mode >= TrialExec && !offer.flags) {
            error = buildOffer(&plan, &offer, slide, TrialSlicedStack);
            if (error) break;
        }
    }
    forkTrialTrace("fork trial parent=%d observer=%d caller_pc=0x%lx caller_sp=0x%lx ranges=%zu bytes=%zu inherit_none=%zu prepare_errno=%d"
                   " placement_failures=%u retried_growth=%d\n",
            LSBGuestPID(), attempt->child, callerPC, callerSP, plan.count, plan.bytes, plan.skipped, error, placementFailures, retriedGrowth);
    return error;
}

int LSBPrepareForkAttempt(LSBForkAttempt *attempt, const LSBForkContext *caller) {
    const char *probe = getenv(option);
    int mode = forkTrialMode(probe);
    if (mode == LSB_FORK_NOT_HANDLED) return LSB_FORK_NOT_HANDLED;
    unsigned flags = 0;
    if (mode == TrialSnapshot) {
        if (probe ? !strcmp(probe, "resume") : defaultResume) flags = TrialRestoreAllocator | TrialResumeGuest;
        else if (probe && !strcmp(probe, "allocator")) flags = TrialRestoreAllocator;
    }
    if (!caller->pc || !caller->sp) return ENOEXEC;
    const struct mach_header_64 *mainImage = NULL;
    intptr_t slide = 0;
    int error = findExecutable(caller->pc, &mainImage, &slide);
    if (error) { fprintf(stderr, "fork trial executable lookup errno=%d\n", error); return error; }
    return prepareForkTrial(attempt, caller, mode, flags, mainImage, slide);
}

static int prepareResumedBash(LSBForkAttempt *attempt, const LSBForkContext *caller) {
    if (!resumedBash) return LSB_FORK_NOT_HANDLED;
    bool procursus = forkTrialProcursusBashImage(resumedBash);
    if (caller->pc != resumedBash + (procursus ? ProcursusBashForkReturn : BashForkReturn)) return LSB_FORK_NOT_HANDLED;
    if (resumedDepth >= TrialMaxDepth) return ENOTSUP;
    // The pinned PIE's __TEXT vmaddr is 0x100000000. Its signed segments are still mapped at the original guest addresses.
    if (resumedBash < LSB_GUEST_MAIN_IMAGE_OFFSET || (!procursus && !forkTrialBashImage(resumedBash))) return ENOEXEC;
    return prepareForkTrial(attempt, caller, TrialShell, 0, (const void *)resumedBash, (intptr_t)(resumedBash - LSB_GUEST_MAIN_IMAGE_OFFSET));
}
