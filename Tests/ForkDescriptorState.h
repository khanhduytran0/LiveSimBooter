// Disposable fork trial: transfer actual kernel open-file descriptions only at libc's prepared checkpoint.
#pragma once
#include "../ProcessSupport/SpawnPlan.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

extern int __proc_info(int, int, unsigned, uint64_t, void *, int);
extern ssize_t __sendmsg_nocancel(int, const struct msghdr *, int);
extern ssize_t __read_nocancel(int, void *, size_t);
extern ssize_t __write_nocancel(int, const void *, size_t);

typedef struct { uintptr_t machReturn, workqReturn, keventReturn; uint64_t stopWitnessThread; dev_t nullDevice; } ForkDescriptorTraps;
typedef struct { int32_t fd; uint32_t type; } ForkDescriptorInfo;
typedef struct { uint32_t openFlags, status; int64_t offset; int32_t type; uint32_t guards; } ForkFileInfo;
typedef struct { uint16_t number, flags; } ForkDescriptorEntry;
typedef struct { uint32_t magic, count; ForkDescriptorEntry entries[LSB_MAX_FDS]; } ForkDescriptorPacket;
_Static_assert(sizeof(ForkFileInfo) == 24 && sizeof(ForkDescriptorEntry) == 4 && sizeof(ForkDescriptorInfo) == 8, "XNU file info and fork rights layout");

enum { ForkSDRightsMagic = 0x46445331, ForkSDMarker = 0xd7, ForkSDGetConfined = 96, ForkFDMaxPeers = 8 };
typedef union { struct cmsghdr alignment; unsigned char bytes[CMSG_SPACE(sizeof(int) * LSB_MAX_FDS)]; } ForkAncillary;

static inline uintptr_t forkDescriptorTrap(const char *name) {
    const uint32_t *code = dlsym(RTLD_DEFAULT, name);
    if (!code) return 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (code[i] == 0xd4001001u) return (uintptr_t)&code[i + 1]; // ARM64 Darwin SVC #0x80
        if (code[i] == 0xd65f03c0u) break;
    }
    return 0;
}

static inline int forkDescriptorPrepare(ForkDescriptorTraps *traps) {
    struct stat st;
    if (stat("/dev/null", &st)) return errno;
    if (!S_ISCHR(st.st_mode)) return ENOTSUP;
    *traps = (ForkDescriptorTraps){.machReturn = forkDescriptorTrap("mach_msg2_trap"), .workqReturn = forkDescriptorTrap("__workq_kernreturn"),
                                  .keventReturn = forkDescriptorTrap("kevent64"), .nullDevice = st.st_rdev};
    return 0; // Single-thread donors also work when either native trap cannot be resolved.
}

typedef struct {
    mach_port_t self, held[ForkFDMaxPeers];
    unsigned count;
    sigset_t savedSignals;
    bool signalsBlocked;
} ForkDescriptorPeers;

// libc's prepare handlers can hold the dyld/allocator locks. Keep syscall imports non-lazy, including rarely used failure cleanup.
static const volatile struct {
    __typeof__(&mach_port_deallocate) releasePort;
    __typeof__(&vm_deallocate) releaseArray;
    __typeof__(&task_threads) listThreads;
    __typeof__(&mach_thread_self) currentThread;
    __typeof__(&thread_suspend) suspendThread;
    __typeof__(&thread_resume) resumeThread;
    __typeof__(&thread_get_state) getState;
    __typeof__(&thread_info) threadInfo;
    __typeof__(&sigprocmask) setMask;
    __typeof__(&__proc_info) procInfo;
    __typeof__(&fcntl) fcntlFD;
    __typeof__(&fstat) statFD;
    __typeof__(&__sendmsg_nocancel) sendMsg;
    __typeof__(&__write_nocancel) writeBytes;
    __typeof__(&__read_nocancel) readBytes;
} forkFDOperations = {
    mach_port_deallocate, vm_deallocate, task_threads, mach_thread_self, thread_suspend,
    thread_resume, thread_get_state, thread_info, sigprocmask, __proc_info,
    fcntl, fstat, __sendmsg_nocancel, __write_nocancel, __read_nocancel,
};

