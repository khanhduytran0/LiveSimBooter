#include "../ProcessSupport/NativeRuntimePolicy.h"
#include <assert.h>
#include <stdio.h>

static void assertFlags(const char *const environment[], bool retain, bool hardware) {
    assert(LSBRuntimeBooleanEnvironment(environment, LSB_RETAIN_HOST_CACHE_ENV, !retain) == retain);
    assert(LSBRuntimeBooleanEnvironment(environment, LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV, !hardware) == hardware);
}

static void testDefaultInjection(void) {
    for (uint32_t status = LSB_NATIVE_PRODUCTION_UNKNOWN; status <= LSB_NATIVE_DEVELOPMENT; ++status) {
        const char *environment[8] = {"PATH=/usr/bin", LSB_RETAIN_HOST_CACHE_ENV "_EXTRA=1", LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV,
            "EffectiveProductionStatusAp=0", "LSB_NATIVE_PRODUCTION_STATUS=2", NULL};
        LSBNativeRuntimePolicy policy;
        assert(!LSBResolveNativeRuntimePolicy(status, environment, &policy));
        bool retain = status != LSB_NATIVE_DEVELOPMENT;
        assert(policy.retainHostCache == retain && policy.hardwareWorkqueue == retain);
        unsigned count = 5;
        assert(!LSBAppendNativeRuntimePolicy(environment, &count, 8, policy));
        assert(count == 7 && !environment[7] && !strcmp(environment[0], "PATH=/usr/bin"));
        assertFlags(environment, retain, retain);
        assert(!LSBAppendNativeRuntimePolicy(environment, &count, 8, policy) && count == 7);
        const char *replacement[3] = {NULL};
        count = 0;
        assert(!LSBResolveNativeRuntimePolicy(status, replacement, &policy));
        assert(!LSBAppendNativeRuntimePolicy(replacement, &count, 3, policy));
        assert(count == 2 && !replacement[2]);
        assertFlags(replacement, retain, retain);
    }
}

static void testExplicitFirstMatch(void) {
    const char *values[] = {"0", "1", "", "01", "true", "1=extra"};
    for (uint32_t status = LSB_NATIVE_PRODUCTION_UNKNOWN; status <= LSB_NATIVE_DEVELOPMENT; ++status) {
        for (unsigned index = 0; index < sizeof(values) / sizeof(*values); ++index) {
            char retain[80], hardware[80];
            snprintf(retain, sizeof(retain), LSB_RETAIN_HOST_CACHE_ENV "=%s", values[index]);
            snprintf(hardware, sizeof(hardware), LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "=%s", values[index]);
            const char *environment[5] = {retain, hardware, LSB_RETAIN_HOST_CACHE_ENV "=1", LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "=1", NULL};
            bool retainRequested = !strcmp(values[index], "1");
            LSBNativeRuntimePolicy policy;
            int error = LSBResolveNativeRuntimePolicy(status, environment, &policy);
            if (!retainRequested && status != LSB_NATIVE_DEVELOPMENT) {
                assert(error == EPERM && policy.retainHostCache && policy.hardwareWorkqueue);
                continue;
            }
            assert(!error && policy.retainHostCache == retainRequested && policy.hardwareWorkqueue == retainRequested);
            unsigned count = 4;
            assert(!LSBAppendNativeRuntimePolicy(environment, &count, 5, policy) && count == 4);
            assertFlags(environment, retainRequested, retainRequested);
        }
        const char *keepNative[3] = {LSB_RETAIN_HOST_CACHE_ENV "=1", NULL};
        LSBNativeRuntimePolicy policy;
        assert(!LSBResolveNativeRuntimePolicy(status, keepNative, &policy));
        assert(policy.retainHostCache && policy.hardwareWorkqueue); // Explicit retention selects a coherent hardware default even on development devices.
        unsigned count = 1;
        assert(!LSBAppendNativeRuntimePolicy(keepNative, &count, 3, policy));
        assert(count == 2);
        assertFlags(keepNative, true, true);
        const char *hardwareOff[] = {LSB_RETAIN_HOST_CACHE_ENV "=1", LSB_WORKQUEUE_HARDWARE_BREAKPOINT_ENV "=0", NULL};
        assert(!LSBResolveNativeRuntimePolicy(status, hardwareOff, &policy));
        assert(policy.retainHostCache && !policy.hardwareWorkqueue); // The independent explicit bridge diagnostic still exists.
    }
}

static void testInvalidInputsAndCapacity(void) {
    LSBNativeRuntimePolicy policy;
    assert(LSBResolveNativeRuntimePolicy(LSB_NATIVE_DEVELOPMENT + 1, NULL, &policy) == EINVAL);
    assert(policy.retainHostCache && policy.hardwareWorkqueue);
    assert(LSBResolveNativeRuntimePolicy(LSB_NATIVE_PRODUCTION_UNKNOWN, NULL, NULL) == EINVAL);
    assert(!LSBResolveNativeRuntimePolicy(LSB_NATIVE_DEVELOPMENT, NULL, &policy));
    const char *environment[3] = {"KEEP=unchanged", NULL, "guard"};
    unsigned count = 1;
    const char *original[3];
    memcpy(original, environment, sizeof(environment));
    assert(LSBAppendNativeRuntimePolicy(environment, &count, 2, policy) == E2BIG);
    assert(count == 1 && !memcmp(environment, original, sizeof(environment)));
    assert(LSBAppendNativeRuntimePolicy(environment, &count, 1, policy) == EINVAL);
    assert(LSBAppendNativeRuntimePolicy(NULL, &count, 3, policy) == EINVAL);
    assert(LSBAppendNativeRuntimePolicy(environment, NULL, 3, policy) == EINVAL);
    count = 2;
    assert(LSBAppendNativeRuntimePolicy(environment, &count, 3, policy) == EINVAL);
    environment[2] = NULL;
    assert(LSBAppendNativeRuntimePolicy(environment, &count, 3, policy) == EINVAL);
}

int main(void) {
    testDefaultInjection();
    testExplicitFirstMatch();
    testInvalidInputsAndCapacity();
    puts("Native runtime policy tests passed");
    return 0;
}
