#include "../Guest/ForkContext.h"
// The fixture keeps its bootstrap and controller stack outside the snapshot.
.text
.p2align 2
.globl _LSBForkFixtureRun
_LSBForkFixtureRun:
    stp x19, x20, [sp, #-96]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x29, x30, [sp, #48]
    str d8, [sp, #64]
    mrs x9, fpcr
    mrs x10, fpsr
    stp w9, w10, [sp, #72]
    adrp x20, _LSBCaptureForkContext@PAGE
    add x20, x20, _LSBCaptureForkContext@PAGEOFF
    cbnz x3, LhaveTarget
    // The public mode calls the application's ordinary imported libc function at the same fixed-VA instruction.
    adrp x20, _fork@GOTPAGE
    ldr x20, [x20, _fork@GOTPAGEOFF]
LhaveTarget:
    mov x21, sp
    mov x22, x2             // Controller context on original stack.
    mov x23, x3             // Coordinator function.
    mov x24, x4             // Checkpoint on original stack.
    mov sp, x1
    blr x0                  // Enter the fixed executable alias.
    mov sp, x21
    ldr d8, [sp, #64]
    ldp w9, w10, [sp, #72]
    msr fpcr, x9
    msr fpsr, x10
    ldp x29, x30, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #96
    ret

.globl _LSBForkFixtureResumeChild
_LSBForkFixtureResumeChild:
    // public-exec reserves two words in this fixture's child-private copy of the caller frame.
    // Only the child fills them, with its own saved bootstrap context and its own restore function.
    cbz x1, LresumeCaller
    ldr x9, [x0, #LSB_FORK_STACK_POINTER]
    str x1, [x9, #8]
    adrp x10, _LSBResumeForkContext@PAGE
    add x10, x10, _LSBResumeForkContext@PAGEOFF
    str x10, [x9, #80]
LresumeCaller:
    mov x19, #0             // Give the fresh child different values before resumption.
    fmov d8, xzr
    msr fpcr, xzr
    msr fpsr, xzr
    b _LSBResumeForkContext

// Until it exits or explicitly restores its own bootstrap, the child runs entirely within this signed, position-independent page.
// The parked child receives the checkpoint on its own startup stack before switching to the copy.
.section __TEXT,__fork_fixture,regular,pure_instructions
.p2align 14
.globl _LSBForkFixturePage
_LSBForkFixturePage:
    sub sp, sp, #96
    str x30, [sp, #88]      // Native return used only on the parent branch below.
    mov x19, #0x2468
    mov x9, #0x1357
    stp x9, xzr, [sp]       // Live stack local and optional child-bootstrap context.
    mov x9, #0x7bdf
    fmov d8, x9
    mov x9, #0x400000       // Round towards +infinity.
    msr fpcr, x9
    mov x9, #0x8000000      // FPSR cumulative saturation bit.
    msr fpsr, x9
    mov x0, x24
    blr x20
.globl _LSBForkFixtureContinuation
_LSBForkFixtureContinuation:
    cbz x0, Lchild
    cbz x23, LparentReturn   // Public fork returned the real broker-owned child PID, or -1 on failure.
    mov sp, x21             // Run the coordinator entirely off the snapshotted stack.
    ldp w9, w10, [sp, #72]
    msr fpcr, x9
    msr fpsr, x10
    mov x0, x22
    mov x1, x24
    blr x23
    ldr x10, [x24, #LSB_FORK_STACK_POINTER]
    mov sp, x10
LparentReturn:
    ldr x30, [sp, #88]
    add sp, sp, #96
    ret                     // Parent sees logical child PID or -1.

Lchild:
    mov x9, sp
    ldr x10, [sp]
    adr x11, _LSBForkFixtureContinuation
    stp x0, x19, [sp, #16]   // Fork result and preserved x19.
    stp x9, x11, [sp, #32]   // Report actual SP and executing label.
    mov x11, #0x369c
    str x11, [sp]
    ldr x11, [sp]
    stp x10, x11, [sp, #48]  // Original and changed stack local.
    str d8, [sp, #64]
    mrs x9, fpcr
    mrs x10, fpsr
    stp w9, w10, [sp, #72]
    mov x0, #0              // The child's full-duplex control socket.
    add x1, sp, #16
    mov x2, #64
    mov x16, #4             // Darwin write on the inherited socket.
    svc #0x80
    b.cs Lfailed
    cmp x0, #64
    b.ne Lfailed
    ldr x0, [sp, #8]
    cbz x0, Lexit37
    ldr x9, [sp, #80]       // Restore the child's full register context before calling any libc code.
    br x9
Lexit37:
    mov x0, #37
    b Lexit
Lfailed:
    mov x0, #76
Lexit:
    mov x16, #1             // Darwin exit avoids returning to the discarded child bootstrap stack.
    svc #0x80
    brk #0

.globl _LSBForkFixtureMarker
_LSBForkFixtureMarker:
    mov w0, #0x77
    ret
