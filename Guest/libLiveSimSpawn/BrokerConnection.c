#include "BrokerConnection.h"
#include "../../ProcessSupport/PrivateXPC.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

static atomic_bool guestInitialized;
static atomic_int connectionError;
static pthread_mutex_t connectionLock = PTHREAD_MUTEX_INITIALIZER;
static xpc_connection_t brokerConnection;
static mach_port_t brokerEndpoint;
static _Thread_local bool creatingConnection;

// libSystem initialization precedes inserted-dylib constructors. No native XPC object is carried into this image.
__attribute__((constructor(101))) static void enableBrokerConnection(void) {
    atomic_store_explicit(&guestInitialized, true, memory_order_release);
}

bool LSBBrokerConnectionIsReady(void) {
    return atomic_load_explicit(&guestInitialized, memory_order_acquire);
}

static int copyConnection(mach_port_t port, xpc_connection_t *result) {
    *result = NULL;
    if (creatingConnection) return EDEADLK;
    creatingConnection = true;
    int error = pthread_mutex_lock(&connectionLock);
    if (error) { creatingConnection = false; return error; }
    error = atomic_load_explicit(&connectionError, memory_order_acquire);
    if (!error && brokerConnection && port != brokerEndpoint) error = EINVAL;
    if (!error && !brokerConnection) {
        // The private endpoint constructor consumes its send reference; our caller retains ownership of the supplied one.
        if (mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1)) error = EACCES;
        xpc_endpoint_t endpoint = error ? NULL : xpc_endpoint_create_mach_port_4sim(port);
        if (!error && !endpoint) { mach_port_deallocate(mach_task_self(), port); error = ENOMEM; }
        xpc_connection_t connection = endpoint ? xpc_connection_create_from_endpoint(endpoint) : NULL;
        if (endpoint) xpc_release(endpoint);
        if (!error && !connection) error = ENOMEM;
        if (connection) {
            xpc_connection_enable_sim2host_4sim(connection);
            xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
                if (xpc_get_type(event) == XPC_TYPE_ERROR) {
                    atomic_store_explicit(&connectionError, EPIPE, memory_order_release);
                    xpc_connection_cancel(connection);
                }
            });
            // Publish exactly one connection. Guest dispatch owns its event handling; no lock remains held during a synchronous request.
            brokerEndpoint = port;
            brokerConnection = connection;
            xpc_connection_activate(connection);
        }
    }
    if (!error) *result = xpc_retain(brokerConnection);
    pthread_mutex_unlock(&connectionLock);
    creatingConnection = false;
    return error;
}

int LSBBrokerConnectionCall(mach_port_t port, xpc_object_t request, xpc_object_t *reply) {
    if (!reply) return EINVAL;
    *reply = NULL;
    // Entitlement interposers can be reached before guest TLS is usable. Keep this guard ahead of pthread/XPC calls.
    if (!LSBBrokerConnectionIsReady()) return ENOTSUP;
    if (!MACH_PORT_VALID(port) || !request || xpc_get_type(request) != XPC_TYPE_DICTIONARY) return EINVAL;
    xpc_connection_t connection = NULL;
    int error = copyConnection(port, &connection);
    if (error) return error;
    xpc_object_t response = xpc_connection_send_message_with_reply_sync(connection, request);
    if (!response || xpc_get_type(response) == XPC_TYPE_ERROR) {
        // A request may have been committed before interruption. Never resend it or silently open a replacement connection.
        error = EPIPE;
        atomic_store_explicit(&connectionError, error, memory_order_release);
        xpc_connection_cancel(connection);
    } else if (xpc_get_type(response) != XPC_TYPE_DICTIONARY) error = EPROTO;
    else {
        audit_token_t sender = {{0}}, peer = {{0}};
        xpc_dictionary_get_audit_token(response, &sender);
        xpc_connection_get_audit_token(connection, &peer);
        if (!sender.val[5] || sender.val[5] > INT_MAX || !sender.val[7] || memcmp(&sender, &peer, sizeof(sender))) error = EACCES;
    }
    xpc_release(connection);
    if (!error) *reply = response;
    else if (response) xpc_release(response);
    return error;
}
