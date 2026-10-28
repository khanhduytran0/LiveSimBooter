#pragma once

// Shared disk layout for the pinned iOS Simulator runtime; sibling data is private to this exact build.
#define LSB_STORAGE_DIRECTORY "LSB"
#define LSB_RUNTIME_PLATFORM "iOS"
#define LSB_RUNTIME_BUILD "23A343"
#define LSB_RUNTIME_DIRECTORY "RuntimeRoot"
#define LSB_DATA_DIRECTORY "data"
#define LSB_STORAGE_BUILD_RELATIVE_PATH LSB_STORAGE_DIRECTORY "/" LSB_RUNTIME_PLATFORM "/" LSB_RUNTIME_BUILD
