#include "../Guest/SpawnAttributes.h"
#include "../Guest/SpawnAttributeLayout.h"

#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/machine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct { int32_t kind; uint32_t mask; mach_port_t port; int32_t behavior, flavor, which; } TestPortAction;
typedef struct { int32_t allocated, count; TestPortAction actions[133]; } TestPortActions;

static void put32(posix_spawnattr_t attr, size_t offset, uint32_t value) { memcpy((char *)attr + offset, &value, sizeof(value)); }
static void putPorts(posix_spawnattr_t attr, TestPortActions *ports) { memcpy((char *)attr + 0xc8, &ports, sizeof(ports)); }

static void testPolicyAndArchitecture(void) {
    LSBAdditionalSpawnAttributes decoded;
    assert(LSBReadAdditionalSpawnAttributes(NULL, &decoded) == 0 && decoded.ports.count == 0);
    assert(LSBReadAdditionalSpawnAttributes(NULL, NULL) == EINVAL);
    posix_spawnattr_t attr = NULL;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == EINVAL);
    assert(posix_spawnattr_init(&attr) == 0);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    assert(posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT) == 0);
    sigset_t signals = 0xffffffffu;
    assert(posix_spawnattr_setsigdefault(&attr, &signals) == 0);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    put32(attr, 0x24, 0x100);
    put32(attr, 0x58, 1);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    assert(decoded.processType == 0x100 && decoded.darwinRole == 1);
    put32(attr, 0x24, 0x300);
    put32(attr, 0x58, 7);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    assert(decoded.processType == 0x300 && decoded.darwinRole == 7);

    put32(attr, 0x10, CPU_TYPE_ARM64);
    put32(attr, 0x70, CPU_SUBTYPE_ARM64_ALL);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    put32(attr, 0x70, CPU_SUBTYPE_MULTIPLE);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    put32(attr, 0x14, CPU_TYPE_X86_64);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    put32(attr, 0x70, CPU_SUBTYPE_ARM64E);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP);
    put32(attr, 0x70, CPU_SUBTYPE_MULTIPLE);
    put32(attr, 0x10, CPU_TYPE_X86_64);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP);
    put32(attr, 0x10, 0); // A preference after the terminating zero cannot be treated as an arm64-only preference.
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP);
    put32(attr, 0x14, 0);
    put32(attr, 0x70, CPU_SUBTYPE_ARM64_ALL);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP);
    assert(posix_spawnattr_destroy(&attr) == 0);
}

static void testUnsupportedFields(void) {
    posix_spawnattr_t attr;
    assert(posix_spawnattr_init(&attr) == 0);
    LSBAdditionalSpawnAttributes decoded;
    static const uint16_t fields[][2] = {
        {32, 36}, {40, 66}, {68, 88}, {92, 107}, {108, 112}, {128, 156}, {160, 169}, {172, 177}, {180, 200}, {208, 256}
    };
    for (size_t range = 0; range < sizeof(fields) / sizeof(fields[0]); ++range) {
        for (size_t offset = fields[range][0]; offset < fields[range][1]; ++offset) {
            unsigned char *byte = (unsigned char *)attr + offset;
            *byte ^= 1;
            assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP);
            *byte ^= 1;
        }
    }
    // Native init leaves these gaps unspecified. Different padding bytes must not reject otherwise equivalent attributes.
    static const uint16_t padding[][2] = {{66, 68}, {107, 108}, {156, 160}, {169, 172}, {177, 180}};
    for (size_t i = 0; i < sizeof(padding) / sizeof(padding[0]); ++i)
        memset((char *)attr + padding[i][0], 0xa5, padding[i][1] - padding[i][0]);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    assert(posix_spawnattr_destroy(&attr) == 0);
}

static void testLocalFilePolicy(void) {
    posix_spawnattr_t attr;
    assert(!posix_spawnattr_init(&attr));
    LSBAdditionalSpawnAttributes decoded;
    static const struct { uint32_t options, policy; int error; } cases[] = {
        {0, 0, 0}, {4, 5, 0}, {4, 6, 0},
        {0, 5, ENOTSUP}, {0, 6, ENOTSUP}, {4, 0, ENOTSUP}, {4, 1, ENOTSUP}, {4, 2, ENOTSUP}, {4, 4, ENOTSUP},
        {4, 7, ENOTSUP}, {4, UINT32_MAX, ENOTSUP}, {5, 5, ENOTSUP}, {6, 6, ENOTSUP}, {7, 6, ENOTSUP}, {UINT32_MAX, 6, ENOTSUP}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        put32(attr, 0x80, cases[i].options);
        put32(attr, 0x98, cases[i].policy);
        unsigned char snapshot[0x100];
        memcpy(snapshot, attr, sizeof(snapshot));
        assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == cases[i].error);
        assert(!memcmp(snapshot, attr, sizeof(snapshot)));
        assert(!decoded.ports.count && !decoded.processType && !decoded.darwinRole);
    }
    // A valid dataless request cannot hide another option bit or an adjacent resource-limit request.
    put32(attr, 0x98, 6);
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (bit == 2) continue;
        put32(attr, 0x80, 4 | (1u << bit));
        assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP);
    }
    put32(attr, 0x80, 4);
    put32(attr, 0x84, 1);
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP);
    put32(attr, 0x84, 0);
    assert(!LSBReadAdditionalSpawnAttributes(&attr, &decoded));
    assert(!posix_spawnattr_destroy(&attr));
}

