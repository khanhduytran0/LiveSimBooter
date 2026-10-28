#ifndef LSB_SPAWN_TRANSPORT_H
#define LSB_SPAWN_TRANSPORT_H

#include "SpawnPlan.h"
#include "SpawnPorts.h"
#include "PeerProcessIdentity.h"
#include "GuestCodeSignature.h"
#include "NativeRuntimePolicy.h"
#include "ProcessInspection.h"
#include "TaskPortCapture.h"
#include <mach/mach.h>
#include <sys/sysctl.h>

#define LSB_BROKER_PORT_ENV "LSB_BROKER_PORT"
#define LSB_TRANSPORT_VERSION 5

// Bootstrap-only metadata is versioned independently from the outer transport envelope.
#define LSB_BOOTSTRAP_VERSION 3
enum LSBExecutionRole { LSB_EXECUTION_SIMULATOR = 0, LSB_EXECUTION_NATIVE_METAL_PROBE = 1, LSB_EXECUTION_NATIVE_METAL_SERVICE = 2 };

enum LSBRequestKind {
    LSB_REQUEST_SPAWN = 1, LSB_REQUEST_CHECKIN = 2, LSB_REQUEST_READY = 3, LSB_REQUEST_KILL = 4, LSB_REQUEST_EXEC = 5,
    LSB_REQUEST_RESOLVE_AUDIT_PID = 6, LSB_REQUEST_COPY_ENTITLEMENTS = 7, LSB_REQUEST_COPY_PROCESS_IDENTITY = 8,
    LSB_REQUEST_RESOLVE_NATIVE_CHILD_PID = 9, LSB_REQUEST_REBOOT_USERSPACE = 10, LSB_REQUEST_LIST_PROCESSES = 11,
    LSB_REQUEST_REGISTER_DEBUGGER_PORT = 12, LSB_REQUEST_COPY_DEBUGGER_INFO = 13, LSB_REQUEST_COPY_PROCESS_TASK = 14
};
typedef struct {
    uint64_t requestID;
    uint32_t executionRole;
    uint32_t nativeProductionStatus; // Authenticated native host classification, never supplied through a guest spawn plan.
    int32_t error, pid, guestPID, parentPID;
    int statusFD, wakeFD;
    mach_port_t launchdTankPort;
    LSBTaskPortCaptureOffer taskCapture; // Optional simulator-only offer; owns one send reference after successful CheckIn.
    LSBSpawnPorts ports;
    char runtimeRoot[LSB_PATH_BYTES];
    LSBSpawnPlan plan;
} LSBBootstrapReply;

// Borrowed reply metadata. Native PID remains separate from stable guest identity during an exec replacement.
typedef struct {
    pid_t guestPID, parentPID;
    mach_port_t launchdTankPort;
    const LSBSpawnPorts *ports;
    uint32_t executionRole; // Chosen by the native broker, never supplied by a guest spawn plan.
    uint32_t nativeProductionStatus;
    LSBTaskPortCaptureOffer taskCapture; // Borrowed endpoint and nonce supplied only by the native broker.
} LSBBootstrapMetadata;

typedef struct {
    void *object;
    audit_token_t audit;
    uint32_t kind;
    uint64_t requestID;
    int error;
    pid_t targetPID;
    int signalNumber;
    uint32_t entitlementOperation; // COPY_ENTITLEMENTS: csops operation 5 (CDHash), 7 (generated XML) or 16 (original DER).
    audit_token_t targetAudit;
    LSBPeerProcessIdentity peerIdentity; // CHECKIN owns its taskName; audit comes from the kernel and path remains empty.
    mach_port_t nativeServicePort; // Owned send reference, allowed only on a native service READY request.
    mach_port_t processDebuggerPort; // Owned send reference, allowed only on REGISTER_DEBUGGER_PORT after guest initialization.
    bool activateDebuggerPort; // Second phase: endpoint becomes queryable only after the guest publishes its trusted broker audit.
    LSBSpawnPlan plan;
    int fds[LSB_MAX_FDS];
    LSBSpawnPorts ports;
} LSBReceivedMessage;

