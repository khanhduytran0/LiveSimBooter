#include "SpawnDiagnostics.h"
#include "SpawnAttributes.h"
#include "SpawnAttributeLayout.h"

#include <errno.h>
#include <mach/machine.h>
#include <malloc/malloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#ifdef LSB_TRANSPORT_TRACE
#include <stdio.h>
#endif

// Original Mac support and inspected native 23B50/23H128/24A435 keep this action shape; their owning attribute offsets differ.
enum { NativeImportanceWatchPortAction = 3, NativeRegisteredPortAction = 4, NativeMaxImportanceWatchPorts = 128 };
enum { NativeOptionsOffset = 0x80, NativeDatalessPolicyOffset = 0x98, NativeOptionDatalessPolicy = 4,
       NativeDatalessOffOriginal = 5, NativeDatalessOnOriginal = 6 };
typedef struct {
    int32_t kind;
    uint32_t mask;
    mach_port_t port;
    int32_t behavior, flavor, which;
} NativePortAction;
typedef struct { int32_t allocated, count; NativePortAction actions[]; } NativePortActions;
_Static_assert(sizeof(void *) == 8, "Pinned spawn attributes require the 64-bit Darwin ABI");
_Static_assert(sizeof(NativePortAction) == 24, "Pinned port-action stride changed");
_Static_assert(offsetof(NativePortActions, actions) == 8, "Pinned port-action header changed");

#ifdef LSB_TRANSPORT_TRACE
static _Atomic(unsigned) attributeTraceErrors;
#define ATTRIBUTE_TRACE(...) do { if (atomic_fetch_add(&attributeTraceErrors, 1) < 32) LSBSpawnTrace(__VA_ARGS__); } while (0)
#else
#define ATTRIBUTE_TRACE(...) ((void)0)
#endif

// Private setter in libsystem_kernel; this only updates the supplied attribute object.
extern int posix_spawnattr_set_crash_count_np(const posix_spawnattr_t *, uint32_t count, uint32_t timeout);

static int attributeLayout(LSBSpawnAttributeLayout *result) {
    static _Atomic(int) selected;
    *result = atomic_load_explicit(&selected, memory_order_acquire);
    if (*result) return 0;
    posix_spawnattr_t probe;
    int error = posix_spawnattr_init(&probe);
    if (error) return error;
    size_t allocationSize = malloc_size(probe);
    unsigned char before[LSBSpawnAttributeProbeSize];
    if (allocationSize < sizeof(before)) {
        error = ENOTSUP;
    } else {
        memcpy(before, probe, sizeof(before));
        error = posix_spawnattr_set_crash_count_np(&probe, LSBSpawnAttributeProbeCount, LSBSpawnAttributeProbeTimeout);
        if (!error) error = posix_spawnattr_set_csm_np(&probe, POSIX_SPAWN_NP_CSM_TECS);
        if (!error) error = posix_spawnattr_setspecialport_np(&probe, MACH_PORT_NULL, TASK_BOOTSTRAP_PORT);
        if (!error) {
            *result = LSBSpawnAttributeProbeLayout(before, (const unsigned char *)probe, allocationSize);
            if (!*result) error = ENOTSUP;
        }
    }
    posix_spawnattr_destroy(&probe);
    // Cache only successful discovery, so a transient allocator failure does not permanently disable spawning.
    if (!error) atomic_store_explicit(&selected, *result, memory_order_release);
    return error;
}

static int readArchitecture(const posix_spawnattr_t *attributes) {
    cpu_type_t cpus[4];
    cpu_subtype_t subtypes[4];
    size_t count;
    int error = posix_spawnattr_getarchpref_np(attributes, 4, cpus, subtypes, &count);
    if (error) return error;
    if (count != 4) return ENOTSUP;
    for (size_t i = 0; i < 4; ++i) {
        if (!cpus[i] && subtypes[i] == CPU_SUBTYPE_MULTIPLE) continue;
        if (i == 0 && cpus[i] == CPU_TYPE_ARM64 &&
            (subtypes[i] == CPU_SUBTYPE_ARM64_ALL || subtypes[i] == CPU_SUBTYPE_MULTIPLE)) continue;
        // Stock launchd appends x86_64 even for BinaryOrderPreference="arm64". The loader must still find its first arm64 slice.
        if (i == 1 && cpus[0] == CPU_TYPE_ARM64 && cpus[i] == CPU_TYPE_X86_64 && subtypes[i] == CPU_SUBTYPE_MULTIPLE) continue;
        ATTRIBUTE_TRACE("[spawn-trace] unsupported architecture index=%zu cpu=0x%x subtype=0x%x\n", i, cpus[i], subtypes[i]);
        return ENOTSUP;
    }
    return 0;
}

