#ifndef LSB_SPAWN_SHIM_H
#define LSB_SPAWN_SHIM_H

#include <limits.h>
#include <spawn.h>

int LSBResolveExecutableForSpawn(const char *path, char executable[PATH_MAX]);
int LSBPosixSpawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attributes,
                  char *const argv[], char *const envp[]);

#endif
