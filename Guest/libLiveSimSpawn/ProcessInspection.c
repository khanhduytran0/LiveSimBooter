#include "ProcessInspection.h"
#include "ProcessIdentity.h"
#include "RuntimeDirectories.h"
#if defined(LSB_GUEST_RUNTIME) || defined(LSB_PROCESS_INSPECTION_TESTING)
#include "ProcessDebugger.h"
#endif
#include "../../ProcessSupport/SpawnTransport.h"
#include "../../ProcessSupport/GuestExecutablePath.h"
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);
extern kern_return_t mach_vm_read_overwrite(vm_map_t, mach_vm_address_t, mach_vm_size_t, mach_vm_address_t, mach_vm_size_t *);
extern kern_return_t mach_vm_write(vm_map_t, mach_vm_address_t, vm_offset_t, mach_msg_type_number_t);

// PROC_PIDT_SHORTBSDINFO is omitted from the Simulator SDK. This is the public libproc layout, not a kinfo_proc memory patch.
typedef struct {
    uint32_t pid, ppid, pgid, status;
    char comm[16];
    uint32_t flags, uid, gid, ruid, rgid, svuid, svgid, reserved;
} InspectionBSDInfo;
_Static_assert(sizeof(InspectionBSDInfo) == LSB_PEER_SHORT_INFO_BYTES, "short BSD info ABI");

static _Atomic(bool) inspectionReady, hasProcessSnapshot;
static pthread_mutex_t snapshotLock = PTHREAD_MUTEX_INITIALIZER;
static LSBProcessList *processSnapshot;

static bool readBytes(const void *input, void *output, size_t size) {
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)input, size,
                                  (mach_vm_address_t)(uintptr_t)output, &copied) == KERN_SUCCESS && copied == size;
}
static bool writeBytes(void *output, const void *input, size_t size) {
    return size <= UINT32_MAX && mach_vm_write(mach_task_self(), (mach_vm_address_t)(uintptr_t)output,
                                               (vm_offset_t)(uintptr_t)input, (mach_msg_type_number_t)size) == KERN_SUCCESS;
}

static void fillKinfo(struct kinfo_proc *info, const InspectionBSDInfo *bsd, pid_t pid, pid_t parent, const char *path) {
    memset(info, 0, sizeof(*info));
    info->kp_proc.p_pid = pid;
    info->kp_eproc.e_ppid = parent;
    info->kp_proc.p_stat = (char)bsd->status;
    // libproc PROC_FLAG_* and kinfo P_* have different bit positions. Preserve only flags with an exact documented equivalent.
    if (bsd->flags & 1u) info->kp_proc.p_flag |= P_SYSTEM;
    if (bsd->flags & 2u) info->kp_proc.p_flag |= P_TRACED;
    if (bsd->flags & 4u) info->kp_proc.p_flag |= P_WEXIT;
    if (bsd->flags & 8u) info->kp_proc.p_flag |= P_PPWAIT;
    info->kp_proc.p_flag |= P_LP64; // Every managed simulator guest is arm64, including records without native flag 0x10.
    info->kp_eproc.e_ucred.cr_uid = bsd->uid;
    info->kp_eproc.e_ucred.cr_ngroups = 1;
    info->kp_eproc.e_ucred.cr_groups[0] = bsd->gid;
    info->kp_eproc.e_pcred.p_ruid = bsd->ruid;
    info->kp_eproc.e_pcred.p_rgid = bsd->rgid;
    info->kp_eproc.e_pcred.p_svuid = bsd->svuid;
    info->kp_eproc.e_pcred.p_svgid = bsd->svgid;
    info->kp_eproc.e_tdev = (dev_t)-1;
    const char *name = path ? strrchr(path, '/') : NULL;
    if (name) strlcpy(info->kp_proc.p_comm, name + 1, sizeof(info->kp_proc.p_comm));
    else memcpy(info->kp_proc.p_comm, bsd->comm, sizeof(bsd->comm));
}

static int copyOutput(void *output, size_t *length, const void *bytes, size_t required, size_t itemSize) {
    size_t available = 0;
    if (!length || !readBytes(length, &available, sizeof(available))) { errno = EFAULT; return -1; }
    // Darwin returns complete values/records. A short buffer reports the amount actually copied, not the allocation size.
    size_t copied = output && available < required ? available - available % itemSize : required;
    if (output && copied && !writeBytes(output, bytes, copied)) { errno = EFAULT; return -1; }
    if (!writeBytes(length, &copied, sizeof(copied))) { errno = EFAULT; return -1; }
    if (output && available < required) { errno = ENOMEM; return -1; }
    return 0;
}

