#pragma once

// Shared C/assembly layout. Only the low 64 bits of v8-v15 are callee-saved under the arm64 PCS.
#define LSB_FORK_FRAME_POINTER 80
#define LSB_FORK_STACK_POINTER 88
#define LSB_FORK_RETURN_PC 96
#define LSB_FORK_FP_CONTROL 104
#define LSB_FORK_FP_STATUS 108
#define LSB_FORK_FP_REGISTERS 112
#define LSB_FORK_CONTEXT_BYTES 176

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t x19ToX28[10], fp, sp, pc;
    uint32_t fpcr, fpsr;
    uint64_t d8ToD15[8];
} LSBForkContext;

#ifdef __cplusplus
extern "C" {
#define LSB_FORK_STATIC_ASSERT static_assert
#else
#define LSB_FORK_STATIC_ASSERT _Static_assert
#endif
LSB_FORK_STATIC_ASSERT(offsetof(LSBForkContext, fp) == LSB_FORK_FRAME_POINTER && offsetof(LSBForkContext, sp) == LSB_FORK_STACK_POINTER &&
                       offsetof(LSBForkContext, pc) == LSB_FORK_RETURN_PC, "arm64 fork continuation");
LSB_FORK_STATIC_ASSERT(offsetof(LSBForkContext, fpcr) == LSB_FORK_FP_CONTROL && offsetof(LSBForkContext, fpsr) == LSB_FORK_FP_STATUS &&
                       offsetof(LSBForkContext, d8ToD15) == LSB_FORK_FP_REGISTERS && sizeof(LSBForkContext) == LSB_FORK_CONTEXT_BYTES, "arm64 fork FP state");
#undef LSB_FORK_STATIC_ASSERT

// Capture at a plain-arm64 function boundary into storage that remains accessible to the coordinator.
// The initial return is 1. Once the child owns equivalent ranges and valid TLS, resumption returns 0
// at the identical call site. The child retains its platform register (x18) and thread pointer.
int LSBCaptureForkContext(LSBForkContext *context) __attribute__((returns_twice));
void LSBResumeForkContext(const LSBForkContext *context) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
#endif
