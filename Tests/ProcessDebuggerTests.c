#include <assert.h>
#include <stdio.h>
#include <mach/mach.h>
static void testDestroy(mach_msg_header_t *);
#define LSB_PROCESS_DEBUGGER_TESTING
#define mach_msg_destroy testDestroy
#include "../Guest/ProcessDebugger.c"
#undef mach_msg_destroy

static unsigned queries, replies, destroyed;
static int queryError;
static void testDestroy(mach_msg_header_t *message) { assert(message); ++destroyed; }
int LSBTransportCopyDebuggerInfo(pid_t pid, struct kinfo_proc *info) {
    assert(pid == 42);
    ++queries;
    memset(info, 0, sizeof(*info));
    if (!queryError) { info->kp_proc.p_pid = pid; info->kp_proc.p_flag = P_TRACED; }
    return queryError;
}
void LSBPeerDebuggerReply(mach_msg_header_t *message, size_t capacity, const audit_token_t *expected) {
    assert(message && capacity == 4096 && expected == &processDebuggerBroker && expected->val[5] == 123 && expected->val[7] == 456);
    ++replies;
}

int main(void) {
    const char *root = "/private/group/LSB/iOS/23A343/RuntimeRoot";
    const char *path = "/private/group/LSB/iOS/23A343/RuntimeRoot/System/Library/PrivateFrameworks/DragUI.framework/Support/druid";
    assert(isGuestDruid(root, path));
    assert(!isGuestDruid(NULL, path) && !isGuestDruid(root, NULL) && !isGuestDruid("/", path));
    assert(!isGuestDruid(root, "/private/group/LSB/iOS/23A343/RuntimeRoot2/System/Library/PrivateFrameworks/DragUI.framework/Support/druid"));
    assert(!isGuestDruid(root, "/private/group/LSB/iOS/23A343/RuntimeRoot/Plugins/druid"));
    struct kinfo_proc info;
    int error = -1;
    assert(!LSBGuestDebuggerCopyInfo(42, &info, &error) && !queries && error == -1); // No TLS/transport before constructor publication.
    atomic_store(&druidDebuggerQueries, true);
    queryError = ETIMEDOUT;
    assert(LSBGuestDebuggerCopyInfo(42, &info, &error) && error == ETIMEDOUT && !info.kp_proc.p_pid && queries == 1);
    queryError = 0;
    assert(LSBGuestDebuggerCopyInfo(42, &info, &error) && !error && info.kp_proc.p_flag == P_TRACED && queries == 2);
    mach_msg_header_t message = {.msgh_local_port = 1234};
    LSBProcessDebuggerHandleMessage(&message, 4096);
    assert(destroyed == 1 && !replies);
    processDebuggerBroker.val[5] = 123; processDebuggerBroker.val[7] = 456;
    processDebuggerPort = 5678;
    atomic_store(&brokerIdentityReady, true);
    LSBProcessDebuggerHandleMessage(&message, 4096);
    assert(destroyed == 2 && !replies);
    message.msgh_local_port = processDebuggerPort;
    LSBProcessDebuggerHandleMessage(&message, 4096);
    assert(destroyed == 2 && replies == 1);
    puts("ProcessDebugger: exact druid scope, preconstructor guard, real errors and private endpoint dispatch passed");
    return 0;
}
