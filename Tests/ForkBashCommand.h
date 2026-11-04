#pragma once
#include "ForkBashTraps.h"

// Bash 3.2.57 (UUID checked by forkTrialBashImage). llvm-objdump shows make_child's
// six saved registers and frame record, then execute_simple_command's locals at the call at +0x18370.
typedef struct {
    uint8_t locals[0x20];
    uintptr_t x24, x23, x22, x21, x20, x19, fp, lr;
} BashMakeChildFrame;

typedef struct { uintptr_t next, description; } BashWordList;
typedef struct { uintptr_t string; int32_t flags; } BashWordDescription;
typedef struct { int32_t size; uintptr_t bitmap; } BashFDBitmap;
_Static_assert(sizeof(BashMakeChildFrame) == 0x60 && offsetof(BashMakeChildFrame, fp) == 0x50, "make_child frame record");
_Static_assert(sizeof(BashWordList) == 16 && sizeof(BashWordDescription) == 16 && offsetof(BashFDBitmap, bitmap) == 8, "Bash command objects");

// Preserve only pages reached from the reviewed command objects. An object may cross a page boundary,
// or share pages with the executable, original stack, variable context, or another object.
static int addForkBytes(TrialPlan *plan, uintptr_t address, size_t size, size_t pageSize) {
    uintptr_t end;
    if (!address || !size || __builtin_add_overflow(address, size, &end)) return EFAULT;
    for (uintptr_t page = address & ~(pageSize - 1); page < end;) {
        uintptr_t next;
        TrialLeaf leaf = {};
        if (__builtin_add_overflow(page, pageSize, &next)) return EOVERFLOW;
        int error = readLeaf(page, &leaf);
        if (error) return error;
        if (leaf.end < next || leaf.info.inheritance == VM_INHERIT_NONE || !(leaf.info.protection & VM_PROT_READ)) return ENOTSUP;
        if (!contains(plan, page)) {
            // argv and environ may reference literals in a signed image already in the offer.
            if (leaf.info.inheritance != VM_INHERIT_COPY || (leaf.info.protection & VM_PROT_EXECUTE)) return ENOTSUP;
            error = addSpan(plan, "referenced-by-guest", page, next);
            if (error) return error;
        }
        page = next;
    }
    return 0;
}

static int addBashObject(TrialPlan *plan, uintptr_t address, void *buffer, size_t bytes, size_t pageSize) {
    if (address % sizeof(uintptr_t)) return EFAULT;
    int error = addForkBytes(plan, address, bytes, pageSize);
    if (error) return error;
    return forkTrialRead(address, buffer, bytes) ? 0 : EFAULT;
}

static int addForkString(TrialPlan *plan, uintptr_t start, size_t *budget, size_t pageSize) {
    if (!start) return EFAULT;
    for (size_t used = 0; used < *budget;) {
        char block[256];
        uintptr_t address;
        if (__builtin_add_overflow(start, used, &address)) return EFAULT;
        size_t bytes = pageSize - (address & (pageSize - 1));
        if (bytes > sizeof(block)) bytes = sizeof(block);
        if (bytes > *budget - used) bytes = *budget - used;
        if (!forkTrialRead(address, block, bytes)) return EFAULT;
        const char *nul = memchr(block, '\0', bytes);
        if (nul) {
            used += (size_t)(nul - block) + 1;
            *budget -= used;
            return addForkBytes(plan, start, used, pageSize);
        }
        used += bytes;
    }
    return E2BIG;
}

#include "ForkBashRedirect.h"

static int checkBashCommandMode(uintptr_t image, TrialTerminalAlias *terminal) {
    enum { InteractiveShell = 0xa6720, JobControl = 0x9f660, DefaultBufferedInput = 0x9eee0, ShellTTY = 0x9f648 };
    int32_t interactive, jobControl, bufferedInput, tty;
    if (!forkTrialRead(image + InteractiveShell, &interactive, sizeof(interactive)) || !forkTrialRead(image + JobControl, &jobControl, sizeof(jobControl)) ||
        !forkTrialRead(image + DefaultBufferedInput, &bufferedInput, sizeof(bufferedInput))) return EFAULT;
    if (bufferedInput > 0) return ENOTSUP;
    if (!interactive && !jobControl) return 0;
    if (!terminal || interactive != 1 || jobControl != 1) return ENOTSUP;
    return forkTrialRead(image + ShellTTY, &tty, sizeof(tty)) ? trialCaptureTerminal(tty, terminal) : EFAULT;
}

