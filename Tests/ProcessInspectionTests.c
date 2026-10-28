#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <unistd.h>
static int testSysctl(int *, u_int, void *, size_t *, void *, size_t);
static int testRusage(int, int, rusage_info_t *);
static kern_return_t testTaskForPID(mach_port_name_t, int, mach_port_name_t *);
#define sysctl testSysctl
#define proc_pid_rusage testRusage
#define task_for_pid testTaskForPID
#define LSB_PROCESS_INSPECTION_TESTING
#include "../Guest/ProcessInspection.c"
#undef sysctl
#undef proc_pid_rusage
#undef task_for_pid

static unsigned nativeCalls, listCalls;
static int nativeError, listError, lastMIB[4], lastRusagePID, lastTaskPID;
static unsigned rusageCalls, taskCalls;
static bool debuggerEnabled;
static int debuggerError;
static unsigned debuggerCalls;
bool LSBGuestDebuggerCopyInfo(pid_t pid, struct kinfo_proc *info, int *error) {
    if (!debuggerEnabled) return false;
    ++debuggerCalls;
    *error = debuggerError;
    memset(info, 0, sizeof(*info));
    info->kp_proc.p_pid = pid;
    info->kp_proc.p_flag = P_TRACED | P_LP64;
    return true;
}
static int testRusage(int pid, int flavor, rusage_info_t *buffer) {
    (void)flavor; (void)buffer; ++rusageCalls; lastRusagePID = pid; return 0;
}
static kern_return_t testTaskForPID(mach_port_name_t target, int pid, mach_port_name_t *task) {
    (void)target; ++taskCalls; lastTaskPID = pid; *task = 123; return KERN_SUCCESS;
}
static int testSysctl(int *name, u_int count, void *output, size_t *length, void *input, size_t inputSize) {
    (void)output; (void)length; (void)input; (void)inputSize;
    ++nativeCalls;
    if (count <= 4 && name) memcpy(lastMIB, name, count * sizeof(*name));
    errno = nativeError;
    return nativeError ? -1 : 0;
}
pid_t LSBGuestPID(void) { return 21; }
pid_t LSBNativePID(void) { return 31; }
pid_t LSBGuestParentPID(void) { return 11; }
int LSBTransportListProcesses(LSBProcessList **list, size_t *size) {
    ++listCalls;
    *list = NULL; *size = 0;
    if (listError) return listError;
    *size = LSBProcessListSize(2);
    *list = calloc(1, *size);
    assert(*list);
    (*list)->version = LSB_PROCESS_LIST_VERSION;
    (*list)->recordSize = sizeof(LSBProcessRecord);
    (*list)->count = 2;
    for (unsigned i = 0; i < 2; ++i) {
        LSBProcessRecord *record = &(*list)->records[i];
        record->guestPID = 21 + i; record->parentPID = 11;
        record->audit.val[5] = 31 + i; record->audit.val[7] = 101 + i;
        InspectionBSDInfo info = {.pid = 31 + i, .ppid = 99, .status = SRUN, .flags = 2, .uid = 501, .gid = 20, .ruid = 502, .rgid = 21};
        memcpy(record->shortInfo, &info, sizeof(info));
        strcpy(record->executablePath, i ? "/bin/df" : "/Applications/CocoaTop.app/CocoaTop");
    }
    return 0;
}

static void testValidation(void) {
    LSBProcessList *list; size_t size;
    assert(!LSBTransportListProcesses(&list, &size) && !LSBValidateProcessList(list, size));
    assert(LSBValidateProcessList(NULL, size) == EPROTO);
    assert(LSBValidateProcessList(list, size - 1) == EPROTO);
    assert(LSBValidateProcessList(list, size + 1) == EPROTO);
    list->version++; assert(LSBValidateProcessList(list, size) == EPROTO); list->version--;
    list->recordSize--; assert(LSBValidateProcessList(list, size) == EPROTO); list->recordSize++;
    list->reserved = 1; assert(LSBValidateProcessList(list, size) == EPROTO); list->reserved = 0;
    list->records[1].guestPID = 21; assert(LSBValidateProcessList(list, size) == EPROTO); list->records[1].guestPID = 22;
    list->records[1].audit.val[5] = 999; assert(LSBValidateProcessList(list, size) == EPROTO); list->records[1].audit.val[5] = 32;
    strcpy(list->records[1].executablePath, "/bin/../secret"); assert(LSBValidateProcessList(list, size) == EPROTO);
    memset(list->records[1].executablePath, 'x', sizeof(list->records[1].executablePath));
    assert(LSBValidateProcessList(list, size) == EPROTO);
    free(list);
}

