#ifndef LSB_CODE_SIGNING_COMPATIBILITY_H
#define LSB_CODE_SIGNING_COMPATIBILITY_H

#include <mach/message.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define LSB_CODE_SIGNING_ENABLE_ENV "LSB_SIMULATE_CODE_SIGNING"
#define LSB_CODE_SIGNING_STATUS (UINT32_C(0x20000000) | UINT32_C(0x00000001)) // CS_SIGNED | CS_VALID

// Operation numbers from XNU bsd/sys/codesign.h; the public simulator SDK omits this header.
enum {
    LSBCSOpStatus = 0,
    LSBCSOpCDHash = 5,
    LSBCSOpEntitlementsBlob = 7,
    LSBCSOpIdentity = 11,
    LSBCSOpTeamID = 14,
    LSBCSOpDEREntitlementsBlob = 16,
    LSBCSOpValidationCategory = 17
};

#ifdef __cplusplus
extern "C" {
#endif

int csops(pid_t pid, unsigned int operation, void *buffer, size_t size);
int csops_audittoken(pid_t pid, unsigned int operation, void *buffer, size_t size, audit_token_t *token);

// Guest-only opt-in: XML/DER reads always request the exact managed peer's immutable
// original DER snapshot (or XML derived from it) from the authenticated native broker. Positive PIDs must match the audit token;
// pid zero and NULL-token queries are accepted only for the actual kernel self identity. Retired or unmanaged peers fail closed.
// Other listed reads try the exact original request, then adapt only EPERM. STATUS reports signed/valid after a successful kernel
// self query. CDHash uses the signed guest image's actual CodeDirectory hash, preventing cross-guest signature-cache collisions.
// Identity/team/category reads use the current extension's real signature. This changes only guest reports, never native kernel policy.
// No PID or audit token is rewritten, no mutation operation is retried, and csops itself is not interposed.
int LSBCSopsAuditToken(pid_t pid, unsigned int operation, void *buffer, size_t size, audit_token_t *token);

#ifdef __cplusplus
}
#endif
#endif
