#pragma once
#include "ForkTrial.c"
#include <assert.h>
#include <sys/mman.h>

static int fixtureTerminal = -1;
int LSBPTYNameR(int fd, char *path, size_t capacity) {
    struct stat a, b;
    if (fixtureTerminal < 0 || fstat(fd, &a) || fstat(fixtureTerminal, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) return ENOTTY;
    return strlcpy(path, "/dev/lsbtty", capacity) < capacity ? 0 : ERANGE;
}

pid_t LSBGetPID(void) { return getpid(); }

// Only the header, UUID, call and import stub inspected by forkTrialBashImage are present. No code in this fixture executes.
static uint8_t *bashFixture(void) {
    uint8_t *image = calloc(1, 0xac000);
    assert(image);
    struct mach_header_64 *mh = (void *)image;
    *mh = (struct mach_header_64){.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = MH_EXECUTE, .ncmds = 2,
                                  .sizeofcmds = sizeof(struct uuid_command) + sizeof(struct segment_command_64)};
    struct uuid_command *uuid = (void *)(mh + 1);
    *uuid = (struct uuid_command){LC_UUID, sizeof(*uuid), {0x95, 0xda, 0x63, 0xf7, 0xde, 0xfa, 0x33, 0x94, 0xa3, 0x3f, 0x37, 0x4d, 0x41, 0xf1, 0x19, 0x0c}};
    struct segment_command_64 *seg = (void *)(uuid + 1);
    *seg = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(*seg), .segname = "__TEXT",
                                       .vmaddr = (uintptr_t)image & ~(vm_page_size - 1), .vmsize = vm_page_size};
    *(uint32_t *)(image + BashGetpidCall) = 0x94018f9a;
    const uint32_t stub[] = {0xd503201f, 0x580963b0, 0xd61f0200};
    memcpy(image + BashGetpidStub, stub, sizeof(stub));
    return image;
}
