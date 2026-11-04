#include "ForkBashFixture.h"

static char *wordString(const char *text) {
    char *copy = bashEarlyMalloc(strlen(text) + 1);
    assert(copy);
    return strcpy(copy, text);
}

static BashWordList *wordList(const char *text, BashWordList *next) {
    BashWordDescription *desc = bashEarlyMalloc(sizeof(*desc));
    BashWordList *word = bashEarlyMalloc(sizeof(*word));
    assert(desc && word);
    *desc = (BashWordDescription){.string = (uintptr_t)wordString(text)};
    *word = (BashWordList){.next = (uintptr_t)next, .description = (uintptr_t)desc};
    return word;
}

int main(void) {
    uint8_t *image = bashFixture();
    const uintptr_t header = (uintptr_t)image;
    struct uuid_command *id = (void *)(image + sizeof(struct mach_header_64));
    const uint8_t uuid[16] = {5, 0x79, 0x98, 11, 0x28, 0x5b, 0x3e, 0x49, 0xa3, 0xb1, 0xff, 0x5e, 0xa2, 9, 0x9c, 0x97};
    memcpy(id->uuid, uuid, sizeof(uuid));
    *(uint32_t *)(image + ProcursusBashForkReturn - 4) = 0x940181d9;
    const uint32_t stub[] = {0x900000f0, 0xf9413a10, 0xd61f0200};
    memcpy(image + ProcursusBashGetpidStub, stub, sizeof(stub));
    bashEarlyHeap = calloc(1, BashEarlyHeapBytes);
    assert(bashEarlyHeap);
    bashHeapInit(bashEarlyHeap);

    _Alignas(16) struct { ProcursusBashFrame saved; uintptr_t callerSlots[0x20]; } stack = {};
    ProcursusBashFrame *frame = &stack.saved;
    TrialOffer offer = {.mainHeader = header, .caller = {.sp = (uintptr_t)frame, .fp = (uintptr_t)&frame->fp, .pc = header + ProcursusBashForkReturn}};
    frame->fp = (uintptr_t)stack.callerSlots + 0x70;
    frame->lr = header + ProcursusBashLateChildReturn;
    frame->x20 = (uintptr_t)wordString("/bin/df");
    frame->x22 = frame->x23 = UINT32_MAX;
    stack.callerSlots[1] = (uintptr_t)wordList("df", wordList("-P", wordList(".", NULL)));
    uintptr_t *env = bashEarlyMalloc(2 * sizeof(*env));
    env[0] = (uintptr_t)wordString("PATH=/bin");
    env[1] = 0;
    *(uintptr_t *)(image + 0xa6c68) = (uintptr_t)env;
    assert(checkProcursusBashFork(&offer) == 0);
    stack.callerSlots[3] = 1; // A redirect changes child-side FD and expansion semantics.
    assert(checkProcursusBashFork(&offer) == ENOTSUP);
    stack.callerSlots[3] = 0;
    *(int32_t *)(image + 0xa7c08) = 1;
    assert(checkProcursusBashFork(&offer) == ENOTSUP); // Interactive input without foreground job control.
    *(int32_t *)(image + 0xa43c4) = 1;
    int pair[2], savedInput = dup(STDIN_FILENO);
    assert(savedInput >= 0 && !socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    int high = fcntl(pair[1], F_DUPFD, 20);
    assert(high >= 20 && high < 256);
    *(int32_t *)(image + 0xa4364) = high;
    fixtureTerminal = pair[1];
    assert(dup2(fixtureTerminal, STDIN_FILENO) == STDIN_FILENO);
    assert(checkProcursusBashFork(&offer) == ENOTSUP); // A non-CLOEXEC descriptor is not the expected alias.
    assert(!fcntl(high, F_SETFD, FD_CLOEXEC));
    assert(!checkProcursusBashFork(&offer) && offer.terminal.source == STDIN_FILENO && offer.terminal.target == high);
    assert(dup2(savedInput, STDIN_FILENO) == STDIN_FILENO);
    assert(checkProcursusBashFork(&offer) == ENOTSUP); // No standard descriptor identifies this PTY.
    close(high);
    close(pair[0]);
    close(pair[1]);
    close(savedInput);
    fixtureTerminal = -1;
    *(int32_t *)(image + 0xa7c08) = *(int32_t *)(image + 0xa43c4) = 0;
    offer.terminal = (TrialTerminalAlias){};
    uintptr_t path = frame->x20;
    frame->x20 = (uintptr_t)"/bin/df"; // A string in the parent's unrelated mapping is not copied.
    assert(checkProcursusBashFork(&offer) == ENOTSUP);
    frame->x20 = path;
    *(uint32_t *)(image + 0xa62a0 + SIGCHLD * sizeof(uint32_t)) = 1;
    assert(checkProcursusBashFork(&offer) == ENOTSUP);
    *(uint32_t *)(image + 0xa62a0 + SIGCHLD * sizeof(uint32_t)) = 0;

    BashCommandNode *outer = bashEarlyMalloc(sizeof(*outer)), *body = bashEarlyMalloc(sizeof(*body));
    BashSubshellNode *group = bashEarlyMalloc(sizeof(*group));
    BashSimpleCommand *simple = bashEarlyMalloc(sizeof(*simple));
    BashWordList *status = wordList("37", NULL);
    *simple = (BashSimpleCommand){.words = (uintptr_t)wordList("exit", status)};
    *body = (BashCommandNode){.type = 4, .value = (uintptr_t)simple};
    *group = (BashSubshellNode){.flags = 1, .command = (uintptr_t)body};
    *outer = (BashCommandNode){.type = 13, .flags = 1, .value = (uintptr_t)group};
    frame->fp = (uintptr_t)stack.callerSlots + 0xa0;
    frame->lr = header + ProcursusBashSubshellReturn;
    frame->x19 = (uintptr_t)outer;
    frame->x20 = frame->x21 = UINT32_MAX;
    frame->x22 = 0;
    assert(checkProcursusBashFork(&offer) == 0);
    BashWordDescription *arg = (void *)status->description;
    arg->string = (uintptr_t)wordString("$(id)");
    assert(checkProcursusBashFork(&offer) == ENOTSUP);
    arg->string = (uintptr_t)wordString("0");
    frame->x20 = STDOUT_FILENO;
    assert(checkProcursusBashFork(&offer) == ENOTSUP);
    frame->x20 = UINT32_MAX;
    frame->lr = header + 0x109f0; // Same make_child prologue, but execute_simple_command's early pipeline site.
    assert(checkProcursusBashFork(&offer) == ENOTSUP);
    free(bashEarlyHeap);
    bashEarlyHeap = NULL;
    free(image);
    puts("Procursus Bash parent: external and exit-group stack layouts, copied words, traps, redirects and unsupported callers passed");
}