static inline void forkDescriptorReleaseList(thread_act_array_t list, mach_msg_type_number_t count) {
    for (unsigned i = 0; i < count; ++i) if (MACH_PORT_VALID(list[i])) forkFDOperations.releasePort(mach_task_self(), list[i]);
    if (list) forkFDOperations.releaseArray(mach_task_self(), (vm_address_t)list, (vm_size_t)count * sizeof(*list));
}

static inline int forkDescriptorSameThreads(const ForkDescriptorPeers *peers) {
    thread_act_array_t list = NULL;
    mach_msg_type_number_t count = 0;
    if (forkFDOperations.listThreads(mach_task_self(), &list, &count) != KERN_SUCCESS) return EBUSY;
    bool same = count == peers->count + 1;
    for (unsigned i = 0; same && i < count; ++i) {
        if (list[i] == peers->self) continue;
        unsigned j = 0;
        while (j < peers->count && peers->held[j] != list[i]) ++j;
        same = j != peers->count;
    }
    forkDescriptorReleaseList(list, count);
    return same ? 0 : EAGAIN;
}

static inline int forkDescriptorPark(const ForkDescriptorTraps *traps, ForkDescriptorPeers *peers) {
    *peers = (ForkDescriptorPeers){};
    sigset_t blocked;
    sigfillset(&blocked);
    if (forkFDOperations.setMask(SIG_SETMASK, &blocked, &peers->savedSignals) < 0) return errno;
    peers->signalsBlocked = true; // Even a signal handler on the forking thread can close or replace an FD.
    peers->self = forkFDOperations.currentThread();
    thread_act_array_t list = NULL;
    mach_msg_type_number_t count = 0;
    if (forkFDOperations.listThreads(mach_task_self(), &list, &count) != KERN_SUCCESS) return EBUSY;
    int error = count > ForkFDMaxPeers + 1 ? ENOTSUP : 0;
    bool selfListed = false;
    for (unsigned i = 0; !error && i < count; ++i) {
        thread_t peer = list[i];
        if (peer == peers->self) { selfListed = true; continue; }
        if (peers->count == ForkFDMaxPeers) { error = ENOTSUP; break; }
        if (forkFDOperations.suspendThread(peer) != KERN_SUCCESS) { error = EBUSY; break; }
        peers->held[peers->count++] = peer;
        list[i] = MACH_PORT_NULL; // Keep this send right until this specific suspension has been balanced.
        arm_thread_state64_t state = {};
        mach_msg_type_number_t size = ARM_THREAD_STATE64_COUNT;
        if (forkFDOperations.getState(peer, ARM_THREAD_STATE64, (thread_state_t)&state, &size) != KERN_SUCCESS || size != ARM_THREAD_STATE64_COUNT) {
            error = EBUSY;
            break;
        }
        thread_basic_info_data_t basic = {};
        mach_msg_type_number_t words = THREAD_BASIC_INFO_COUNT;
        uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state);
        bool idle = pc && (pc == traps->machReturn || pc == traps->workqReturn);
        if (!idle && pc && pc == traps->keventReturn && traps->stopWitnessThread) {
            thread_identifier_info_data_t id;
            mach_msg_type_number_t n = THREAD_IDENTIFIER_INFO_COUNT;
            idle = !forkFDOperations.threadInfo(peer, THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &n) &&
                   n == THREAD_IDENTIFIER_INFO_COUNT && id.thread_id == traps->stopWitnessThread;
        }
        if (!idle || forkFDOperations.threadInfo(peer, THREAD_BASIC_INFO, (thread_info_t)&basic, &words) != KERN_SUCCESS || words != THREAD_BASIC_INFO_COUNT ||
            basic.suspend_count < 1 || (basic.run_state != TH_STATE_STOPPED && basic.run_state != TH_STATE_WAITING)) error = EAGAIN;
    }
    forkDescriptorReleaseList(list, count);
    if (!error) error = selfListed ? forkDescriptorSameThreads(peers) : EBUSY;
    return error;
}

