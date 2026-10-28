#ifndef LSB_GUEST_PROCESS_DEBUGGER_H
#define LSB_GUEST_PROCESS_DEBUGGER_H
#include <stdbool.h>
#include <mach/mach.h>
#include <sys/sysctl.h>

bool LSBGuestDebuggerCopyInfo(pid_t nativePID, struct kinfo_proc *info, int *error);
// Only the existing workqueue bridge thread invokes this on the separate private control port.
// Consumes the message and all received rights on every path.
void LSBProcessDebuggerHandleMessage(mach_msg_header_t *message, size_t capacity);
#endif
