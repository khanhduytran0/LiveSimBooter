#ifndef LSB_PRIVATE_XPC_H
#define LSB_PRIVATE_XPC_H

#include <mach/mach.h>
#include <stdint.h>
#include <uuid/uuid.h>
#include <xpc/xpc.h>

#ifdef __cplusplus
extern "C" {
#endif

// Private synchronous pipe interface exported by libSystem on the supported iOS runtime.
extern xpc_object_t xpc_pipe_create_from_port(mach_port_t port, uint32_t flags);
extern int xpc_pipe_routine(xpc_object_t pipe, xpc_object_t message, xpc_object_t *reply);
extern int xpc_pipe_receive(mach_port_t port, xpc_object_t *message, uint64_t flags);
extern int xpc_pipe_routine_reply(xpc_object_t reply);
// Returns an owned entitlement value for the current native process, or NULL when unavailable.
extern xpc_object_t xpc_copy_entitlement_for_self(const char *name);
extern void xpc_dictionary_get_audit_token(xpc_object_t dictionary, audit_token_t *token);
extern const struct _xpc_type_s _xpc_type_mach_send;
extern void xpc_dictionary_set_mach_send(xpc_object_t dictionary, const char *key, mach_port_t port);
extern mach_port_t xpc_dictionary_copy_mach_send(xpc_object_t dictionary, const char *key);
extern void xpc_array_set_mach_send(xpc_object_t array, size_t index, mach_port_t port);
extern mach_port_t xpc_array_copy_mach_send(xpc_object_t array, size_t index);
extern mach_port_t xpc_mach_send_get_right(xpc_object_t object);
// Synchronous launchd startup notification; this does not initialize an NSXPC listener or extension context.
extern void launch_extension_check_in_live_4UIKit(void);
// Native launchd service-instance signaling. Neither call requires activating the connection or starting an NSXPC handshake.
extern void xpc_connection_set_oneshot_instance(xpc_connection_t connection, const uuid_t instance);
extern void xpc_connection_kill(xpc_connection_t connection, int signal);

extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_check_in(mach_port_t bootstrap, const char *name, mach_port_t *port);
extern kern_return_t bootstrap_look_up(mach_port_t bootstrap, const char *name, mach_port_t *port);

#ifdef __cplusplus
}
#endif
#endif
