#pragma once

// At execute_simple_command's earlier make_child call (+0x178d4), w22 is async, w23/w25 are pipe out/in,
// x24 is the close bitmap and x28 is the unexpanded SIMPLE_COM. make_child saved x19..x24 in its frame.
typedef struct { int32_t flags, line; uintptr_t words, redirects; } BashSimpleCommand;
_Static_assert(sizeof(BashSimpleCommand) == 24 && offsetof(BashSimpleCommand, words) == 8, "Bash SIMPLE_COM");

static int preserveBashPipeFD(TrialOffer *offer, int fd) {
    if (fd < 0 || fd >= LSB_MAX_FDS) return ENOTSUP;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return errno;
    if (flags & FD_CLOEXEC) offer->closeOnExec[fd / 64] |= UINT64_C(1) << (fd % 64);
    return 0;
}

static int addBashCloseBitmap(TrialPlan *plan, TrialOffer *offer, uintptr_t address, size_t page) {
    if (!address) return 0;
    BashFDBitmap fds;
    int error = addBashObject(plan, address, &fds, sizeof(fds), page);
    if (error) return error;
    if (fds.size < 0 || fds.size > LSB_MAX_FDS) return ENOTSUP;
    if (!fds.size) return 0;
    uint8_t flags[LSB_MAX_FDS];
    if ((error = addBashBytes(plan, fds.bitmap, (size_t)fds.size, page))) return error;
    if (!forkTrialRead(fds.bitmap, flags, (size_t)fds.size)) return EFAULT;
    for (int fd = 0; fd < fds.size; ++fd) if (flags[fd] && (error = preserveBashPipeFD(offer, fd))) return error;
    return 0;
}

// Process substitution uses a different make_child caller and a separate FIFO list.
static bool unsupportedBashSubstitution(const char *word) {
    return strchr(word, '`') || strstr(word, "<(") || strstr(word, ">(");
}

static int addBashSimpleCommand(TrialPlan *plan, uintptr_t image, uintptr_t simple, size_t page) {
    BashSimpleCommand command;
    int error = addBashObject(plan, simple, &command, sizeof(command), page);
    if (error) return error;
    if (!command.words) return ENOTSUP;
    if ((error = addBashRedirectPages(plan, image, command.redirects, page))) return error;
    uintptr_t word = command.words;
    size_t budget = LSB_STRING_BYTES;
    for (unsigned count = 0; word; ++count) {
        if (count == LSB_MAX_VECTOR) return E2BIG;
        BashWordList list;
        BashWordDescription desc;
        if ((error = addBashObject(plan, word, &list, sizeof(list), page)) ||
            (error = addBashObject(plan, list.description, &desc, sizeof(desc), page)) ||
            (error = addBashString(plan, desc.string, &budget, page))) return error;
        if (unsupportedBashSubstitution((const char *)desc.string)) return ENOTSUP;
        word = list.next;
    }
    return 0;
}

#include "ForkBashSubshell.h"
#include "ForkBashSubstitution.h"

static int addBashShellCommand(TrialPlan *plan, TrialOffer *offer, size_t page) {
    uintptr_t image = offer->mainHeader;
    LSBForkContext *caller = &offer->caller;
    BashMakeChildFrame frame;
    if (caller->pc != image + 0x2570c || (caller->sp & 15) || caller->fp != caller->sp + offsetof(BashMakeChildFrame, fp) ||
        !forkTrialRead(caller->sp, &frame, sizeof(frame))) return ENOTSUP;
    int error = checkBashCommandMode(image, &offer->terminal);
    if (error) return error;
    if (frame.lr == image + BashLateChildReturn) return addBashCommandPages(plan, image, caller, page, TrialShell);
    error = checkBashTraps(image, true, NULL);
    if (error) return error;
    if (frame.lr == image + BashSubshellReturn) return addBashSubshellCommand(plan, offer, &frame, page);
    if (frame.lr == image + BashSubstitutionReturn) return addBashSubstitution(plan, offer, &frame);
    if (frame.lr != image + BashEarlyChildReturn) return ENOTSUP;
    int pipeIn = (int32_t)caller->x19ToX28[6], pipeOut = (int32_t)frame.x23, async = (int32_t)frame.x22;
    if ((async != 0 && async != 1) || pipeIn < -1 || pipeOut < -1 ||
        (!async && pipeIn < 0 && pipeOut < 0)) return ENOTSUP;
    if (pipeIn >= 0) error = preserveBashPipeFD(offer, pipeIn);
    if (!error && pipeOut >= 0) error = preserveBashPipeFD(offer, pipeOut);
    if (error) return error;
    if ((error = addBashCloseBitmap(plan, offer, frame.x24, page))) return error;
    if ((error = addBashSimpleCommand(plan, image, caller->x19ToX28[9], page))) return error;
    forkTrialTrace("fork trial Bash early async=%d pipe_in=%d pipe_out=%d heap=0x%lx\n", async, pipeIn, pipeOut, (unsigned long)bashEarlyHeapBase());
    return 0;
}