static void testPortActions(void) {
    posix_spawnattr_t attr;
    assert(posix_spawnattr_init(&attr) == 0);
    TestPortActions *ports = calloc(1, sizeof(*ports));
    assert(ports);
    putPorts(attr, ports);
    ports->allocated = 4;
    ports->count = 3;
    mach_port_t port;
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    for (size_t i = 0; i < 3; ++i) ports->actions[i].kind = 4;
    ports->actions[0].port = port;
    ports->actions[2].port = port;
    LSBAdditionalSpawnAttributes decoded;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    assert(decoded.ports.registeredPresent && decoded.ports.count == 3 && decoded.ports.registered[0] == port);
    assert(decoded.ports.registered[1] == MACH_PORT_NULL && decoded.ports.registered[2] == port);
    mach_port_urefs_t refs;
    assert(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 1);
    ports->actions[1].port = MACH_PORT_DEAD;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0 && decoded.ports.registered[1] == MACH_PORT_DEAD);
    ports->actions[1].port = MACH_PORT_NULL;
    ports->actions[3] = (TestPortAction){.kind = 0, .port = port, .which = TASK_BOOTSTRAP_PORT};
    ports->count = 4;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    assert(decoded.ports.bootstrapPresent && decoded.ports.bootstrapPort == port);
    ports->actions[3].port = MACH_PORT_NULL;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0);
    assert(decoded.ports.bootstrapPresent && decoded.ports.bootstrapPort == MACH_PORT_NULL);
    ports->actions[3].which = TASK_HOST_PORT;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP);
    ports->count = 3;
    for (int kind = 0; kind <= 6; ++kind) {
        if (kind == 3 || kind == 4) continue;
        ports->actions[1].kind = kind;
        assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP && decoded.ports.count == 0);
    }
    ports->actions[1].kind = 4;
    uint32_t *unused[] = {&ports->actions[1].mask, (uint32_t *)&ports->actions[1].behavior,
                          (uint32_t *)&ports->actions[1].flavor, (uint32_t *)&ports->actions[1].which};
    for (size_t i = 0; i < 4; ++i) {
        *unused[i] = 1;
        assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == EINVAL);
        *unused[i] = 0;
    }
    ports->count = -1;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == EINVAL);
    ports->count = 4;
    ports->actions[3] = (TestPortAction){.kind = 4};
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == E2BIG);
    ports->allocated = 2;
    ports->count = 3;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == EINVAL);
    ports->count = 0;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == 0 && !decoded.ports.registeredPresent && decoded.ports.count == 0);
    assert(posix_spawnattr_destroy(&attr) == 0);
    assert(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 1);
    assert(mach_port_deallocate(mach_task_self(), port) == KERN_SUCCESS);
    assert(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS);
}

// Use the real setter once to verify the pinned action shape independently of the fabricated malformed cases below.
extern int posix_spawnattr_set_importancewatch_port_np(posix_spawnattr_t *, int, mach_port_t *);
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *, mach_port_t *, unsigned int);

