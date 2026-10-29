#import "../Guest/HapticUIKitSupport.h"
#include <assert.h>
#include <stdlib.h>

static NSDictionary *profile(id closedLoop, id level) {
    NSMutableDictionary *capabilities = [NSMutableDictionary dictionary];
    if (closedLoop) capabilities[@"DeviceSupportsClosedLoopHaptics"] = closedLoop;
    if (level) capabilities[@"LSBUIKitFeedbackSupportLevel"] = level;
    return @{@"capabilities": capabilities};
}

static void capabilityTests(void) {
    for (NSInteger value = 0; value <= 2; value++) {
        NSDictionary *original = profile(@YES, @(value));
        NSData *data = [NSPropertyListSerialization dataWithPropertyList:original format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
        NSDictionary *decoded = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:NULL];
        assert(LSBHapticUIKitSupportLevel(decoded) == value);
    }
    for (id level in @[@YES, @NO, @(-1), @3, @(UINT64_MAX), @1.5, @"2", @[], @{}])
        assert(LSBHapticUIKitSupportLevel(profile(@YES, level)) == -1);
    for (id capability in @[@NO, @1, @"true", @[], @{}])
        assert(LSBHapticUIKitSupportLevel(profile(capability, @2)) == -1);
    assert(LSBHapticUIKitSupportLevel(profile(@YES, nil)) == -1);
    assert(LSBHapticUIKitSupportLevel(profile(nil, @2)) == -1);
    assert(LSBHapticUIKitSupportLevel(@{@"capabilities": @YES}) == -1);
    assert(LSBHapticUIKitSupportLevel((id)@[]) == -1);
    assert(LSBHapticUIKitSupportLevel(nil) == -1);
}

static void stubTests(void) {
    const size_t offset = 0xaa0a24, size = offset + 64;
    unsigned char *image = calloc(1, size);
    assert(image);
    struct mach_header_64 *header = (void *)image;
    *header = (struct mach_header_64){.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = MH_DYLIB, .ncmds = 2,
        .sizeofcmds = sizeof(struct uuid_command) + sizeof(struct segment_command_64)};
    struct uuid_command *uuid = (void *)(header + 1);
    *uuid = (struct uuid_command){.cmd = LC_UUID, .cmdsize = sizeof(*uuid),
        .uuid = {0xf3, 0x92, 0x1c, 0x0a, 0x13, 0x32, 0x37, 0x79, 0x97, 0xc7, 0x62, 0xd9, 0xf3, 0xfa, 0x6b, 0x77}};
    struct segment_command_64 *text = (void *)(uuid + 1);
    *text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(*text), .segname = "__TEXT",
        .vmsize = size, .filesize = size, .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    uint32_t *stub = (void *)(image + offset);
    const uint32_t original[] = {
        0xf000e248, 0xf9471908, 0xb100051f, 0x54000061, 0xd2800000, 0xd65f03c0, 0xa9bf7bfd, 0x910003fd,
        0xf000e240, 0x9138c000, 0xb000ba61, 0x9103c021, 0x9434f3fd, 0xa8c17bfd, 0x17fffff6, 0xd65f03c0
    };
    memcpy(stub, original, sizeof(original));
    assert(LSBHapticUIKitStubMatches(header, stub));
    text->fileoff = 0x4d217000; // Real rebuilt-cache __TEXT offset; it is not relative to its Mach-O header.
    text->vmaddr = 0x15d217000;
    assert(LSBHapticUIKitStubMatches(header, stub));
    // Cache relocation can change these address immediates without changing the exact zero-return method contract.
    for (unsigned index = 0; index < 16; index++) {
        if (index == 0 || index == 8 || index == 10) stub[index] ^= 1u << 29;
        if (index == 1 || index == 9 || index == 11) stub[index] ^= 1u << 12;
        if (index == 12) stub[index] ^= 1u << 16;
    }
    assert(LSBHapticUIKitStubMatches(header, stub));
    stub[4] = 0xd2800020; // A different zero/constant-return contract must not be overwritten.
    assert(!LSBHapticUIKitStubMatches(header, stub));
    memcpy(stub, original, sizeof(original));
    stub[15] = 0xd503201f; // The once block must still be empty.
    assert(!LSBHapticUIKitStubMatches(header, stub));
    memcpy(stub, original, sizeof(original));
    stub[1] ^= 1; // Address masks do not permit a different destination register.
    assert(!LSBHapticUIKitStubMatches(header, stub));
    memcpy(stub, original, sizeof(original));
    assert(!LSBHapticUIKitStubMatches(header, stub + 1));
    uuid->uuid[0] ^= 1;
    assert(!LSBHapticUIKitStubMatches(header, stub));
    uuid->uuid[0] ^= 1;
    text->initprot = VM_PROT_READ;
    assert(!LSBHapticUIKitStubMatches(header, stub));
    text->initprot |= VM_PROT_EXECUTE;
    text->vmsize--;
    assert(!LSBHapticUIKitStubMatches(header, stub));
    text->vmsize++;
    uuid->cmdsize = 4;
    assert(!LSBHapticUIKitStubMatches(header, stub));
    uuid->cmdsize = sizeof(*uuid);
    header->ncmds++;
    assert(!LSBHapticUIKitStubMatches(header, stub));
    header->ncmds--;
    header->sizeofcmds++;
    assert(!LSBHapticUIKitStubMatches(header, stub));
    free(image);
}

int main(void) {
    @autoreleasepool {
        capabilityTests();
        stubTests();
        puts("UIKit haptic capability types and pinned simulator stub tests passed");
    }
    return 0;
}
