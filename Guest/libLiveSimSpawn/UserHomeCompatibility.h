#ifndef LSB_USER_HOME_COMPATIBILITY_H
#define LSB_USER_HOME_COMPATIBILITY_H

#include <pwd.h>
#include <stddef.h>
#include <sys/types.h>

// Reentrant passwd lookups retain native account metadata. For the current UID only, canonical /var/mobile homes become
// the physical SIMULATOR_SHARED_RESOURCES_DIRECTORY. The replacement stays in the caller's buffer; insufficient room is ERANGE.
int LSBUserHomeGetPWUIDR(uid_t uid, struct passwd *entry, char *buffer, size_t size, struct passwd **result);
int LSBUserHomeGetPWNAMR(const char *name, struct passwd *entry, char *buffer, size_t size, struct passwd **result);

#endif
