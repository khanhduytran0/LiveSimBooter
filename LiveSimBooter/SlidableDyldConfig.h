#pragma once

#include <stddef.h>
#include <stdint.h>

// Tools/patch-ios26-cache-slide.py installs this record outside every section in the pinned 23A343 dyld_sim's __DATA_DIRTY padding.
// Native preparation updates only this writable mapping; the signed instruction stub remains identical at every selected cache base.
#define LSB_DYLD_SLIDE_CONFIG_VM_OFFSET UINT64_C(0x7c200)
#define LSB_DYLD_SLIDE_CONFIG_FILE_OFFSET UINT64_C(0x5c200)
#define LSB_DYLD_SLIDE_CONFIG_MAGIC "LSBDyldSlideV1"
#define LSB_DYLD_SLIDE_PREFERRED_BASE UINT64_C(0x180000000)

typedef struct {
    char magic[16];
    uint32_t version, size;
    uint64_t preferredBase, actualBase, cacheSpan;
    uint8_t cacheUUID[16];
} LSBDyldSlideConfiguration;

static_assert(sizeof(LSBDyldSlideConfiguration) == 64 && offsetof(LSBDyldSlideConfiguration, actualBase) == 32);
