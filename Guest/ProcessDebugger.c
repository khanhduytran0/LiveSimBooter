#include "ProcessDebugger.h"
#include "WorkqueueBootstrap.h"
#include "../ProcessSupport/PeerDebugger.h"
#include "../ProcessSupport/SpawnTransport.h"
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic(bool) druidDebuggerQueries, brokerIdentityReady;
static audit_token_t processDebuggerBroker;
static _Atomic(mach_port_t) processDebuggerPort;

bool LSBGuestDebuggerCopyInfo(pid_t nativePID, struct kinfo_proc *info, int *error) {
    if (!atomic_load_explicit(&druidDebuggerQueries, memory_order_acquire)) return false;
    *error = LSBTransportCopyDebuggerInfo(nativePID, info);
    return true;
}

void LSBProcessDebuggerHandleMessage(mach_msg_header_t *message, size_t capacity) {
    if (atomic_load_explicit(&brokerIdentityReady, memory_order_acquire) && message->msgh_local_port == processDebuggerPort)
        LSBPeerDebuggerReply(message, capacity, &processDebuggerBroker);
    else mach_msg_destroy(message);
}

static bool isGuestDruid(const char *root, const char *executable) {
    static const char suffix[] = "/System/Library/PrivateFrameworks/DragUI.framework/Support/druid";
    size_t length = root ? strlen(root) : 0;
    return length > 1 && executable && !strncmp(executable, root, length) && !strcmp(executable + length, suffix);
}

// libSystem and guest TLS are initialized before inserted-library constructors. Never create a native transport worker.
#ifndef LSB_PROCESS_DEBUGGER_TESTING
__attribute__((constructor)) static void initializeProcessDebugger(void) {
    int saved = errno;
    char root[PATH_MAX], executable[PATH_MAX], physical[PATH_MAX];
    const char *configuredRoot = getenv("DYLD_ROOT_PATH");
    uint32_t size = sizeof(executable);
    if (configuredRoot && realpath(configuredRoot, root) && !_NSGetExecutablePath(executable, &size) && realpath(executable, physical))
        atomic_store_explicit(&druidDebuggerQueries, isGuestDruid(root, physical), memory_order_release);

    mach_port_t endpoint = MACH_PORT_NULL;
    int error = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &endpoint) ? EIO : 0;
    bool ownsSend = false;
    if (!error) {
        error = mach_port_insert_right(mach_task_self(), endpoint, endpoint, MACH_MSG_TYPE_MAKE_SEND) ? EIO : 0;
        ownsSend = !error;
    }
    if (!error) error = LSBWorkqueueAddProcessControlPort(endpoint);
    if (!error) {
        processDebuggerPort = endpoint;
        audit_token_t broker;
        error = LSBTransportRegisterDebuggerPort(endpoint, &broker);
        if (!error) {
            processDebuggerBroker = broker;
            atomic_store_explicit(&brokerIdentityReady, true, memory_order_release);
            error = LSBTransportActivateDebuggerPort(&broker);
        }
    }
    // Keep only the receive capability; the broker owns the registered send reference. Destroying receive removes port-set membership.
    if (ownsSend) mach_port_deallocate(mach_task_self(), endpoint);
    if (error && MACH_PORT_VALID(endpoint)) {
        atomic_store_explicit(&brokerIdentityReady, false, memory_order_release);
        mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1);
        processDebuggerPort = MACH_PORT_NULL;
    }
    const char *trace = getenv("LSB_TRACE_PEER_DEBUGGER_STATE");
    if (trace && !strcmp(trace, "1")) fprintf(stderr, "[LSB debugger endpoint] startup result=%d\n", error);
    errno = saved;
}
#endif