#ifdef __cplusplus
extern "C" {
#endif
// Synchronous transport; no callback queues. The caller owns all returned FDs and registered-port send references.
// The private pipe call has no documented timeout; the broker must deadline pending launches and reply on failure.
// Returns one owned send right. Native callers discover it through bootstrap; the guest copies the handed-off right.
// A native child may keep one raw Mach send right across handoff; no XPC object or native function pointer is retained.
int LSBTransportCopyBrokerPort(mach_port_t *port);
int LSBTransportSpawn(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], pid_t *pid);
// Optional registered/bootstrap ports copied with COPY_SEND. Presence and null slots are preserved; the caller keeps its references.
int LSBTransportSpawnWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports, pid_t *pid);
// Returns an errno value; zero means the broker accepted replacement. The guest exec shim must never return after that success.
int LSBTransportExecWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports);
int LSBTransportCheckIn(LSBBootstrapReply *reply, int fds[LSB_MAX_FDS]);
int LSBTransportReady(uint64_t requestID, int error);
// Native service only: success publishes one listener send right; caller retains its reference. Failure carries no endpoint.
int LSBTransportReadyNativeServiceOnPort(mach_port_t broker, uint64_t requestID, int error, mach_port_t servicePort);
// Uses a borrowed held broker send right, even after native bootstrap/registered ports have been replaced. Caller retains ownership.
int LSBTransportReadyOnPort(mach_port_t broker, uint64_t requestID, int error);
// Exact positive PID of a running broker-managed child. Signal 0 validates identity without sending a signal.
// Returns an errno value. A successful launchd fallback means submitted; waitpid observes actual termination.
int LSBTransportKill(pid_t targetPID, int signalNumber);
// Simulator launchd only: request a replacement userspace after an accepted guest reboot reaches its terminal exit.
// Carries no payload or rights. Returns an errno value; success means broker acceptance, not completion of the replacement boot.
int LSBTransportRebootUserspace(void);
// Actual Boolean proc_info-allow required. Current committed simulator jobs only; no rights or native helper processes.
// Success owns a malloc snapshot; failure clears both outputs. Caller must free the successful result.
int LSBTransportListProcesses(LSBProcessList **list, size_t *size);
// Registers one self-query endpoint after guest TLS is ready. Success returns the reply's genuine kernel sender audit identity.
int LSBTransportRegisterDebuggerPort(mach_port_t endpoint, audit_token_t *brokerAudit);
int LSBTransportActivateDebuggerPort(const audit_token_t *brokerAudit);
// Exact native managed peer only. Broker restricts this to stock druid and its original Boolean entitlement.
// Returns a fresh cooperative native self KERN_PROC_PID result, never a cached or fabricated debugger flag.
int LSBTransportCopyDebuggerInfo(pid_t nativePID, struct kinfo_proc *info);
// Exact logical PID of a committed simulator guest. Broker requires the caller's original Boolean task_for_pid-allow.
// Success owns one task send right validated against the reply's complete native audit token; failure clears *task.
int LSBTransportCopyProcessTask(pid_t guestPID, mach_port_t *task);
// Simulator launchd only: resolve the complete kernel audit identity of a request sender to its committed stable guest PID.
// Unmanaged native identities retain their native PID unless they alias a known managed identity. Never changes the audit token.
// Returns an errno value and clears *pid on failure; rejected or retired identities must not fall back to their native PID.
int LSBTransportResolveAuditPID(const audit_token_t *audit, pid_t *pid);
// Copies the exact managed target's entitlement blob for csops operation 7 (XML) or 16 (DER), including its big-endian header.
// On success the caller owns malloc storage and must free it; both outputs are cleared on failure. The broker authenticates both identities.
int LSBTransportCopyEntitlements(const audit_token_t *target, unsigned operation, unsigned char **blob, size_t *size);
// Copies the immutable signed guest CodeDirectory's actual 20-byte digest. Clears hash on failure; same full-audit authentication.
int LSBTransportCopyCodeDirectoryHash(const audit_token_t *target, unsigned char hash[LSB_GUEST_CDHASH_BYTES]);
// Exact native PID, restricted by the broker to managed peers. Clears the output on failure; dispose the owned task-name right on success.
int LSBTransportCopyProcessIdentity(pid_t nativePID, LSBPeerProcessIdentity *identity);
// Simulator launchd only: resolve a direct child's logical PID to its current genuine native identity after exec replacement.
// The broker authenticates this mode from the sender's role; the returned audit token and task-name right remain native.
int LSBTransportCopyChildIdentity(pid_t guestPID, LSBPeerProcessIdentity *identity);
// Simulator launchd only: map an exact current native PID to its owned live child's stable guest PID.
// The broker validates the child's genuine task-name/audit identity; no native identity field is rewritten or returned.
// Returns an errno value and clears *guestPID on failure. Never falls back to interpreting nativePID as a logical child ID.
int LSBTransportResolveNativeChildPID(pid_t nativePID, pid_t *guestPID);
// Receive retains the request for a later reply; audit comes from the kernel, never the message payload.
// On failure, sends an error reply when possible and disposes the malformed message, imported FDs and copied send references.
int LSBTransportReceive(mach_port_t port, LSBReceivedMessage *message);
// Replies and releases the request object. Imported FDs and registered ports remain owned by message until DisposeMessage is called.
int LSBTransportReply(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]);
// COPY_ENTITLEMENTS only. Success borrows a bounded blob matching the requested operation; errors carry no bytes. Releases the request.
int LSBTransportReplyEntitlements(LSBReceivedMessage *message, int error, const void *bytes, size_t size);
// LIST_PROCESSES only; borrows the bounded validated snapshot and releases the request. Errors carry no bytes.
int LSBTransportReplyProcessList(LSBReceivedMessage *message, int error, const LSBProcessList *list, size_t size);
int LSBTransportReplyDebuggerInfo(LSBReceivedMessage *message, int error, const struct kinfo_proc *info);
// COPY_PROCESS_TASK only. Borrows the kernel-captured control right and full native identity; errors carry neither. Releases the request.
int LSBTransportReplyProcessTask(LSBReceivedMessage *message, int error, mach_port_t task, const audit_token_t *targetAudit);
// COPY_PROCESS_IDENTITY only. Borrows identity and copies its task-name right; errors carry no identity. Releases the request object.
int LSBTransportReplyProcessIdentity(LSBReceivedMessage *message, int error, const LSBPeerProcessIdentity *identity);
// Bootstrap additionally copies a shared status-file FD and nonblocking wake-pipe read FD; caller retains ownership.
// A successful CheckIn returns these in reply.statusFD/wakeFD. The caller must close them separately from the plan FDs.
int LSBTransportReplyBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                               const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD);
// CheckIn returns independent owned send references in reply.ports; release them after applying registered and task-bootstrap ports.
int LSBTransportReplyBootstrapWithPorts(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                        const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, const LSBSpawnPorts *ports);
// guestPID must be positive; parentPID may be zero for a guest root process. Existing wrappers use pid/getpid() respectively.
int LSBTransportReplyBootstrapWithMetadata(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                           const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD,
                                           const LSBBootstrapMetadata *metadata);
// Optional launchd-only temporary bootstrap send right. CheckIn returns an owned send reference; the caller must release it.
int LSBTransportReplyLaunchdBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, mach_port_t tankPort);
// Only pass a message initialized by Receive, and do not reuse it for another Receive before disposal.
void LSBTransportDisposeMessage(LSBReceivedMessage *message);
// Reject handoff if a native transport worker remains; called after all native IPC objects have been released.
int LSBRequireSingleThread(void);
#ifdef __cplusplus
}
#endif
#endif
