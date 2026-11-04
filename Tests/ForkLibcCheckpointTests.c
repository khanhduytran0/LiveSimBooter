#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static const char *owner = "/usr/lib/system/libsystem_c.dylib";
static void testHandler(unsigned flags) { assert(!flags); }
static int testDladdr(const void *address, Dl_info *info) {
    *info = (Dl_info){.dli_fname = address == (const void *)&testHandler ? "/usr/lib/libSystem.B.dylib" : owner};
    return 1;
}
#define dladdr testDladdr
#include "ForkLibcCheckpoint.h"
#undef dladdr

static uintptr_t forkPC, vforkPC;
uintptr_t LSBTrialLibcForkReturnPC(bool isVfork) { return isVfork ? vforkPC : forkPC; }

static void testSlots(const uint32_t *prefix, size_t length, bool isVfork) {
    _Alignas(4096) unsigned char page[4096] = {};
    uint32_t *instructions = (uint32_t *)page;
    memcpy(instructions, prefix, length);
    TrialLibcForkHandlers functions = {testHandler, testHandler, testHandler}, result;
    memcpy(page + 512, &functions, sizeof(functions));
    instructions[3] = instructions[15] = instructions[22] = 0x90000008; // ADRP x8, same page
    instructions[4] = 0xf9400108 | ((512 / 8) << 10);
    instructions[17] = instructions[4] + (1u << 10);
    instructions[23] = instructions[17] + (1u << 10);
    assert(!trialResolveLibcForkHandlers((uintptr_t)&instructions[13], isVfork, &result));
    assert(result.prepare == testHandler && result.parent == testHandler && result.child == testHandler);
    instructions[17] += 1u << 10;
    assert(trialResolveLibcForkHandlers((uintptr_t)&instructions[13], isVfork, &result) == ENOEXEC);
}

int main(void) {
    uint32_t code[26] = {[0] = 0xa9be4ff4, [1] = 0xa9017bfd, [2] = 0x910043fd, [6] = 0x52800000, [12] = 0x940187b1, [13] = 0xaa0003f3,
                         [14] = 0x34000100, [25] = 0x52800000};
    uint32_t vforkCode[26];
    memcpy(vforkCode, code, sizeof(code));
    vforkCode[6] = vforkCode[25] = 0x52800020;
    forkPC = (uintptr_t)&code[13];
    vforkPC = (uintptr_t)&vforkCode[13];
    _Alignas(16) uint64_t frames[16] = {};
    LSBForkWireRange stack = {.start = (uintptr_t)frames, .size = sizeof(frames)};
    LSBForkContext libc = {.pc = (uintptr_t)&code[13], .sp = (uintptr_t)&frames[0], .fp = (uintptr_t)&frames[2]};
    LSBForkContext app = {.sp = (uintptr_t)&frames[12]};
    frames[2] = (uintptr_t)&frames[6];
    frames[3] = 0x1004;
    testSlots(code, sizeof(code), false);
    testSlots(vforkCode, sizeof(vforkCode), true);
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, true) == ENOEXEC);
    libc.pc = vforkPC;
    assert(!trialValidateLibcCheckpoint(&libc, &app, stack, true));
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, false) == ENOEXEC);
    vforkCode[6] = code[6];
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, true) == ENOEXEC);
    vforkCode[6] = 0x52800020;
    vforkCode[25] = code[25];
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, true) == ENOEXEC);
    libc.pc = forkPC;
    assert(!trialValidateLibcCheckpoint(&libc, &app, stack, false));
    owner = "/usr/lib/system/libsystem_kernel.dylib";
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, false) == ENOEXEC);
    owner = "/usr/lib/system/libsystem_c.dylib";
    code[12] = 0x140187b1;
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, false) == ENOEXEC);
    code[12] = 0x940187b1;
    frames[2] = app.sp;
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, false) == EPROTO);
    frames[2] = (uintptr_t)&frames[6];
    app.sp = libc.sp + 16;
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, false) == EPROTO);
    app.sp = (uintptr_t)&frames[12];
    stack.size = UINT64_MAX;
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, false) == EPROTO);
    libc.sp = 0;
    libc.fp = 16;
    app.sp = 64;
    stack = (LSBForkWireRange){0, 128};
    assert(trialValidateLibcCheckpoint(&libc, &app, stack, false) == EFAULT);
    puts("Prepared libc fork/vfork checkpoints validate the selected branch, handler flags, cloned frame and stack bounds");
    return 0;
}
