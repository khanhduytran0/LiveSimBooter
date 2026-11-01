#ifndef LSB_EXECUTABLE_PATH_H
#define LSB_EXECUTABLE_PATH_H

#include <limits.h>

// Resolve a guest absolute path, a contained physical path/alias, or a relative path after ordered cwd actions.
// The result must remain below RuntimeRoot or an installed .app in sibling data/Containers/Bundle/Application.
int LSBResolveGuestExecutable(const char *runtimeRoot, const char *path, char output[PATH_MAX]);

#endif
