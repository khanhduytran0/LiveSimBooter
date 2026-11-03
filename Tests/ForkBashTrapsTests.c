#include "ForkBashTraps.h"
#include <assert.h>
#include <signal.h>
#include <stdlib.h>

int main(void) {
    uintptr_t header = (uintptr_t)calloc(1, 0xac000);
    assert(header);
    uint32_t stub[] = {0xd503201f, 0x580965b0, 0xd61f0200};
    memcpy((void *)(header + BashFreeStub), stub, sizeof(stub));
    uint32_t *call = (void *)(header + BashRestoreTrapFreeReturn - 4);
    *call = 0x940144e6;
    uint32_t *modes = (void *)(header + BashSignalModes);
    uintptr_t *traps = (void *)(header + BashTrapList), *originals = (void *)(header + BashOriginalSignals);
    BashTrapReleases refs;
    assert(!checkBashTraps(header, false, &refs) && !refs.count);

    modes[SIGUSR1] = 1;
    traps[SIGUSR1] = 1; // SIG_IGN is an immediate; the child has no release to skip.
    assert(!checkBashTraps(header, true, &refs) && !refs.count);
    assert(checkBashTraps(header, false, &refs) == ENOTSUP);
    modes[SIGCHLD] = 1;
    assert(checkBashTraps(header, true, &refs) == ENOTSUP);
    modes[SIGCHLD] = 0;
    modes[SIGUSR2] = 1 | 4 | 8;
    traps[SIGUSR2] = 0x4157; // Not mapped. The copied child must never dereference or actually free these.
    traps[SIGUSR1] = 0x5123;
    originals[SIGUSR2] = 1;
    assert(!checkBashTraps(header, true, &refs) && refs.count == 2);
    assert(!consumeBashTrapRelease(&refs, refs.freeCaller + 4, traps[SIGUSR2]));
    assert(!consumeBashTrapRelease(&refs, refs.freeCaller, traps[SIGUSR1] + 1));
    assert(consumeBashTrapRelease(&refs, refs.freeCaller, traps[SIGUSR1]));
    assert(!consumeBashTrapRelease(&refs, refs.freeCaller, traps[SIGUSR1]));
    assert(consumeBashTrapRelease(&refs, refs.freeCaller, traps[SIGUSR2]) && refs.count == 0);
    *call ^= 1;
    assert(checkBashTraps(header, true, &refs) == ENOEXEC);
    *call ^= 1;

    originals[SIGUSR2] = header + 0x2901c; // Even a pointer into signed Bash code is not a portable disposition.
    assert(checkBashTraps(header, true, &refs) == ENOTSUP);
    originals[SIGUSR2] = 0;
    modes[SIGUSR2] |= 16; // Fork inside an active trap is outside this path.
    assert(checkBashTraps(header, true, &refs) == ENOTSUP);
    modes[SIGUSR2] = 4;
    originals[SIGUSR2] = UINTPTR_MAX;
    assert(checkBashTraps(header, true, &refs) == ENOTSUP);
    originals[SIGUSR2] = 0;
    traps[SIGUSR1] = header + BashInitializeTraps;
    assert(!checkBashTraps(header, true, &refs) && !refs.count);
    free((void *)header);
    puts("Bash trap dispositions and exact inherited string releases passed");
    return 0;
}
