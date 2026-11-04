#include "SystemPowerCompatibility.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool occupied;
    io_connect_t connection;
    io_object_t notifier;
} PowerRegistration;

static pthread_mutex_t registrationLock = PTHREAD_MUTEX_INITIALIZER;
static PowerRegistration registrations[LSB_SYSTEM_POWER_MAX_REGISTRATIONS];

static PowerRegistration *findRegistration(mach_port_name_t handle, bool connection) {
    if (!handle) return NULL;
    for (unsigned i = 0; i < LSB_SYSTEM_POWER_MAX_REGISTRATIONS; ++i) {
        PowerRegistration *registration = &registrations[i];
        if (registration->occupied && (connection ? registration->connection : registration->notifier) == handle) return registration;
    }
    return NULL;
}

io_connect_t LSBRegisterForSystemPower(void *refcon, IONotificationPortRef *port, IOServiceInterestCallback callback, io_object_t *notifier) {
    const char *enabled = getenv(LSB_SYSTEM_POWER_ENABLE_ENV);
    if (!enabled || strcmp(enabled, "1")) return IORegisterForSystemPower(refcon, port, callback, notifier);
    if (port) *port = NULL;
    if (notifier) *notifier = IO_OBJECT_NULL;
    if (!port || !notifier || !callback) return IO_OBJECT_NULL;

    PowerRegistration *registration = NULL;
    pthread_mutex_lock(&registrationLock);
    for (unsigned i = 0; i < LSB_SYSTEM_POWER_MAX_REGISTRATIONS; ++i) {
        if (registrations[i].occupied) continue;
        registration = &registrations[i];
        registration->occupied = true;
        break;
    }
    pthread_mutex_unlock(&registrationLock);
    if (!registration) return IO_OBJECT_NULL;

    // Reserve the table slot before allocating, but never hold its lock across IOKit initialization.
    IONotificationPortRef notificationPort = IONotificationPortCreate(MACH_PORT_NULL);
    mach_port_name_t connection = MACH_PORT_NULL, notification = MACH_PORT_NULL;
    if (!notificationPort || mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_DEAD_NAME, &connection) != KERN_SUCCESS ||
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_DEAD_NAME, &notification) != KERN_SUCCESS) {
        if (connection) mach_port_deallocate(mach_task_self(), connection);
        if (notificationPort) IONotificationPortDestroy(notificationPort);
        pthread_mutex_lock(&registrationLock);
        *registration = (PowerRegistration){0};
        pthread_mutex_unlock(&registrationLock);
        return IO_OBJECT_NULL;
    }

    pthread_mutex_lock(&registrationLock);
    registration->connection = connection;
    registration->notifier = notification;
    *port = notificationPort;
    *notifier = notification;
    pthread_mutex_unlock(&registrationLock);
    // No callback/refcon is stored: this guest never emits a sleep transition or acknowledgment ID.
    return connection;
}

static kern_return_t releaseHandle(PowerRegistration *registration, bool connection) {
    mach_port_name_t *handle = connection ? &registration->connection : &registration->notifier;
    kern_return_t result = mach_port_deallocate(mach_task_self(), *handle);
    if (result == KERN_SUCCESS) {
        *handle = MACH_PORT_NULL;
        if (!registration->connection && !registration->notifier) registration->occupied = false;
    }
    return result;
}

IOReturn LSBDeregisterForSystemPower(io_object_t *notifier) {
    if (!notifier) return kIOReturnBadArgument;
    pthread_mutex_lock(&registrationLock);
    PowerRegistration *registration = findRegistration(*notifier, false);
    if (registration) {
        kern_return_t result = releaseHandle(registration, false);
        if (result == KERN_SUCCESS) *notifier = IO_OBJECT_NULL;
        pthread_mutex_unlock(&registrationLock);
        return result;
    }
    bool wrongKind = findRegistration(*notifier, true) != NULL;
    pthread_mutex_unlock(&registrationLock);
    return wrongKind ? kIOReturnBadArgument : IODeregisterForSystemPower(notifier);
}

kern_return_t LSBSystemPowerServiceClose(io_connect_t connection) {
    pthread_mutex_lock(&registrationLock);
    PowerRegistration *registration = findRegistration(connection, true);
    if (registration) {
        kern_return_t result = releaseHandle(registration, true);
        pthread_mutex_unlock(&registrationLock);
        return result;
    }
    bool wrongKind = findRegistration(connection, false) != NULL;
    pthread_mutex_unlock(&registrationLock);
    return wrongKind ? kIOReturnBadArgument : IOServiceClose(connection);
}

static bool virtualHandle(io_connect_t connection) {
    pthread_mutex_lock(&registrationLock);
    bool found = findRegistration(connection, true) || findRegistration(connection, false);
    pthread_mutex_unlock(&registrationLock);
    return found;
}

IOReturn LSBAllowPowerChange(io_connect_t connection, intptr_t notificationID) {
    return virtualHandle(connection) ? kIOReturnUnsupported : IOAllowPowerChange(connection, notificationID);
}

IOReturn LSBCancelPowerChange(io_connect_t connection, intptr_t notificationID) {
    return virtualHandle(connection) ? kIOReturnUnsupported : IOCancelPowerChange(connection, notificationID);
}

#ifndef LSB_SYSTEM_POWER_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBRegisterForSystemPower, (const void *)&IORegisterForSystemPower},
    {(const void *)&LSBDeregisterForSystemPower, (const void *)&IODeregisterForSystemPower},
    {(const void *)&LSBSystemPowerServiceClose, (const void *)&IOServiceClose},
    {(const void *)&LSBAllowPowerChange, (const void *)&IOAllowPowerChange},
    {(const void *)&LSBCancelPowerChange, (const void *)&IOCancelPowerChange}
};
#endif