// XNU sysctl_oidfmt returns a uint32_t kind followed immediately by the NUL-terminated format string.
typedef struct { uint32_t kind; char format[sizeof("S,timeval")]; } InspectionOIDFormat;
_Static_assert(offsetof(InspectionOIDFormat, format) == sizeof(uint32_t), "sysctl OIDFMT layout");
enum { InspectionOIDName = 1, InspectionOIDFmt = 4, InspectionInternalPages = 3, InspectionPurgeablePages = 4 };
static struct {
    const char *name;
    int group;
    InspectionOIDFormat format;
    int oid;
} metrics[] = {
    // Type and read-only flags measured from the native Darwin 25 sysctl tree on 23B50. OID_AUTO numbers are looked up on the running kernel.
    {"kern.boottime", CTL_KERN, {0x81e00005, "S,timeval"}, 0},
    {"hw.pagesize", CTL_HW, {0x81e00004, "Q"}, 0},
    {"hw.memsize", CTL_HW, {0x81e00004, "Q"}, 0},
    [InspectionInternalPages] = {"vm.page_pageable_internal_count", CTL_VM, {0x80e00002, "IU"}, 0},
    [InspectionPurgeablePages] = {"vm.page_purgeable_count", CTL_VM, {0x80e00002, "I"}, 0}
};
static pthread_once_t metricOnce = PTHREAD_ONCE_INIT;

static void initializeMetricOIDs(void) {
    const LSBGuestRuntimeDirectories *dirs;
    char path[PATH_MAX];
    uint32_t capacity = sizeof(path);
    if (LSBGetRuntimeDirectories(&dirs) || !dirs->bootstrapLength || _NSGetExecutablePath(path, &capacity) ||
        !LSBGuestPathInBootstrap(dirs->runtime, path)) return;
    const char *suffix = path + strlen(LSBGuestStorageRootSpelling(dirs->bootstrap, path));
    if (strcmp(suffix, "/usr/sbin/sysctl") && strcmp(suffix, "/sbin/sysctl")) return;
    for (unsigned i = 0; i < sizeof(metrics) / sizeof(*metrics); ++i) {
        int oid[CTL_MAXNAME];
        size_t n = CTL_MAXNAME;
        if (!sysctlnametomib(metrics[i].name, oid, &n) && n == 2 && oid[0] == metrics[i].group && oid[1] > 0) metrics[i].oid = oid[1];
    }
}

static int readMetric(int *mib, u_int count, void *output, size_t *length) {
    int saved = errno;
    int result = sysctl(mib, count, output, length, NULL, 0);
    if (result != -1 || errno != EPERM) return result;
    pthread_once(&metricOnce, initializeMetricOIDs);
    const int *key = mib + count - 2;
    for (unsigned i = 0; i < sizeof(metrics) / sizeof(*metrics); ++i) {
        if (!metrics[i].oid || key[0] != metrics[i].group || key[1] != metrics[i].oid) continue;
        if (count == 4) {
            const InspectionOIDFormat *format = &metrics[i].format;
            const char *name = metrics[i].name;
            result = mib[1] == InspectionOIDName ? copyOutput(output, length, name, strlen(name) + 1, 1) :
                     copyOutput(output, length, format, sizeof(format->kind) + strlen(format->format) + 1, 1);
        } else {
            vm_statistics64_data_t vm;
            mach_msg_type_number_t words = HOST_VM_INFO64_COUNT;
            host_t host = mach_host_self();
            kern_return_t kr = host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &words);
            mach_port_deallocate(mach_task_self(), host);
            if (kr || words * sizeof(integer_t) < offsetof(vm_statistics64_data_t, internal_page_count) + sizeof(vm.internal_page_count)) break;
            // XNU host_statistics64 reports internal pages including the local queue. Its purgeable count is the same global VM counter.
            uint32_t pages = i == InspectionInternalPages ? vm.internal_page_count : vm.purgeable_count;
            result = copyOutput(output, length, &pages, sizeof(pages), sizeof(pages));
        }
        if (!result) errno = saved;
        return result;
    }
    errno = EPERM;
    return -1;
}

int LSBInspectionUnsupportedPID(int pid) {
    if (!atomic_load_explicit(&hasProcessSnapshot, memory_order_acquire)) return 0;
    if (pid > 0 && pid == LSBGuestPID()) return 0;
    int saved = errno, error = ESRCH;
    pthread_mutex_lock(&snapshotLock);
    for (uint32_t i = 0; i < processSnapshot->count; ++i) if (processSnapshot->records[i].guestPID == pid) {
        error = ENOTSUP;
        break;
    }
    pthread_mutex_unlock(&snapshotLock);
    // A reused native self PID can equal another live job's logical PID after exec. The known guest namespace wins.
    if (error == ESRCH && pid > 0 && pid == LSBNativePID()) error = 0;
    errno = saved;
    return error;
}

