#ifndef LSB_CHILD_WAIT_H
#define LSB_CHILD_WAIT_H

#include "../../ProcessSupport/ChildStatus.h"
#include <signal.h>
#include <sys/resource.h>

int LSBChildWaitConfigure(LSBChildStatus *ledger, int wakeFD);
int LSBGuestChildWakeFD(void);
int LSBTerminalStopSelf(int reason);
LSBChildStatus *LSBGuestChildStatus(void);
pid_t LSBWaitPID(pid_t pid, int *status, int options);
// Resource usage is not collected. By default non-NULL usage is rejected without consuming a terminal status.
// LSB_WAIT4_STATUS_ONLY=1 at initialization opts into zero (uncollected) usage with the actual terminal status.
pid_t LSBWait4(pid_t pid, int *status, int options, struct rusage *usage);
pid_t LSBWait(int *status);
int LSBSigaction(int number, const struct sigaction *action, struct sigaction *oldAction);
sig_t LSBSignal(int number, sig_t handler);

#endif
