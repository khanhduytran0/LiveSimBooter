#include "ForkBashResume.h"

#if !defined(__arm64__) || defined(__arm64e__)
#error The copied Bash binary uses the plain arm64 ABI
#endif

.text
.p2align 2
.private_extern _LSBTrialBashGetpid
_LSBTrialBashGetpid:
    adrp x9, _LSBTrialObservedCalls@PAGE
    add x9, x9, _LSBTrialObservedCalls@PAGEOFF
    ldr x10, [x9, #LSB_BASH_GETPID_SP]
    cbnz x10, 1f
    mov x10, sp
    stp x10, x30, [x9, #LSB_BASH_GETPID_SP]
    // Park this first getpid call while the observer verifies the copied stack and commits a general fork.
    stp x29, x30, [sp, #-16]!
    adrp x0, _LSBTrialGetpidContext@PAGE
    add x0, x0, _LSBTrialGetpidContext@PAGEOFF
    bl _LSBCaptureForkContext
    cbnz w0, LresumeObserver
    ldp x29, x30, [sp], #16
1:  b _getpid

.private_extern _LSBTrialBashExecve
_LSBTrialBashExecve:
    adrp x9, _LSBTrialObservedCalls@PAGE
    add x9, x9, _LSBTrialObservedCalls@PAGEOFF
    stp x0, x1, [x9, #LSB_BASH_EXEC_PATH]
    stp x2, x29, [x9, #LSB_BASH_EXEC_ENVP]
    mov x10, sp
    stp x10, x30, [x9, #LSB_BASH_EXEC_SP]
LresumeObserver:
    adrp x0, _LSBTrialObserverContext@PAGE
    add x0, x0, _LSBTrialObserverContext@PAGEOFF
    b _LSBResumeForkContext