static inline int forkDescriptorUnpark(ForkDescriptorPeers *peers) {
    int error = 0;
    while (peers->count) {
        mach_port_t thread = peers->held[--peers->count];
        if (forkFDOperations.resumeThread(thread) != KERN_SUCCESS) error = EBUSY;
        forkFDOperations.releasePort(mach_task_self(), thread);
    }
    if (MACH_PORT_VALID(peers->self)) forkFDOperations.releasePort(mach_task_self(), peers->self);
    peers->self = MACH_PORT_NULL;
    if (peers->signalsBlocked && forkFDOperations.setMask(SIG_SETMASK, &peers->savedSignals, NULL) < 0 && !error) error = errno;
    peers->signalsBlocked = false;
    return error;
}

// XNU proc_info.h, PROC_PIDLISTFDS and the PROC_PIDFD*INFO flavors. Each type-specific result starts with struct proc_fileinfo.
static inline int forkDescriptorList(pid_t pid, int socket, int wake, int watch, dev_t nullDevice, ForkDescriptorPacket *packet, int fds[LSB_MAX_FDS]) {
    ForkDescriptorInfo list[LSB_MAX_FDS + 4];
    int bytes = forkFDOperations.procInfo(2, pid, 1, 0, list, sizeof(list));
    if (bytes < 0) return errno;
    if (bytes >= (int)sizeof(list) || bytes % sizeof(*list)) return E2BIG;
    *packet = (ForkDescriptorPacket){.magic = ForkSDRightsMagic};
    for (unsigned i = 0; i < (unsigned)bytes / sizeof(*list); ++i) {
        int fd = list[i].fd;
        if (fd == socket || fd == wake || fd == watch) continue;
        if (fd < 0) return EPROTO;
        // XNU skips confined fileglobs on fork. In particular, a confined kqueue cannot be used for SCM_RIGHTS.
        int confined = forkFDOperations.fcntlFD(fd, ForkSDGetConfined);
        if (confined < 0) return errno;
        if (confined) continue;
        unsigned flavor;
        switch (list[i].type) {
            case 1: flavor = 1; break; // vnode
            case 2: flavor = 3; break; // socket
            case 3: flavor = 5; break; // POSIX SHM
            case 4: flavor = 4; break; // POSIX semaphore
            case 5: flavor = 7; break; // kqueue
            case 6: flavor = 6; break; // pipe
            default: return ENOTSUP;
        }
        union { ForkFileInfo info; uint64_t spare[256]; } detail;
        int size = forkFDOperations.procInfo(3, pid, flavor, (uint64_t)fd, &detail, sizeof(detail));
        if (size < (int)sizeof(detail.info)) return size < 0 ? errno : EIO;
        if (detail.info.status & 8) continue; // FP_CLOFORK is not reported by F_GETFD on 23B50.
        if ((detail.info.status & 4) || detail.info.guards) return ENOTSUP; // Sending a guarded fd can terminate the donor.
        if (flavor != 1 && flavor != 3 && flavor != 6) return ENOTSUP;
        if (fd >= LSB_MAX_FDS || packet->count >= LSB_MAX_FDS) return E2BIG;
        if (flavor == 1) {
            struct stat st;
            if (forkFDOperations.statFD(fd, &st)) return errno;
            if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !(S_ISCHR(st.st_mode) && st.st_rdev == nullDevice)) return ENOTSUP;
        }
        if (packet->count && fd <= packet->entries[packet->count - 1].number) return EPROTO;
        unsigned j = packet->count++;
        packet->entries[j] = (ForkDescriptorEntry){(uint16_t)fd, (uint16_t)((detail.info.status & 2) ? FD_CLOEXEC : 0)};
        fds[j] = fd;
    }
    return 0;
}

