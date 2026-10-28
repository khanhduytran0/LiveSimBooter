#ifndef LSB_SPAWN_PLAN_H
#define LSB_SPAWN_PLAN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define LSB_EXTENSION_IDENTIFIER "com.kdt.LiveSimBooter.LiveProcess"
#define LSB_PLAN_VERSION 1
#define LSB_MAX_VECTOR 256
#define LSB_MAX_ACTIONS 64
#define LSB_MAX_FDS 128
#define LSB_STRING_BYTES 49152
#define LSB_PATH_BYTES 1024

enum LSBFileActionKind { LSB_ACTION_CLOSE = 1, LSB_ACTION_DUP2, LSB_ACTION_OPEN, LSB_ACTION_CHDIR, LSB_ACTION_FCHDIR, LSB_ACTION_INHERIT };
typedef struct { uint32_t kind; int32_t fd, target, flags; uint32_t mode, path; } LSBFileAction;
typedef struct { int32_t number; uint32_t flags; } LSBInheritedFD;

typedef struct {
    uint32_t version, argc, envc, actionCount, fdCount, stringBytes;
    uint32_t path, cwd, spawnFlags, signalMask, signalDefault, signalIgnored, creationMask;
    int32_t processGroup;
    uint32_t argv[LSB_MAX_VECTOR], envp[LSB_MAX_VECTOR];
    LSBFileAction actions[LSB_MAX_ACTIONS];
    LSBInheritedFD fds[LSB_MAX_FDS];
    char strings[LSB_STRING_BYTES];
} LSBSpawnPlan;

#ifdef __cplusplus
extern "C" {
#endif
// All functions return errno values, never -1. Offsets refer to terminated strings in this owned plan.
int LSBPlanAddString(LSBSpawnPlan *plan, const char *value, uint32_t *offset);
int LSBValidateSpawnPlan(const LSBSpawnPlan *plan);
// Receives already-imported FD duplicates in plan order; consumes them on success or failure.
int LSBApplySpawnPlan(const LSBSpawnPlan *plan, int receivedFDs[LSB_MAX_FDS]);
// Keeps one internal descriptor across both close sweeps. It must be above every plan/action descriptor and remains caller-owned.
int LSBApplySpawnPlanPreservingFD(const LSBSpawnPlan *plan, int receivedFDs[LSB_MAX_FDS], int preservedFD);
#ifdef __cplusplus
}
#endif
#endif