static int addBashCommandPages(TrialPlan *plan, uintptr_t header, const LSBForkContext *context, size_t pageSize, TrialMode mode) {
    enum { MakeChildReturn = 0x18374, ForkReturn = 0x2570c, ExportEnv = 0xa6838 };
    BashMakeChildFrame frame;
    if (!pageSize || (pageSize & (pageSize - 1)) || !forkTrialBashImage(header) ||
        context->pc != header + ForkReturn || (context->sp & 15) || context->sp > UINTPTR_MAX - sizeof(frame) - 0xc0 ||
        context->fp != context->sp + offsetof(BashMakeChildFrame, fp)) return ENOTSUP;
    uintptr_t callerSP = context->sp + sizeof(frame), words, redirects, env;
    int32_t pipeOut;
    if (!forkTrialRead(context->sp, &frame, sizeof(frame)) || !forkTrialRead(callerSP + 0x80, &words, sizeof(words)) ||
        !forkTrialRead(callerSP + 0xa4, &pipeOut, sizeof(pipeOut)) || !forkTrialRead(callerSP + 0xb8, &redirects, sizeof(redirects)) ||
        !forkTrialRead(header + ExportEnv, &env, sizeof(env))) return EFAULT;
    if (frame.lr != header + MakeChildReturn || !contains(plan, context->sp) || !contains(plan, callerSP + 0xbf) ||
        (uint32_t)frame.x22 ||
        (int32_t)context->x19ToX28[6] != -1 || pipeOut != -1 || (redirects && mode == TrialDF)) return ENOTSUP;
    if (mode != TrialShell) {
        int error = checkBashCommandMode(header, NULL);
        if (error) return error;
    }
    int redirectError = addBashRedirectPages(plan, header, redirects, pageSize);
    if (redirectError) return redirectError;
    int trapError = checkBashTraps(header, mode != TrialDF, NULL);
    if (trapError) return trapError;

    // x21 is Bash's resolved executable. The normal execve interposer applies the runtime's path policy after the child reaches it.
    if (!frame.x21) return ENOTSUP; // Bash's separate command-not-found branch does not reach execve.
    size_t pathBudget = LSB_PATH_BYTES;
    int error = addForkString(plan, frame.x21, &pathBudget, pageSize);
    if (error) return error == E2BIG ? ENAMETOOLONG : error;
    size_t pathBytes = LSB_PATH_BYTES - pathBudget;
    if (mode == TrialDF) {
        char expected[LSB_PATH_BYTES], actual[LSB_PATH_BYTES];
        const char *root = getenv("DYLD_ROOT_PATH");
        if (!root || *root != '/') return ENOENT;
        int length = snprintf(expected, sizeof(expected), "%s/bin/df", root);
        if (length < 0 || (size_t)length >= sizeof(expected)) return ENAMETOOLONG;
        if (pathBytes != (size_t)length + 1 || !forkTrialRead(frame.x21, actual, pathBytes) || memcmp(actual, expected, pathBytes)) return ENOTSUP;
    }

    if (frame.x24) {
        BashFDBitmap fds;
        error = addBashObject(plan, frame.x24, &fds, sizeof(fds), pageSize);
        if (error) return error;
        if (fds.size < 0 || fds.size > (int)LSB_MAX_FDS) return ENOTSUP;
        if (fds.size) {
            uint8_t flags[LSB_MAX_FDS];
            error = addForkBytes(plan, fds.bitmap, (size_t)fds.size, pageSize);
            if (error) return error;
            if (!forkTrialRead(fds.bitmap, flags, (size_t)fds.size)) return EFAULT;
            // The standalone foreground command has no pipe ends to close.
            for (int i = 0; i < fds.size; ++i) if (flags[i]) return ENOTSUP;
        }
    }
    size_t budget = LSB_STRING_BYTES - pathBytes;
    unsigned argc = 0, envc = 0;
    if (!words || !env) return EFAULT;
    while (words) {
        BashWordList list;
        BashWordDescription desc;
        if (argc == LSB_MAX_VECTOR) return E2BIG;
        if ((error = addBashObject(plan, words, &list, sizeof(list), pageSize)) ||
            (error = addBashObject(plan, list.description, &desc, sizeof(desc), pageSize)) ||
            (error = addForkString(plan, desc.string, &budget, pageSize))) return error;
        words = list.next;
        ++argc;
    }
    for (;;) {
        uintptr_t entry;
        if ((error = addBashObject(plan, env, &entry, sizeof(entry), pageSize))) return error;
        if (!entry) break;
        if (envc == LSB_MAX_VECTOR) return E2BIG;
        if ((error = addForkString(plan, entry, &budget, pageSize))) return error;
        env += sizeof(entry); // addBashObject checked that this addition fits.
        ++envc;
    }
    forkTrialTrace("fork trial Bash command pages complete argc=%u envc=%u\n", argc, envc);
    return 0;
}
