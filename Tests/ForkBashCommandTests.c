#include "ForkBashFixture.h"

// An env string below the new stack boundary used to be covered implicitly. Rebuilding must find its page without
// copying an unrelated page between it and the guard. A second string at the guard address makes resumption unsupported.
static void testStackReinventory(uintptr_t image, LSBForkContext caller, uintptr_t *env) {
    size_t page = vm_page_size, size = TrialStackHeadroom + 9 * page;
    uint8_t *base = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(base != MAP_FAILED && !mprotect(base + page, size - 2 * page, PROT_READ | PROT_WRITE));
    uint8_t *sp = base + TrialStackHeadroom + 5 * page;
    memcpy(sp, (const void *)caller.sp, 0x120);
    caller.sp = (uintptr_t)sp;
    caller.fp = caller.sp + 0x50;
    uintptr_t original = env[0], low = (uintptr_t)(base + 2 * page), guard = (uintptr_t)(base + 4 * page);
    strcpy((char *)low, "LOW=preserved");
    env[0] = low;
    TrialPlan plan = {};
    TrialOffer offer = {.version = TrialWireVersion, .mainHeader = image, .caller = caller, .resume = TrialExec};
    assert(!buildOffer(&plan, &offer, 0, 0) && contains(&plan, low) && contains(&plan, low + page) && contains(&plan, guard));
    assert(!buildOffer(&plan, &offer, 0, TrialSlicedStack) && offer.fullStackStart == (uintptr_t)(base + page));
    assert(contains(&plan, low) && !contains(&plan, low + page) && !contains(&plan, guard) && contains(&plan, caller.sp));
    strcpy((char *)guard, "GUARD=referenced");
    env[0] = guard;
    assert(buildOffer(&plan, &offer, 0, TrialSlicedStack) == ENOTSUP);
    env[0] = original;
    assert(!munmap(base, size));
}

int main(void) {
    assert(!setenv("LSB_FORK_SNAPSHOT_PROBE", "exec", 1));
    assert(!LSBForkTrialEnabled()); // A different guest executable can inherit the same environment.
    assert(!unsetenv("LSB_FORK_SNAPSHOT_PROBE"));
    uint8_t *image = bashFixture();
    uintptr_t header = (uintptr_t)image;
    size_t page = getpagesize();
    uint8_t *data = mmap(NULL, 9 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(data != MAP_FAILED);
    uintptr_t sp = (uintptr_t)data;
    LSBForkContext context = {.sp = sp, .fp = sp + 0x50, .pc = header + 0x2570c};
    context.x19ToX28[6] = UINT32_MAX;
    BashMakeChildFrame *frame = (void *)data;
    frame->lr = header + 0x18374;
    char *path = (char *)data + 2 * page - 3, *arg = (char *)data + 4 * page - 1, *value = (char *)data + 6 * page - 3;
    assert(!setenv("DYLD_ROOT_PATH", "/fixture/RuntimeRoot", 1));
    strcpy(path, "/fixture/RuntimeRoot/bin/df");
    strcpy(arg, "-P");
    strcpy(value, "TEST=a b");
    frame->x21 = (uintptr_t)path;
    BashWordList *list = (void *)(data + 3 * page);
    BashWordDescription *words = (void *)(list + 2);
    words[0].string = (uintptr_t)path;
    words[1].string = (uintptr_t)arg;
    list[0] = (BashWordList){(uintptr_t)(list + 1), (uintptr_t)words};
    list[1].description = (uintptr_t)(words + 1);
    *(uintptr_t *)(data + 0x60 + 0x80) = (uintptr_t)list;
    *(int32_t *)(data + 0x60 + 0xa4) = -1;
    uintptr_t *env = (void *)(data + 5 * page);
    env[0] = (uintptr_t)value;
    *(uintptr_t *)(image + 0xa6838) = (uintptr_t)env;
    BashFDBitmap *fds = (void *)(words + 2);
    frame->x24 = (uintptr_t)fds;
    *fds = (BashFDBitmap){32, (uintptr_t)(data + 7 * page)};

    TrialPlan plan = {};
    assert(!addSpan(&plan, "fixture-stack", sp, sp + page));
    assert(!addBashCommandPages(&plan, header, &context, page, TrialDF));
    assert(plan.count == 8 && plan.bytes == 8 * page);
    for (size_t i = 0; i < 8; ++i) assert(contains(&plan, sp + i * page));
    assert(!contains(&plan, sp + 8 * page));
    // Check rejection before a copied caller could take an unsupported branch or access an uncovered page.
    frame->x22 = 1;
    assert(addBashCommandPages(&plan, header, &context, page, TrialDF) == ENOTSUP);
    frame->x22 = 0;
    path[0] = '?';
    assert(addBashCommandPages(&plan, header, &context, page, TrialDF) == ENOTSUP);
    assert(!addBashCommandPages(&plan, header, &context, page, TrialExec)); // A relative target is resolved by guest execve.
    path[0] = '/';
    uintptr_t oldPath = frame->x21;
    frame->x21 = 0;
    assert(addBashCommandPages(&plan, header, &context, page, TrialExec) == ENOTSUP);
    char *longPath = (char *)data + 8 * page;
    memset(longPath, 'a', LSB_PATH_BYTES);
    frame->x21 = (uintptr_t)longPath;
    assert(addBashCommandPages(&plan, header, &context, page, TrialExec) == ENAMETOOLONG);
    frame->x21 = oldPath;
    data[7 * page] = 1;
    assert(addBashCommandPages(&plan, header, &context, page, TrialDF) == ENOTSUP);
    data[7 * page] = 0;
    list[1].next = (uintptr_t)list;
    assert(addBashCommandPages(&plan, header, &context, page, TrialDF) == E2BIG);
    list[1].next = 0;
    env[0] = 1;
    assert(addBashCommandPages(&plan, header, &context, page, TrialDF) == EFAULT);
    env[0] = (uintptr_t)value;
    testStackReinventory(header, context, env);
    assert(!vm_inherit(mach_task_self(), sp + 6 * page, page, VM_INHERIT_NONE));
    assert(addBashCommandPages(&plan, header, &context, page, TrialDF) == ENOTSUP);
    assert(!munmap(data, 9 * page));
    free(image);
    puts("Bash command page inventory passed");
    return 0;
}