static int inspectionRusage(int pid, int flavor, rusage_info_t *buffer) {
    int error = LSBInspectionUnsupportedPID(pid);
    if (error) { errno = error; return -1; }
    if (atomic_load_explicit(&hasProcessSnapshot, memory_order_acquire) && pid == LSBGuestPID()) pid = LSBNativePID();
    return proc_pid_rusage(pid, flavor, buffer);
}

static kern_return_t inspectionTaskForPID(mach_port_name_t target, int pid, mach_port_name_t *task) {
    if (!atomic_load_explicit(&inspectionReady, memory_order_acquire)) return task_for_pid(target, pid, task);
    int saved = errno;
    bool self = pid > 0 && (pid == LSBGuestPID() || (pid == LSBNativePID() && !LSBInspectionUnsupportedPID(pid)));
    if (self) {
        if (target != mach_task_self()) return task_for_pid(target, LSBNativePID(), task);
        mach_port_t copy = MACH_PORT_NULL;
        if (!task || !writeBytes(task, &copy, sizeof(copy))) { errno = saved; return KERN_INVALID_ADDRESS; }
        copy = mach_task_self();
        // This immovable right stays in its own process. Add the reference callers expect to release after task_for_pid succeeds.
        kern_return_t result = mach_port_mod_refs(mach_task_self(), copy, MACH_PORT_RIGHT_SEND, 1);
        if (!result && !writeBytes(task, &copy, sizeof(copy))) {
            mach_port_deallocate(mach_task_self(), copy);
            result = KERN_INVALID_ADDRESS;
        }
        errno = saved;
        return result;
    }
    int error = 0;
    if (target != mach_task_self() || pid <= 0) {
        error = LSBInspectionUnsupportedPID(pid);
        if (!error) return task_for_pid(target, pid, task);
    }
    mach_port_t copy = MACH_PORT_NULL;
    if (!task || !writeBytes(task, &copy, sizeof(copy))) { errno = saved; return KERN_INVALID_ADDRESS; }
    // Authorization and live logical/native identity resolution belong to the broker, independently of a prior process-list snapshot.
    if (!error) error = LSBTransportCopyProcessTask(pid, &copy);
    if (!error && !writeBytes(task, &copy, sizeof(copy))) {
        mach_port_deallocate(mach_task_self(), copy);
        errno = saved;
        return KERN_INVALID_ADDRESS;
    }
    errno = saved;
    if (!error) return KERN_SUCCESS;
    if (error == EPERM || error == EACCES) return KERN_PROTECTION_FAILURE;
    if (error == ESRCH || error == EINVAL) return KERN_INVALID_ARGUMENT;
    return error == ENOTSUP ? KERN_NOT_SUPPORTED : KERN_FAILURE;
}

