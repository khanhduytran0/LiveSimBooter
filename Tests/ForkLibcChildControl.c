// Disposable 23A343 guest control. Call the pair registered with libc on this process's own prepared state.
// This does not simulate a child PID or resume a parent snapshot. Terminate without reusing the reset broker connection.
#include "ForkLibcChildBranch.h"
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t phase;
static void onPrepare(void) { phase = phase == 0 ? 1 : -1; }
static void onParent(void) { phase = phase == 1 ? 2 : -1; }
static void onChild(void) { phase = phase == 1 ? 3 : -1; }

static void describeStack(thread_t port) {
    arm_thread_state64_t state = {};
    mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&state, &words);
    if (kr || words != ARM_THREAD_STATE64_COUNT) { printf("  thread_get_state kr=%d words=%u\n", kr, words); return; }
    uintptr_t pc = arm_thread_state64_get_pc(state), fp = arm_thread_state64_get_fp(state), lr = arm_thread_state64_get_lr(state);
    // Read a bounded diagnostic sample without stopping a peer. Discard PAC bits only for symbol lookup and bounds checking.
    const uintptr_t mask = UINT64_C(0x0000ffffffffffff);
    for (unsigned i = 0; i < 8; ++i) {
        Dl_info symbol = {};
        pc &= mask;
        if (!pc) break;
        (void)dladdr((const void *)pc, &symbol); // Retained native-cache frames may be absent from guest dyld's metadata.
        const char *name = symbol.dli_fname ? strrchr(symbol.dli_fname, '/') : NULL;
        printf("  #%u %p %s %s\n", i, (const void *)pc, name ? name + 1 : "?", symbol.dli_sname ? symbol.dli_sname : "?");
        if (!i) { pc = lr; continue; }
        fp &= mask;
        struct { uintptr_t fp, lr; } frame;
        vm_size_t size = 0;
        if (!fp || (fp & 15) || vm_read_overwrite(mach_task_self(), fp, sizeof(frame), (vm_address_t)&frame, &size) || size != sizeof(frame)) break;
        if ((frame.fp & mask) <= fp || (frame.fp & mask) - fp > 8 * 1024 * 1024) break;
        fp = frame.fp;
        pc = frame.lr;
    }
}

static int threadCount(int describe) {
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count)) return -1;
    for (unsigned i = 0; i < count; ++i) {
        if (describe && count != 1) {
            pthread_t thread = pthread_from_mach_thread_np(threads[i]);
            char name[64] = "";
            if (thread) (void)pthread_getname_np(thread, name, sizeof(name));
            printf("thread %u port=%u main=%d pthread=%d name=%s\n", i, threads[i], thread == pthread_self(), thread != NULL, name);
            if (thread != pthread_self()) describeStack(threads[i]);
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(*threads));
    return (int)count;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int threads = threadCount(1);
    printf("child-handler control preflight threads=%d main=%d\n", threads, pthread_main_np());
    if (threads > 1 && pthread_main_np()) {
        unsigned elapsed = 0;
        while (threads > 1 && elapsed < 20) {
            sleep(1);
            threads = threadCount(0);
            ++elapsed;
        }
        printf("after %u seconds threads=%d\n", elapsed, threads);
        if (threads > 1) (void)threadCount(1);
    }
    if (threads != 1 || !pthread_main_np()) return EBUSY;
    uintptr_t returnPC = trialControlForkReturnPC();
    TrialLibcForkHandlers handlers = {};
    int error = trialResolveLibcForkHandlers(returnPC, false, &handlers);
    if (error) { printf("libc registered-handler lookup errno=%d\n", error); return error; }
    unsigned char *object = malloc(512);
    if (!object) return ENOMEM;
    memset(object, 0x47, 512);
    if ((error = pthread_atfork(onPrepare, onParent, onChild))) return error;
    // Prime stdout and the Mach thread enumeration before atfork can take dyld or allocator locks.
    if (threadCount(1) != 1) return EBUSY;
    puts("entering registered prepare");
    handlers.prepare(0);
    if (phase != 1 || threadCount(0) != 1) {
        handlers.parent(0);
        printf("child-handler control not entered; parent callback phase=%d\n", phase);
        _exit(EBUSY);
    }
    static const char prepared[] = "prepared; entering libc fork child branch\n";
    write(STDOUT_FILENO, prepared, sizeof(prepared) - 1);
    if (trialRunLibcChildBranch(returnPC)) _exit(70);
    unsigned char *grown = realloc(object, 65536);
    if (!grown || phase != 3) _exit(71);
    for (size_t i = 0; i < 512; ++i) if (grown[i] != 0x47) _exit(72);
    free(grown);
    void *fresh = calloc(4, 256);
    if (!fresh) _exit(73);
    ((volatile unsigned char *)fresh)[1023] = 1;
    free(fresh);
    printf("child-handler control child_callback=%d old_realloc=ok fresh_calloc_free=ok\n", phase);
    _exit(0);
}
