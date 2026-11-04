// Optional pre-READY hint between a fork donor and the native LiveProcess observer, outside the guest handshake.
#pragma once
#include "SpawnTransport.h"
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define LSB_FORK_STACK_HINT_ENV "LSB_FORK_DONOR_STACK"
enum { LSBForkStackHintLength = 33, LSBForkStackCollisionByte = 0xa7 };

static inline bool LSBForkStackHintValid(const char *text, uint64_t *bottom, uint64_t *top) {
    if (!text || strlen(text) != LSBForkStackHintLength || text[16] != ':' ||
        strspn(text, "0123456789abcdef") != 16 || strspn(text + 17, "0123456789abcdef") != 16) return false;
    *bottom = strtoull(text, NULL, 16);
    *top = strtoull(text + 17, NULL, 16);
    return *bottom && *top > *bottom && *top - *bottom <= LSB_MAX_FORK_BYTES && !((*bottom | *top) & (vm_page_size - 1));
}

static inline bool LSBForkStackUnplaceable(uint64_t bottom, uint64_t top) {
    task_vm_info_data_t vm;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (!task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) && count >= TASK_VM_INFO_REV2_COUNT && bottom < vm.min_address) return true;
    pthread_t main = pthread_self();
    uint64_t high = (uintptr_t)pthread_get_stackaddr_np(main), size = pthread_get_stacksize_np(main);
    uintptr_t sp = (uintptr_t)&main;
    if (!pthread_main_np() || size > high || sp < high - size || sp >= high) return false;
    // Only reject the actual mapped leaf containing this native main thread's live frame.
    vm_address_t low = sp;
    vm_size_t bytes = 0;
    natural_t depth = UINT32_MAX;
    vm_region_submap_info_data_64_t info;
    count = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (vm_region_recurse_64(mach_task_self(), &low, &bytes, &depth, (vm_region_recurse_info_t)&info, &count) ||
        count < VM_REGION_SUBMAP_INFO_V0_COUNT_64 || info.is_submap || low > sp || bytes <= sp - low || bytes > UINT64_MAX - low) return false;
    return bottom < high && bottom < low + bytes && top > high - size && top > low;
}

static inline bool LSBForkNotifyStackCollision(const LSBSpawnPlan *plan, const char *hint) {
    uint64_t bottom, top;
    if (!LSBForkStackHintValid(hint, &bottom, &top) || !LSBForkStackUnplaceable(bottom, top)) return false;
    // Only write to the explicit control socket passed in the caller's initial FD table and INHERIT action.
    if (plan->fdCount != 1 || plan->actionCount != 4 || plan->actions[0].kind != LSB_ACTION_INHERIT || plan->argc != 3) return false;
    int fd = plan->fds[0].number;
    char number[16];
    snprintf(number, sizeof(number), "%d", fd);
    if (fd < LSB_MAX_FDS || plan->actions[0].fd != fd || strcmp(plan->strings + plan->argv[1], number)) return false;
    for (int i = 0; i < 3; ++i)
        if (plan->actions[i+1].kind != LSB_ACTION_OPEN || plan->actions[i+1].fd != i || strcmp(plan->strings + plan->actions[i+1].path, "/dev/null")) return false;
    struct sockaddr_storage peer;
    socklen_t length = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &length) || peer.ss_family != AF_UNIX) return false;
    // One byte is indivisible on this fresh per-candidate socket. Fall back to normal startup if it cannot be sent.
    const uint8_t receipt = LSBForkStackCollisionByte;
    return send(fd, &receipt, 1, MSG_DONTWAIT | MSG_NOSIGNAL) == 1;
}

static inline bool LSBForkAcknowledgedStackCollision(int error, int fd) {
    if (error != EADDRINUSE || fd < 0) return false;
    uint8_t receipt;
    ssize_t n;
    do { n = recv(fd, &receipt, 1, MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
    return n == 1 && receipt == LSBForkStackCollisionByte;
}
