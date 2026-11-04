#ifndef LSB_USERSPACE_REBOOT_H
#define LSB_USERSPACE_REBOOT_H

#include <xpc/xpc.h>

// Only an accepted reboot request in the pinned simulator launchd can arm its exact clean-exit handoff.
// Other XPC callers and all ordinary exit paths keep their original behavior. The broker authenticates the sender.
xpc_object_t LSBUserspaceRebootCreateReply(xpc_object_t request);
__attribute__((noreturn)) void LSBUserspaceRebootExit(int status);

#endif
