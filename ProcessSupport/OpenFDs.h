#pragma once
#include "SpawnPlan.h"
#include <stdbool.h>

// XNU's PROC_PIDLISTFDS flavor returns proc_fdinfo entries in ascending descriptor order.
typedef struct { int32_t fd; uint32_t type; } LSBProcFDInfo;
_Static_assert(sizeof(LSBProcFDInfo) == 8, "Darwin proc_fdinfo layout");
extern int __proc_info(int call, int pid, unsigned flavor, uint64_t arg, void *buffer, int size);

typedef struct {
    LSBProcFDInfo entries[LSB_MAX_FDS + 1];
    int index, count;
    bool listed;
} LSBOpenFDScan;

static inline void LSBBeginOpenFDScan(LSBOpenFDScan *scan, pid_t nativePID, int limit) {
    scan->index = 0;
    scan->count = limit;
    scan->listed = false;
    int bytes = __proc_info(2, nativePID, 1, 0, scan->entries, sizeof(scan->entries));
    // A full buffer can be truncated. Never treat a partial list or a denied lookup as a complete FD table.
    if (bytes <= 0 || bytes >= (int)sizeof(scan->entries) || bytes % sizeof(LSBProcFDInfo)) return;
    int count = bytes / (int)sizeof(LSBProcFDInfo);
    int previous = -1;
    for (int i = 0; i < count; ++i) {
        int fd = scan->entries[i].fd;
        if (fd <= previous) return;
        previous = fd;
    }
    while (count && scan->entries[count - 1].fd >= limit) --count;
    scan->count = count;
    scan->listed = true;
}

static inline int LSBNextOpenFD(LSBOpenFDScan *scan) {
    if (scan->index >= scan->count) return -1;
    int index = scan->index++;
    return scan->listed ? scan->entries[index].fd : index;
}
