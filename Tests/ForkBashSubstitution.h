#pragma once

// command_substitute (+0x2f1f8) saved its source in x19 and passed caller SP+0x10 to pipe().
// make_child saves x19 in its own frame. The signed child rewrites and eventually frees this exact string.
typedef struct { int32_t readEnd, writeEnd; } BashCapturePipe;
_Static_assert(sizeof(BashCapturePipe) == 8, "Bash pipe local");

static int checkBashCaptureFD(TrialOffer *offer, int fd, int accessMode) {
    if (fd <= STDERR_FILENO || fd >= LSB_MAX_FDS) return ENOTSUP;
    struct stat status;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fstat(fd, &status) < 0) return errno;
    if (!S_ISFIFO(status.st_mode) || (flags & O_ACCMODE) != accessMode) return ENOTSUP;
    return preserveBashPipeFD(offer, fd);
}

static int addBashSubstitution(TrialPlan *plan, TrialOffer *offer, const BashMakeChildFrame *frame) {
    enum { PushedStringList = 0xa6790, ReturnCatchFlag = 0xa6e44 };
    const uintptr_t image = offer->mainHeader, sp = offer->caller.sp;
    uintptr_t pushedStrings;
    int32_t returnCatch;
    BashCapturePipe ends;
    if (frame->lr != image + BashSubstitutionReturn || sp > UINTPTR_MAX - sizeof(*frame) - 0xff || frame->fp != sp + sizeof(*frame) + 0xf0 ||
        !contains(plan, sp) || !contains(plan, sp + sizeof(*frame) + 0xff)) return ENOTSUP;
    if (!forkTrialRead(sp + sizeof(*frame) + 0x10, &ends, sizeof(ends)) || !forkTrialRead(image + PushedStringList, &pushedStrings, sizeof(pushedStrings)) ||
        !forkTrialRead(image + ReturnCatchFlag, &returnCatch, sizeof(returnCatch))) return EFAULT;
    // Alias input and return from a function use a different saved string/longjmp chain.
    if (pushedStrings || returnCatch || ends.readEnd == ends.writeEnd || !bashEarlyHeapBase()) return ENOTSUP;
    const char *source = (const char *)frame->x19;
    BashHeapBlock *block = bashHeapAllocation((BashHeap *)bashEarlyHeapBase(), source);
    if (!block) return ENOTSUP;
    size_t limit = bashHeapSpan(block) - sizeof(*block);
    if (limit > LSB_STRING_BYTES) limit = LSB_STRING_BYTES;
    size_t length = strnlen(source, limit);
    if (length == limit) return E2BIG;
    if (!length || !contains(plan, (uintptr_t)source) || !contains(plan, (uintptr_t)source + length) ||
        unsupportedBashSubstitution(source)) return ENOTSUP;
    int error = checkBashCaptureFD(offer, ends.readEnd, O_RDONLY);
    if (!error) error = checkBashCaptureFD(offer, ends.writeEnd, O_WRONLY);
    if (!error) forkTrialTrace("fork trial Bash substitution pipe_read=%d pipe_write=%d source_bytes=%zu\n", ends.readEnd, ends.writeEnd, length);
    return error;
}
