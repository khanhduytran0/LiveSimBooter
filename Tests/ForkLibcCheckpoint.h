#pragma once
#include "ForkTrialWire.h"
#include "ForkLibcChildBranch.h"

// Guest 23A343 libsystem_c fork/vfork: save x20/x19 and fp/lr in a 32-byte frame, call __fork at +0x30, then dispatch on w0.
// Read the observer's actual cache and its newly cloned stack. Parent and observer shim addresses may differ.
static inline int trialValidateLibcCheckpoint(const LSBForkContext *libc, const LSBForkContext *application, LSBForkWireRange stack, bool isVfork) {
    typedef struct { uint64_t x20, x19, fp, lr; } LibcForkFrame;
    _Static_assert(sizeof(LibcForkFrame) == 32 && offsetof(LibcForkFrame, fp) == 16, "23A343 libc fork frame");
    if (stack.size > UINT64_MAX - stack.start || libc->sp < stack.start || libc->sp > application->sp ||
        application->sp >= stack.start + stack.size || application->sp - libc->sp < sizeof(LibcForkFrame) ||
        ((libc->sp | application->sp) & 15) || libc->fp != libc->sp + offsetof(LibcForkFrame, fp)) return EPROTO;

    // Both tasks map this libc at the same guest address. Query the shim's original import, which bypasses its public interposer.
    if (libc->pc != LSBTrialLibcForkReturnPC(isVfork)) return ENOEXEC;
    int error = trialCheckLibcChildBranch(libc->pc, isVfork);
    if (error) return error;
    vm_size_t copied = 0;
    LibcForkFrame frame;
    if (vm_read_overwrite(mach_task_self(), libc->sp, sizeof(frame), (vm_address_t)&frame, &copied) || copied != sizeof(frame)) return EFAULT;
    if (!frame.lr || (frame.lr & 3) || (frame.fp & 15) || frame.fp <= libc->fp || frame.fp >= application->sp) return EPROTO;
    return 0;
}
