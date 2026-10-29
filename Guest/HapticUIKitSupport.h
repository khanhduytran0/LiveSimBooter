#pragma once
#import <Foundation/Foundation.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>
#include <stdint.h>
#include <string.h>

// The native host publishes its real UIDevice feedback level beside the bridged actuator capability. A missing or malformed
// value must leave UIKit's simulator behavior alone; CFBoolean and floating-point values are not integer support levels.
static inline NSInteger LSBHapticUIKitSupportLevel(NSDictionary *profile) {
    if (![profile isKindOfClass:NSDictionary.class]) return -1;
    id capabilities = profile[@"capabilities"];
    if (![capabilities isKindOfClass:NSDictionary.class]) return -1;
    id closedLoop = capabilities[@"DeviceSupportsClosedLoopHaptics"], level = capabilities[@"LSBUIKitFeedbackSupportLevel"];
    if (!closedLoop || CFGetTypeID((__bridge CFTypeRef)closedLoop) != CFBooleanGetTypeID() || ![closedLoop boolValue] ||
        !level || CFGetTypeID((__bridge CFTypeRef)level) != CFNumberGetTypeID() || CFNumberIsFloatType((__bridge CFNumberRef)level)) return -1;
    int64_t value;
    if (!CFNumberGetValue((__bridge CFNumberRef)level, kCFNumberSInt64Type, &value) || value < 0 || value > 2) return -1;
    return (NSInteger)value;
}

// Original arm64 iOS Simulator 26.0/23A343 UIKitCore, -[UIDevice(_UIFeedbackEngineSupport) _feedbackSupportLevel], +0xaa0a24.
// Its once block is empty and both paths return zero. Rebuilt caches relocate ADRP/LDR/ADD and BL targets, so validate those
// opcodes/registers while checking every other instruction exactly, including the return and empty once block.
static inline BOOL LSBHapticUIKitStubMatches(const struct mach_header_64 *header, const void *implementation) {
    static const uint8_t expectedUUID[16] = {
        0xf3, 0x92, 0x1c, 0x0a, 0x13, 0x32, 0x37, 0x79, 0x97, 0xc7, 0x62, 0xd9, 0xf3, 0xfa, 0x6b, 0x77
    };
    static const uint32_t expected[16] = {
        0xf000e248, 0xf9471908, 0xb100051f, 0x54000061, 0xd2800000, 0xd65f03c0, 0xa9bf7bfd, 0x910003fd,
        0xf000e240, 0x9138c000, 0xb000ba61, 0x9103c021, 0x9434f3fd, 0xa8c17bfd, 0x17fffff6, 0xd65f03c0
    };
    static const uint32_t masks[16] = {
        0x9f00001f, 0xffc003ff, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        0x9f00001f, 0xffc003ff, 0x9f00001f, 0xffc003ff, 0xfc000000, UINT32_MAX, UINT32_MAX, UINT32_MAX
    };
    const uintptr_t offset = 0xaa0a24;
    if (!header || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64 || header->sizeofcmds > 1024 * 1024 ||
        (uintptr_t)implementation < (uintptr_t)header || (uintptr_t)implementation - (uintptr_t)header != offset) return NO;
    const unsigned char *cursor = (const unsigned char *)(header + 1), *end = cursor + header->sizeofcmds;
    BOOL uuidMatches = NO, executableText = NO;
    for (uint32_t index = 0; index < header->ncmds; index++) {
        if ((size_t)(end - cursor) < sizeof(struct load_command)) return NO;
        const struct load_command *command = (const void *)cursor;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(end - cursor)) return NO;
        if (command->cmd == LC_UUID) {
            if (command->cmdsize != sizeof(struct uuid_command)) return NO;
            uuidMatches = !memcmp(((const struct uuid_command *)command)->uuid, expectedUUID, sizeof(expectedUUID));
        } else if (command->cmd == LC_SEGMENT_64) {
            if (command->cmdsize < sizeof(struct segment_command_64)) return NO;
            const struct segment_command_64 *segment = (const void *)command;
            // Cached Mach-O file offsets refer to the shared-cache file, not the image header. Only mapped extents matter here.
            if (!strncmp(segment->segname, "__TEXT", sizeof(segment->segname)) &&
                segment->vmsize >= offset + sizeof(expected) && (segment->initprot & VM_PROT_EXECUTE)) executableText = YES;
        }
        cursor += command->cmdsize;
    }
    if (cursor != end || !uuidMatches || !executableText) return NO;
    uint32_t instructions[16];
    memcpy(instructions, implementation, sizeof(instructions));
    for (size_t index = 0; index < sizeof(instructions) / sizeof(instructions[0]); index++)
        if ((instructions[index] & masks[index]) != (expected[index] & masks[index])) return NO;
    return YES;
}
