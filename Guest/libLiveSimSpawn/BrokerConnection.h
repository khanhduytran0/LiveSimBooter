#pragma once

#include <mach/mach.h>
#include <stdbool.h>
#include <xpc/xpc.h>

// Atomic-only readiness query, safe before guest TLS or libSystem constructors have run.
bool LSBBrokerConnectionIsReady(void);

// Guest-only RPC. Borrows a simulator-to-host endpoint and request; success returns one owned dictionary reply.
// The persistent connection is created after guest constructors begin. Calls before then fail without touching XPC or TLS.
int LSBBrokerConnectionCall(mach_port_t port, xpc_object_t request, xpc_object_t *reply);
