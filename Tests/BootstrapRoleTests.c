#include "../ProcessSupport/SpawnTransport.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    xpc_object_t reply = xpc_dictionary_create(NULL, NULL, 0);
    assert(reply);
    uint32_t role = UINT32_MAX, productionStatus = UINT32_MAX;
    assert(decodeExecutionRole(reply, &role, &productionStatus) == EPROTO);
    assert(role == LSB_EXECUTION_SIMULATOR && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN); // No legacy policy inference.
    xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION);
    assert(decodeExecutionRole(reply, &role, &productionStatus) == EPROTO && role == LSB_EXECUTION_SIMULATOR);
    xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_NATIVE_METAL_PROBE);
    assert(decodeExecutionRole(reply, &role, &productionStatus) == EPROTO && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "native-production-status", LSB_NATIVE_PRODUCTION_UNKNOWN);
    assert(!decodeExecutionRole(reply, &role, &productionStatus) && role == LSB_EXECUTION_NATIVE_METAL_PROBE);
    assert(productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_NATIVE_METAL_SERVICE);
    assert(!decodeExecutionRole(reply, &role, &productionStatus) && role == LSB_EXECUTION_NATIVE_METAL_SERVICE);
    xpc_dictionary_set_uint64(reply, "execution-role", LSB_EXECUTION_SIMULATOR);
    for (uint32_t status = LSB_NATIVE_PRODUCTION_UNKNOWN; status <= LSB_NATIVE_DEVELOPMENT; ++status) {
        xpc_dictionary_set_uint64(reply, "native-production-status", status);
        assert(!decodeExecutionRole(reply, &role, &productionStatus) && role == LSB_EXECUTION_SIMULATOR && productionStatus == status);
    }
    xpc_dictionary_set_uint64(reply, "bootstrap-version", 1);
    assert(decodeExecutionRole(reply, &role, &productionStatus) == EPROTO && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "bootstrap-version", LSB_BOOTSTRAP_VERSION);
    xpc_dictionary_set_bool(reply, "native-production-status", false);
    assert(decodeExecutionRole(reply, &role, &productionStatus) == EPROTO && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "native-production-status", LSB_NATIVE_DEVELOPMENT + 1);
    assert(decodeExecutionRole(reply, &role, &productionStatus) == EPROTO && productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_dictionary_set_uint64(reply, "native-production-status", LSB_NATIVE_DEVELOPMENT);
    xpc_dictionary_set_value(reply, "bootstrap-version", NULL);
    assert(decodeExecutionRole(reply, &role, &productionStatus) == EPROTO && role == LSB_EXECUTION_SIMULATOR);
    assert(productionStatus == LSB_NATIVE_PRODUCTION_UNKNOWN);
    xpc_release(reply);
    LSBSpawnPorts ports = {0};
    assert(!validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 10, MACH_PORT_NULL, &ports));
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 11, MACH_PORT_NULL, &ports) == EINVAL);
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 10, 99, &ports) == EINVAL);
    ports.bootstrapPresent = true;
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 10, MACH_PORT_NULL, &ports) == EINVAL);
    ports.bootstrapPresent = false;
    ports.registeredPresent = true;
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_PROBE, 10, 10, MACH_PORT_NULL, &ports) == EINVAL);
    assert(validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_SERVICE, 10, 10, MACH_PORT_NULL, &ports) == EINVAL);
    ports = (LSBSpawnPorts){0};
    assert(!validateRoleMetadata(LSB_EXECUTION_NATIVE_METAL_SERVICE, 10, 10, MACH_PORT_NULL, &ports));
    assert(!validateRoleMetadata(LSB_EXECUTION_SIMULATOR, 10, 11, 99, &ports));
    puts("BootstrapRole: required native production policy and strict native identity/port boundary passed");
    return 0;
}
