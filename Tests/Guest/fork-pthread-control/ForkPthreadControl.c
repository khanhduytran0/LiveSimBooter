// Run export and import in separate disposable, single-threaded guest jobs with the same signed executable at the same VA.
#include "../../../Guest/Shared/Fork/ForkPthreadRegistry.h"
#include <fcntl.h>
#include <mach-o/ldsyms.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
static volatile sig_atomic_t prepares, parents, children;
static void prepare(void) { ++prepares; }
static void parent(void) { ++parents; }
static void child(void) { ++children; }
static int inFixture(void *context, uint64_t address, uint64_t size) {
    (void)context;
    if (!size || size - 1 > UINT64_MAX - address) return EINVAL;
    Dl_info first, last;
    return dladdr((void *)address, &first) && dladdr((void *)(address + size - 1), &last) &&
           first.dli_fbase == (const void *)&_mh_execute_header && first.dli_fbase == last.dli_fbase ? 0 : EACCES;
}

static void dump(const char *label, const ForkPthreadState *state) {
    printf("%s count=%u\n", label, state->count);
    for (unsigned i = 0; i < state->count; ++i) for (unsigned k = 0; k < 3; ++k) {
        const ForkPthreadTarget *fn = &state->entries[i].functions[k];
        if (fn->pc) {
            uint64_t identity[2];
            memcpy(identity, fn->uuid, sizeof(identity));
            printf("  %u,%u pc=%llx image=%llx identity=%llx:%llx\n", i, k, fn->pc, fn->image, identity[0], identity[1]);
        }
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 3 || (strcmp(argv[1], "export") && strcmp(argv[1], "import"))) return 64;
    uintptr_t pc = trialControlForkReturnPC();
    TrialLibcForkHandlers handlers;
    int error = forkPthreadSingleMain();
    if (!error) error = trialResolveLibcForkHandlers(pc, false, &handlers);
    if (error) { printf("preflight errno=%d\n", error); return error; }
    ForkPthreadState state;
    if (!strcmp(argv[1], "export")) {
        error = pthread_atfork(prepare, parent, child);
        if (!error) error = forkPthreadCapture(&state);
        if (error) return error;
        dump("parent", &state);
        handlers.prepare(0);
        error = forkPthreadCheckPrepared(&state, false);
        handlers.parent(0);
        printf("parent checkpoint=%d prepare=%d parent=%d child=%d\n", error, prepares, parents, children);
        if (error || prepares != 1 || parents != 1 || children) return error ? error : 65;
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) return errno;
        if (write(fd, &state, sizeof(state)) != sizeof(state)) return EIO;
        return close(fd) ? errno : 0;
    }
    int fd = open(argv[2], O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return errno;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_size != sizeof(state) || read(fd, &state, sizeof(state)) != sizeof(state))
        return EPROTO;
    close(fd);
    ForkPthreadState local;
    if ((error = forkPthreadCapture(&local))) return error;
    dump("observer", &local);
    if (state.count != local.count + 1 || local.count >= ForkPthreadMaxRegistrations) return EPROTO;
    const ForkPthreadTarget *expected = state.entries[local.count].functions;
    if (expected[0].pc != (uintptr_t)prepare || expected[1].pc != (uintptr_t)parent || expected[2].pc != (uintptr_t)child) return EACCES;
    ForkPthreadStage stage;
    state.entries[local.count].functions[2].uuid[0] ^= 1;
    int invalidImage = forkPthreadStage(&state, inFixture, NULL, &stage, NULL);
    state.entries[local.count].functions[2].uuid[0] ^= 1;
    state.entries[local.count].functions[2].image |= 1;
    int foreignCallback = forkPthreadStage(&state, inFixture, NULL, &stage, NULL);
    state.entries[local.count].functions[2].image &= ~UINT64_C(1);
    printf("mismatched UUID errno=%d, cached suffix errno=%d\n", invalidImage, foreignCallback);
    if (invalidImage != ENOEXEC || foreignCallback != ENOTSUP) return EPROTO;
    error = forkPthreadStage(&state, inFixture, NULL, &stage, NULL);
    printf("stage errno=%d prefix=%u count=%u\n", error, stage.prefix, stage.count);
    if (error) return error;
    handlers.prepare(0);
    error = forkPthreadFinish(&stage);
    if (error || prepares) { handlers.parent(0); _exit(error ? error : 66); }
    if (trialRunLibcChildBranch(pc)) _exit(67);
    error = forkPthreadCapture(&local);
    printf("after child capture=%d prepare=%d parent=%d child=%d registered=%u\n", error, prepares, parents, children, local.count);
    _exit(!error && !prepares && !parents && children == 1 &&
          local.count == state.count && !memcmp(local.entries, state.entries, state.count * sizeof(local.entries[0])) ? 0 : 68);
}