static void testImportanceWatchHints(void) {
    posix_spawnattr_t attr;
    assert(!posix_spawnattr_init(&attr));
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
    mach_port_t importance[128], registered[] = {port, MACH_PORT_NULL};
    for (unsigned i = 0; i < 128; ++i) importance[i] = port;
    assert(!posix_spawnattr_set_importancewatch_port_np(&attr, 21, importance));
    assert(!posix_spawnattr_set_registered_ports_np(&attr, registered, 2));
    LSBAdditionalSpawnAttributes decoded;
    assert(!LSBReadAdditionalSpawnAttributes(&attr, &decoded));
    assert(decoded.ports.registeredPresent && decoded.ports.count == 2);
    assert(decoded.ports.registered[0] == port && decoded.ports.registered[1] == MACH_PORT_NULL && !decoded.ports.bootstrapPresent);
    assert(!posix_spawnattr_destroy(&attr));

    assert(!posix_spawnattr_init(&attr));
    TestPortActions *ports = calloc(1, sizeof(*ports));
    assert(ports);
    putPorts(attr, ports);
    ports->allocated = 133;
    ports->count = 24;
    for (int i = 0; i < ports->count; ++i) ports->actions[i] = (TestPortAction){.kind = 3, .port = port};
    ports->actions[3] = (TestPortAction){.kind = 4, .port = port};
    ports->actions[14] = (TestPortAction){.kind = 0, .port = port, .which = TASK_BOOTSTRAP_PORT};
    ports->actions[22] = (TestPortAction){.kind = 4, .port = MACH_PORT_NULL};
    assert(!LSBReadAdditionalSpawnAttributes(&attr, &decoded));
    assert(decoded.ports.count == 2 && decoded.ports.registered[0] == port && decoded.ports.registered[1] == MACH_PORT_NULL);
    assert(decoded.ports.bootstrapPresent && decoded.ports.bootstrapPort == port);
    uint32_t *unused[] = {&ports->actions[0].mask, (uint32_t *)&ports->actions[0].behavior,
                          (uint32_t *)&ports->actions[0].flavor, (uint32_t *)&ports->actions[0].which};
    for (unsigned i = 0; i < 4; ++i) {
        *unused[i] = 1;
        assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == EINVAL && !decoded.ports.count && !decoded.ports.bootstrapPresent);
        *unused[i] = 0;
    }
    ports->actions[23].kind = 2;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP && !decoded.ports.count && !decoded.ports.bootstrapPresent);
    for (unsigned i = 0; i < 133; ++i) ports->actions[i] = (TestPortAction){.kind = 3, .port = port};
    ports->count = 128;
    assert(!LSBReadAdditionalSpawnAttributes(&attr, &decoded) && !decoded.ports.count && !decoded.ports.registeredPresent);
    ports->count = 129;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == E2BIG);
    ports->count = 132;
    for (unsigned i = 128; i < 131; ++i) ports->actions[i] = (TestPortAction){.kind = 4, .port = port};
    ports->actions[131] = (TestPortAction){.kind = 0, .port = port, .which = TASK_BOOTSTRAP_PORT};
    assert(!LSBReadAdditionalSpawnAttributes(&attr, &decoded) && decoded.ports.count == 3 && decoded.ports.bootstrapPresent);
    ports->actions[127] = (TestPortAction){.kind = 4, .port = port};
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == E2BIG && !decoded.ports.count);
    ports->actions[127] = (TestPortAction){.kind = 3, .port = port};
    ports->actions[130] = ports->actions[131];
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == ENOTSUP && !decoded.ports.count);
    ports->actions[130] = (TestPortAction){.kind = 4, .port = port};
    ports->count = 133;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == E2BIG);
    ports->count = 132;
    ports->allocated = 131;
    assert(LSBReadAdditionalSpawnAttributes(&attr, &decoded) == EINVAL);
    assert(!posix_spawnattr_destroy(&attr));
    mach_port_urefs_t references;
    assert(!mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &references) && references == 1);
    assert(!mach_port_deallocate(mach_task_self(), port));
    assert(!mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
}