static inline int forkDescriptorBytes(int socket, void *buffer, size_t bytes, bool send) {
    unsigned char *ptr = buffer;
    while (bytes) {
        ssize_t n = send ? forkFDOperations.writeBytes(socket, ptr, bytes) : forkFDOperations.readBytes(socket, ptr, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n ? errno : EPIPE;
        bytes -= (size_t)n;
        ptr += n;
    }
    return 0;
}

// The caller must unpark on success AND failure, after the child has finished its checkpoint or the attempt is cancelled.
static inline int forkDescriptorSendParked(int socket, pid_t pid, int wake, int watch, const ForkDescriptorTraps *traps, ForkDescriptorPeers *peers) {
    int error = forkDescriptorPark(traps, peers);
    ForkDescriptorPacket packet = {};
    int fds[LSB_MAX_FDS];
    if (!error) error = forkDescriptorList(pid, socket, wake, watch, traps->nullDevice, &packet, fds);
    if (!error) {
        unsigned char marker = ForkSDMarker;
        struct iovec io = {&marker, 1};
        ForkAncillary ancillary = {};
        struct msghdr msg = {.msg_iov = &io, .msg_iovlen = 1};
        if (packet.count) {
            msg.msg_control = ancillary.bytes;
            msg.msg_controllen = (socklen_t)CMSG_SPACE(packet.count * sizeof(int));
            struct cmsghdr *rights = CMSG_FIRSTHDR(&msg);
            rights->cmsg_level = SOL_SOCKET;
            rights->cmsg_type = SCM_RIGHTS;
            rights->cmsg_len = (socklen_t)CMSG_LEN(packet.count * sizeof(int));
            memcpy(CMSG_DATA(rights), fds, packet.count * sizeof(int));
        }
        ssize_t sent;
        do { sent = forkFDOperations.sendMsg(socket, &msg, MSG_DONTWAIT); } while (sent < 0 && errno == EINTR);
        if (sent != 1) error = sent < 0 ? errno : EPIPE;
        if (!error) error = forkDescriptorSameThreads(peers);
    }
    return error ? error : forkDescriptorBytes(socket, &packet, sizeof(packet), true);
}

// Independent FD-only control. The actual fork holds peers across the VM checkpoint using the entry point above.
static inline int forkDescriptorSend(int socket, pid_t pid, int wake, int watch, const ForkDescriptorTraps *traps) {
    ForkDescriptorPeers peers = {};
    int error = forkDescriptorSendParked(socket, pid, wake, watch, traps, &peers);
    int resumeError = forkDescriptorUnpark(&peers);
    return error ? error : resumeError;
}

enum { ForkFDReceiving, ForkFDPeerCount, ForkFDControlCollision, ForkFDWakeCollision, ForkFDWatchCollision, ForkFDOccupied, ForkFDStage, ForkFDInstall };
typedef struct {
    int32_t phase, target, type, fdFlags;
    uint32_t threads, status, guards, mode;
    uint64_t inode, device;
} ForkDescriptorIssue;
_Static_assert(sizeof(ForkDescriptorIssue) == 48, "descriptor rejection receipt");

static inline void forkDescriptorDescribe(ForkDescriptorIssue *issue, int phase, int fd) {
    issue->phase = phase;
    issue->target = fd;
    issue->fdFlags = fcntl(fd, F_GETFD);
    struct stat st;
    if (!fstat(fd, &st)) { issue->inode = st.st_ino; issue->device = st.st_dev; issue->mode = st.st_mode; }
    audit_token_t audit = {};
    mach_msg_type_number_t words = TASK_AUDIT_TOKEN_COUNT;
    if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &words) != KERN_SUCCESS) return;
    ForkDescriptorInfo list[LSB_MAX_FDS * 2 + 4];
    int n = __proc_info(2, (int)audit.val[5], 1, 0, list, sizeof(list));
    for (int i = 0; i < n / (int)sizeof(*list); ++i) if (list[i].fd == fd) {
        issue->type = (int32_t)list[i].type;
        const unsigned flavor[] = {0, 1, 3, 5, 4, 7, 6};
        if (list[i].type < sizeof(flavor) / sizeof(flavor[0]) && flavor[list[i].type]) {
            union { ForkFileInfo f; uint64_t spare[256]; } detail;
            if (__proc_info(3, (int)audit.val[5], flavor[list[i].type], (uint64_t)fd, &detail, sizeof(detail)) >= (int)sizeof(detail.f)) {
                issue->status = detail.f.status; issue->guards = detail.f.guards;
            }
        }
        break;
    }
}

