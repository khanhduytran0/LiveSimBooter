#include "../ProcessSupport/SpawnTransport.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

#ifdef LSB_INSPECTION_INTERPOSE_FIXTURE
pid_t LSBNativePID(void) { return getpid(); }
pid_t LSBGuestPID(void) { return getpid() + 100000; }
int LSBTransportCopyProcessTask(pid_t pid, mach_port_t *task) {
    *task = MACH_PORT_NULL;
    return pid == getpid() + 200000 ? ENOTSUP : ESRCH;
}
int LSBTransportListProcesses(LSBProcessList **list, size_t *size) {
    static unsigned requests;
    *list = NULL; *size = 0;
    if (++requests <= 2) return EPERM;
    *size = LSBProcessListSize(1); *list = calloc(1, *size); assert(*list);
    (*list)->version = LSB_PROCESS_LIST_VERSION; (*list)->count = 1; (*list)->recordSize = sizeof(LSBProcessRecord);
    LSBProcessRecord *record = &(*list)->records[0];
    record->guestPID = getpid() + 200000; record->audit.val[5] = getpid() + 300000;
    memcpy(record->shortInfo, &record->audit.val[5], sizeof(uint32_t));
    strcpy(record->executablePath, "/bin/df");
    return 0;
}
#else
extern int proc_pid_rusage(int, int, rusage_info_t *);
int main(void) {
    int native[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    int logical[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() + 100000};
    struct kinfo_proc original = {0}, translated = {0};
    size_t originalSize = sizeof(original), translatedSize = sizeof(translated);
    assert(!sysctl(native, 4, &original, &originalSize, NULL, 0));
    assert(!sysctl(logical, 4, &translated, &translatedSize, NULL, 0));
    assert(originalSize == sizeof(original) && translatedSize == sizeof(translated));
    assert(original.kp_proc.p_pid == getpid() && translated.kp_proc.p_pid == getpid());
    assert((original.kp_proc.p_flag & P_TRACED) == (translated.kp_proc.p_flag & P_TRACED));
    int all[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    assert(sysctl(all, 4, NULL, &translatedSize, NULL, 0) == -1 && errno == EPERM);
    int maximum[] = {CTL_KERN, KERN_MAXPROC}, value;
    size_t size = sizeof(value);
    assert(sysctl(maximum, 2, &value, &size, NULL, 0) == -1 && errno == EPERM);
    int type[] = {CTL_KERN, KERN_OSTYPE}; char name[64] = {0}; size = sizeof(name);
    assert(!sysctl(type, 2, name, &size, NULL, 0) && !strcmp(name, "Darwin"));
    size = 0;
    assert(!sysctl(all, 4, NULL, &size, NULL, 0) && size == sizeof(struct kinfo_proc));
    int arguments[] = {CTL_KERN, KERN_PROCARGS2, getpid() + 200000};
    assert(sysctl(arguments, 3, NULL, &size, NULL, 0) == -1 && errno == ENOTSUP);
    arguments[2] = getpid() + 400000;
    assert(sysctl(arguments, 3, NULL, &size, NULL, 0) == -1 && errno == ESRCH);
    struct rusage_info_v2 usage = {0};
    assert(proc_pid_rusage(getpid() + 200000, RUSAGE_INFO_V2, (rusage_info_t *)&usage) == -1 && errno == ENOTSUP);
    assert(proc_pid_rusage(getpid() + 400000, RUSAGE_INFO_V2, (rusage_info_t *)&usage) == -1 && errno == ESRCH);
    assert(!proc_pid_rusage(getpid() + 100000, RUSAGE_INFO_V2, (rusage_info_t *)&usage) && usage.ri_resident_size > 0);
    mach_port_name_t task = 123;
    assert(task_for_pid(mach_task_self(), getpid() + 200000, &task) == KERN_NOT_SUPPORTED && !task);
    task = 123;
    assert(task_for_pid(mach_task_self(), getpid() + 400000, &task) == KERN_INVALID_ARGUMENT && !task);
    mach_port_urefs_t before = 0, after = 0;
    assert(!mach_port_get_refs(mach_task_self(), mach_task_self(), MACH_PORT_RIGHT_SEND, &before));
    for (unsigned i = 0; i < 10; ++i) {
        errno = EDOM; task = MACH_PORT_NULL;
        assert(!task_for_pid(mach_task_self(), getpid() + (i % 2 ? 100000 : 0), &task) && task == mach_task_self() && errno == EDOM);
        assert(!mach_port_get_refs(mach_task_self(), task, MACH_PORT_RIGHT_SEND, &after) && after == before + 1);
        mach_task_basic_info_data_t info; mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        assert(!task_info(task, MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) && info.resident_size > 0);
        assert(!mach_port_deallocate(mach_task_self(), task));
        assert(!mach_port_get_refs(mach_task_self(), mach_task_self(), MACH_PORT_RIGHT_SEND, &after) && after == before);
    }
    puts("ProcessInspectionInterpose: real self/debugger queries, peer logical-PID guards, self task/rusage and native forwarding passed");
    return 0;
}
#endif
