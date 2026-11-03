// XNU proc_info_extended_id(PIDINFO, PIDTBSDINFO) returns an idversion-checked BSD state and p_xstat in one syscall.
// The iPhone SDK omits the libproc declaration. PROC_FLAG_TRACED distinguishes a debugger stop from a job-control stop.
#pragma once
#include <errno.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/proc.h>

extern int __proc_info_extended_id(int32_t, int32_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, int32_t);

// iOS PT_ATTACHEXC itself posts SIGSTOP. LLDB installs a task EXC_SOFTWARE handler to receive the resulting EXC_SOFT_SIGNAL.
// XNU's task_get_exception_ports_info returns identifiers without transferring rights to the debugger's exception port.
static inline bool LSBTerminalTaskHasNoSoftwareHandler(mach_port_t task) {
    exception_mask_t mask = 0;
    exception_handler_info_t handler = {};
    exception_behavior_t behavior;
    thread_state_flavor_t flavor;
    mach_msg_type_number_t count = 1;
    return MACH_PORT_VALID(task) && task_get_exception_ports_info(task, EXC_MASK_SOFTWARE, &mask, &count, &handler, &behavior, &flavor) == KERN_SUCCESS &&
           count == 1 && mask == EXC_MASK_SOFTWARE && !handler.iip_port_object && !handler.iip_receiver_object;
}

static inline int LSBReadTerminalJobState(const audit_token_t *token, int *stop) {
    enum { PIDInfo = 2, BSDInfo = 3, CompareIDVersion = 1, Traced = 2 };
    struct {
        uint32_t flags, status, xstatus, pid;
        uint8_t remaining[120];
    } info;
    _Static_assert(sizeof(info) == 136, "XNU proc_bsdinfo ABI");
    int bytes = __proc_info_extended_id(PIDInfo, (int)token->val[5], BSDInfo, CompareIDVersion, token->val[7], 0, (uintptr_t)&info, sizeof(info));
    if (bytes < 0) return errno;
    if (bytes != sizeof(info) || info.pid != token->val[5]) return EPROTO;
    if (info.flags & Traced) return EBUSY;
    if (info.status == SSTOP) {
        if (info.xstatus != SIGSTOP && info.xstatus != SIGTSTP && info.xstatus != SIGTTIN && info.xstatus != SIGTTOU) return EBUSY;
        *stop = (int)info.xstatus;
        return 0;
    }
    if (info.status != SRUN && info.status != SSLEEP) return EAGAIN;
    *stop = 0;
    return 0;
}
