#include "ForkBashFixture.h"

int main(void) {
    uint8_t *image = bashFixture();
    size_t page = vm_page_size;
    struct {
        BashMakeChildFrame frame;
        BashCommandNode outer, body, leaf, nested;
        BashSubshellNode group, subgroup;
        BashConnectionNode connection;
        BashSimpleCommand simple;
        BashWordList words;
        BashWordDescription desc;
        char literal[8];
    } *data = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(data != MAP_FAILED && sizeof(*data) < page);
    TrialOffer offer = {.version = TrialWireVersion, .resume = TrialShell, .mainHeader = (uintptr_t)image};
    LSBForkContext *caller = &offer.caller;
    caller->pc = offer.mainHeader + 0x2570c;
    caller->sp = (uintptr_t)data;
    caller->fp = caller->sp + offsetof(BashMakeChildFrame, fp);
    caller->x19ToX28[6] = UINT32_MAX;
    data->frame = (BashMakeChildFrame){.lr = offer.mainHeader + BashSubshellReturn, .x23 = UINT32_MAX, .x19 = (uintptr_t)&data->outer};
    data->outer = (BashCommandNode){.type = BashSubshellType, .value = (uintptr_t)&data->group};
    data->group.command = (uintptr_t)&data->body;
    data->body = (BashCommandNode){.type = BashConnectionType, .value = (uintptr_t)&data->connection};
    data->connection = (BashConnectionNode){.first = (uintptr_t)&data->leaf, .second = (uintptr_t)&data->leaf, .connector = ';'};
    data->leaf = (BashCommandNode){.type = BashSimpleType, .value = (uintptr_t)&data->simple};
    data->simple.words = (uintptr_t)&data->words;
    data->words.description = (uintptr_t)&data->desc;
    data->desc.string = (uintptr_t)data->literal;
    strcpy(data->literal, "exit");

    TrialPlan plan = {};
    assert(!addSpan(&plan, "fixture-stack", (uintptr_t)data, (uintptr_t)data + page));
    assert(!addBashShellCommand(&plan, &offer, page));
    data->nested = (BashCommandNode){.type = BashSubshellType, .value = (uintptr_t)&data->subgroup};
    data->subgroup.command = (uintptr_t)&data->leaf;
    data->connection.first = (uintptr_t)&data->nested;
    assert(!addBashShellCommand(&plan, &offer, page)); // ((simple); simple)
    data->subgroup.command = (uintptr_t)&data->nested;
    assert(addBashShellCommand(&plan, &offer, page) == E2BIG);
    data->subgroup.command = (uintptr_t)&data->leaf;
    data->nested.flags = 0x80;
    assert(addBashShellCommand(&plan, &offer, page) == ENOTSUP);
    data->nested.flags = 0;
    data->connection.connector = BashAndAnd;
    assert(!addBashShellCommand(&plan, &offer, page));
    data->connection.connector = BashOrOr;
    assert(!addBashShellCommand(&plan, &offer, page));
    data->connection.connector = '|';
    assert(addBashShellCommand(&plan, &offer, page) == ENOTSUP);
    data->connection.connector = ';';
    data->connection.first = data->group.command;
    assert(addBashShellCommand(&plan, &offer, page) == E2BIG);
    data->group.command = (uintptr_t)&data->leaf;
    assert(!addBashShellCommand(&plan, &offer, page));
    *(uint32_t *)(image + BashSignalModes) = 1;
    assert(addBashShellCommand(&plan, &offer, page) == ENOTSUP);
    *(uint32_t *)(image + BashSignalModes) = 0;
    data->outer.type = BashConnectionType;
    assert(addBashShellCommand(&plan, &offer, page) == ENOTSUP);
    assert(!munmap(data, page));
    free(image);
    puts("Bash subshell and bounded grouped command inventory passed");
    return 0;
}
