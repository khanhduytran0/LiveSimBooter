// Match the Procursus libiosexec wrappers while letting LiveSimBooter handle POSIX spawn.
#include <cstdlib>
#include <unistd.h>
#include <spawn.h>
#include <pwd.h>
#include <grp.h>
#include <libiosexec.h>
#undef posix_spawn
#undef posix_spawnp
