#include "SpawnTransport.h"
#include "PrivateXPC.h"
#include "SingleThreadGuard.h"
#include "ChildStatus.h"
#include "GuestEntitlements.h"
#include "AudioOutput.h"
#if !defined(LSB_GUEST_RUNTIME) && !defined(LSB_TRANSPORT_TESTING)
#include "StoreAppGroups.h"
#endif
#if defined(LSB_XPC_WIRE_COMPATIBILITY)
#include "../Guest/XPCWireCompatibility.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(LSB_TRANSPORT_TRACE)
#include <stdio.h>
#endif

// The simulator SDK omits libproc.h; the same native ABI is exported by both libSystem variants.
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int size);

static int traceError(const char *stage, int error, unsigned detail) {
#if defined(LSB_TRANSPORT_TRACE)
    if (error) {
        int savedErrno = errno;
        fprintf(stderr, "LiveSimBooter XPC: %s failed: error=%d detail=%u\n", stage, error, detail);
        errno = savedErrno;
    }
#else
    (void)stage;
    (void)detail;
#endif
    return error;
}

static bool unsignedValue(xpc_object_t dictionary, const char *key, uint64_t *value) {
    xpc_object_t object = xpc_dictionary_get_value(dictionary, key);
    if (!object || xpc_get_type(object) != XPC_TYPE_UINT64) return false;
    *value = xpc_uint64_get_value(object);
    return true;
}

static bool signedValue(xpc_object_t dictionary, const char *key, int64_t *value) {
    xpc_object_t object = xpc_dictionary_get_value(dictionary, key);
    if (!object || xpc_get_type(object) != XPC_TYPE_INT64) return false;
    *value = xpc_int64_get_value(object);
    return true;
}

static int decodeExecutionRole(xpc_object_t dictionary, uint32_t *role, uint32_t *productionStatus) {
    *role = LSB_EXECUTION_SIMULATOR;
    *productionStatus = LSB_NATIVE_PRODUCTION_UNKNOWN;
    // Version 3 adds a simulator-only task capture offer. Legacy replies cannot authorize the current native bootstrap policy.
    uint64_t version, value, status;
    if (!unsignedValue(dictionary, "bootstrap-version", &version) || version != LSB_BOOTSTRAP_VERSION ||
        !unsignedValue(dictionary, "execution-role", &value) || value > LSB_EXECUTION_NATIVE_METAL_SERVICE ||
        !unsignedValue(dictionary, "native-production-status", &status) || status > LSB_NATIVE_DEVELOPMENT) return EPROTO;
    *role = (uint32_t)value;
    *productionStatus = (uint32_t)status;
    return 0;
}

static int validateRoleMetadata(uint32_t role, pid_t pid, pid_t guestPID, mach_port_t tank, const LSBSpawnPorts *ports) {
    if (role > LSB_EXECUTION_NATIVE_METAL_SERVICE) return EINVAL;
    if (role != LSB_EXECUTION_SIMULATOR &&
        (pid != guestPID || tank != MACH_PORT_NULL || (ports && (ports->registeredPresent || ports->bootstrapPresent)))) return EINVAL;
    return 0;
}

static void initializeFDs(int fds[LSB_MAX_FDS]) {
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) fds[i] = -1;
}

static void closeFDs(int fds[LSB_MAX_FDS]) {
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) {
        if (fds[i] >= 0) close(fds[i]);
        fds[i] = -1;
    }
}

static bool sendRight(mach_port_t port) {
    mach_port_type_t type = 0;
    return MACH_PORT_VALID(port) && !mach_port_type(mach_task_self(), port, &type) && (type & MACH_PORT_TYPE_SEND);
}

static int validateTaskCapture(const LSBTaskPortCaptureOffer *offer, uint32_t role) {
    if (!offer) return 0;
    if (offer->port != MACH_PORT_NULL) return role == LSB_EXECUTION_SIMULATOR && sendRight(offer->port) ? 0 : EINVAL;
    for (unsigned i = 0; i < sizeof(offer->nonce); ++i) if (offer->nonce[i]) return EINVAL;
    return 0;
}

static int decodeTaskCapture(xpc_object_t reply, uint32_t role, LSBTaskPortCaptureOffer *offer) {
    xpc_object_t port = xpc_dictionary_get_value(reply, "task-capture-port"), nonce = xpc_dictionary_get_value(reply, "task-capture-nonce");
    if (!port && !nonce) return 0;
    if (role != LSB_EXECUTION_SIMULATOR || !port || !nonce || xpc_get_type(port) != &_xpc_type_mach_send ||
        xpc_get_type(nonce) != XPC_TYPE_DATA || xpc_data_get_length(nonce) != sizeof(offer->nonce)) return EPROTO;
    offer->port = xpc_dictionary_copy_mach_send(reply, "task-capture-port");
    if (!sendRight(offer->port)) return EPROTO;
    memcpy(offer->nonce, xpc_data_get_bytes_ptr(nonce), sizeof(offer->nonce));
    return 0;
}

static bool identityPIDMatches(const LSBPeerProcessIdentity *identity, pid_t pid) {
    return pid > 0 && identity->audit.val[5] == (uint32_t)pid && LSBPeerProcessIdentityPID(identity) == (uint32_t)pid;
}

static bool taskNameMatchesAudit(mach_port_t task, const audit_token_t *audit) {
    audit_token_t actual;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    return sendRight(task) && task_info(task, TASK_AUDIT_TOKEN, (task_info_t)&actual, &count) == KERN_SUCCESS &&
        count == TASK_AUDIT_TOKEN_COUNT && !memcmp(&actual, audit, sizeof(*audit));
}

static int captureSelfIdentity(LSBPeerProcessIdentity *identity) {
    memset(identity, 0, sizeof(*identity));
    pid_t pid = getpid();
    int count = proc_pidinfo(pid, LSB_PEER_SHORT_INFO_FLAVOR, 0, identity->shortInfo, sizeof(identity->shortInfo));
    if (count != sizeof(identity->shortInfo)) return count < 0 || !count ? (errno ? errno : EIO) : EPROTO;
    audit_token_t audit;
    mach_msg_type_number_t auditCount = TASK_AUDIT_TOKEN_COUNT;
    if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &auditCount) || auditCount != TASK_AUDIT_TOKEN_COUNT) return EACCES;
    identity->audit = audit;
    if (!identityPIDMatches(identity, pid)) return EPROTO;
    if (task_name_for_pid(mach_task_self(), pid, &identity->taskName) || !taskNameMatchesAudit(identity->taskName, &identity->audit)) {
        LSBPeerProcessIdentityDispose(identity);
        return EACCES;
    }
    return 0;
}