int main(void) {
    testValidation(); listCalls = 0;
    char temporary[] = "/tmp/LSBInspection.XXXXXX", canonical[PATH_MAX], physicalDF[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    assert(configureInspectionRoot(NULL) == ENOENT && !inspectionRuntimeRoot[0]);
    assert(configureInspectionRoot("relative") == EINVAL && !inspectionRuntimeRoot[0]);
    assert(configureInspectionRoot("/") == EINVAL && !inspectionRuntimeRoot[0]);
    assert(configureInspectionRoot("/dev/null") == ENOTDIR && !inspectionRuntimeRoot[0]);
    assert(configureInspectionRoot("/nonexistent-LSB-runtime") == ENOENT && !inspectionRuntimeRoot[0]);
    inspectionRootError = configureInspectionRoot(temporary);
    assert(!inspectionRootError && !strcmp(inspectionRuntimeRoot, canonical)); // Resolve the /tmp alias once, after guest TLS is available.
    assert(snprintf(physicalDF, sizeof(physicalDF), "%s/bin/df", canonical) > 0);
    int physicalLength = (int)strlen(physicalDF);
    int maximum[] = {CTL_KERN, KERN_MAXPROC}, all[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    int value = 0; size_t length = sizeof(value);
    nativeError = EBUSY;
    mach_port_name_t task = 0;
    assert(!LSBInspectionUnsupportedPID(22));
    assert(!inspectionRusage(22, 2, NULL) && rusageCalls == 1 && lastRusagePID == 22);
    assert(!inspectionTaskForPID(mach_task_self(), 22, &task) && taskCalls == 1 && lastTaskPID == 22 && task == 123);
    assert(inspectionSysctl(maximum, 2, &value, &length, NULL, 0) == -1 && errno == EBUSY && nativeCalls == 1 && !listCalls);
    atomic_store(&inspectionReady, true);
    listError = EPERM;
    assert(inspectionSysctl(maximum, 2, &value, &length, NULL, 0) == -1 && errno == EPERM && nativeCalls == 1);
    assert(inspectionSysctl(all, 4, NULL, &length, NULL, 0) == -1 && errno == EPERM && nativeCalls == 1);
    int pathResult; char path[128];
    errno = ERANGE;
    assert(!LSBInspectionPIDPath(21, path, sizeof(path), &pathResult));
    assert(errno == ERANGE && !LSBInspectionPIDPath(21, (void *)1, 1, (int *)1));
    listError = 0; errno = EDOM;
    assert(!inspectionSysctl(maximum, 2, &value, &length, NULL, 0) && value == LSB_PROCESS_LIST_CAPACITY && errno == EDOM);
    length = 0;
    assert(!inspectionSysctl(all, 4, NULL, &length, NULL, 0) && length == 2 * sizeof(struct kinfo_proc));
    struct kinfo_proc info[2]; length = sizeof(info);
    assert(!inspectionSysctl(all, 4, info, &length, NULL, 0) && length == sizeof(info));
    assert(info[0].kp_proc.p_pid == 21 && info[0].kp_eproc.e_ppid == 11 && !strcmp(info[0].kp_proc.p_comm, "CocoaTop"));
    assert(info[1].kp_proc.p_pid == 22 && !strcmp(info[1].kp_proc.p_comm, "df"));
    assert(info[0].kp_proc.p_stat == SRUN && (info[0].kp_proc.p_flag & P_TRACED) && (info[0].kp_proc.p_flag & P_LP64));
    assert(info[0].kp_eproc.e_ucred.cr_uid == 501 && info[0].kp_eproc.e_pcred.p_rgid == 21);
    length = 0;
    assert(inspectionSysctl(all, 4, info, &length, NULL, 0) == -1 && errno == ENOMEM && !length);
    unsigned char partial[sizeof(info) + 8]; memset(partial, 0xa5, sizeof(partial));
    length = sizeof(struct kinfo_proc) + 37;
    assert(inspectionSysctl(all, 4, partial, &length, NULL, 0) == -1 && errno == ENOMEM && length == sizeof(struct kinfo_proc));
    assert(((struct kinfo_proc *)partial)->kp_proc.p_pid == 21);
    for (size_t i = length; i < sizeof(partial); ++i) assert(partial[i] == 0xa5);
    memset(partial, 0xa5, sizeof(partial)); length = sizeof(struct kinfo_proc) - 1;
    assert(inspectionSysctl(all, 4, partial, &length, NULL, 0) == -1 && errno == ENOMEM && !length);
    for (size_t i = 0; i < sizeof(partial); ++i) assert(partial[i] == 0xa5);
    value = 0x12345678; length = 1;
    assert(inspectionSysctl(maximum, 2, &value, &length, NULL, 0) == -1 && errno == ENOMEM && !length && value == 0x12345678);
    assert(inspectionSysctl(all, 4, info, (size_t *)1, NULL, 0) == -1 && errno == EFAULT);
    errno = EDOM;
    assert(LSBInspectionPIDPath(22, path, sizeof(path), &pathResult) && pathResult == physicalLength && !strcmp(path, physicalDF) && errno == EDOM);
    assert(!strcmp(processSnapshot->records[1].executablePath, "/bin/df")); // API projection must not mutate the authenticated identity snapshot.
    memset(path, 0xa5, sizeof(path));
    assert(LSBInspectionPIDPath(22, path, (uint32_t)physicalLength, &pathResult) && !pathResult && errno == ENOSPC);
    for (size_t i = 0; i < sizeof(path); ++i) assert((unsigned char)path[i] == 0xa5);
    assert(LSBInspectionPIDPath(22, path, (uint32_t)physicalLength + 1, &pathResult) && pathResult == physicalLength && !strcmp(path, physicalDF));
    assert((unsigned char)path[physicalLength + 1] == 0xa5);
    assert(LSBInspectionPIDPath(22, path, 3, &pathResult) && !pathResult && errno == ENOSPC);
    assert(LSBInspectionPIDPath(999, path, sizeof(path), &pathResult) && !pathResult && errno == ESRCH);
    assert(LSBInspectionPIDPath(22, (void *)1, sizeof(path), &pathResult) && !pathResult && errno == EFAULT);
    assert(LSBInspectionPIDPath(22, NULL, sizeof(path), &pathResult) && !pathResult && errno == EFAULT);
    inspectionRootError = configureInspectionRoot(NULL);
    assert(LSBInspectionPIDPath(22, path, sizeof(path), &pathResult) && !pathResult && errno == ENOENT);
    inspectionRootError = 0;
    memset(inspectionRuntimeRoot, 'a', sizeof(inspectionRuntimeRoot));
    inspectionRuntimeRoot[0] = '/'; inspectionRuntimeRoot[sizeof(inspectionRuntimeRoot) - 1] = '\0';
    memset(path, 0xa5, sizeof(path));
    assert(LSBInspectionPIDPath(22, path, sizeof(path), &pathResult) && !pathResult && errno == ENAMETOOLONG && (unsigned char)path[0] == 0xa5);
    inspectionRootError = configureInspectionRoot(temporary);
    assert(!inspectionRootError);
    unsigned calls = listCalls;
    int self[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, 21};
    nativeError = 0;
    assert(!inspectionSysctl(self, 4, info, &length, NULL, 0) && lastMIB[3] == 31 && self[3] == 21 && listCalls == calls);
    nativeError = EACCES;
    assert(inspectionSysctl(self, 4, info, &length, NULL, 0) == -1 && errno == EACCES && listCalls == calls);
    unsigned forwarded = nativeCalls;
    self[3] = 99;
    assert(inspectionSysctl(self, 4, info, &length, NULL, 0) == -1 && errno == ESRCH && nativeCalls == forwarded);
    self[3] = 22;
    assert(inspectionSysctl(self, 4, info, &length, NULL, 0) == -1 && errno == ENOTSUP && nativeCalls == forwarded);
    int arguments[] = {CTL_KERN, KERN_PROCARGS2, 22};
    assert(inspectionSysctl(arguments, 3, info, &length, NULL, 0) == -1 && errno == ENOTSUP && nativeCalls == forwarded);
    arguments[2] = 999;
    assert(inspectionSysctl(arguments, 3, info, &length, NULL, 0) == -1 && errno == ESRCH && nativeCalls == forwarded);
    arguments[2] = 21; nativeError = 0;
    assert(!inspectionSysctl(arguments, 3, info, &length, NULL, 0) && lastMIB[2] == 31 && arguments[2] == 21);
    assert(LSBInspectionUnsupportedPID(22) == ENOTSUP && LSBInspectionUnsupportedPID(999) == ESRCH);
    assert(!LSBInspectionUnsupportedPID(21) && !LSBInspectionUnsupportedPID(31));
    assert(inspectionRusage(22, 2, NULL) == -1 && errno == ENOTSUP && rusageCalls == 1);
    assert(inspectionRusage(999, 2, NULL) == -1 && errno == ESRCH && rusageCalls == 1);
    assert(!inspectionRusage(21, 2, NULL) && rusageCalls == 2 && lastRusagePID == 31);
    assert(!inspectionRusage(31, 2, NULL) && rusageCalls == 3 && lastRusagePID == 31);
    errno = EDOM; task = 123;
    assert(inspectionTaskForPID(mach_task_self(), 22, &task) == KERN_NOT_SUPPORTED && !task && taskCalls == 1 && errno == EDOM);
    task = 123;
    assert(inspectionTaskForPID(mach_task_self(), 999, &task) == KERN_INVALID_ARGUMENT && !task && taskCalls == 1);
    assert(inspectionTaskForPID(mach_task_self(), 22, (void *)1) == KERN_INVALID_ADDRESS && taskCalls == 1);
    assert(!inspectionTaskForPID(mach_task_self(), 21, &task) && taskCalls == 2 && lastTaskPID == 31 && task == 123);
    // A replacement's native self PID can be recycled from another live job's stable guest PID.
    processSnapshot->records[1].guestPID = 31;
    forwarded = nativeCalls;
    assert(!LSBInspectionUnsupportedPID(21) && LSBInspectionUnsupportedPID(31) == ENOTSUP);
    self[3] = 31;
    assert(inspectionSysctl(self, 4, info, &length, NULL, 0) == -1 && errno == ENOTSUP && nativeCalls == forwarded);
    arguments[2] = 31;
    assert(inspectionSysctl(arguments, 3, info, &length, NULL, 0) == -1 && errno == ENOTSUP && nativeCalls == forwarded);
    assert(inspectionRusage(31, 2, NULL) == -1 && errno == ENOTSUP && rusageCalls == 3);
    assert(inspectionTaskForPID(mach_task_self(), 31, &task) == KERN_NOT_SUPPORTED && !task && taskCalls == 2);
    assert(LSBInspectionPIDPath(31, path, sizeof(path), &pathResult) && pathResult == physicalLength && !strcmp(path, physicalDF));
    assert(!inspectionRusage(21, 2, NULL) && rusageCalls == 4 && lastRusagePID == 31);
    assert(!inspectionTaskForPID(mach_task_self(), 21, &task) && taskCalls == 3 && lastTaskPID == 31);
    nativeError = EACCES;
    assert(inspectionSysctl(all, 4, info, &length, &value, sizeof(value)) == -1 && listCalls == calls);
    debuggerEnabled = true; debuggerError = ETIMEDOUT; self[3] = 42;
    forwarded = nativeCalls;
    assert(inspectionSysctl(self, 4, info, &length, NULL, 0) == -1 && errno == ETIMEDOUT && nativeCalls == forwarded && debuggerCalls == 1);
    debuggerError = 0; errno = EDOM; length = sizeof(info);
    assert(!inspectionSysctl(self, 4, info, &length, NULL, 0) && errno == EDOM && length == sizeof(*info) &&
           info[0].kp_proc.p_pid == 42 && (info[0].kp_proc.p_flag & P_TRACED) && debuggerCalls == 2);
    length = 1; memset(partial, 0xa5, sizeof(partial));
    assert(inspectionSysctl(self, 4, partial, &length, NULL, 0) == -1 && errno == ENOMEM && !length && partial[0] == 0xa5);
    self[3] = 21; nativeError = 0;
    assert(!inspectionSysctl(self, 4, info, &length, NULL, 0) && lastMIB[3] == 31 && debuggerCalls == 3);
    free(processSnapshot);
    assert(!rmdir(temporary));
    puts("ProcessInspectionTests: bounded guest list, logical identities, real self translation, native forwarding and output faults passed");
    return 0;
}
