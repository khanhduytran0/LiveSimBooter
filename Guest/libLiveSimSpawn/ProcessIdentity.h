#ifndef LSB_PROCESS_IDENTITY_H
#define LSB_PROCESS_IDENTITY_H

#include <sys/types.h>
#include <mach/mach.h>

// Native process identity for kernel-facing APIs; the guest PID may survive a replacement process.
pid_t LSBNativePID(void);
pid_t LSBGuestPID(void);
// Kernel-facing libxpc, BaseBoard, RunningBoardServices and FrontBoard calls retain native identity; ordinary guest code keeps its logical PID.
pid_t LSBGetPID(void);
// Translate guest self lookups; opted-in RunningBoard may obtain a managed peer's own published task-name right after native denial.
kern_return_t LSBTaskNameForPID(mach_port_name_t target, int pid, mach_port_name_t *task);
// Parent metadata is refreshed from the shared ledger so adoption does not require changing the child environment.
pid_t LSBGuestParentPID(void);

#endif
