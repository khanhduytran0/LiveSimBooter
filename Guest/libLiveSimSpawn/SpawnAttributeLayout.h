#ifndef LSB_SPAWN_ATTRIBUTE_LAYOUT_H
#define LSB_SPAWN_ATTRIBUTE_LAYOUT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    LSBSpawnAttributeLayoutUnknown,
    LSBSpawnAttributeLayoutModern,
    LSBSpawnAttributeLayout23B50,
    LSBSpawnAttributeLayout23H128,
} LSBSpawnAttributeLayout;

// Identify the private layout by native setters on a disposable object, not by compiler instructions or the host OS version.
// These three field positions distinguish the inspected 248-byte variants and the 256-byte layout.
enum { LSBSpawnAttributeProbeSize = 0xf8, LSBSpawnAttributeProbeCount = 0x13579bdf, LSBSpawnAttributeProbeTimeout = 0x2468ace0 };
static inline LSBSpawnAttributeLayout LSBSpawnAttributeProbeLayout(const unsigned char *before, const unsigned char *after, size_t allocationSize) {
    static const struct { size_t size, crash, tecs, ports; LSBSpawnAttributeLayout layout; } profiles[] = {
        {0xf8, 0xac, 0x69, 0xc0, LSBSpawnAttributeLayout23B50},
        {0xf8, 0xac, 0x6a, 0xc0, LSBSpawnAttributeLayout23H128},
        {0x100, 0xb4, 0x6a, 0xc8, LSBSpawnAttributeLayoutModern},
    };
    if (!before || !after || allocationSize < LSBSpawnAttributeProbeSize) return LSBSpawnAttributeLayoutUnknown;
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        if (allocationSize < profiles[i].size) continue;
        uint64_t oldPorts, newPorts;
        memcpy(&oldPorts, before + profiles[i].ports, sizeof(oldPorts));
        memcpy(&newPorts, after + profiles[i].ports, sizeof(newPorts));
        if (oldPorts || !newPorts || before[profiles[i].tecs]) continue;
        unsigned char expected[LSBSpawnAttributeProbeSize];
        memcpy(expected, before, sizeof(expected));
        const uint32_t crash[] = {LSBSpawnAttributeProbeCount, LSBSpawnAttributeProbeTimeout};
        memcpy(expected + profiles[i].crash, crash, sizeof(crash));
        memcpy(expected + profiles[i].ports, &newPorts, sizeof(newPorts));
        expected[profiles[i].tecs] = 1;
        if (!memcmp(expected, after, sizeof(expected))) return profiles[i].layout;
    }
    return LSBSpawnAttributeLayoutUnknown;
}

static inline size_t LSBSpawnAttributePortActionsOffset(LSBSpawnAttributeLayout layout) {
    return layout == LSBSpawnAttributeLayoutModern ? 0xc8 : 0xc0;
}

// Only unsupported initialized fields are compared. Standard POSIX fields and separately decoded fields are excluded.
// 23B50 writes a uint16_t at 0x68; 23H128/24A435 additionally initialize byte 0x6a. Both older layouts keep ports at 0xc0.
static inline int LSBSpawnAttributeFindDifference(LSBSpawnAttributeLayout layout, const unsigned char *attributes,
                                                 const unsigned char *defaults, size_t *offset) {
    static const uint16_t modern[][2] = {
        {32, 36}, {40, 66}, {68, 88}, {92, 107}, {108, 112}, {132, 152}, {160, 169}, {172, 177}, {180, 200}, {208, 256}
    };
    static const uint16_t old26[][2] = {
        {32, 36}, {40, 88}, {92, 106}, {108, 112}, {132, 152}, {160, 169}, {170, 192}, {200, 248}
    };
    if (layout == LSBSpawnAttributeLayoutUnknown || !attributes || !defaults || !offset) return -1;
    const uint16_t (*ranges)[2] = layout == LSBSpawnAttributeLayoutModern ? modern : old26;
    size_t count = layout == LSBSpawnAttributeLayoutModern ? sizeof(modern) / sizeof(modern[0]) : sizeof(old26) / sizeof(old26[0]);
    for (size_t i = 0; i < count; ++i) {
        size_t end = ranges[i][1];
        if (layout == LSBSpawnAttributeLayout23H128 && end == 106) end = 107;
        for (size_t at = ranges[i][0]; at < end; ++at) {
            if (attributes[at] != defaults[at]) { *offset = at; return 1; }
        }
    }
    return 0;
}

#endif
