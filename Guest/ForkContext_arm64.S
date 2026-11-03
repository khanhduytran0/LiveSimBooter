#include "ForkContext.h"

#if !defined(__arm64__) || defined(__arm64e__)
#error Fork context requires a plain arm64 guest ABI
#endif

.text
.p2align 2
.globl _LSBCaptureForkContext
_LSBCaptureForkContext:
    stp x19, x20, [x0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    mov x9, sp
    stp x29, x9, [x0, #LSB_FORK_FRAME_POINTER]
    str x30, [x0, #LSB_FORK_RETURN_PC]
    mrs x9, fpcr
    mrs x10, fpsr
    stp w9, w10, [x0, #LSB_FORK_FP_CONTROL]
    stp d8, d9, [x0, #LSB_FORK_FP_REGISTERS]
    stp d10, d11, [x0, #(LSB_FORK_FP_REGISTERS + 16)]
    stp d12, d13, [x0, #(LSB_FORK_FP_REGISTERS + 32)]
    stp d14, d15, [x0, #(LSB_FORK_FP_REGISTERS + 48)]
    mov w0, #1
    ret

.globl _LSBResumeForkContext
_LSBResumeForkContext:
    ldp d8, d9, [x0, #LSB_FORK_FP_REGISTERS]
    ldp d10, d11, [x0, #(LSB_FORK_FP_REGISTERS + 16)]
    ldp d12, d13, [x0, #(LSB_FORK_FP_REGISTERS + 32)]
    ldp d14, d15, [x0, #(LSB_FORK_FP_REGISTERS + 48)]
    ldp w9, w10, [x0, #LSB_FORK_FP_CONTROL]
    msr fpcr, x9
    msr fpsr, x10
    ldp x19, x20, [x0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x9, [x0, #LSB_FORK_FRAME_POINTER]
    ldr x30, [x0, #LSB_FORK_RETURN_PC]
    mov sp, x9
    mov w0, #0
    ret
