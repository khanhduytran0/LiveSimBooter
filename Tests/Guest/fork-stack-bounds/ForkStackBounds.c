// Check the resumed child's actual SP, pthread stack bounds and lower guard through public guest APIs.
#include <errno.h>
#include <inttypes.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static const uint64_t parentWord = UINT64_C(0x6c7362466f726b), childWord = UINT64_C(0xba5ed);

static int checkChild(uintptr_t parentTop, size_t parentSize, volatile uint64_t *marker) {
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);
    uintptr_t bottom = size < top ? top - size : 0;
    bool covers = bottom && bottom <= sp && sp < top;
    bool matches = top == parentTop && size == parentSize;

    // A Mach region query may return the next mapping when the requested byte lies in a hole.
    uintptr_t target = bottom ? bottom - 1 : 0;
    vm_address_t address = target;
    vm_size_t bytes = 0;
    natural_t depth = UINT32_MAX;
    vm_region_submap_info_data_64_t info = {};
    mach_msg_type_number_t words = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = vm_region_recurse_64(mach_task_self(), &address, &bytes, &depth, (vm_region_recurse_info_t)&info, &words);
    bool guard = bottom && kr == KERN_SUCCESS && words >= VM_REGION_SUBMAP_INFO_V0_COUNT_64 && !info.is_submap &&
                 address <= target && target - address < bytes && info.protection == VM_PROT_NONE;
    dprintf(STDERR_FILENO, "guardchild pid=%d at=%#" PRIxPTR " start=%#lx size=%#lx prot=%d matches=%d\n",
            getpid(), target, (unsigned long)address, (unsigned long)bytes, kr == KERN_SUCCESS ? info.protection : -1, guard);

    bool inherited = *marker == parentWord;
    *marker = childWord;
    bool local = inherited && *marker == childWord;
    dprintf(STDERR_FILENO, "boundschild pid=%d sp=%#" PRIxPTR " lo=%#" PRIxPTR " hi=%#" PRIxPTR " parent_top=%#" PRIxPTR
            " parent_size=%zu covers=%d matches=%d local=%d\n", getpid(), sp, bottom, top, parentTop, parentSize, covers, matches, local);
    if (!local) return 39;
    return matches && covers && guard ? 37 : 38;
}

int main(void) {
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);
    volatile uint64_t marker = parentWord;
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (!child) _exit(checkChild(top, size, &marker));

    int status;
    pid_t reaped;
    do { reaped = waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
    if (reaped != child) { perror("waitpid"); return 1; }
    bool private = marker == parentWord;
    dprintf(STDERR_FILENO, "boundsparent pid=%d child=%d wait=%d status=%#x cow=%d\n", getpid(), child, reaped, status, private);
    return WIFEXITED(status) && WEXITSTATUS(status) == 37 && private ? 0 : 1;
}
