#include "../Guest/libLiveSimSpawn/ForkTrial.c"
#include "../Guest/lsb-fork-snapshot-child/ForkBashImports.h"
#include <assert.h>
#include <sys/mman.h>
#include <sys/wait.h>

static void testRedirectPages(void) {
    size_t page = getpagesize();
    uintptr_t header = (uintptr_t)calloc(1, 0xa7000);
    uint8_t *storage = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(header && storage != MAP_FAILED);
    BashRedirect *first = (void *)storage, *second = (void *)(storage + page);
    *first = (BashRedirect){.next = (uintptr_t)second, .fd = 2, .instruction = 7, .target.fd = 1};
    *second = (BashRedirect){.fd = 1, .instruction = 7, .target.fd = 2};
    TrialPlan plan = {};
    assert(!addBashRedirectPages(&plan, header, (uintptr_t)first, page));
    assert(plan.bytes == 2 * page && contains(&plan, (uintptr_t)first) && contains(&plan, (uintptr_t)second));
    second->next = (uintptr_t)first;
    assert(addBashRedirectPages(&plan, header, (uintptr_t)first, page) == E2BIG);
    second->next = 0;
    second->target.fd = 3;
    assert(addBashRedirectPages(&plan, header, (uintptr_t)first, page) == ENOTSUP);
    second->target.fd = 2;
    first->instruction = 0; // >file uses a different union member and redirection_expand.
    assert(addBashRedirectPages(&plan, header, (uintptr_t)first, page) == ENOTSUP);
    first->instruction = 7;
    int flags = fcntl(STDERR_FILENO, F_GETFD);
    assert(flags >= 0 && !fcntl(STDERR_FILENO, F_SETFD, flags | FD_CLOEXEC));
    assert(addBashRedirectPages(&plan, header, (uintptr_t)first, page) == ENOTSUP);
    assert(!fcntl(STDERR_FILENO, F_SETFD, flags));
    *(int32_t *)(header + 0xa6c78) = 4;
    *(int32_t *)(header + 0xa6c88) = 1;
    assert(addBashRedirectPages(&plan, header, (uintptr_t)first, page) == ENOTSUP);
    assert(!munmap(storage, 2 * page));
    free((void *)header);
}

static void testChildDup(bool fail) {
    int fds[2];
    assert(!pipe(fds));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(fds[0]);
        int capture = fail ? STDERR_FILENO : STDOUT_FILENO;
        assert(dup2(fds[1], capture) == capture);
        close(fds[1]);
        if (fail) close(STDOUT_FILENO);
        assert(forkTrialBashDup2(STDOUT_FILENO, STDERR_FILENO) == STDERR_FILENO);
        assert(write(STDERR_FILENO, "a", 1) == 1);
        assert(forkTrialBashDup2(STDERR_FILENO, STDOUT_FILENO) == STDOUT_FILENO);
        assert(write(STDOUT_FILENO, "b", 1) == 1);
        _exit(0);
    }
    close(fds[1]);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == (fail ? 1 : 0));
    char text[128] = {};
    assert(read(fds[0], text, sizeof(text) - 1) > 0);
    if (fail) assert(strstr(text, strerror(EBADF)));
    else assert(!strcmp(text, "ab"));
    close(fds[0]);
}

int main(void) {
    testRedirectPages();
    testChildDup(false);
    testChildDup(true);
    puts("Bash numeric output redirections passed");
    return 0;
}
