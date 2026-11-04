#include "../Guest/libLiveSimSpawn/LaunchdIdentity.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static audit_token_t received;
static int calls, replyError;
static pid_t replyPID;

int LSBTransportResolveAuditPID(const audit_token_t *audit, pid_t *pid) {
    ++calls;
    received = *audit;
    *pid = replyPID;
    errno = EIO;
    return replyError;
}

int main(void) {
    audit_token_t audit = {{17, 501, 20, 501, 20, 712, 19, 28}}, original = audit;
    replyPID = 700;
    errno = EBUSY;
    assert(LSBLaunchdResolveAuditPID(&audit) == 700 && errno == EBUSY && calls == 1);
    assert(!memcmp(&audit, &original, sizeof(audit)) && !memcmp(&received, &original, sizeof(audit)));
    replyError = ESRCH;
    assert(LSBLaunchdResolveAuditPID(&audit) == -1 && errno == EBUSY);
    replyError = 0;
    replyPID = 0;
    assert(LSBLaunchdResolveAuditPID(&audit) == -1);
    int previous = calls;
    assert(LSBLaunchdResolveAuditPID(NULL) == -1);
    audit.val[5] = 0;
    assert(LSBLaunchdResolveAuditPID(&audit) == -1);
    audit.val[5] = UINT32_MAX;
    assert(LSBLaunchdResolveAuditPID(&audit) == -1 && calls == previous);
    puts("LaunchdIdentity tests passed");
    return 0;
}
