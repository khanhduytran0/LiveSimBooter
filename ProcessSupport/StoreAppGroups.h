#ifndef LSB_STORE_APP_GROUPS_H
#define LSB_STORE_APP_GROUPS_H

#include <stddef.h>

enum { LSB_STORE_APP_GROUP_COUNT = 2, LSB_STORE_APP_GROUP_BYTES = 128, LSB_BROKER_SERVICE_BYTES = 128 };

typedef struct {
    size_t count;
    char identifiers[LSB_STORE_APP_GROUP_COUNT][LSB_STORE_APP_GROUP_BYTES];
} LSBStoreAppGroups;

#ifdef __cplusplus
extern "C" {
#endif

// Copies actual signed store groups, ordered SideStore then AltStore. No native XPC objects survive this call.
// Exact store identifiers and their .TEAMID variants are supported; ambiguous/duplicate names fail with EINVAL.
// Returns errno; the caller owns the copied bytes, and an error leaves the output empty.
int LSBCopyStoreAppGroups(LSBStoreAppGroups *groups);
int LSBParseStoreAppGroups(const char *const identifiers[], size_t count, LSBStoreAppGroups *groups);
// The service remains within the actual entitled App Group namespace and the 128-byte bootstrap name limit.
int LSBStoreBrokerServiceName(const char *identifier, char output[LSB_BROKER_SERVICE_BYTES]);

#ifdef __cplusplus
}
#endif
#endif