static int decodeIdentity(xpc_object_t dictionary, LSBPeerProcessIdentity *identity, pid_t pid, const audit_token_t *checkinAudit) {
    const char *infoKey = checkinAudit ? "self-short-info" : "peer-short-info";
    const char *portKey = checkinAudit ? "self-task-name" : "peer-task-name";
    xpc_object_t info = xpc_dictionary_get_value(dictionary, infoKey), task = xpc_dictionary_get_value(dictionary, portKey);
    if (!info || xpc_get_type(info) != XPC_TYPE_DATA || xpc_data_get_length(info) != sizeof(identity->shortInfo) ||
        !task || xpc_get_type(task) != &_xpc_type_mach_send) return EPROTO;
    if (checkinAudit) identity->audit = *checkinAudit;
    else {
        xpc_object_t audit = xpc_dictionary_get_value(dictionary, "peer-audit"), path = xpc_dictionary_get_value(dictionary, "peer-path");
        if (!audit || xpc_get_type(audit) != XPC_TYPE_DATA || xpc_data_get_length(audit) != sizeof(identity->audit) ||
            !path || xpc_get_type(path) != XPC_TYPE_STRING) return EPROTO;
        size_t length = xpc_string_get_length(path);
        const char *text = xpc_string_get_string_ptr(path);
        if (!length || length >= sizeof(identity->executablePath) || text[0] != '/' || strlen(text) != length) return EPROTO;
        memcpy(&identity->audit, xpc_data_get_bytes_ptr(audit), sizeof(identity->audit));
        memcpy(identity->executablePath, text, length + 1);
    }
    memcpy(identity->shortInfo, xpc_data_get_bytes_ptr(info), sizeof(identity->shortInfo));
    if (!identityPIDMatches(identity, pid)) return EPROTO;
    identity->taskName = xpc_dictionary_copy_mach_send(dictionary, portKey);
    if (!sendRight(identity->taskName)) return EPROTO;
    // CHECKIN is independently authenticated by the broker. Replies must also bind the returned capability to the full identity.
    return checkinAudit || taskNameMatchesAudit(identity->taskName, &identity->audit) ? 0 : EPROTO;
}

static int encodePorts(xpc_object_t dictionary, const LSBSpawnPorts *ports) {
    if (!ports) return 0;
    if (ports->count > LSB_MAX_REGISTERED_PORTS || (!ports->registeredPresent && ports->count)) return EINVAL;
    for (unsigned i = 0; i < LSB_MAX_REGISTERED_PORTS; ++i) {
        mach_port_t port = ports->registered[i];
        if (i >= ports->count) { if (port != MACH_PORT_NULL) return EINVAL; else continue; }
        if (port == MACH_PORT_NULL) continue;
        if (!sendRight(port)) return EBADF;
    }
    if (!ports->bootstrapPresent && ports->bootstrapPort != MACH_PORT_NULL) return EINVAL;
    if (ports->bootstrapPort != MACH_PORT_NULL && !sendRight(ports->bootstrapPort)) return EBADF;
    if (ports->registeredPresent) {
        xpc_object_t array = xpc_array_create(NULL, 0);
        if (!array) return ENOMEM;
        for (unsigned i = 0; i < ports->count; ++i) xpc_array_set_mach_send(array, XPC_ARRAY_APPEND, ports->registered[i]);
        xpc_dictionary_set_value(dictionary, "registered-ports", array);
        xpc_release(array);
    }
    if (ports->bootstrapPresent) xpc_dictionary_set_mach_send(dictionary, "bootstrap-port", ports->bootstrapPort);
    return 0;
}

static int decodePorts(xpc_object_t dictionary, LSBSpawnPorts *ports) {
    xpc_object_t array = xpc_dictionary_get_value(dictionary, "registered-ports");
    if (array && (xpc_get_type(array) != XPC_TYPE_ARRAY || xpc_array_get_count(array) > LSB_MAX_REGISTERED_PORTS)) return EPROTO;
    ports->registeredPresent = array != NULL;
    ports->count = array ? (uint32_t)xpc_array_get_count(array) : 0;
    for (unsigned i = 0; i < ports->count; ++i) {
        xpc_object_t object = xpc_array_get_value(array, i);
        if (!object || xpc_get_type(object) != &_xpc_type_mach_send) { LSBSpawnPortsDispose(ports); return EPROTO; }
        mach_port_t borrowed = xpc_mach_send_get_right(object);
        if (borrowed == MACH_PORT_NULL) continue;
        mach_port_t copy = xpc_array_copy_mach_send(array, i);
        ports->registered[i] = copy;
        if (!sendRight(copy)) {
            LSBSpawnPortsDispose(ports);
            return EPROTO;
        }
    }
    xpc_object_t bootstrap = xpc_dictionary_get_value(dictionary, "bootstrap-port");
    if (bootstrap) {
        if (xpc_get_type(bootstrap) != &_xpc_type_mach_send) { LSBSpawnPortsDispose(ports); return EPROTO; }
        ports->bootstrapPresent = true;
        if (xpc_mach_send_get_right(bootstrap) != MACH_PORT_NULL) {
            ports->bootstrapPort = xpc_dictionary_copy_mach_send(dictionary, "bootstrap-port");
            if (!sendRight(ports->bootstrapPort)) { LSBSpawnPortsDispose(ports); return EPROTO; }
        }
    }
    return 0;
}

static int validateBootstrapFDs(int statusFD, int wakeFD) {
    struct stat status, wake;
    if (fstat(statusFD, &status) || fstat(wakeFD, &wake)) return errno;
    int statusFlags = fcntl(statusFD, F_GETFL), wakeFlags = fcntl(wakeFD, F_GETFL);
    if (statusFlags < 0 || wakeFlags < 0) return errno;
    if (!S_ISREG(status.st_mode) || status.st_size != LSB_CHILD_STATUS_MAP_SIZE || (statusFlags & O_ACCMODE) != O_RDWR ||
        !S_ISFIFO(wake.st_mode) || (wakeFlags & O_ACCMODE) != O_RDONLY || !(wakeFlags & O_NONBLOCK)) return EPROTO;
    return 0;
}

static int decodeBootstrapFD(xpc_object_t dictionary, const char *key, int *fd) {
    xpc_object_t object = xpc_dictionary_get_value(dictionary, key);
    if (!object || xpc_get_type(object) != XPC_TYPE_FD) return EPROTO;
    *fd = xpc_fd_dup(object);
    if (*fd < 0) return errno ? errno : EBADF;
    if (fcntl(*fd, F_SETFD, FD_CLOEXEC)) return errno;
    return 0;
}

static int encodePlan(xpc_object_t dictionary, const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]) {
    int error = LSBValidateSpawnPlan(plan);
    if (error) return traceError("validate plan", error, 0);
    if (plan->fdCount && !fds) return traceError("missing FDs", EINVAL, plan->fdCount);
    xpc_object_t array = xpc_array_create(NULL, 0);
    if (!array) return traceError("create FD array", ENOMEM, plan->fdCount);
    for (unsigned i = 0; i < plan->fdCount; ++i) {
        if (fds[i] < 0) { error = traceError("negative FD", EBADF, i); break; }
        xpc_object_t descriptor = xpc_fd_create(fds[i]);
        if (!descriptor) { error = traceError("xpc_fd_create", errno ? errno : EBADF, (unsigned)fds[i]); break; }
        xpc_array_append_value(array, descriptor);
        xpc_release(descriptor);
    }
    if (!error) {
        xpc_dictionary_set_data(dictionary, "plan", plan, sizeof(*plan));
        xpc_dictionary_set_value(dictionary, "fds", array);
    }
    xpc_release(array);
    return error;
}

static int decodePlan(xpc_object_t dictionary, LSBSpawnPlan *plan, int fds[LSB_MAX_FDS]) {
    xpc_object_t data = xpc_dictionary_get_value(dictionary, "plan");
    xpc_object_t array = xpc_dictionary_get_value(dictionary, "fds");
    if (!data || xpc_get_type(data) != XPC_TYPE_DATA || xpc_data_get_length(data) != sizeof(*plan) ||
        !array || xpc_get_type(array) != XPC_TYPE_ARRAY) return EPROTO;
    memcpy(plan, xpc_data_get_bytes_ptr(data), sizeof(*plan));
    int error = LSBValidateSpawnPlan(plan);
    if (error) return error;
    if (xpc_array_get_count(array) != plan->fdCount) return EPROTO;
    for (unsigned i = 0; i < plan->fdCount; ++i) {
        xpc_object_t descriptor = xpc_array_get_value(array, i);
        if (!descriptor || xpc_get_type(descriptor) != XPC_TYPE_FD) { error = EPROTO; break; }
        fds[i] = xpc_fd_dup(descriptor);
        if (fds[i] < 0) { error = errno ? errno : EBADF; break; }
    }
    if (error) closeFDs(fds);
    return error;
}

