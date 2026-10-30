#include "../LiveProcess/ProcessDisplayName.h"
#include "../LiveProcess/StackReservation.h"
#include <assert.h>
#include <errno.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const struct mach_header_64 _mh_execute_header;

int main(int argc, char **argv) {
    assert(argc == 2 && !strcmp(argv[1], "argument-canary"));
    assert(getenv("LSB_STACK_TEST_ENV") && !strcmp(getenv("LSB_STACK_TEST_ENV"), "environment-canary"));
    uint64_t top = 0;
    size_t length = sizeof(top);
    int stackMIB[] = {CTL_KERN, KERN_USRSTACK64};
    assert(!sysctl(stackMIB, 2, &top, &length, NULL, 0));
    assert(top == (uintptr_t)pthread_get_stackaddr_np(pthread_self()) && pthread_get_stacksize_np(pthread_self()) == 0x800000);
    uintptr_t slide = (uintptr_t)&_mh_execute_header - 0x100000000ULL;
    assert(slide < LSB_STACK_RESERVATION_MAX_SLIDE && top <= 0x110000000ULL);
    const struct load_command *command = (const void *)(&_mh_execute_header + 1);
    unsigned reservations = 0;
    for (unsigned index = 0; index < _mh_execute_header.ncmds; ++index) {
        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const void *)command;
            if (!strncmp(segment->segname, LSB_STACK_RESERVATION_SEGMENT, sizeof(segment->segname))) {
                ++reservations;
                assert(segment->vmaddr == LSB_STACK_RESERVATION_ADDRESS && segment->vmsize == LSB_STACK_RESERVATION_SIZE);
                assert(!segment->fileoff && !segment->filesize && !segment->initprot && !segment->maxprot && segment->nsects == 1);
            }
        }
        command = (const void *)((const char *)command + command->cmdsize);
    }
    assert(reservations == 1);
    // The initial kernel stack has already moved. Release only the zero-file guard, then prove its address can be reused.
    vm_address_t guard = LSB_STACK_RESERVATION_ADDRESS + slide;
    assert(vm_deallocate(mach_task_self(), guard, LSB_STACK_RESERVATION_SIZE) == KERN_SUCCESS);
    vm_address_t reused = guard;
    assert(vm_allocate(mach_task_self(), &reused, LSB_STACK_RESERVATION_SIZE, VM_FLAGS_FIXED) == KERN_SUCCESS && reused == guard);
    assert(mprotect((void *)reused, LSB_STACK_RESERVATION_SIZE, PROT_NONE) == 0);
    assert(vm_deallocate(mach_task_self(), reused, LSB_STACK_RESERVATION_SIZE) == KERN_SUCCESS);
    assert((uintptr_t)argv[0] < top && (uintptr_t)argv[0] >= top - 0x800000);
    int error = LSBSetProcessDisplayName("/guest/bin/LiveSimTest", 1234, argv[0], strlen(argv[0]) + 1);
    assert(!error || error == EPERM); // The macOS execution sandbox may deny only the optional long-name setter.
    int argumentsMIB[] = {CTL_KERN, KERN_PROCARGS2, getpid()};
    length = 0;
    assert(!sysctl(argumentsMIB, 3, NULL, &length, NULL, 0) && length > sizeof(int));
    char *arguments = malloc(length);
    assert(arguments && !sysctl(argumentsMIB, 3, arguments, &length, NULL, 0));
    int recordedCount = 0;
    memcpy(&recordedCount, arguments, sizeof(recordedCount));
    assert(recordedCount == argc);
    size_t offset = sizeof(recordedCount);
    while (offset < length && arguments[offset]) ++offset; // Kernel executable path precedes the argv strings.
    while (offset < length && !arguments[offset]) ++offset;
    assert(offset < length && !strcmp(arguments + offset, "LSB:1234 LiveSimTest"));
    assert(!strcmp(argv[1], "argument-canary") && !strcmp(getenv("LSB_STACK_TEST_ENV"), "environment-canary"));
    printf("Native stack probe: slide=0x%llx kernel-top=0x%llx argv0=%s\n", (unsigned long long)slide, (unsigned long long)top, arguments + offset);
    free(arguments);
    return 0;
}
