#include "StoreAppGroups.h"
#include "PrivateXPC.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>

static const char *const storeBases[] = {"group.com.SideStore.SideStore", "group.com.rileytestut.AltStore"};
static const char brokerSuffix[] = ".LiveSimBooter.spawn";

static int storeIndex(const char *identifier) {
    if (!identifier) return -1;
    for (unsigned index = 0; index < LSB_STORE_APP_GROUP_COUNT; ++index) {
        size_t length = strlen(storeBases[index]);
        if (strncmp(identifier, storeBases[index], length)) continue;
        if (!identifier[length]) return (int)index;
        const char *suffix = identifier + length;
        if (*suffix++ != '.' || strlen(suffix) != 10) continue;
        bool valid = true;
        for (unsigned byte = 0; byte < 10; ++byte) {
            unsigned char value = (unsigned char)suffix[byte];
            if (!((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9'))) { valid = false; break; }
        }
        if (valid) return (int)index;
    }
    return -1;
}

static int addIdentifier(LSBStoreAppGroups *groups, const char *identifier) {
    if (!identifier) return EINVAL;
    int index = storeIndex(identifier);
    if (index < 0) return 0; // Other signed groups never become runtime or broker candidates.
    if (groups->identifiers[index][0]) return EINVAL;
    size_t length = strlen(identifier);
    if (length + sizeof(brokerSuffix) > LSB_BROKER_SERVICE_BYTES || length >= LSB_STORE_APP_GROUP_BYTES) return ENAMETOOLONG;
    memcpy(groups->identifiers[index], identifier, length + 1);
    ++groups->count;
    return 0;
}

static int finishGroups(LSBStoreAppGroups *groups) {
    if (!groups->count) return ENOENT;
    if (!groups->identifiers[0][0]) {
        memcpy(groups->identifiers[0], groups->identifiers[1], sizeof(groups->identifiers[0]));
        memset(groups->identifiers[1], 0, sizeof(groups->identifiers[1]));
    }
    return 0;
}

int LSBParseStoreAppGroups(const char *const identifiers[], size_t count, LSBStoreAppGroups *groups) {
    if (!groups) return EINVAL;
    memset(groups, 0, sizeof(*groups));
    if (count && !identifiers) return EINVAL;
    LSBStoreAppGroups found = {0};
    int error = 0;
    for (size_t index = 0; index < count && !error; ++index) error = addIdentifier(&found, identifiers[index]);
    if (!error) error = finishGroups(&found);
    if (!error) *groups = found;
    return error;
}

int LSBCopyStoreAppGroups(LSBStoreAppGroups *groups) {
    if (!groups) return EINVAL;
    memset(groups, 0, sizeof(*groups));
    xpc_object_t value = xpc_copy_entitlement_for_self("com.apple.security.application-groups");
    if (!value) return EACCES;
    int error = xpc_get_type(value) == XPC_TYPE_ARRAY ? 0 : EINVAL;
    LSBStoreAppGroups found = {0};
    if (!error) {
        size_t count = xpc_array_get_count(value);
        for (size_t index = 0; index < count && !error; ++index) {
            xpc_object_t entry = xpc_array_get_value(value, index);
            if (!entry || xpc_get_type(entry) != XPC_TYPE_STRING) { error = EINVAL; break; }
            const char *identifier = xpc_string_get_string_ptr(entry);
            if (!identifier || strlen(identifier) != xpc_string_get_length(entry)) { error = EINVAL; break; }
            error = addIdentifier(&found, identifier);
        }
    }
    if (!error) error = finishGroups(&found);
    xpc_release(value);
    if (!error) *groups = found;
    return error;
}

int LSBStoreBrokerServiceName(const char *identifier, char output[LSB_BROKER_SERVICE_BYTES]) {
    if (!output) return EINVAL;
    memset(output, 0, LSB_BROKER_SERVICE_BYTES);
    if (storeIndex(identifier) < 0) return EINVAL;
    size_t length = strlen(identifier);
    if (length + sizeof(brokerSuffix) > LSB_BROKER_SERVICE_BYTES) return ENAMETOOLONG;
    memcpy(output, identifier, length);
    memcpy(output + length, brokerSuffix, sizeof(brokerSuffix));
    return 0;
}
