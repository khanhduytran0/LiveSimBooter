#include "../ProcessSupport/StoreAppGroups.h"
#include "../ProcessSupport/PrivateXPC.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#if defined(LSB_TEST_STORE_BROKER_DISCOVERY)
#include "../ProcessSupport/SpawnTransport.h"
#endif

static const char side[] = "group.com.SideStore.SideStore";
static const char alt[] = "group.com.rileytestut.AltStore";
static const char sideTeam[] = "group.com.SideStore.SideStore.A1B2C3D4E5";
static const char altTeam[] = "group.com.rileytestut.AltStore.Z9Y8X7W6V5";

static xpc_object_t mockEntitlement;
static unsigned copies;

// The returned object is owned, matching the native function. The fixture retains its own independent reference.
xpc_object_t xpc_copy_entitlement_for_self(const char *name) {
    assert(!strcmp(name, "com.apple.security.application-groups"));
    ++copies;
    return mockEntitlement ? xpc_retain(mockEntitlement) : NULL;
}

static void checkEmpty(const LSBStoreAppGroups *groups) {
    const LSBStoreAppGroups empty = {0};
    assert(!memcmp(groups, &empty, sizeof(empty)));
}

static void checkParse(const char *const *names, size_t count, int expected, const char *first, const char *second) {
    LSBStoreAppGroups groups;
    memset(&groups, 0xaa, sizeof(groups));
    assert(LSBParseStoreAppGroups(names, count, &groups) == expected);
    if (expected) { checkEmpty(&groups); return; }
    assert(groups.count == (second ? 2 : 1));
    assert(!strcmp(groups.identifiers[0], first));
    assert(!strcmp(groups.identifiers[1], second ? second : ""));
}

static void checkParser(void) {
    assert(LSBParseStoreAppGroups(NULL, 0, NULL) == EINVAL);
    checkParse(NULL, 0, ENOENT, NULL, NULL);
    checkParse(NULL, 1, EINVAL, NULL, NULL);
    const char *empty[] = {NULL};
    checkParse(empty, 1, EINVAL, NULL, NULL);
    const char *both[] = {alt, side};
    checkParse(both, 2, 0, side, alt);
    const char *teams[] = {altTeam, sideTeam};
    checkParse(teams, 2, 0, sideTeam, altTeam);
    const char *one[] = {altTeam};
    checkParse(one, 1, 0, altTeam, NULL);
    const char *mixed[] = {altTeam, "group.another.entitled.app", side};
    checkParse(mixed, 3, 0, side, altTeam);
    const char *duplicates[][2] = {{side, side}, {altTeam, altTeam}, {side, sideTeam}, {alt, altTeam},
        {sideTeam, "group.com.SideStore.SideStore.Z9Y8X7W6V5"}};
    for (size_t index = 0; index < sizeof(duplicates) / sizeof(duplicates[0]); ++index)
        checkParse(duplicates[index], 2, EINVAL, NULL, NULL);
    const char *unrelated[] = {"", "group.com.kdt.LiveSimBooter", "group.com.SideStore.SideStoreX", "group.com.SideStore.SideStore.",
        "group.com.SideStore.SideStore.A1B2C3D4E", "group.com.SideStore.SideStore.A1B2C3D4E56", "group.com.SideStore.SideStore.a1B2C3D4E5",
        "group.com.SideStore.SideStore.A1B2C3D4E5.more", "group.com.SideStore.SideStore.A1B2C3D4E/", "group.com.rileytestut.AltStore.123456789_"};
    checkParse(unrelated, sizeof(unrelated) / sizeof(unrelated[0]), ENOENT, NULL, NULL);
    char name[LSB_BROKER_SERVICE_BYTES];
    assert(LSBStoreBrokerServiceName(NULL, name) == EINVAL && !name[0]);
    assert(LSBStoreBrokerServiceName(side, NULL) == EINVAL);
    for (size_t index = 0; index < sizeof(unrelated) / sizeof(unrelated[0]); ++index) {
        memset(name, 0xaa, sizeof(name));
        assert(LSBStoreBrokerServiceName(unrelated[index], name) == EINVAL);
        for (size_t byte = 0; byte < sizeof(name); ++byte) assert(!name[byte]);
    }
    assert(!LSBStoreBrokerServiceName(side, name));
    assert(!strcmp(name, "group.com.SideStore.SideStore.LiveSimBooter.spawn"));
    assert(!LSBStoreBrokerServiceName(altTeam, name));
    assert(!strcmp(name, "group.com.rileytestut.AltStore.Z9Y8X7W6V5.LiveSimBooter.spawn"));
}

