#ifndef LSB_SYSTEM_POWER_COMPATIBILITY_H
#define LSB_SYSTEM_POWER_COMPATIBILITY_H

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>

#define LSB_SYSTEM_POWER_ENABLE_ENV "LSB_SIMULATE_SYSTEM_POWER"
#define LSB_SYSTEM_POWER_MAX_REGISTRATIONS 64u

#ifdef __cplusplus
extern "C" {
#endif

// These IOPMLib C functions are exported by simulator IOKit but absent from its public SDK headers.
io_connect_t IORegisterForSystemPower(void *refcon, IONotificationPortRef *port, IOServiceInterestCallback callback, io_object_t *notifier);
IOReturn IODeregisterForSystemPower(io_object_t *notifier);
IOReturn IOAllowPowerChange(io_connect_t connection, intptr_t notificationID);
IOReturn IOCancelPowerChange(io_connect_t connection, intptr_t notificationID);

// Opt-in always-awake subscriptions: no host power registration, sleep events, or kernel acknowledgment IDs.
// The caller owns the real notification port. Connection and notifier rights must be released through these wrappers;
// arbitrary IOConnect operations and direct IOObjectRetain/Release on those virtual handles are unsupported.
io_connect_t LSBRegisterForSystemPower(void *refcon, IONotificationPortRef *port, IOServiceInterestCallback callback, io_object_t *notifier);
IOReturn LSBDeregisterForSystemPower(io_object_t *notifier);
kern_return_t LSBSystemPowerServiceClose(io_connect_t connection);
IOReturn LSBAllowPowerChange(io_connect_t connection, intptr_t notificationID);
IOReturn LSBCancelPowerChange(io_connect_t connection, intptr_t notificationID);

#ifdef __cplusplus
}
#endif
#endif
