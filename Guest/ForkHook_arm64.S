#include "ForkContext.h"

.text
.p2align 2
.private_extern _LSBPublicFork
_LSBPublicFork:
    .cfi_startproc
    sub sp, sp, #(LSB_FORK_CONTEXT_BYTES + 16)
    .cfi_def_cfa_offset (LSB_FORK_CONTEXT_BYTES + 16)
    str lr, [sp, #LSB_FORK_CONTEXT_BYTES]
    .cfi_offset lr, -16
    mov x0, sp
    bl _LSBCaptureForkContext
    // The capture helper saw this wrapper's frame. Restore the SP and return PC of the actual fork caller.
    add x9, sp, #(LSB_FORK_CONTEXT_BYTES + 16)
    ldr x10, [sp, #LSB_FORK_CONTEXT_BYTES]
    stp x9, x10, [sp, #LSB_FORK_STACK_POINTER]
    mov x0, sp
    bl _LSBForkAt
    ldr lr, [sp, #LSB_FORK_CONTEXT_BYTES]
    add sp, sp, #(LSB_FORK_CONTEXT_BYTES + 16)
    ret
    .cfi_endproc