int LSBTransportCopyBrokerPort(mach_port_t *port) {
    if (!port) return EINVAL;
    *port = MACH_PORT_NULL;
#if defined(LSB_GUEST_RUNTIME)
    // Mach names remain valid across the in-process dyld handoff. Simulator libxpc deliberately disables its
    // bootstrap lookup without a simulator launchd, so use the right obtained by our native entry instead.
    const char *text = getenv(LSB_BROKER_PORT_ENV);
    if (!text) return ENOENT;
    uint64_t value = 0;
    if (!*text) return EINVAL;
    for (unsigned i = 0; text[i]; ++i) {
        if (i >= 10 || text[i] < '0' || text[i] > '9') return EINVAL;
        value = value * 10 + (unsigned)(text[i] - '0');
        if (value >= UINT32_MAX) return EINVAL;
    }
    if (!value) return EINVAL;
    mach_port_t candidate = (mach_port_t)value;
    mach_port_type_t type = 0;
    if (mach_port_type(mach_task_self(), candidate, &type) || !(type & MACH_PORT_TYPE_SEND)) return EACCES;
    if (mach_port_mod_refs(mach_task_self(), candidate, MACH_PORT_RIGHT_SEND, 1)) return EACCES;
    *port = candidate;
    return 0;
#elif defined(LSB_TRANSPORT_TESTING)
    extern int LSBTestCopyBrokerPort(mach_port_t *port);
    return LSBTestCopyBrokerPort(port);
#else
    LSBStoreAppGroups groups;
    int error = LSBCopyStoreAppGroups(&groups);
    if (error) return error;
    int failure = ENOENT;
    for (size_t index = 0; index < groups.count; ++index) {
        char name[LSB_BROKER_SERVICE_BYTES];
        error = LSBStoreBrokerServiceName(groups.identifiers[index], name);
        if (error) return error;
        mach_port_t candidate = MACH_PORT_NULL;
        kern_return_t result = bootstrap_look_up(bootstrap_port, name, &candidate);
        if (result == KERN_SUCCESS && sendRight(candidate)) { *port = candidate; return 0; }
        if (MACH_PORT_VALID(candidate)) mach_port_deallocate(mach_task_self(), candidate);
        // An inactive store is expected. A denied one may still precede the host's other entitled store namespace.
        if (result == 1102) continue; // BOOTSTRAP_UNKNOWN_SERVICE
        if (result == 1100) { failure = EACCES; continue; } // BOOTSTRAP_NOT_PRIVILEGED
        return EIO;
    }
    return failure;
#endif
}

static xpc_object_t createRequest(uint32_t kind, uint64_t requestID, int error) {
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    if (!request) return NULL;
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", kind);
    xpc_dictionary_set_uint64(request, "request-id", requestID);
    xpc_dictionary_set_int64(request, "error", error);
    return request;
}

// This private API has no documented timeout. The native broker must finish or fail each accepted request.
// Nothing from this call (including a pipe or reply object) may survive the native-cache handoff.
static int callBrokerOnPort(mach_port_t port, xpc_object_t request, xpc_object_t *reply) {
    *reply = NULL;
    if (!sendRight(port)) return EINVAL;
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0);
    if (!pipe) return traceError("xpc_pipe_create_from_port", ENOMEM, port);
    int error;
#if defined(LSB_XPC_WIRE_COMPATIBILITY)
    error = LSBXPCWireBegin(port);
    if (!error) {
        error = xpc_pipe_routine(pipe, request, reply);
        LSBXPCWireEnd();
    }
#else
    error = xpc_pipe_routine(pipe, request, reply);
#endif
    if (error) traceError("xpc_pipe_routine", error, port);
    xpc_release(pipe);
    if (error && *reply) { xpc_release(*reply); *reply = NULL; }
    return error;
}

static int callBroker(xpc_object_t request, xpc_object_t *reply) {
    *reply = NULL;
    mach_port_t port = MACH_PORT_NULL;
    int error = LSBTransportCopyBrokerPort(&port);
    if (error) return traceError("copy broker port", error, port);
    error = callBrokerOnPort(port, request, reply);
    mach_port_deallocate(mach_task_self(), port);
    return error;
}

static int decodeReplyHeader(xpc_object_t reply, int *remoteError, pid_t *pid, uint64_t *requestID) {
    uint64_t version;
    int64_t encodedError, encodedPID;
    if (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY || !unsignedValue(reply, "version", &version) ||
        version != LSB_TRANSPORT_VERSION || !unsignedValue(reply, "request-id", requestID) ||
        !signedValue(reply, "error", &encodedError) || encodedError < 0 || encodedError > INT_MAX ||
        !signedValue(reply, "pid", &encodedPID) || encodedPID < 0 || encodedPID > INT_MAX) return EPROTO;
    *remoteError = (int)encodedError;
    *pid = (pid_t)encodedPID;
    return 0;
}

static int processRequest(uint32_t kind, const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports, pid_t *pid) {
    if (!pid) return EINVAL;
    *pid = 0;
    xpc_object_t request = createRequest(kind, 0, 0);
    if (!request) return traceError("create process request", ENOMEM, kind);
    int error = encodePlan(request, plan, fds);
    if (!error) error = encodePorts(request, ports);
    xpc_object_t reply = NULL;
    if (!error) error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, pid, &requestID);
        if (error) traceError("decode process reply", error, kind);
        if (!error && remoteError) traceError("broker process reply", remoteError, kind);
        if (!error) error = remoteError;
        if (!error && *pid <= 0) error = EPROTO;
    }
    if (reply) xpc_release(reply);
    if (error) *pid = 0;
    return error;
}

int LSBTransportSpawnWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports, pid_t *pid) {
    return processRequest(LSB_REQUEST_SPAWN, plan, fds, ports, pid);
}

int LSBTransportExecWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports) {
    pid_t pid = 0;
    return processRequest(LSB_REQUEST_EXEC, plan, fds, ports, &pid);
}

int LSBTransportSpawn(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], pid_t *pid) {
    return LSBTransportSpawnWithPorts(plan, fds, NULL, pid);
}