static int readLocalFilePolicy(const unsigned char *native) {
    uint32_t options, policy;
    memcpy(&options, native + NativeOptionsOffset, sizeof(options));
    memcpy(&policy, native + NativeDatalessPolicyOffset, sizeof(policy));
    if (!options && !policy) return 0;
    // iOS 26 libkernel's setdataless_iopolicy_np sets psa_options bit 4 and psa_dataless_iopolicy = input | IOPOL_*_ORIG (4).
    // xpcproxy_sim requests OFF (1) or ON (2). RuntimeRoot has materialized local files; FileProvider placeholders are unsupported.
    // Consume only those pairs without changing the extension's native I/O policy. PAC/Rosetta and all other options remain rejected.
    // Field/option names: https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/sys/spawn_internal.h#L221-L227
    if (options == NativeOptionDatalessPolicy && (policy == NativeDatalessOffOriginal || policy == NativeDatalessOnOriginal)) return 0;
    ATTRIBUTE_TRACE("[spawn-trace] unsupported spawn options=0x%x dataless-policy=0x%x\n", options, policy);
    return ENOTSUP;
}

static int readPorts(const unsigned char *native, LSBSpawnAttributeLayout layout, LSBSpawnPorts *ports) {
    const NativePortActions *actions;
    memcpy(&actions, native + LSBSpawnAttributePortActionsOffset(layout), sizeof(actions));
    if (!actions) return 0;
    if (actions->count < 0 || actions->allocated < actions->count) return EINVAL;
    if (actions->count > NativeMaxImportanceWatchPorts + LSB_MAX_REGISTERED_PORTS + 1) return E2BIG;
    unsigned importanceCount = 0;
    for (int32_t i = 0; i < actions->count; ++i) {
        const NativePortAction *action = &actions->actions[i];
        if (action->mask || action->behavior || action->flavor) return EINVAL;
        if (action->kind == NativeImportanceWatchPortAction) {
            if (action->which) return EINVAL;
            if (++importanceCount > NativeMaxImportanceWatchPorts) return E2BIG;
            // launchd adds one importance-watch hint per MachService. These request kernel scheduling boosts,
            // which the extension broker cannot reproduce; they neither transfer service rights nor register ports.
            // The native setter writes kind=3, mask=behavior=flavor=which=0 and borrows the supplied port name.
            continue;
        } else if (action->kind == NativeRegisteredPortAction) {
            if (action->which) return EINVAL;
            if (ports->count == LSB_MAX_REGISTERED_PORTS) return E2BIG;
            ports->registeredPresent = true;
            ports->registered[ports->count++] = action->port;
        } else if (action->kind == 0 && action->which == TASK_BOOTSTRAP_PORT) {
            if (action->port == MACH_PORT_DEAD || ports->bootstrapPresent) {
                ATTRIBUTE_TRACE("[spawn-trace] unsupported bootstrap port-action index=%d dead=%d duplicate=%d\n",
                                i, action->port == MACH_PORT_DEAD, ports->bootstrapPresent);
                return ENOTSUP;
            }
            ports->bootstrapPresent = true;
            ports->bootstrapPort = action->port;
        } else {
            ATTRIBUTE_TRACE("[spawn-trace] unsupported port-action index=%d kind=%d which=%d mask=0x%x behavior=%d flavor=%d\n",
                            i, action->kind, action->which, action->mask, action->behavior, action->flavor);
            return ENOTSUP;
        }
    }
    return 0;
}

int LSBReadAdditionalSpawnAttributes(const posix_spawnattr_t *attributes, LSBAdditionalSpawnAttributes *result) {
    if (!result) return EINVAL;
    memset(result, 0, sizeof(*result));
    if (!attributes) return 0;
    if (!*attributes) return EINVAL;
    LSBSpawnAttributeLayout layout;
    int error = attributeLayout(&layout);
    if (error) return error;
    const unsigned char *native = (const unsigned char *)*attributes;
    error = readArchitecture(attributes);
    if (error) return error;
    error = readLocalFilePolicy(native);
    if (error) return error;

    posix_spawnattr_t defaults;
    error = posix_spawnattr_init(&defaults);
    if (error) return error;
    size_t changed = 0;
    if (LSBSpawnAttributeFindDifference(layout, native, (const unsigned char *)defaults, &changed)) {
#ifdef LSB_TRANSPORT_TRACE
        uint32_t processType, darwinRole;
        memcpy(&processType, native + 0x24, sizeof(processType));
        memcpy(&darwinRole, native + 0x58, sizeof(darwinRole));
        ATTRIBUTE_TRACE("[spawn-trace] unsupported attribute offset=0x%zx value=0x%x default=0x%x type=0x%x role=0x%x\n",
                        changed, native[changed], ((const unsigned char *)defaults)[changed], processType, darwinRole);
#endif
        error = ENOTSUP;
    }
    posix_spawnattr_destroy(&defaults);
    if (error) return error;

    LSBAdditionalSpawnAttributes decoded = {0};
    error = readPorts(native, layout, &decoded.ports);
    if (error) return error;
    memcpy(&decoded.processType, native + 0x24, sizeof(decoded.processType));
    memcpy(&decoded.darwinRole, native + 0x58, sizeof(decoded.darwinRole));
    *result = decoded;
    return 0;
}