static void checkCopy(int expected, const char *first, const char *second) {
    xpc_object_t snapshot = mockEntitlement ? xpc_copy(mockEntitlement) : NULL;
    LSBStoreAppGroups groups;
    memset(&groups, 0xaa, sizeof(groups));
    assert(LSBCopyStoreAppGroups(&groups) == expected);
    if (snapshot) { assert(xpc_equal(mockEntitlement, snapshot)); xpc_release(snapshot); }
    if (expected) { checkEmpty(&groups); return; }
    assert(groups.count == (second ? 2 : 1));
    assert(!strcmp(groups.identifiers[0], first));
    assert(!strcmp(groups.identifiers[1], second ? second : ""));
}

static void checkNativeEntitlement(void) {
    assert(LSBCopyStoreAppGroups(NULL) == EINVAL && !copies);
    checkCopy(EACCES, NULL, NULL);
    mockEntitlement = xpc_string_create("not an array");
    checkCopy(EINVAL, NULL, NULL);
    xpc_release(mockEntitlement);
    mockEntitlement = xpc_null_create();
    checkCopy(EINVAL, NULL, NULL);
    xpc_release(mockEntitlement);
    mockEntitlement = xpc_array_create(NULL, 0);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, side);
    xpc_array_set_bool(mockEntitlement, XPC_ARRAY_APPEND, true);
    checkCopy(EINVAL, NULL, NULL);
    xpc_release(mockEntitlement);
    mockEntitlement = xpc_array_create(NULL, 0);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, altTeam);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, "group.other");
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, side);
    checkCopy(0, side, altTeam);
    LSBStoreAppGroups owned;
    assert(!LSBCopyStoreAppGroups(&owned));
    xpc_release(mockEntitlement);
    mockEntitlement = NULL;
    assert(!strcmp(owned.identifiers[0], side) && !strcmp(owned.identifiers[1], altTeam)); // Copies survive the source object's release.
    mockEntitlement = xpc_array_create(NULL, 0);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, side);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, sideTeam);
    checkCopy(EINVAL, NULL, NULL);
    xpc_release(mockEntitlement);
    // The formatted constructor preserves the embedded NUL's counted length, exercising an actual malformed XPC string.
    xpc_object_t embedded = xpc_string_create_with_format("%s%c%s", side, 0, ".A1B2C3D4E5");
    assert(strlen(xpc_string_get_string_ptr(embedded)) != xpc_string_get_length(embedded));
    mockEntitlement = xpc_array_create(&embedded, 1);
    xpc_release(embedded);
    checkCopy(EINVAL, NULL, NULL);
    xpc_release(mockEntitlement);
    char longName[256];
    memset(longName, 'a', sizeof(longName));
    longName[sizeof(longName) - 1] = '\0';
    mockEntitlement = xpc_array_create(NULL, 0);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, longName);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, "group.éxample");
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, alt);
    checkCopy(0, alt, NULL);
    xpc_release(mockEntitlement);
    mockEntitlement = xpc_array_create(NULL, 0);
    checkCopy(ENOENT, NULL, NULL);
    xpc_release(mockEntitlement);
    mockEntitlement = NULL;
    checkCopy(EACCES, NULL, NULL);
}

#if defined(LSB_TEST_STORE_BROKER_DISCOVERY)
typedef struct { const char *name; kern_return_t result; bool supplyRight; } LookupStep;
static LookupStep lookupSteps[LSB_STORE_APP_GROUP_COUNT];
static size_t lookupCount, lookupIndex;
static mach_port_t mockPort;