// Setter offsets independently recorded from extracted native libraries; padding is deliberately nonzero.
static void testNativeLayoutProfiles(void) {
    static const struct { LSBSpawnAttributeLayout layout; size_t size, crash, tecs, ports; } profiles[] = {
        {LSBSpawnAttributeLayout23B50, 0xf8, 0xac, 0x69, 0xc0},
        {LSBSpawnAttributeLayout23H128, 0xf8, 0xac, 0x6a, 0xc0},
        {LSBSpawnAttributeLayoutModern, 0x100, 0xb4, 0x6a, 0xc8},
    };
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        unsigned char before[0xf8], after[0xf8];
        memset(before, 0xa5, sizeof(before));
        memset(before + profiles[i].ports, 0, 8);
        memset(before + profiles[i].crash, 0, 8);
        before[profiles[i].tecs] = 0;
        memcpy(after, before, sizeof(after));
        const uint32_t crash[] = {0x13579bdf, 0x2468ace0};
        const uintptr_t ports = 0x123456780;
        memcpy(after + profiles[i].ports, &ports, sizeof(ports));
        memcpy(after + profiles[i].crash, crash, sizeof(crash));
        after[profiles[i].tecs] = 1;
        assert(LSBSpawnAttributeProbeLayout(before, after, profiles[i].size) == profiles[i].layout);
        assert(LSBSpawnAttributeProbeLayout(before, after, profiles[i].size - 1) == LSBSpawnAttributeLayoutUnknown);
        // The allocator may round the old 248-byte request to 256; it must not select the modern layout.
        assert(LSBSpawnAttributeProbeLayout(before, after, 0x100) == profiles[i].layout);
        after[0x30] ^= 1;
        assert(LSBSpawnAttributeProbeLayout(before, after, profiles[i].size) == LSBSpawnAttributeLayoutUnknown);
        after[0x30] ^= 1;
        after[profiles[i].tecs] = 0;
        assert(LSBSpawnAttributeProbeLayout(before, after, profiles[i].size) == LSBSpawnAttributeLayoutUnknown);
        after[profiles[i].tecs] = 1;
        after[profiles[i].crash] ^= 1;
        assert(LSBSpawnAttributeProbeLayout(before, after, profiles[i].size) == LSBSpawnAttributeLayoutUnknown);
        after[profiles[i].crash] ^= 1;
        memset(after + profiles[i].ports, 0, 8);
        assert(LSBSpawnAttributeProbeLayout(before, after, profiles[i].size) == LSBSpawnAttributeLayoutUnknown);
    }
    assert(LSBSpawnAttributeProbeLayout(NULL, NULL, 0) == LSBSpawnAttributeLayoutUnknown);
    assert(LSBSpawnAttributePortActionsOffset(LSBSpawnAttributeLayoutModern) == 0xc8);
    assert(LSBSpawnAttributePortActionsOffset(LSBSpawnAttributeLayout23B50) == 0xc0);
    assert(LSBSpawnAttributePortActionsOffset(LSBSpawnAttributeLayout23H128) == 0xc0);

    size_t page = (size_t)getpagesize();
    unsigned char *mapping = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(mapping != MAP_FAILED);
    assert(!mprotect(mapping + page, page, PROT_NONE) && !mprotect(mapping + 3 * page, page, PROT_NONE));
    unsigned char *attributes = mapping + page - 0xf8, *defaults = mapping + 3 * page - 0xf8;
    memset(attributes, 0x5a, 0xf8);
    memset(defaults, 0x5a, 0xf8);
    // Guard pages prove there is no comparison of the rounded allocator tail at 0xf8..0xff.
    size_t difference = SIZE_MAX;
    assert(!LSBSpawnAttributeFindDifference(LSBSpawnAttributeLayout23B50, attributes, defaults, &difference));
    static const uint16_t padding[][2] = {{0x6a, 0x6c}, {0x9c, 0xa0}, {0xa9, 0xaa}};
    for (size_t i = 0; i < sizeof(padding) / sizeof(padding[0]); ++i)
        memset(attributes + padding[i][0], 0xa5, padding[i][1] - padding[i][0]);
    memset(attributes + 0xc0, 0, 8); // Borrowed port actions are decoded separately, not rejected as an unknown field.
    assert(!LSBSpawnAttributeFindDifference(LSBSpawnAttributeLayout23B50, attributes, defaults, &difference));
    static const uint16_t unsupported[][2] = {
        {32, 36}, {40, 88}, {92, 106}, {108, 112}, {132, 152}, {160, 169}, {170, 192}, {200, 248}
    };
    for (size_t range = 0; range < sizeof(unsupported) / sizeof(unsupported[0]); ++range) {
        for (size_t at = unsupported[range][0]; at < unsupported[range][1]; ++at) {
            attributes[at] ^= 1;
            assert(LSBSpawnAttributeFindDifference(LSBSpawnAttributeLayout23B50, attributes, defaults, &difference) == 1);
            assert(difference == at);
            attributes[at] ^= 1;
        }
    }
    // 23H128 still has the old extent/port layout, but byte 0x6a is now a real initialized control field.
    assert(LSBSpawnAttributeFindDifference(LSBSpawnAttributeLayout23H128, attributes, defaults, &difference) == 1 && difference == 0x6a);
    attributes[0x6a] = defaults[0x6a];
    assert(!LSBSpawnAttributeFindDifference(LSBSpawnAttributeLayout23H128, attributes, defaults, &difference));
    assert(LSBSpawnAttributeFindDifference(LSBSpawnAttributeLayoutUnknown, attributes, defaults, &difference) == -1);
    assert(!munmap(mapping, 4 * page));
}

int main(void) {
    testNativeLayoutProfiles();
    testPolicyAndArchitecture();
    testUnsupportedFields();
    testLocalFilePolicy();
    testPortActions();
    testImportanceWatchHints();
    puts("SpawnAttributesTests passed");
    return 0;
}
