#include "LaunchdIdentity.h"
#include "../../ProcessSupport/SpawnTransport.h"

#include <errno.h>
#include <limits.h>

__attribute__((visibility("default"), noinline)) pid_t LSBLaunchdResolveAuditPID(const audit_token_t *audit) {
    int savedErrno = errno;
    pid_t result = -1;
    // A missing broker or stale execution identity must never silently select the original proxy's job.
    if (audit && audit->val[5] && audit->val[5] <= INT_MAX) {
        pid_t resolved = -1;
        if (!LSBTransportResolveAuditPID(audit, &resolved) && resolved > 0) result = resolved;
    }
    errno = savedErrno;
    return result;
}
