#ifndef LSB_EXECUTABLE_PATH_H
#define LSB_EXECUTABLE_PATH_H

#include <limits.h>

// Resolve a guest absolute path, a contained physical path/alias, or a relative path after ordered cwd actions.
// Executables live in RuntimeRoot, an installed guest app bundle, or the runtime's sibling data/var/jb.
int LSBResolveGuestExecutable(const char *runtimeRoot, const char *path, char output[PATH_MAX]);

#endif
