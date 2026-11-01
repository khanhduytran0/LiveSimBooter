#ifndef LSB_LAZY_SIGNATURE_PROTOCOL_H
#define LSB_LAZY_SIGNATURE_PROTOCOL_H

#include <mach/mach.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

enum {
    LSB_LAZY_SIGNATURE_VERSION = 1,
    LSB_LAZY_SIGNATURE_REQUEST_ID = 0x4c534253,
    LSB_LAZY_SIGNATURE_REPLY_ID = 0x4c534254,
    LSB_LAZY_SIGNATURE_PATH_BYTES = 1024,
    LSB_LAZY_SIGNATURE_TIMEOUT_MS = 120000
};

typedef struct {
    uint64_t device, inode, size;
    int64_t modifiedSeconds, modifiedNanoseconds, changedSeconds, changedNanoseconds;
    uint32_t mode, owner;
} LSBLazySignatureFileIdentity;

// Only the recorded regular file generation may be signed. The host resolves and confines the path independently.
// Signing must preserve the file size and every byte preceding this existing signature slot, including all load commands.
typedef struct {
    mach_msg_header_t header;
    uint32_t version, reserved;
    uint64_t requestID;
    LSBLazySignatureFileIdentity file;
    uint64_t sliceOffset, blobOffset, blobSize;
    char path[LSB_LAZY_SIGNATURE_PATH_BYTES];
} LSBLazySignatureRequest;

typedef struct {
    mach_msg_header_t header;
    uint32_t version;
    int32_t error;
    uint64_t requestID;
    LSBLazySignatureFileIdentity file;
} LSBLazySignatureResponse;

#if defined(__cplusplus)
#define LSB_SIGNATURE_STATIC_ASSERT static_assert
#else
#define LSB_SIGNATURE_STATIC_ASSERT _Static_assert
#endif
LSB_SIGNATURE_STATIC_ASSERT(sizeof(LSBLazySignatureFileIdentity) == 64, "lazy signature file identity ABI");
LSB_SIGNATURE_STATIC_ASSERT(offsetof(LSBLazySignatureRequest, file) == 40 && offsetof(LSBLazySignatureRequest, path) == 128 &&
                            sizeof(LSBLazySignatureRequest) == 1152, "lazy signature request ABI");
LSB_SIGNATURE_STATIC_ASSERT(offsetof(LSBLazySignatureResponse, file) == 40 && sizeof(LSBLazySignatureResponse) == 104,
                            "lazy signature response ABI");
#undef LSB_SIGNATURE_STATIC_ASSERT

static inline LSBLazySignatureFileIdentity LSBLazySignatureIdentityFromStat(const struct stat *info) {
    LSBLazySignatureFileIdentity result = {
        (uint64_t)info->st_dev, info->st_ino, (uint64_t)info->st_size,
        info->st_mtimespec.tv_sec, info->st_mtimespec.tv_nsec, info->st_ctimespec.tv_sec, info->st_ctimespec.tv_nsec,
        info->st_mode, info->st_uid
    };
    return result;
}

static inline int LSBLazySignatureIdentitiesEqual(const LSBLazySignatureFileIdentity *left, const LSBLazySignatureFileIdentity *right) {
    return left->device == right->device && left->inode == right->inode && left->size == right->size &&
        left->modifiedSeconds == right->modifiedSeconds && left->modifiedNanoseconds == right->modifiedNanoseconds &&
        left->changedSeconds == right->changedSeconds && left->changedNanoseconds == right->changedNanoseconds &&
        left->mode == right->mode && left->owner == right->owner;
}

#endif
