#ifndef LSB_METAL_COMPILER_REGISTRATION_H
#define LSB_METAL_COMPILER_REGISTRATION_H

#include <xpc/xpc.h>

// Register the fixed guest compiler bundle once, immediately before its first PID-domain connection request.
// Other services and calls before guest initialization forward unchanged, including connection ownership and errno.
xpc_connection_t LSBMetalCompilerConnectionCreate(const char *name, dispatch_queue_t targetQueue);

#endif