static int inspectionSysctl(int *name, u_int count, void *output, size_t *length, void *input, size_t inputSize) {
    // No errno, allocation, or native TLS access before constructors. Unmatched queries keep the original API and errors.
    if (!atomic_load_explicit(&inspectionReady, memory_order_acquire) || (count != 2 && count != 3 && count != 4))
        return sysctl(name, count, output, length, input, inputSize);
    int mib[4] = {0};
    if (!readBytes(name, mib, count * sizeof(*mib)))
        return sysctl(name, count, output, length, input, inputSize);
    if (!input && !inputSize && ((count == 2 && mib[0] == CTL_VM) ||
        (count == 4 && mib[0] == CTL_UNSPEC && (mib[1] == InspectionOIDName || mib[1] == InspectionOIDFmt) &&
         (mib[2] == CTL_KERN || mib[2] == CTL_HW || mib[2] == CTL_VM)))) return readMetric(mib, count, output, length);
    if (mib[0] != CTL_KERN)
        return sysctl(name, count, output, length, input, inputSize);
    if (count == 4 && mib[1] == KERN_PROC && mib[2] == KERN_PROC_PID && mib[3] > 0 &&
        (mib[3] == LSBGuestPID() || mib[3] == LSBNativePID()) && !LSBInspectionUnsupportedPID(mib[3])) {
        // Simulator druid checks its debugger state using getpid(), which is logical after an exec replacement.
        // The native kernel accepts only its current real PID. Preserve the genuine reply, including current P_TRACED and errno.
        mib[3] = LSBNativePID();
        return sysctl(mib, count, output, length, input, inputSize);
    }
    bool arguments = count == 3 && mib[1] == KERN_PROCARGS2;
    bool process = count == 4 && mib[1] == KERN_PROC && mib[2] == KERN_PROC_PID;
#if defined(LSB_GUEST_RUNTIME) || defined(LSB_PROCESS_INSPECTION_TESTING)
    if (process && mib[3] > 0 && !input && !inputSize) {
        int saved = errno, error = 0;
        struct kinfo_proc info;
        if (LSBGuestDebuggerCopyInfo(mib[3], &info, &error)) {
            if (error) { errno = error; return -1; }
            int result = copyOutput(output, length, &info, sizeof(info), sizeof(info));
            if (!result) errno = saved;
            return result;
        }
    }
#endif
    if ((arguments || process) && atomic_load_explicit(&hasProcessSnapshot, memory_order_acquire)) {
        unsigned index = arguments ? 2 : 3;
        int error = LSBInspectionUnsupportedPID(mib[index]);
        if (error) { errno = error; return -1; }
        mib[index] = LSBNativePID();
        return sysctl(mib, count, output, length, input, inputSize);
    }
    if (input || inputSize) return sysctl(name, count, output, length, input, inputSize);
    bool maximum = count == 2 && mib[1] == KERN_MAXPROC;
    bool all = count == 4 && mib[1] == KERN_PROC && mib[2] == KERN_PROC_ALL && !mib[3];
    if (!maximum && !all) return sysctl(name, count, output, length, input, inputSize);
    int saved = errno;
    LSBProcessList *list = NULL;
    size_t size = 0;
    int error = LSBTransportListProcesses(&list, &size);
    if (error) { errno = error; return -1; } // Never fall back to enumerating native processes after an authorization denial.
    struct kinfo_proc *processes = NULL;
    if (all && output && list->count) {
        processes = calloc(list->count, sizeof(*processes));
        if (!processes) { free(list); errno = ENOMEM; return -1; }
        for (uint32_t i = 0; i < list->count; ++i) {
            InspectionBSDInfo bsd;
            memcpy(&bsd, list->records[i].shortInfo, sizeof(bsd));
            fillKinfo(&processes[i], &bsd, list->records[i].guestPID, list->records[i].parentPID, list->records[i].executablePath);
        }
    }
    int capacity = LSB_PROCESS_LIST_CAPACITY;
    int result = maximum ? copyOutput(output, length, &capacity, sizeof(capacity), sizeof(capacity)) :
                           copyOutput(output, length, processes, list->count * sizeof(*processes), sizeof(*processes));
    int outputError = errno;
    free(processes);
    pthread_mutex_lock(&snapshotLock);
    LSBProcessList *previous = processSnapshot;
    processSnapshot = list;
    atomic_store_explicit(&hasProcessSnapshot, true, memory_order_release);
    pthread_mutex_unlock(&snapshotLock);
    free(previous);
    errno = result ? outputError : saved;
    return result;
}

bool LSBInspectionPIDPath(int pid, void *buffer, uint32_t size, int *result) {
    if (!atomic_load_explicit(&hasProcessSnapshot, memory_order_acquire)) return false;
    int saved = errno, error = ESRCH;
    *result = 0;
    pthread_mutex_lock(&snapshotLock);
    for (uint32_t i = 0; i < processSnapshot->count; ++i) if (processSnapshot->records[i].guestPID == pid) {
        // Like macOS Simulator, proc_pidpath exposes physical runtime and installed-app executables for bundle/resource lookup.
        char path[PATH_MAX];
        const LSBGuestRuntimeDirectories *directories;
        int pathError = LSBGetRuntimeDirectories(&directories);
        if (!pathError) pathError = LSBGuestExecutablePhysicalPath(directories->runtime, processSnapshot->records[i].executablePath, path, sizeof(path));
        size_t length = pathError ? 0 : strlen(path);
        if (!buffer) error = EFAULT;
        else if (pathError) error = pathError;
        else if ((uint32_t)length >= size) error = ENOSPC;
        else if (!writeBytes(buffer, path, (size_t)length + 1)) error = EFAULT;
        else { *result = length; error = 0; }
        break;
    }
    pthread_mutex_unlock(&snapshotLock);
    errno = error ? error : saved;
    return true;
}

#ifndef LSB_PROCESS_INSPECTION_TESTING
__attribute__((constructor)) static void initializeInspection(void) {
    atomic_store_explicit(&inspectionReady, true, memory_order_release);
}
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } inspectionInterpose[] = {
    {(const void *)inspectionSysctl, (const void *)sysctl},
    {(const void *)inspectionRusage, (const void *)proc_pid_rusage},
    {(const void *)inspectionTaskForPID, (const void *)task_for_pid},
};
#endif
