#pragma once
#include <stdint.h>

// Procursus' fork-fd-close.diff uses this Darwin libproc ABI, omitted from the public iOS SDK.
struct proc_fdinfo { int32_t proc_fd; uint32_t proc_fdtype; };
static_assert(sizeof(proc_fdinfo) == 8, "unexpected PROC_PIDLISTFDS record size");
#define PROC_PIDLISTFDS 1
#define PROX_FDTYPE_VNODE 1
#define PROC_PIDLISTFD_SIZE sizeof(struct proc_fdinfo)
extern "C" int proc_pidinfo(int, int, uint64_t, void *, int);