static inline int forkDescriptorReceiveReport(int socket, int wake, int watch, unsigned bootstrapStdioMask, ForkDescriptorIssue *issue) {
    *issue = (ForkDescriptorIssue){.target = -1, .type = -1, .fdFlags = -1};
    if (bootstrapStdioMask & ~7u) return EINVAL;
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t nthreads = 0;
    if (task_threads(mach_task_self(), &threads, &nthreads) != KERN_SUCCESS) { issue->phase = ForkFDPeerCount; return EBUSY; }
    issue->threads = nthreads;
    forkDescriptorReleaseList(threads, nthreads);
    if (nthreads != 1) { issue->phase = ForkFDPeerCount; return EAGAIN; } // The terminal bootstrap cannot race another thread's open/close during exact-number replay.
    ForkAncillary ancillary = {};
    unsigned char marker = 0;
    struct iovec iov = {&marker, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    ssize_t n;
    do { n = recvmsg(socket, &msg, 0); } while (n < 0 && errno == EINTR);
    if (n != 1) return n < 0 ? errno : EPIPE;
    int received[LSB_MAX_FDS], staged[LSB_MAX_FDS];
    unsigned count = 0;
    int error = (msg.msg_flags & MSG_CTRUNC) || marker != ForkSDMarker ? EPROTO : 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        size_t remaining = msg.msg_controllen - (size_t)((unsigned char *)c - ancillary.bytes);
        if (remaining < sizeof(*c) || c->cmsg_len < sizeof(*c) || c->cmsg_len > remaining) { error = EPROTO; break; }
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0)) { error = EPROTO; continue; }
        size_t payload = c->cmsg_len - CMSG_LEN(0);
        if (payload % sizeof(int)) error = EPROTO;
        const int *rights = (const void *)CMSG_DATA(c);
        for (unsigned j = 0; j < payload / sizeof(int); ++j) {
            if (count == LSB_MAX_FDS) { close(rights[j]); error = E2BIG; }
            else { received[count] = rights[j]; staged[count++] = -1; }
        }
    }
    ForkDescriptorPacket packet = {};
    if (!error) error = forkDescriptorBytes(socket, &packet, sizeof(packet), false);
    if (!error && (packet.magic != ForkSDRightsMagic || packet.count != count)) error = EPROTO;
    unsigned stdioToClose = bootstrapStdioMask;
    int stagedMinimum = !error && count ? (int)packet.entries[count - 1].number + 1 : 3;
    for (unsigned i = 0; !error && i < count; ++i) {
        int target = packet.entries[i].number;
        if (target >= LSB_MAX_FDS || (i && packet.entries[i-1].number >= target) || (packet.entries[i].flags & ~FD_CLOEXEC)) { error = EPROTO; break; }
        if (target == socket || target == wake || target == watch) {
            forkDescriptorDescribe(issue, target == socket ? ForkFDControlCollision : target == wake ? ForkFDWakeCollision : ForkFDWatchCollision, target);
            error = EBUSY; break;
        }
        if (target <= 2) stdioToClose &= ~(1u << target);
        staged[i] = fcntl(received[i], F_DUPFD_CLOEXEC, stagedMinimum);
        if (staged[i] < 0) { error = errno; forkDescriptorDescribe(issue, ForkFDStage, received[i]); }
    }
    for (unsigned i = 0; i < count; ++i) close(received[i]);
    for (unsigned i = 0; !error && i < count; ++i) {
        int target = packet.entries[i].number;
        if (target <= 2 && (bootstrapStdioMask & (1u << target))) continue;
        if (fcntl(target, F_GETFD) >= 0) { forkDescriptorDescribe(issue, ForkFDOccupied, target); error = EBUSY; }
        else if (errno != EBADF) error = errno;
    }
    for (unsigned i = 0; !error && i < count; ++i) {
        int target = packet.entries[i].number;
        if (dup2(staged[i], target) < 0 || fcntl(target, F_SETFD, packet.entries[i].flags) < 0) {
            error = errno; forkDescriptorDescribe(issue, ForkFDInstall, target);
        }
    }
    for (unsigned i = 0; i < count; ++i) if (staged[i] >= 0) close(staged[i]);
    for (unsigned fd = 0; !error && fd < 3; ++fd)
        if ((stdioToClose & (1u << fd)) && fd != (unsigned)socket && fd != (unsigned)wake && fd != (unsigned)watch && close((int)fd) < 0 && errno != EBADF)
            error = errno;
    return error;
}

static inline int forkDescriptorReceive(int socket, int wake, int watch, unsigned bootstrapStdioMask) {
    ForkDescriptorIssue issue;
    return forkDescriptorReceiveReport(socket, wake, watch, bootstrapStdioMask, &issue);
}