kern_return_t bootstrap_look_up(mach_port_t bootstrap, const char *name, mach_port_t *port) {
    assert(bootstrap == bootstrap_port && port && *port == MACH_PORT_NULL && lookupIndex < lookupCount);
    LookupStep step = lookupSteps[lookupIndex++];
    assert(!strcmp(name, step.name));
    if (step.supplyRight) {
        assert(!mach_port_insert_right(mach_task_self(), mockPort, mockPort, MACH_MSG_TYPE_MAKE_SEND));
        *port = mockPort;
    }
    return step.result;
}

static void checkLookup(int expected) {
    mach_port_t result = MACH_PORT_DEAD;
    lookupIndex = 0;
    assert(LSBTransportCopyBrokerPort(&result) == expected);
    assert(lookupIndex == lookupCount);
    mach_port_urefs_t references = 0;
    assert(!mach_port_get_refs(mach_task_self(), mockPort, MACH_PORT_RIGHT_SEND, &references));
    if (expected) assert(result == MACH_PORT_NULL && references == 1);
    else {
        assert(result == mockPort && references == 2);
        assert(!mach_port_deallocate(mach_task_self(), result));
    }
    assert(xpc_get_type(mockEntitlement) == XPC_TYPE_ARRAY && xpc_array_get_count(mockEntitlement));
}

static void checkBrokerDiscovery(void) {
    assert(LSBTransportCopyBrokerPort(NULL) == EINVAL);
    mockEntitlement = xpc_array_create(NULL, 0);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, altTeam);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, side);
    assert(mockEntitlement);
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &mockPort));
    assert(!mach_port_insert_right(mach_task_self(), mockPort, mockPort, MACH_MSG_TYPE_MAKE_SEND));
    const char *sideName = "group.com.SideStore.SideStore.LiveSimBooter.spawn";
    const char *altName = "group.com.rileytestut.AltStore.Z9Y8X7W6V5.LiveSimBooter.spawn";
    lookupCount = 1;
    lookupSteps[0] = (LookupStep){sideName, KERN_SUCCESS, true};
    checkLookup(0);
    lookupCount = 2;
    lookupSteps[0] = (LookupStep){sideName, 1102, true};
    lookupSteps[1] = (LookupStep){altName, KERN_SUCCESS, true};
    checkLookup(0); // The first failed lookup's unexpected send right is released before trying the active store.
    lookupSteps[0].result = 1100;
    checkLookup(0); // A denied store must not prevent discovery of the other entitled one.
    lookupSteps[1].result = 1102;
    checkLookup(EACCES);
    lookupSteps[0].result = 1102;
    checkLookup(ENOENT);
    lookupSteps[1].result = 1100;
    checkLookup(EACCES);
    lookupCount = 1;
    lookupSteps[0] = (LookupStep){sideName, KERN_SUCCESS, false};
    checkLookup(EIO); // Success without a send right is malformed, not an inactive-store retry.
    lookupSteps[0] = (LookupStep){sideName, KERN_INVALID_ARGUMENT, true};
    checkLookup(EIO);
    xpc_release(mockEntitlement);
    mockEntitlement = xpc_array_create(NULL, 0);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, altTeam);
    lookupSteps[0] = (LookupStep){altName, KERN_SUCCESS, true};
    checkLookup(0);
    xpc_release(mockEntitlement);
    mockEntitlement = xpc_array_create(NULL, 0);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, side);
    xpc_array_set_string(mockEntitlement, XPC_ARRAY_APPEND, side);
    lookupCount = 0;
    checkLookup(EINVAL); // Malformed entitlement discovery performs no bootstrap operation.
    xpc_release(mockEntitlement);
    mockEntitlement = NULL;
    assert(!mach_port_deallocate(mach_task_self(), mockPort));
    assert(!mach_port_mod_refs(mach_task_self(), mockPort, MACH_PORT_RIGHT_RECEIVE, -1));
    mockPort = MACH_PORT_NULL;
}
#endif

int main(void) {
    checkParser();
    checkNativeEntitlement();
#if defined(LSB_TEST_STORE_BROKER_DISCOVERY)
    checkBrokerDiscovery();
#endif
    puts("StoreAppGroupsTests passed");
    return 0;
}
