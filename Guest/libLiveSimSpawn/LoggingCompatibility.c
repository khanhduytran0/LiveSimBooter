#include "LoggingCompatibility.h"
#include <limits.h>
#include <mach/mach.h>

#ifdef LSB_GUEST_RUNTIME
static bool exactKey(const char *key, const char *expected) {
    size_t index = 0;
    while (expected[index] && key[index] == expected[index]) ++index;
    return key[index] == expected[index];
}
#endif

xpc_object_t LSBLoggingCopyEntitlementForToken(const char *key, const audit_token_t *token) {
#ifdef LSB_GUEST_RUNTIME
    if (!token && key) {
        if (exactKey(key, LSB_LOGGING_DISABLE_MACH_PORTS_ENTITLEMENT)) {
            // Sim26Booter PATCHING.md (2025-09-11) forces this libtrace guard while diagnosticd is absent:
            // https://github.com/khanhduytran0/Sim26Booter/blob/main/PATCHING.md#2025-09-11
            // The matching libtrace calls xpc_copy_entitlement_for_token(key, NULL) at +0x7330 before caching the result.
            // This can precede guest TLS/constructors: no environment, errno, allocation, path, or audit inspection here.
            // The pinned libxpc Boolean factory is a leaf returning its immutable singleton with copy ownership.
            return xpc_bool_create(true);
        }
        if (exactKey(key, LSB_NOTIFY_RESTRICTION_ENTITLEMENT)) {
            // 23A343 libnotify +0x34fc caches this self query before deciding whether notify_post is process-local.
            // Its libxpc NULL-token path uses csops with native getpid, bypassing our guest entitlement snapshot.
            // Supply the genuine kernel self token so stock libxpc uses the authenticated csops_audittoken path instead.
            // Do this on the first query, not in a constructor: libnotify caches the result once. Unlike libtrace's guard
            // above, this existing libxpc query already requires initialized libSystem allocation and IPC facilities.
            audit_token_t self = {{0}};
            mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
            if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&self, &count) != KERN_SUCCESS ||
                count != TASK_AUDIT_TOKEN_COUNT || !self.val[5] || self.val[5] > INT_MAX) return NULL;
            return xpc_copy_entitlement_for_token(key, &self);
        }
    }
#endif
    __attribute__((musttail)) return xpc_copy_entitlement_for_token(key, token);
}

#if defined(LSB_GUEST_RUNTIME) && !defined(LSB_LOGGING_TESTING)
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interposition = {
    (const void *)&LSBLoggingCopyEntitlementForToken, (const void *)&xpc_copy_entitlement_for_token
};
#endif
