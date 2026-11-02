#include "../ProcessSupport/SpawnTransport.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    xpc_object_t reply = xpc_dictionary_create(NULL, NULL, 0);
    assert(reply);
    uint32_t role = UINT32_MAX, productionStatus = UINT32_MAX, nativeSupport = UINT32_MAX;
    uint64_t cacheBase = UINT64_MAX;
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO);
    assert(role == LSB_EXECUTION_SIMULATOR && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN); // No legacy policy inference.
    xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION);
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && role == LSB_EXECUTION_SIMULATOR);
    xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_NATIVE_METAL_PROBE);
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "native-production-status", LSB_NATIVE_PRODUCTION_UNKNOWN);
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && cacheBase == 0);
    xpc_dictionary_set_uint64(reply, "simulator-cache-base", 0);
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO);
    xpc_dictionary_set_uint64(reply, "native-support", 0);
    assert(!decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) && role == LSB_EXECUTION_NATIVE_METAL_PROBE);
    assert(productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_NATIVE_METAL_SERVICE);
    assert(!decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) && role == LSB_EXECUTION_NATIVE_METAL_SERVICE);
    xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_SIMULATOR);
    for (uint32_t status = LSB_NATIVE_PRODUCTION_UNKNOWN; status <= LSB_NATIVE_DEVELOPMENT; ++status) {
        xpc_dictionary_set_uint64(reply, "native-production-status", status);
        assert(!decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) && role == LSB_EXECUTION_SIMULATOR && productionStatus == status);
        assert(cacheBase == 0);
    }
    xpc_dictionary_set_uint64(reply, "simulator-cache-base", UINT64_C(0x7100000000));
    assert(!decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) && cacheBase == UINT64_C(0x7100000000));
    xpc_dictionary_set_uint64(reply, "native-support", 1);
    for (uint32_t status = LSB_NATIVE_PRODUCTION_UNKNOWN; status <= LSB_NATIVE_DEVELOPMENT; ++status) {
        xpc_dictionary_set_uint64(reply, "native-production-status", status);
        assert(!decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport));
        assert(nativeSupport == 1 && productionStatus == status && cacheBase == UINT64_C(0x7100000000));
    }
    for (unsigned malformed = 0; malformed < 6; ++malformed) {
        switch (malformed) {
            case 0: xpc_dictionary_set_value(reply, "native-support", NULL); break;
            case 1: xpc_dictionary_set_int64(reply, "native-support", 1); break;
            case 2: xpc_dictionary_set_bool(reply, "native-support", true); break;
            case 3: xpc_dictionary_set_string(reply, "native-support", "1"); break;
            case 4: xpc_dictionary_set_uint64(reply, "native-support", 2); break;
            case 5: xpc_dictionary_set_uint64(reply, "native-support", UINT64_MAX); break;
        }
        role = productionStatus = nativeSupport = UINT32_MAX;
        cacheBase = UINT64_MAX;
        assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO);
        assert(role == LSB_EXECUTION_SIMULATOR && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN && !cacheBase && !nativeSupport);
    }
    xpc_dictionary_set_uint64(reply, "native-support", 1);
    for (uint32_t nativeRole = LSB_EXECUTION_NATIVE_METAL_PROBE; nativeRole <= LSB_EXECUTION_NATIVE_SIGNER; ++nativeRole) {
        xpc_dictionary_set_uint64(reply, "execution-role", nativeRole);
        for (unsigned withBase = 0; withBase < 2; ++withBase) {
            xpc_dictionary_set_uint64(reply, "simulator-cache-base", withBase ? UINT64_C(0x7100000000) : 0);
            assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && !cacheBase && !nativeSupport);
        }
    }
    xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_SIMULATOR);
    xpc_dictionary_set_uint64(reply, "simulator-cache-base", 0);
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO);
    xpc_dictionary_set_uint64(reply, "native-support", 0);
    const uint64_t invalidBases[] = {1, LSB_SLIDABLE_CACHE_PREFERRED_BASE - 0x4000, UINT64_C(0x7100000001),
                                    UINT64_C(0x0080000000000000), UINT64_MAX};
    for (unsigned i = 0; i < sizeof(invalidBases) / sizeof(*invalidBases); ++i) {
        xpc_dictionary_set_uint64(reply, "simulator-cache-base", invalidBases[i]);
        assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && cacheBase == 0);
    }
    for (unsigned wrongType = 0; wrongType < 3; ++wrongType) {
        if (wrongType == 0) xpc_dictionary_set_bool(reply, "simulator-cache-base", false);
        else if (wrongType == 1) xpc_dictionary_set_int64(reply, "simulator-cache-base", 0);
        else xpc_dictionary_set_string(reply, "simulator-cache-base", "0");
        assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && cacheBase == 0);
    }
    xpc_dictionary_set_uint64(reply, "simulator-cache-base", 0);
    xpc_dictionary_set_uint64(reply, "bootstrap-version", 1);
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION);
    xpc_dictionary_set_bool(reply, "native-production-status", false);
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "native-production-status", LSB_NATIVE_DEVELOPMENT + 1);
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "native-production-status", LSB_NATIVE_DEVELOPMENT);
    xpc_dictionary_set_value(reply, "bootstrap-version", NULL);
    assert(decodeExecutionRole(reply, &role, &productionStatus, &cacheBase, &nativeSupport) == EPROTO && role == LSB_EXECUTION_SIMULATOR);
    assert(productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_release(reply);
    LSBSpawnPorts ports = {0};
    assert(!validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 10, MACH_PORT_NULL, &ports, MACH_PORT_NULL));
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 11, MACH_PORT_NULL, &ports, MACH_PORT_NULL) == EINVAL);
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 10, 99, &ports, MACH_PORT_NULL) == EINVAL);
    ports.bootstrapPresent = true;
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 10, MACH_PORT_NULL, &ports, MACH_PORT_NULL) == EINVAL);
    ports.bootstrapPresent = false;
    ports.registeredPresent = true;
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 10, MACH_PORT_NULL, &ports, MACH_PORT_NULL) == EINVAL);
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_SERVICE, 10, 10, MACH_PORT_NULL, &ports, MACH_PORT_NULL) == EINVAL);
    ports = (LSBSpawnPorts){0};
    assert(!validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_SERVICE, 10, 10, MACH_PORT_NULL, &ports, MACH_PORT_NULL));
    mach_port_t endpoint = MACH_PORT_NULL;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &endpoint));
    assert(!mach_port_insert_right(mach_task_self(), endpoint, endpoint, MACH_MSG_TYPE_MAKE_SEND));
    assert(!validateRoleMetadata(LSB_EXECUTION_SIMULATOR, 10, 11, 99, &ports, endpoint));
    assert(validateRoleMetadata(LSB_EXECUTION_SIMULATOR, 10, 11, 99, &ports, MACH_PORT_NULL) == EINVAL);
    assert(validateRoleMetadata(LSB_EXECUTION_SIMULATOR, 10, 11, 99, &ports, MACH_PORT_DEAD) == EINVAL);
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 10, MACH_PORT_NULL, &ports, endpoint) == EINVAL);
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_SERVICE, 10, 10, MACH_PORT_NULL, &ports, endpoint) == EINVAL);
    assert(!mach_port_deallocate(mach_task_self(), endpoint));
    assert(!mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1));
    puts("BootstrapRole: strict native policy/cache mode metadata, malformed addresses and native identity/port boundary passed");
    return 0;
}
