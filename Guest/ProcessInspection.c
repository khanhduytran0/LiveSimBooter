#include "ProcessInspection.h"
#include "ProcessIdentity.h"
#if defined(LSB_GUEST_RUNTIME) || defined(LSB_PROCESS_INSPECTION_TESTING)
#include "ProcessDebugger.h"
#endif
#include "../ProcessSupport/SpawnTransport.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/resource.h>
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
    int error = LSBInspectionUnsupportedPID(pid);
    if (error) {
        int saved = errno;
        mach_port_name_t empty = MACH_PORT_NULL;
        bool valid = task && writeBytes(task, &empty, sizeof(empty));
        errno = saved;
        return !valid ? KERN_INVALID_ADDRESS : error == ENOTSUP ? KERN_NOT_SUPPORTED : KERN_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(&hasProcessSnapshot, memory_order_acquire) && pid == LSBGuestPID()) pid = LSBNativePID();
    return task_for_pid(target, pid, task);
}

static int inspectionSysctl(int *name, u_int count, void *output, size_t *length, void *input, size_t inputSize) {
    // No errno, allocation, or native TLS access before constructors. Unmatched queries keep the original API and errors.
    if (!atomic_load_explicit(&inspectionReady, memory_order_acquire) || (count != 2 && count != 3 && count != 4))
        return sysctl(name, count, output, length, input, inputSize);
    int mib[4] = {0};
    if (!readBytes(name, mib, count * sizeof(*mib)) || mib[0] != CTL_KERN)
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
    if (all && list->count) {
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
        const char *path = processSnapshot->records[i].executablePath;
        size_t length = strlen(path);
        if (!buffer) error = EFAULT;
        else if (length >= size) error = ENOSPC;
        else if (!writeBytes(buffer, path, length + 1)) error = EFAULT;
        else { *result = (int)length; error = 0; }
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