int LSBTransportCheckIn(LSBBootstrapReply *result, int fds[LSB_MAX_FDS]) {
    if (!result || !fds) return EINVAL;
    memset(result, 0, sizeof(*result));
    result->statusFD = result->wakeFD = -1;
    initializeFDs(fds);
    xpc_object_t request = createRequest(LSB_REQUEST_CHECKIN, 0, 0);
    if (!request) return ENOMEM;
    LSBPeerProcessIdentity self;
    int error = captureSelfIdentity(&self);
    if (!error) {
        xpc_dictionary_set_data(request, "self-short-info", self.shortInfo, sizeof(self.shortInfo));
        xpc_dictionary_set_mach_send(request, "self-task-name", self.taskName);
    }
    LSBPeerProcessIdentityDispose(&self);
    xpc_object_t reply = NULL;
    if (!error) error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &result->requestID);
        result->error = remoteError;
        result->pid = pid;
        if (!error) error = remoteError;
        if (!error && (!result->requestID || pid <= 0)) error = EPROTO;
        if (!error) error = decodeExecutionRole(reply, &result->executionRole, &result->nativeProductionStatus);
        if (!error) error = decodeTaskCapture(reply, result->executionRole, &result->taskCapture);
        if (!error) {
            int64_t guestPID, parentPID;
            if (!signedValue(reply, "guest-pid", &guestPID) || guestPID <= 0 || guestPID > INT_MAX ||
                !signedValue(reply, "parent-pid", &parentPID) || parentPID < 0 || parentPID > INT_MAX) error = EPROTO;
            else { result->guestPID = (int32_t)guestPID; result->parentPID = (int32_t)parentPID; }
        }
        if (!error) {
            xpc_object_t root = xpc_dictionary_get_value(reply, "runtime-root");
            if (!root || xpc_get_type(root) != XPC_TYPE_STRING || !xpc_string_get_length(root) ||
                xpc_string_get_length(root) >= sizeof(result->runtimeRoot)) error = EPROTO;
            else memcpy(result->runtimeRoot, xpc_string_get_string_ptr(root), xpc_string_get_length(root) + 1);
        }
        if (!error) error = decodePlan(reply, &result->plan, fds);
        if (!error) error = decodeBootstrapFD(reply, "child-status-fd", &result->statusFD);
        if (!error) error = decodeBootstrapFD(reply, "child-wake-fd", &result->wakeFD);
        if (!error) error = validateBootstrapFDs(result->statusFD, result->wakeFD);
        if (!error) error = decodePorts(reply, &result->ports);
        if (!error) {
            xpc_object_t port = xpc_dictionary_get_value(reply, "launchd-tank-port");
            if (port) {
                if (xpc_get_type(port) != &_xpc_type_mach_send) error = EPROTO;
                else {
                    result->launchdTankPort = xpc_dictionary_copy_mach_send(reply, "launchd-tank-port");
                    if (!MACH_PORT_VALID(result->launchdTankPort)) error = EPROTO;
                }
            }
        }
        if (!error && validateRoleMetadata(result->executionRole, result->pid, result->guestPID,
                                           result->launchdTankPort, &result->ports)) error = EPROTO;
    }
    if (reply) xpc_release(reply);
    if (error) {
        result->nativeProductionStatus = LSB_NATIVE_PRODUCTION_UNKNOWN;
        closeFDs(fds);
        if (result->statusFD >= 0) close(result->statusFD);
        if (result->wakeFD >= 0) close(result->wakeFD);
        result->statusFD = result->wakeFD = -1;
        if (MACH_PORT_VALID(result->launchdTankPort)) mach_port_deallocate(mach_task_self(), result->launchdTankPort);
        result->launchdTankPort = MACH_PORT_NULL;
        if (MACH_PORT_VALID(result->taskCapture.port)) mach_port_deallocate(mach_task_self(), result->taskCapture.port);
        memset(&result->taskCapture, 0, sizeof(result->taskCapture));
        LSBSpawnPortsDispose(&result->ports);
    }
    return error;
}

static int readyOnPort(mach_port_t broker, uint64_t requestID, int childError, mach_port_t servicePort) {
    if (!requestID || childError < 0 || (servicePort != MACH_PORT_NULL && (childError || !sendRight(servicePort)))) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_READY, requestID, childError);
    if (!request) return ENOMEM;
    if (servicePort != MACH_PORT_NULL) xpc_dictionary_set_mach_send(request, "native-service-port", servicePort);
    xpc_object_t reply = NULL;
    int error = callBrokerOnPort(broker, request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t replyID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &replyID);
        if (!error && replyID != requestID) error = EPROTO;
        if (!error) error = remoteError;
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportReadyOnPort(mach_port_t broker, uint64_t requestID, int childError) {
    return readyOnPort(broker, requestID, childError, MACH_PORT_NULL);
}

int LSBTransportReadyNativeServiceOnPort(mach_port_t broker, uint64_t requestID, int childError, mach_port_t servicePort) {
    if (!childError && !sendRight(servicePort)) return EINVAL;
    return readyOnPort(broker, requestID, childError, servicePort);
}

int LSBTransportReady(uint64_t requestID, int childError) {
    if (!requestID || childError < 0) return EINVAL;
    mach_port_t port = MACH_PORT_NULL;
    int error = LSBTransportCopyBrokerPort(&port);
    if (error) return error;
    error = LSBTransportReadyOnPort(port, requestID, childError);
    mach_port_deallocate(mach_task_self(), port);
    return error;
}

int LSBTransportKill(pid_t targetPID, int signalNumber) {
    if (signalNumber < 0 || signalNumber >= NSIG) return EINVAL;
    if (targetPID <= 0) return ENOTSUP;
    xpc_object_t request = createRequest(LSB_REQUEST_KILL, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_int64(request, "target-pid", targetPID);
    xpc_dictionary_set_int64(request, "signal", signalNumber);
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (requestID || (!remoteError && pid != targetPID))) error = EPROTO;
        if (!error) error = remoteError;
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportRebootUserspace(void) {
    xpc_object_t request = createRequest(LSB_REQUEST_REBOOT_USERSPACE, 0, 0);
    if (!request) return ENOMEM;
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (pid || requestID || xpc_dictionary_get_count(reply) != 4)) error = EPROTO;
        if (!error) error = remoteError;
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportListProcesses(LSBProcessList **list, size_t *size) {
    if (list) *list = NULL;
    if (size) *size = 0;
    if (!list || !size) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_LIST_PROCESSES, 0, 0);
    if (!request) return ENOMEM;
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        xpc_object_t data = error ? NULL : xpc_dictionary_get_value(reply, "process-list");
        if (!error && (pid || requestID || xpc_dictionary_get_count(reply) != (remoteError ? 4u : 5u) ||
                       (remoteError ? data != NULL : !data || xpc_get_type(data) != XPC_TYPE_DATA))) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) {
            size_t length = xpc_data_get_length(data);
            const void *bytes = xpc_data_get_bytes_ptr(data);
            error = LSBValidateProcessList(bytes, length);
            if (!error) {
                LSBProcessList *copy = malloc(length);
                if (!copy) error = ENOMEM;
                else { memcpy(copy, bytes, length); *list = copy; *size = length; }
            }
        }
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportRegisterDebuggerPort(mach_port_t endpoint, audit_token_t *brokerAudit) {
    if (brokerAudit) memset(brokerAudit, 0, sizeof(*brokerAudit));
    if (!brokerAudit || !sendRight(endpoint)) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_REGISTER_DEBUGGER_PORT, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_mach_send(request, "process-debugger-port", endpoint);
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (pid || requestID || xpc_dictionary_get_count(reply) != 4)) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) {
            audit_token_t audit = {{0}};
            xpc_dictionary_get_audit_token(reply, &audit);
            if (!audit.val[5] || audit.val[5] > INT_MAX || !audit.val[7]) error = EPROTO;
            else *brokerAudit = audit;
        }
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportActivateDebuggerPort(const audit_token_t *brokerAudit) {
    if (!brokerAudit || !brokerAudit->val[5] || brokerAudit->val[5] > INT_MAX || !brokerAudit->val[7]) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_REGISTER_DEBUGGER_PORT, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_bool(request, "activate-debugger-port", true);
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (pid || requestID || xpc_dictionary_get_count(reply) != 4)) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) {
            audit_token_t audit = {{0}};
            xpc_dictionary_get_audit_token(reply, &audit);
            if (memcmp(&audit, brokerAudit, sizeof(audit))) error = EACCES;
        }
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportCopyDebuggerInfo(pid_t nativePID, struct kinfo_proc *info) {
    if (info) memset(info, 0, sizeof(*info));
    if (!info || nativePID <= 0) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_COPY_DEBUGGER_INFO, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_int64(request, "target-pid", nativePID);
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        xpc_object_t data = error ? NULL : xpc_dictionary_get_value(reply, "process-debugger-info");
        if (!error && (requestID || pid != (remoteError ? 0 : nativePID) || xpc_dictionary_get_count(reply) != (remoteError ? 4u : 5u) ||
            (remoteError ? data != NULL : !data || xpc_get_type(data) != XPC_TYPE_DATA || xpc_data_get_length(data) != sizeof(*info)))) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) {
            struct kinfo_proc copy;
            memcpy(&copy, xpc_data_get_bytes_ptr(data), sizeof(copy));
            if (copy.kp_proc.p_pid != nativePID) error = EPROTO;
            else *info = copy;
        }
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportOpenAudioOutput(void **mapping, size_t *size) {
    if (mapping) *mapping = NULL;
    if (size) *size = 0;
    if (!mapping || !size) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_OPEN_AUDIO_OUTPUT, 0, 0), reply = NULL;
    if (!request) return ENOMEM;
    int error = callBroker(request, &reply);
    xpc_release(request);
    void *region = NULL;
    size_t length = 0;
    bool remoteOpened = false;
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        remoteOpened = !error && !remoteError && !requestID && !pid;
        xpc_object_t memory = error ? NULL : xpc_dictionary_get_value(reply, "audio-ring");
        if (!error && (requestID || pid || xpc_dictionary_get_count(reply) != (remoteError ? 4u : 5u) ||
            (remoteError ? memory != NULL : !memory || xpc_get_type(memory) != XPC_TYPE_SHMEM))) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) {
            length = xpc_shmem_map(memory, &region);
            if (!region || length != LSBAudioMapBytes) error = EPROTO;
        }
    }
    if (reply) xpc_release(reply);
    if (error) {
        if (region && length) munmap(region, length);
        if (remoteOpened) LSBTransportCloseAudioOutput();
    }
    else { *mapping = region; *size = length; }
    return error;
}

