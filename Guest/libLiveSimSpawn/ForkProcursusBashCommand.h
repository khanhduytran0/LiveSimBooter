#pragma once
#include "../Shared/Fork/ForkBashEarlyHeap.h"
#include "../Shared/Fork/ForkContext.h"

// Procursus 5.2.21 make_child (+0x23720) saves all ten callee-saved registers.
// Its frame record is at SP+0xa0, with the parent command's locals above it.
typedef struct {
    uint8_t locals[0x50];
    uintptr_t x28, x27, x26, x25, x24, x23, x22, x21, x20, x19, fp, lr;
} ProcursusBashFrame;
_Static_assert(offsetof(ProcursusBashFrame, fp) == 0xa0 && sizeof(ProcursusBashFrame) == 0xb0, "Procursus make_child frame");

// Installed Bash 5.2 headers command.h and execute_cmd.h match the layouts used at +0xe850, +0xe878 and +0x16490.
_Static_assert(offsetof(BashCommandNode, redirects) == 0x10 && offsetof(BashCommandNode, value) == 0x18 &&
               offsetof(BashSubshellNode, command) == 8 && offsetof(BashSimpleCommand, words) == 8 &&
               offsetof(BashSimpleCommand, redirects) == 0x10 && offsetof(BashWordList, description) == 8 &&
               offsetof(BashWordDescription, string) == 0, "Procursus parsed command");

static size_t procursusHeapAvailable(uintptr_t address) {
    uintptr_t base = bashEarlyHeapBase();
    return base && address >= base && address - base < BashEarlyHeapBytes ? BashEarlyHeapBytes - (address - base) : 0;
}

static bool procursusHeapObject(uintptr_t address, void *out, size_t size) {
    return !(address % sizeof(uintptr_t)) && procursusHeapAvailable(address) >= size && forkTrialRead(address, out, size);
}

static const char *procursusHeapString(uintptr_t address, size_t limit) {
    size_t room = procursusHeapAvailable(address);
    if (room > limit) room = limit;
    return room && memchr((const void *)address, 0, room) ? (const char *)address : NULL;
}

static bool procursusNoPendingCloses(uintptr_t address) {
    if (!address) return true;
    BashFDBitmap fds;
    if (!procursusHeapObject(address, &fds, sizeof(fds)) || fds.size < 0 || fds.size > LSB_MAX_FDS ||
        procursusHeapAvailable(fds.bitmap) < (size_t)fds.size) return false;
    const uint8_t *bits = (const uint8_t *)fds.bitmap;
    for (int i = 0; i < fds.size; ++i) if (bits[i]) return false;
    return true;
}

static int procursusWords(uintptr_t words, bool exitBuiltin) {
    size_t remaining = LSB_STRING_BYTES;
    for (unsigned i = 0; i <= LSB_MAX_VECTOR; ++i) {
        if (!words) return (exitBuiltin ? i == 2 : i > 0) ? 0 : ENOTSUP;
        if (i == LSB_MAX_VECTOR) break;
        BashWordList link;
        BashWordDescription desc;
        if (!procursusHeapObject(words, &link, sizeof(link)) || !procursusHeapObject(link.description, &desc, sizeof(desc))) return ENOTSUP;
        const char *word = procursusHeapString(desc.string, remaining);
        if (!word) return ENOTSUP;
        size_t len = strlen(word);
        if (exitBuiltin && (desc.flags || i > 1 || (i == 0 ? strcmp(word, "exit") != 0 : !len || len > 3 || strspn(word, "0123456789") != len)))
            return ENOTSUP;
        remaining -= len + 1;
        words = link.next;
    }
    return E2BIG;
}

// The installed COMMAND, SUBSHELL_COM and SIMPLE_COM layouts and cm_simple=4, cm_subshell=13 come from this release's command.h.
// Only the literal exit builtin in a standalone parenthesized command may use the copied top_level exit path.
static int procursusExitGroup(uintptr_t image, const ProcursusBashFrame *frame) {
    BashCommandNode groupNode, body;
    BashSubshellNode group;
    BashSimpleCommand simple;
    if (frame->x25 || (int32_t)frame->x21 != -1 || (int32_t)frame->x20 != -1 || !procursusNoPendingCloses(frame->x22) ||
        !procursusHeapObject(frame->x19, &groupNode, sizeof(groupNode)) || groupNode.type != 13 || (groupNode.flags & ~1) || groupNode.redirects ||
        !procursusHeapObject(groupNode.value, &group, sizeof(group)) || (group.flags & ~1) ||
        !procursusHeapObject(group.command, &body, sizeof(body)) || body.type != 4 || body.flags || body.redirects ||
        !procursusHeapObject(body.value, &simple, sizeof(simple)) || simple.flags || simple.redirects) return ENOTSUP;
    // A user function named exit could run another command. This baseline only accepts an empty function table (hashlib.h).
    uintptr_t functions;
    struct { uintptr_t buckets; int32_t bucketCount, entries; } table;
    if (!forkTrialRead(image + 0xa6c80, &functions, sizeof(functions)) ||
        (functions && (!procursusHeapObject(functions, &table, sizeof(table)) || table.entries))) return ENOTSUP;
    return procursusWords(simple.words, true);
}

