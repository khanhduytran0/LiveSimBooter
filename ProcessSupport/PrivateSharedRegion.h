#pragma once
#include <stdint.h>

// Native libSystem SPI: a nonnull address queries the loaded shared-cache header without removing its shared region.
int __shared_region_check_np(uint64_t *address);