int LSBTransportCloseAudioOutput(void) {
    xpc_object_t request = createRequest(LSB_REQUEST_CLOSE_AUDIO_OUTPUT, 0, 0), reply = NULL;
    if (!request) return ENOMEM;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (requestID || pid || xpc_dictionary_get_count(reply) != 4)) error = EPROTO;
        if (!error) error = remoteError;
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportCopyProcessTask(pid_t guestPID, mach_port_t *task) {
    if (task) *task = MACH_PORT_NULL;
    if (!task || guestPID <= 0) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_COPY_PROCESS_TASK, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_int64(request, "target-pid", guestPID);
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    mach_port_t copy = MACH_PORT_NULL;
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        xpc_object_t audit = error ? NULL : xpc_dictionary_get_value(reply, "target-audit");
        xpc_object_t port = error ? NULL : xpc_dictionary_get_value(reply, "process-task");
        if (!error && (requestID || pid != (remoteError ? 0 : guestPID) || xpc_dictionary_get_count(reply) != (remoteError ? 4u : 6u) ||
            (remoteError ? audit || port : !audit || xpc_get_type(audit) != XPC_TYPE_DATA || xpc_data_get_length(audit) != sizeof(audit_token_t) ||
             !port || xpc_get_type(port) != &_xpc_type_mach_send))) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) {
            audit_token_t target;
            memcpy(&target, xpc_data_get_bytes_ptr(audit), sizeof(target));
            copy = xpc_dictionary_copy_mach_send(reply, "process-task");
            if (!target.val[5] || target.val[5] > INT_MAX || !target.val[7] || !taskNameMatchesAudit(copy, &target)) error = EPROTO;
        }
    }
    if (reply) xpc_release(reply);
    if (error && MACH_PORT_VALID(copy)) mach_port_deallocate(mach_task_self(), copy);
    else if (!error) *task = copy;
    return error;
}

int LSBTransportResolveAuditPID(const audit_token_t *audit, pid_t *result) {
    if (result) *result = 0;
    if (!audit || !result || !audit->val[5] || audit->val[5] > INT_MAX) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_RESOLVE_AUDIT_PID, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_data(request, "target-audit", audit, sizeof(*audit));
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (requestID || (!remoteError && pid <= 0))) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) *result = pid;
    }
    if (reply) xpc_release(reply);
    return error;
}

static bool validCodeSigningData(const void *bytes, size_t size, unsigned operation) {
    return operation == 5 ? bytes && size == LSB_GUEST_CDHASH_BYTES :
                           LSBGuestEntitlementsHasValidBlobHeaderForOperation(bytes, size, operation);
}

static int copyCodeSigningData(const audit_token_t *target, unsigned operation, unsigned char **blob, size_t *size) {
    if (blob) *blob = NULL;
    if (size) *size = 0;
    if (!target || !blob || !size || !target->val[5] || target->val[5] > INT_MAX) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_COPY_ENTITLEMENTS, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_data(request, "target-audit", target, sizeof(*target));
    xpc_dictionary_set_uint64(request, "operation", operation);
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        xpc_object_t data = error ? NULL : xpc_dictionary_get_value(reply, "entitlements");
        // Exact shape excludes FD/port fields, even under an unexpected key. Error replies never carry entitlement data.
        if (!error && (pid || requestID || xpc_dictionary_get_count(reply) != (remoteError ? 4u : 5u) ||
                       (remoteError ? data != NULL : !data || xpc_get_type(data) != XPC_TYPE_DATA))) error = EPROTO;
        if (!error && remoteError) error = remoteError;
        if (!error) {
            size_t length = xpc_data_get_length(data);
            const void *bytes = xpc_data_get_bytes_ptr(data);
            if (!validCodeSigningData(bytes, length, operation)) error = EPROTO;
            else {
                unsigned char *copy = malloc(length);
                if (!copy) error = ENOMEM;
                else { memcpy(copy, bytes, length); *blob = copy; *size = length; }
            }
        }
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportCopyEntitlements(const audit_token_t *target, unsigned operation, unsigned char **blob, size_t *size) {
    if (blob) *blob = NULL;
    if (size) *size = 0;
    if (operation != 7 && operation != 16) return EINVAL;
    return copyCodeSigningData(target, operation, blob, size);
}

int LSBTransportCopyCodeDirectoryHash(const audit_token_t *target, unsigned char hash[LSB_GUEST_CDHASH_BYTES]) {
    if (!hash) return EINVAL;
    memset(hash, 0, LSB_GUEST_CDHASH_BYTES);
    unsigned char *bytes = NULL;
    size_t size = 0;
    int error = copyCodeSigningData(target, 5, &bytes, &size);
    if (!error) memcpy(hash, bytes, LSB_GUEST_CDHASH_BYTES);
    free(bytes);
    return error;
}

static int copyProcessIdentity(pid_t targetPID, bool logicalPID, LSBPeerProcessIdentity *identity) {
    if (identity) memset(identity, 0, sizeof(*identity));
    if (!identity || targetPID <= 0) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_COPY_PROCESS_IDENTITY, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_int64(request, "target-pid", targetPID);
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (requestID || (remoteError ? pid != 0 : pid <= 0 || (!logicalPID && pid != targetPID)) ||
                       xpc_dictionary_get_count(reply) != (remoteError ? 4u : 8u))) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) error = decodeIdentity(reply, identity, pid, NULL);
    }
    if (reply) xpc_release(reply);
    if (error) LSBPeerProcessIdentityDispose(identity);
    return error;
}

