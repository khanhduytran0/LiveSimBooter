#pragma once
#include "GuestExecutablePath.h"

#define LSB_FORK_MODE_ENV "LSB_FORK_SNAPSHOT_PROBE"

// The loader and the guest shim use the resolved executable path. An explicit diagnostic mode takes precedence.
static inline bool LSBUseForkDonorLayout(const char *root, const char *path, const char *mode) {
    if (mode) return !strcmp(mode, "resume");
    return root && path && LSBGuestPathInBootstrap(root, path);
}