static int procursusExternal(uintptr_t image, const ProcursusBashFrame *frame, uintptr_t callerSP) {
    // execute_disk_command (+0x16450) saves the WORD_LIST, fd_bitmap and redirects at caller SP+8,+16,+24.
    struct { uintptr_t words, bitmap, redirects; } args;
    uintptr_t env;
    if ((int32_t)frame->x23 != -1 || (int32_t)frame->x22 != -1 || frame->x26 || !procursusHeapString(frame->x20, LSB_PATH_BYTES) ||
        !forkTrialRead(callerSP + 8, &args, sizeof(args)) || args.redirects || !procursusNoPendingCloses(args.bitmap) ||
        !forkTrialRead(image + 0xa6c68, &env, sizeof(env))) return ENOTSUP;
    int error = procursusWords(args.words, false);
    if (error) return error;
    for (unsigned i = 0; i <= LSB_MAX_VECTOR; ++i) {
        uintptr_t entry;
        if (!procursusHeapObject(env, &entry, sizeof(entry))) return ENOTSUP;
        if (!entry) return 0;
        if (!procursusHeapString(entry, LSB_STRING_BYTES)) {
            forkTrialTrace("fork trial Procursus outside copied heap: export_env[%u]\n", i);
            return ENOTSUP;
        }
        env += sizeof(entry);
    }
    return E2BIG;
}

static int checkProcursusBashFork(TrialOffer *offer) {
    const uintptr_t image = offer->mainHeader;
    const LSBForkContext *caller = &offer->caller;
    if (!forkTrialProcursusBashImage(image) || caller->pc != image + ProcursusBashForkReturn || caller->sp % 16 ||
        caller->sp > UINTPTR_MAX - 2 * sizeof(ProcursusBashFrame) || caller->fp != caller->sp + offsetof(ProcursusBashFrame, fp)) return ENOTSUP;
    ProcursusBashFrame frame;
    if (!forkTrialRead(caller->sp, &frame, sizeof(frame))) return EFAULT;
    uintptr_t callerSP = caller->sp + sizeof(frame);
    bool group = frame.lr == image + ProcursusBashSubshellReturn;
    if ((!group && frame.lr != image + ProcursusBashLateChildReturn) || frame.fp != callerSP + (group ? 0xa0 : 0x70)) return ENOTSUP;
    // Published globals from nm; initialize_traps+0x2c selects the 35-entry sigmodes array.
    enum { Interactive = 0xa7c08, JobControl = 0xa43c4, BufferedInput = 0xa5d00, ReturnCatch = 0xa6c10, RunningTrap = 0xa64d4,
           SignalModes = 0xa62a0, ShellTTY = 0xa4364 };
    int32_t interactive, jobControl, input, returnCatch, runningTrap, modes[35];
    if (!forkTrialRead(image + Interactive, &interactive, sizeof(interactive)) || !forkTrialRead(image + JobControl, &jobControl, sizeof(jobControl)) ||
        !forkTrialRead(image + BufferedInput, &input, sizeof(input)) || !forkTrialRead(image + ReturnCatch, &returnCatch, sizeof(returnCatch)) ||
        !forkTrialRead(image + RunningTrap, &runningTrap, sizeof(runningTrap)) || !forkTrialRead(image + SignalModes, modes, sizeof(modes))) return EFAULT;
    if ((interactive != 0 && interactive != 1) || interactive != jobControl || input > 0 || returnCatch || runningTrap) {
        forkTrialTrace("fork trial Procursus unsupported shell state: interactive=%d job_control=%d input=%d return_catch=%d running_trap=%d\n",
                       interactive, jobControl, input, returnCatch, runningTrap);
        return ENOTSUP;
    }
    if (interactive) {
        // initialize_job_control+0xf4 stores move_to_high_fd's result in shell_tty. Recreate just its CLOEXEC alias.
        int32_t fd;
        if (!forkTrialRead(image + ShellTTY, &fd, sizeof(fd))) return EFAULT;
        int error = trialCaptureTerminal(fd, &offer->terminal);
        if (error) return error;
    }
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        if (!(modes[i] & 1)) continue;
        forkTrialTrace("fork trial Procursus inherited trap %u\n", i);
        return ENOTSUP;
    }
    int error = group ? procursusExitGroup(image, &frame) : procursusExternal(image, &frame, callerSP);
    forkTrialTrace("fork trial Procursus caller=+0x%lx command_errno=%d\n", (unsigned long)(frame.lr - image), error);
    return error;
}