int LSBTransportCopyProcessIdentity(pid_t nativePID, LSBPeerProcessIdentity *identity) {
    return copyProcessIdentity(nativePID, false, identity);
}

int LSBTransportCopyChildIdentity(pid_t guestPID, LSBPeerProcessIdentity *identity) {
    return copyProcessIdentity(guestPID, true, identity);
}

int LSBTransportResolveNativeChildPID(pid_t nativePID, pid_t *guestPID) {
    if (guestPID) *guestPID = 0;
    if (!guestPID || nativePID <= 0) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_RESOLVE_NATIVE_CHILD_PID, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_int64(request, "target-pid", nativePID);
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (requestID || xpc_dictionary_get_count(reply) != 4 || (remoteError ? pid != 0 : pid <= 0))) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) *guestPID = pid;
    }
    if (reply) xpc_release(reply);
    return error;
}

void LSBTransportDisposeMessage(LSBReceivedMessage *message) {
    if (!message) return;
    if (message->object) xpc_release((xpc_object_t)message->object);
    message->object = NULL;
    if (MACH_PORT_VALID(message->processDebuggerPort)) mach_port_deallocate(mach_task_self(), message->processDebuggerPort);
    message->processDebuggerPort = MACH_PORT_NULL;
    closeFDs(message->fds);
    LSBSpawnPortsDispose(&message->ports);
    LSBPeerProcessIdentityDispose(&message->peerIdentity);
    if (MACH_PORT_VALID(message->nativeServicePort)) mach_port_deallocate(mach_task_self(), message->nativeServicePort);
    message->nativeServicePort = MACH_PORT_NULL;
}

static int replyToMessage(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                          const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], bool bootstrap, int statusFD, int wakeFD,
                          mach_port_t tankPort, const LSBSpawnPorts *ports, pid_t guestPID, pid_t parentPID, uint32_t executionRole,
                          uint32_t productionStatus, const LSBTaskPortCaptureOffer *taskCapture) {
    if (!message || !message->object || error < 0 || pid < 0) return EINVAL;
    xpc_object_t reply = xpc_dictionary_create_reply((xpc_object_t)message->object);
    if (!reply) {
        xpc_release((xpc_object_t)message->object);
        message->object = NULL;
        return EPIPE;
    }
    int encodingError = 0;
    if (!error && plan) {
        if (!runtimeRoot || !runtimeRoot[0] || strnlen(runtimeRoot, LSB_PATH_BYTES) >= LSB_PATH_BYTES) encodingError = EINVAL;
        else {
            encodingError = encodePlan(reply, plan, fds);
            if (!encodingError) xpc_dictionary_set_string(reply, "runtime-root", runtimeRoot);
        }
    }
    if (!error && !encodingError && bootstrap) {
        encodingError = plan && pid > 0 && guestPID > 0 && parentPID >= 0 ? validateBootstrapFDs(statusFD, wakeFD) : EINVAL;
        if (!encodingError) encodingError = validateRoleMetadata(executionRole, pid, guestPID, tankPort, ports);
        if (!encodingError && !LSBValidNativeProductionStatus(productionStatus)) encodingError = EINVAL;
        if (!encodingError) encodingError = validateTaskCapture(taskCapture, executionRole);
        if (!encodingError) {
            xpc_object_t status = xpc_fd_create(statusFD), wake = xpc_fd_create(wakeFD);
            if (!status || !wake) encodingError = errno ? errno : EBADF;
            else {
                xpc_dictionary_set_value(reply, "child-status-fd", status);
                xpc_dictionary_set_value(reply, "child-wake-fd", wake);
            }
            if (status) xpc_release(status);
            if (wake) xpc_release(wake);
        }
        if (!encodingError && MACH_PORT_VALID(tankPort)) xpc_dictionary_set_mach_send(reply, "launchd-tank-port", tankPort);
        if (!encodingError) encodingError = encodePorts(reply, ports);
        if (!encodingError) {
            xpc_dictionary_set_int64(reply, "guest-pid", guestPID);
            xpc_dictionary_set_int64(reply, "parent-pid", parentPID);
            xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION);
            xpc_dictionary_set_uint64(reply, "execution-role", executionRole);
            xpc_dictionary_set_uint64(reply, "native-production-status", productionStatus);
            if (taskCapture && taskCapture->port != MACH_PORT_NULL) {
                xpc_dictionary_set_mach_send(reply, "task-capture-port", taskCapture->port);
                xpc_dictionary_set_data(reply, "task-capture-nonce", taskCapture->nonce, sizeof(taskCapture->nonce));
            }
        }
    }
    if (encodingError) error = encodingError;
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", requestID);
    xpc_dictionary_set_int64(reply, "error", error);
    xpc_dictionary_set_int64(reply, "pid", error ? 0 : pid);
    int sendError = xpc_pipe_routine_reply(reply);
    xpc_release(reply);
    xpc_release((xpc_object_t)message->object);
    message->object = NULL;
    return sendError ? sendError : encodingError;
}

int LSBTransportReply(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]) {
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, false, -1, -1, MACH_PORT_NULL, NULL, 0, 0,
                          LSB_EXECUTION_SIMULATOR, LSB_NATIVE_PRODUCTION_UNKNOWN, NULL);
}

int LSBTransportReplyEntitlements(LSBReceivedMessage *message, int error, const void *bytes, size_t size) {
    if (!message || !message->object || error < 0 || message->kind != LSB_REQUEST_COPY_ENTITLEMENTS || message->requestID) return EINVAL;
    int encodingError = (error ? bytes != NULL || size != 0 :
        !validCodeSigningData(bytes, size, message->entitlementOperation)) ? EINVAL : 0;
    if (encodingError) error = encodingError;
    xpc_object_t reply = xpc_dictionary_create_reply((xpc_object_t)message->object);
    if (!reply) {
        xpc_release((xpc_object_t)message->object);
        message->object = NULL;
        return EPIPE;
    }
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", 0);
    xpc_dictionary_set_int64(reply, "error", error);
    xpc_dictionary_set_int64(reply, "pid", 0);
    if (!error) xpc_dictionary_set_data(reply, "entitlements", bytes, size);
    int sendError = xpc_pipe_routine_reply(reply);
    xpc_release(reply);
    xpc_release((xpc_object_t)message->object);
    message->object = NULL;
    return sendError ? sendError : encodingError;
}

int LSBTransportReplyProcessList(LSBReceivedMessage *message, int error, const LSBProcessList *list, size_t size) {
    if (!message || !message->object || error < 0 || message->kind != LSB_REQUEST_LIST_PROCESSES || message->requestID) return EINVAL;
    int encodingError = (error ? list != NULL || size != 0 : LSBValidateProcessList(list, size) != 0) ? EINVAL : 0;
    if (encodingError) error = encodingError;
    xpc_object_t reply = xpc_dictionary_create_reply((xpc_object_t)message->object);
    if (!reply) {
        xpc_release((xpc_object_t)message->object);
        message->object = NULL;
        return EPIPE;
    }
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", 0);
    xpc_dictionary_set_int64(reply, "error", error);
    xpc_dictionary_set_int64(reply, "pid", 0);
    if (!error) xpc_dictionary_set_data(reply, "process-list", list, size);
    int sendError = xpc_pipe_routine_reply(reply);
    xpc_release(reply);
    xpc_release((xpc_object_t)message->object);
    message->object = NULL;
    return sendError ? sendError : encodingError;
}

