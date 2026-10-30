#ifndef LSB_PROCESS_DISPLAY_NAME_H
#define LSB_PROCESS_DISPLAY_NAME_H

#include <stddef.h>
#include <sys/types.h>

// Native diagnostic metadata only. Neither the guest's arguments nor its executable identity is changed.
// Positive guestPID prepends "LSB:N "; zero identifies a native role without a guest PID. Negative values are invalid.
// argv0Capacity is the original writable argv[0] string size including its terminator, or zero to leave it alone.
// Native argv and the 32-byte kernel long name truncate the basename independently, preserving the complete PID prefix.
// An argv buffer too small for one basename byte and the prefix returns ENOSPC without changes. path may alias nativeArgv0.
// Returns an errno value without changing errno. A failed kernel-name update leaves the native argv update in place.
int LSBSetProcessDisplayName(const char *path, pid_t guestPID, char *nativeArgv0, size_t argv0Capacity);

#endif
