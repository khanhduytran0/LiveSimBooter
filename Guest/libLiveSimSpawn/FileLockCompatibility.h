#ifndef LSB_FILE_LOCK_COMPATIBILITY_H
#define LSB_FILE_LOCK_COMPATIBILITY_H

#include <stdint.h>

#define LSB_FILE_LOCK_ENABLE_ENV "LSB_SIMULATE_FILE_LOCK_SUSPENSION"
#define LSB_FILE_LOCK_ROOT_ENV "SIMULATOR_SHARED_RESOURCES_DIRECTORY"
#define LSB_FILE_LOCK_ATTRIBUTE "com.apple.runningboard.can-suspend-locked"

// Best-effort metadata for eligible simulator files only; preserves errno and does not acquire or release a lock.
void LSBFileLockPrepare(int descriptor);
int LSBFileLockFlock(int descriptor, int operation);
int LSBFileLockFCntl(int descriptor, int command, ...);
int LSBFileLockFCntlNoCancel(int descriptor, int command, ...);
int LSBFileLockFCntlRaw(int descriptor, int command, uintptr_t argument);
int LSBFileLockFCntlRawNoCancel(int descriptor, int command, uintptr_t argument);

#endif