int LSBTransportReplyDebuggerInfo(LSBReceivedMessage *message, int error, const struct kinfo_proc *info) {
    if (!message || !message->object || error < 0 || message->kind != LSB_REQUEST_COPY_DEBUGGER_INFO || message->requestID) return EINVAL;
    int encodingError = error ? (info != NULL ? EINVAL : 0) :
        (!info || message->targetPID <= 0 || info->kp_proc.p_pid != message->targetPID ? EINVAL : 0);
    if (encodingError) error = encodingError;
    xpc_object_t reply = xpc_dictionary_create_reply((xpc_object_t)message->object);
    if (!reply) { xpc_release((xpc_object_t)message->object); message->object = NULL; return EPIPE; }
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", 0);
    xpc_dictionary_set_int64(reply, "error", error);
    xpc_dictionary_set_int64(reply, "pid", error ? 0 : message->targetPID);
    if (!error) xpc_dictionary_set_data(reply, "process-debugger-info", info, sizeof(*info));
    int sendError = xpc_pipe_routine_reply(reply);
    xpc_release(reply);
    xpc_release((xpc_object_t)message->object);
    message->object = NULL;
    return sendError ? sendError : encodingError;
}

int LSBTransportReplyProcessTask(LSBReceivedMessage *message, int error, mach_port_t task, const audit_token_t *targetAudit) {
    if (!message || !message->object || error < 0 || message->kind != LSB_REQUEST_COPY_PROCESS_TASK || message->requestID) return EINVAL;
    // Audit validation binds identity, not task flavor. The broker supplies only control rights received from kernel exception messages.
    int encodingError = error ? (task != MACH_PORT_NULL || targetAudit ? EINVAL : 0) :
        (message->targetPID <= 0 || !targetAudit || !targetAudit->val[5] || targetAudit->val[5] > INT_MAX || !targetAudit->val[7] ||
         !taskNameMatchesAudit(task, targetAudit) ? EINVAL : 0);
    if (encodingError) error = encodingError;
    xpc_object_t reply = xpc_dictionary_create_reply((xpc_object_t)message->object);
    if (!reply) { xpc_release((xpc_object_t)message->object); message->object = NULL; return EPIPE; }
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", 0);
    xpc_dictionary_set_int64(reply, "error", error);
    xpc_dictionary_set_int64(reply, "pid", error ? 0 : message->targetPID);
    if (!error) {
        xpc_dictionary_set_data(reply, "target-audit", targetAudit, sizeof(*targetAudit));
        xpc_dictionary_set_mach_send(reply, "process-task", task);
    }
    int sendError = xpc_pipe_routine_reply(reply);
    xpc_release(reply);
    xpc_release((xpc_object_t)message->object);
    message->object = NULL;
    return sendError ? sendError : encodingError;
}

int LSBTransportReplyAudioOutput(LSBReceivedMessage *message, int error, void *sharedMemory) {
    if (!message || !message->object || error < 0 || message->kind != LSB_REQUEST_OPEN_AUDIO_OUTPUT || message->requestID) return EINVAL;
    xpc_object_t memory = sharedMemory;
    int encodingError = error ? (memory ? EINVAL : 0) : (!memory || xpc_get_type(memory) != XPC_TYPE_SHMEM ? EINVAL : 0);
    if (encodingError) error = encodingError;
    xpc_object_t reply = xpc_dictionary_create_reply(message->object);
    if (!reply) { xpc_release(message->object); message->object = NULL; return EPIPE; }
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", 0);
    xpc_dictionary_set_int64(reply, "error", error);
    xpc_dictionary_set_int64(reply, "pid", 0);
    if (!error) xpc_dictionary_set_value(reply, "audio-ring", memory);
    int sendError = xpc_pipe_routine_reply(reply);
    xpc_release(reply); xpc_release(message->object); message->object = NULL;
    return sendError ? sendError : encodingError;
}

int LSBTransportReplyProcessIdentity(LSBReceivedMessage *message, int error, const LSBPeerProcessIdentity *identity) {
    if (!message || !message->object || error < 0 || message->kind != LSB_REQUEST_COPY_PROCESS_IDENTITY || message->requestID) return EINVAL;
    int encodingError = 0;
    pid_t nativePID = identity ? (pid_t)identity->audit.val[5] : 0;
    if (error ? identity != NULL : !identity || !identityPIDMatches(identity, nativePID) || !sendRight(identity->taskName) ||
        identity->executablePath[0] != '/' || strnlen(identity->executablePath, sizeof(identity->executablePath)) >= sizeof(identity->executablePath))
        encodingError = EINVAL;
    if (encodingError) error = encodingError;
    xpc_object_t reply = xpc_dictionary_create_reply((xpc_object_t)message->object);
    if (!reply) {
        xpc_release((xpc_object_t)message->object);
        message->object = NULL;
        return EPIPE;
    }
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", 0);
    xpc_dictionary_set_int64(reply, "error", error);
    xpc_dictionary_set_int64(reply, "pid", error ? 0 : nativePID);
    if (!error) {
        xpc_dictionary_set_data(reply, "peer-audit", &identity->audit, sizeof(identity->audit));
        xpc_dictionary_set_data(reply, "peer-short-info", identity->shortInfo, sizeof(identity->shortInfo));
        xpc_dictionary_set_string(reply, "peer-path", identity->executablePath);
        xpc_dictionary_set_mach_send(reply, "peer-task-name", identity->taskName);
    }
    int sendError = xpc_pipe_routine_reply(reply);
    xpc_release(reply);
    xpc_release((xpc_object_t)message->object);
    message->object = NULL;
    return sendError ? sendError : encodingError;
}

int LSBTransportReplyBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                               const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD) {
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, true, statusFD, wakeFD,
                          MACH_PORT_NULL, NULL, pid, getpid(), LSB_EXECUTION_SIMULATOR, LSB_NATIVE_PRODUCTION_UNKNOWN, NULL);
}

int LSBTransportReplyBootstrapWithPorts(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                        const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, const LSBSpawnPorts *ports) {
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, true, statusFD, wakeFD,
                          MACH_PORT_NULL, ports, pid, getpid(), LSB_EXECUTION_SIMULATOR, LSB_NATIVE_PRODUCTION_UNKNOWN, NULL);
}

int LSBTransportReplyBootstrapWithMetadata(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                           const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD,
                                           const LSBBootstrapMetadata *metadata) {
    if (!metadata || (metadata->launchdTankPort != MACH_PORT_NULL && !sendRight(metadata->launchdTankPort))) return EINVAL;
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, true, statusFD, wakeFD,
                          metadata->launchdTankPort, metadata->ports, metadata->guestPID, metadata->parentPID, metadata->executionRole,
                          metadata->nativeProductionStatus, &metadata->taskCapture);
}

int LSBTransportReplyLaunchdBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, mach_port_t tankPort) {
    if (!MACH_PORT_VALID(tankPort)) return EINVAL;
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, true, statusFD, wakeFD,
                          tankPort, NULL, pid, getpid(), LSB_EXECUTION_SIMULATOR, LSB_NATIVE_PRODUCTION_UNKNOWN, NULL);
}

