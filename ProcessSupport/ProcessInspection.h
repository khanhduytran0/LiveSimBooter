#ifndef LSB_PROCESS_INSPECTION_H
#define LSB_PROCESS_INSPECTION_H

#include "PeerProcessIdentity.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>

#define LSB_PROCESS_LIST_VERSION 1u
#define LSB_PROCESS_LIST_CAPACITY 256u

// This snapshot conveys identities, not task capabilities. Native audit identity is never rewritten into a logical PID.
typedef struct {
    int32_t guestPID, parentPID;
    audit_token_t audit;
    unsigned char shortInfo[LSB_PEER_SHORT_INFO_BYTES];
    char executablePath[LSB_PATH_BYTES];
} LSBProcessRecord;

typedef struct {
    uint32_t version, count, recordSize, reserved;
    LSBProcessRecord records[];
} LSBProcessList;

_Static_assert(sizeof(LSBProcessRecord) == 1128, "process record wire layout");
_Static_assert(offsetof(LSBProcessList, records) == 16, "process list wire layout");

static inline size_t LSBProcessListSize(uint32_t count) {
    return sizeof(LSBProcessList) + (size_t)count * sizeof(LSBProcessRecord);
}

static inline int LSBValidateProcessList(const void *bytes, size_t size) {
    if (!bytes || size < sizeof(LSBProcessList)) return EPROTO;
    const LSBProcessList *list = bytes;
    if (list->version != LSB_PROCESS_LIST_VERSION || list->count > LSB_PROCESS_LIST_CAPACITY || list->reserved ||
        list->recordSize != sizeof(LSBProcessRecord) || size != LSBProcessListSize(list->count)) return EPROTO;
    for (uint32_t i = 0; i < list->count; ++i) {
        const LSBProcessRecord *record = &list->records[i];
        uint32_t nativePID = 0;
        memcpy(&nativePID, record->shortInfo, sizeof(nativePID));
        if (record->guestPID <= 0 || record->parentPID < 0 || !nativePID || nativePID > INT_MAX || nativePID != record->audit.val[5] ||
            record->executablePath[0] != '/' || !record->executablePath[1] ||
            !memchr(record->executablePath, '\0', sizeof(record->executablePath))) return EPROTO;
        for (const char *part = record->executablePath + 1; *part;) {
            const char *end = strchr(part, '/');
            size_t length = end ? (size_t)(end - part) : strlen(part);
            if (!length || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.') ||
                (end && !end[1])) return EPROTO;
            part += length + (end != NULL);
        }
        for (uint32_t j = 0; j < i; ++j)
            if (list->records[j].guestPID == record->guestPID || !memcmp(&list->records[j].audit, &record->audit, sizeof(record->audit)))
                return EPROTO;
    }
    return 0;
}
#endif
