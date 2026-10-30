#include "../LiveProcess/ProcessDisplayName.h"
#include "../LiveProcess/StackReservation.h"
#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

static const char expectedName[] = "LSB:1234 NativeArgumentStackProbe";

static void verifyArguments(int argc, char **argv, uint64_t top) {
    assert(argc == 2 && !strcmp(argv[1], "argument-canary"));
    const char *environment = getenv("LSB_STACK_TEST_ENV");
    assert(environment && !strcmp(environment, "environment-canary"));
    const void *saved[] = {argv[0], argv[1], environment};
    for (unsigned index = 0; index < sizeof(saved) / sizeof(*saved); ++index)
        assert((uintptr_t)saved[index] >= top - LSB_GUEST_MAIN_STACK_SIZE && (uintptr_t)saved[index] < top);
    int mib[] = {CTL_KERN, KERN_PROCARGS2, getpid()};
    size_t length = 0;
    assert(!sysctl(mib, 3, NULL, &length, NULL, 0) && length > sizeof(int));
    char *arguments = malloc(length);
    assert(arguments && !sysctl(mib, 3, arguments, &length, NULL, 0));
    int recordedCount = 0;
    memcpy(&recordedCount, arguments, sizeof(recordedCount));
    assert(recordedCount == argc);
    size_t offset = sizeof(recordedCount);
    while (offset < length && arguments[offset]) ++offset; // The signed executable path precedes the native argv window.
    while (offset < length && !arguments[offset]) ++offset;
    assert(offset < length && length - offset >= sizeof(expectedName));
    assert(!memcmp(arguments + offset, expectedName, sizeof(expectedName)) && !strcmp(argv[0], expectedName));
    free(arguments);
}

static uint64_t residentSize(void) {
    struct mach_task_basic_info info = {0};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    assert(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS);
    // The 2.25 GiB initial stack is address space, not resident memory. Keep the check generous across macOS versions.
    assert(info.resident_size < 128ULL * 1024 * 1024);
    return info.resident_size;
}

int main(int argc, char **argv) {
    uint64_t top = 0;
    size_t length = sizeof(top);
    int mib[] = {CTL_KERN, KERN_USRSTACK64};
    assert(!sysctl(mib, 2, &top, &length, NULL, 0) && length == sizeof(top));
    assert(top == (uintptr_t)pthread_get_stackaddr_np(pthread_self()));
    assert(pthread_get_stacksize_np(pthread_self()) == LSB_NATIVE_ARGUMENT_STACK_SIZE);
    const uint64_t cacheStart = 0x110000000ULL, cacheEnd = 0x18e194000ULL;
    size_t pageSize = (size_t)getpagesize();
    assert(pageSize && !(pageSize & (pageSize - 1)) && !(top & (pageSize - 1)));
    assert(top > LSB_GUEST_MAIN_STACK_SIZE + pageSize);
    uintptr_t guard = top - LSB_GUEST_MAIN_STACK_SIZE - pageSize;
    assert(top <= cacheStart || guard >= cacheEnd);
    assert((uintptr_t)__builtin_frame_address(0) >= top - LSB_GUEST_MAIN_STACK_SIZE);
    assert(mprotect((void *)guard, pageSize, PROT_NONE) == 0);
    int error = LSBSetProcessDisplayName("/guest/bin/NativeArgumentStackProbe", 1234, argv[0], strlen(argv[0]) + 1);
    if (error && error != EPERM) fprintf(stderr, "Native process-name update failed: %s (%d)\n", strerror(error), error);
    assert(!error || error == EPERM); // Sandboxing can deny the optional kernel long-name update.
    if (!error) {
        char name[33];
        size_t nameLength = sizeof(name);
        int nameMIB[] = {CTL_KERN, KERN_PROCNAME};
        assert(!sysctl(nameMIB, 2, name, &nameLength, NULL, 0) && nameLength == sizeof(name));
        assert(!strcmp(name, "LSB:1234 NativeArgumentStackProb"));
    }
    verifyArguments(argc, argv, top);
    uint64_t before = residentSize();
    // Never overwrite native mappings. Exclude the native shared region at 0x180000000 even though guest metadata extends beyond it.
    vm_address_t reservation = cacheStart;
    vm_size_t reservationSize = 0x180000000ULL - cacheStart;
    kern_return_t result = vm_allocate(mach_task_self(), &reservation, reservationSize, VM_FLAGS_FIXED);
    if (result == KERN_SUCCESS) {
        assert(reservation == cacheStart && mprotect((void *)reservation, reservationSize, PROT_NONE) == 0);
        verifyArguments(argc, argv, top);
        assert(vm_deallocate(mach_task_self(), reservation, reservationSize) == KERN_SUCCESS);
    } else {
        // A Mac may already own part of this range. The fixed allocation's refusal must leave argv undisturbed.
        verifyArguments(argc, argv, top);
    }
    uint64_t after = residentSize();
    printf("Native argument stack: kernel-top=0x%llx usable=0x%llx reservation=%d rss=%llu/%llu argv0=%s\n",
           (unsigned long long)top, (unsigned long long)LSB_GUEST_MAIN_STACK_SIZE, result,
           (unsigned long long)before, (unsigned long long)after, argv[0]);
    return 0;
}