int LSBTransportReceive(mach_port_t port, LSBReceivedMessage *message) {
    if (!message || !MACH_PORT_VALID(port)) return EINVAL;
    memset(message, 0, sizeof(*message));
    initializeFDs(message->fds);
    xpc_object_t request = NULL;
    int error = xpc_pipe_receive(port, &request, 0);
    if (error || !request) {
        if (request) xpc_release(request);
        return error ? error : EPROTO;
    }
    if (xpc_get_type(request) != XPC_TYPE_DICTIONARY) { xpc_release(request); return EPROTO; }
    message->object = request;
    xpc_dictionary_get_audit_token(request, &message->audit);
    uint64_t version = 0, kind = 0;
    int64_t childError = 0;
    if (!unsignedValue(request, "version", &version) || version != LSB_TRANSPORT_VERSION ||
        !unsignedValue(request, "kind", &kind) || kind < LSB_REQUEST_SPAWN || kind > LSB_REQUEST_CLOSE_AUDIO_OUTPUT ||
        !unsignedValue(request, "request-id", &message->requestID) || !signedValue(request, "error", &childError) ||
        childError < 0 || childError > INT_MAX) error = EPROTO;
    if (!error) {
        message->kind = (uint32_t)kind;
        message->error = (int)childError;
        if (xpc_dictionary_get_value(request, "execution-role") || xpc_dictionary_get_value(request, "bootstrap-version") ||
            xpc_dictionary_get_value(request, "native-production-status") || xpc_dictionary_get_value(request, "task-capture-port") ||
            xpc_dictionary_get_value(request, "task-capture-nonce") || xpc_dictionary_get_value(request, "process-task")) error = EPROTO;
        // This control request must carry only its four-field envelope, including no capabilities under unknown keys.
        if ((kind == LSB_REQUEST_REBOOT_USERSPACE || kind == LSB_REQUEST_LIST_PROCESSES || kind == LSB_REQUEST_OPEN_AUDIO_OUTPUT ||
             kind == LSB_REQUEST_CLOSE_AUDIO_OUTPUT) && xpc_dictionary_get_count(request) != 4) error = EPROTO;
    }
    if (!error) {
        if (kind == LSB_REQUEST_READY) {
            if (!message->requestID || xpc_dictionary_get_value(request, "plan") || xpc_dictionary_get_value(request, "fds") ||
                xpc_dictionary_get_value(request, "registered-ports") || xpc_dictionary_get_value(request, "bootstrap-port")) error = EPROTO;
        } else if (message->requestID || childError) error = EPROTO;
        else if (kind == LSB_REQUEST_SPAWN || kind == LSB_REQUEST_EXEC) {
            error = decodePlan(request, &message->plan, message->fds);
            if (!error) error = decodePorts(request, &message->ports);
        } else if (xpc_dictionary_get_value(request, "plan") || xpc_dictionary_get_value(request, "fds") ||
                   xpc_dictionary_get_value(request, "registered-ports") || xpc_dictionary_get_value(request, "bootstrap-port")) error = EPROTO;
        xpc_object_t nativeEndpoint = xpc_dictionary_get_value(request, "native-service-port");
        if (!error && nativeEndpoint) {
            if (kind != LSB_REQUEST_READY || childError || xpc_get_type(nativeEndpoint) != &_xpc_type_mach_send) error = EPROTO;
            else {
                message->nativeServicePort = xpc_dictionary_copy_mach_send(request, "native-service-port");
                if (!sendRight(message->nativeServicePort)) error = EPROTO;
            }
        }
        xpc_object_t debuggerEndpoint = xpc_dictionary_get_value(request, "process-debugger-port");
        xpc_object_t debuggerActivation = xpc_dictionary_get_value(request, "activate-debugger-port");
        if (!error && kind == LSB_REQUEST_REGISTER_DEBUGGER_PORT) {
            if (xpc_dictionary_get_count(request) != 5 || (!!debuggerEndpoint == !!debuggerActivation)) error = EPROTO;
            else if (debuggerActivation) {
                if (xpc_get_type(debuggerActivation) != XPC_TYPE_BOOL || !xpc_bool_get_value(debuggerActivation)) error = EPROTO;
                else message->activateDebuggerPort = true;
            } else if (xpc_get_type(debuggerEndpoint) != &_xpc_type_mach_send) error = EPROTO;
            else {
                message->processDebuggerPort = xpc_dictionary_copy_mach_send(request, "process-debugger-port");
                if (!sendRight(message->processDebuggerPort)) error = EPROTO;
            }
        } else if (!error && (debuggerEndpoint || debuggerActivation)) error = EPROTO;
        if (!error && kind == LSB_REQUEST_KILL) {
            int64_t target = 0, signal = 0;
            if (!signedValue(request, "target-pid", &target) || target < INT_MIN || target > INT_MAX ||
                !signedValue(request, "signal", &signal) || signal < INT_MIN || signal > INT_MAX) error = EPROTO;
            else { message->targetPID = (pid_t)target; message->signalNumber = (int)signal; }
        }
        if (!error && (kind == LSB_REQUEST_RESOLVE_AUDIT_PID || kind == LSB_REQUEST_COPY_ENTITLEMENTS)) {
            xpc_object_t target = xpc_dictionary_get_value(request, "target-audit");
            if (!target || xpc_get_type(target) != XPC_TYPE_DATA || xpc_data_get_length(target) != sizeof(message->targetAudit) ||
                xpc_dictionary_get_value(request, "target-pid") || xpc_dictionary_get_value(request, "signal")) error = EPROTO;
            else {
                memcpy(&message->targetAudit, xpc_data_get_bytes_ptr(target), sizeof(message->targetAudit));
                if (!message->targetAudit.val[5] || message->targetAudit.val[5] > INT_MAX) error = EPROTO;
            }
        } else if (!error && xpc_dictionary_get_value(request, "target-audit")) error = EPROTO;
        if (!error && kind == LSB_REQUEST_COPY_ENTITLEMENTS) {
            uint64_t operation = 0;
            if (!unsignedValue(request, "operation", &operation) || (operation != 5 && operation != 7 && operation != 16) ||
                xpc_dictionary_get_count(request) != 6) error = EPROTO;
            else message->entitlementOperation = (uint32_t)operation;
        } else if (!error && xpc_dictionary_get_value(request, "operation")) error = EPROTO;
        if (!error && kind == LSB_REQUEST_CHECKIN) {
            if (xpc_dictionary_get_count(request) != 6) error = EPROTO;
            else error = decodeIdentity(request, &message->peerIdentity, (pid_t)message->audit.val[5], &message->audit);
        } else if (!error && (xpc_dictionary_get_value(request, "self-short-info") || xpc_dictionary_get_value(request, "self-task-name"))) error = EPROTO;
        if (!error && (kind == LSB_REQUEST_COPY_PROCESS_IDENTITY || kind == LSB_REQUEST_RESOLVE_NATIVE_CHILD_PID ||
                       kind == LSB_REQUEST_COPY_DEBUGGER_INFO || kind == LSB_REQUEST_COPY_PROCESS_TASK)) {
            int64_t target = 0;
            if (xpc_dictionary_get_count(request) != 5 || !signedValue(request, "target-pid", &target) || target <= 0 || target > INT_MAX) error = EPROTO;
            else message->targetPID = (pid_t)target;
        }
    }
    if (error) {
        (void)LSBTransportReply(message, error, 0, message->requestID, NULL, NULL, NULL);
        LSBTransportDisposeMessage(message);
    }
    return error;
}

int LSBRequireSingleThread(void) {
    return LSBSingleThreadGuard();
}
