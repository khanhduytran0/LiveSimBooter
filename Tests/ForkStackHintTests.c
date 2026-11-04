#include "../ProcessSupport/ForkStackHint.h"
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    int control = fcntl(pair[1], F_DUPFD_CLOEXEC, LSB_MAX_FDS + 4);
    assert(control >= LSB_MAX_FDS + 4);
    close(pair[1]);
    LSBSpawnPlan plan = {.argc = 3, .actionCount = 4, .fdCount = 1};
    plan.fds[0].number = control;
    snprintf(plan.strings, sizeof(plan.strings), "%d", control);
    uint32_t offset = (uint32_t)strlen(plan.strings) + 1;
    strcpy(plan.strings + offset, "/dev/null");
    plan.actions[0] = (LSBFileAction){.kind = LSB_ACTION_INHERIT, .fd = control};
    for (int i = 0; i < 3; ++i) plan.actions[i + 1] = (LSBFileAction){.kind = LSB_ACTION_OPEN, .fd = i, .path = offset};
    uint64_t lo, hi, here = (uintptr_t)&plan & ~(vm_page_size - 1);
    char overlap[LSBForkStackHintLength + 1];
    snprintf(overlap, sizeof(overlap), "%016llx:%016llx", here, here + vm_page_size);
    assert(LSBForkStackHintValid(overlap, &lo, &hi) && lo == here && hi == here + vm_page_size);
    assert(!LSBForkStackHintValid(NULL, &lo, &hi) && !LSBForkStackHintValid("12:34", &lo, &hi));
    assert(!LSBForkStackHintValid("0000000000004001:0000000000008000", &lo, &hi));
    assert(!LSBForkStackHintValid("0000000000008000:0000000000004000", &lo, &hi));
    assert(!LSBForkStackHintValid("0000000000-04000:0000000000008000", &lo, &hi));
    char limit[LSBForkStackHintLength + 1];
    snprintf(limit, sizeof(limit), "%016llx:%016llx", here, here + LSB_MAX_FORK_BYTES);
    assert(LSBForkStackHintValid(limit, &lo, &hi));
    snprintf(limit, sizeof(limit), "%016llx:%016llx", here, here + LSB_MAX_FORK_BYTES + vm_page_size);
    assert(!LSBForkStackHintValid(limit, &lo, &hi));
    assert(!LSBForkAcknowledgedStackCollision(EADDRINUSE, pair[0]));
    assert(LSBForkNotifyStackCollision(&plan, overlap));
    assert(!LSBForkAcknowledgedStackCollision(ENOENT, pair[0])); // This error must not consume any receipt.
    assert(LSBForkAcknowledgedStackCollision(EADDRINUSE, pair[0]) && !LSBForkAcknowledgedStackCollision(EADDRINUSE, pair[0]));
    assert(send(control, "x", 1, 0) == 1 && !LSBForkAcknowledgedStackCollision(EADDRINUSE, pair[0]));
    assert(!LSBForkNotifyStackCollision(&plan, "0000008000000000:0000008000004000")); // Neither the native main stack nor below its floor.
    ++plan.actions[0].fd;
    assert(!LSBForkNotifyStackCollision(&plan, overlap));
    --plan.actions[0].fd;
    close(pair[0]);
    assert(!LSBForkNotifyStackCollision(&plan, overlap)); // Delivery failure cannot trigger early rejection.
    close(control);
    puts("Early stack collision hint validated the active native stack and accepted only a receipt on the matching failed spawn");
}
