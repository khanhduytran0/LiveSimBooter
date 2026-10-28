#pragma once

#include <mach/mach.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/proc.h>
#include <sys/sysctl.h>

enum {
    LSB_PEER_DEBUGGER_VERSION = 1,
    LSB_PEER_DEBUGGER_REQUEST_ID = 0x4c534244,
    LSB_PEER_DEBUGGER_REPLY_ID = 0x4c534245,
    LSB_PEER_DEBUGGER_NONCE_BYTES = 16,
    LSB_PEER_DEBUGGER_MAX_TIMEOUT_MS = 500
};

// A private endpoint can query only the receiving process itself. No target PID or transferred capability is in the payload.
typedef struct {
    mach_msg_header_t header;
    uint32_t version, reserved;
    unsigned char nonce[LSB_PEER_DEBUGGER_NONCE_BYTES];
} LSBPeerDebuggerRequest;

typedef struct {
    mach_msg_header_t header;
    uint32_t version;
    int32_t error;
    unsigned char nonce[LSB_PEER_DEBUGGER_NONCE_BYTES];
    struct kinfo_proc info;
} LSBPeerDebuggerResponse;

_Static_assert(sizeof(struct kinfo_proc) == 648, "arm64 Darwin kinfo_proc ABI");
_Static_assert(sizeof(LSBPeerDebuggerRequest) == 48, "peer debugger request ABI");
_Static_assert(offsetof(LSBPeerDebuggerResponse, info) == 48 && sizeof(LSBPeerDebuggerResponse) == 696, "peer debugger reply ABI");

// Borrows endpoint and expectedPeer. Uses one deadline for send+receive; zeroes info on every failure and preserves errno.
// Only an exact authenticated reply from this native PID/generation, with the current request nonce, can return kernel data.
int LSBPeerDebuggerQuery(mach_port_t endpoint, const audit_token_t *expectedPeer, uint32_t timeoutMS, struct kinfo_proc *info);

// Consume one successfully received kernel message, including its reply/voucher/descriptor rights on all paths.
// capacity is the actual receive-buffer capacity; request reception must include a format-0 audit trailer.
// Called by the initialized guest server. The original sysctl in this image reads its native self, never a logical PID.
void LSBPeerDebuggerReply(mach_msg_header_t *received, size_t capacity, const audit_token_t *expectedHost);
