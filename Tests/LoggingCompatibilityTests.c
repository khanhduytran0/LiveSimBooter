#include "../Guest/LoggingCompatibility.h"
#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

#ifdef LSB_LOGGING_INTERPOSE_TESTING

int main(void) {
    // No configuration or constructor call is required before this first query.
    errno = EDOM;
    xpc_object_t value = xpc_copy_entitlement_for_token(LSB_LOGGING_DISABLE_MACH_PORTS_ENTITLEMENT, NULL);
    assert(value == XPC_BOOL_TRUE && errno == EDOM);
    xpc_release(value);
    audit_token_t audit;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &count));
    value = xpc_copy_entitlement_for_token(LSB_LOGGING_DISABLE_MACH_PORTS_ENTITLEMENT, &audit);
    assert(value != XPC_BOOL_TRUE); // This unsigned fixture has no actual logging entitlement, even for an explicit self token.
    if (value) xpc_release(value);
    value = xpc_copy_entitlement_for_token("com.kdt.LiveSimBooter.test.nonexistent-entitlement", NULL);
    assert(!value);
    value = xpc_copy_entitlement_for_token(LSB_NOTIFY_RESTRICTION_ENTITLEMENT, NULL);
    assert(!value); // The real unsigned process has no restriction; the adapter must not manufacture one.
    value = xpc_copy_entitlement_for_token(NULL, NULL);
    assert(!value || xpc_get_type(value) == XPC_TYPE_DICTIONARY); // A request for the complete dictionary is never synthesized.
    if (value) xpc_release(value);
    puts("LoggingCompatibility interpose: exact self policy, explicit audit validation and unrelated queries passed");
    return 0;
}

#else

static unsigned originalCalls, booleanCalls, taskInfoCalls;
static const char *seenKey;
static const audit_token_t *seenToken;
static xpc_object_t originalResult;
static audit_token_t taskAudit, forwardedAudit;
static kern_return_t taskInfoResult;
static mach_msg_type_number_t returnedCount = TASK_AUDIT_TOKEN_COUNT;
static bool captureForwardedAudit;

static xpc_object_t originalQuery(const char *key, const audit_token_t *token) {
    ++originalCalls;
    seenKey = key;
    seenToken = token;
    if (captureForwardedAudit) { assert(token); forwardedAudit = *token; }
    errno = EACCES;
    return originalResult;
}

static xpc_object_t createBoolean(bool value) {
    ++booleanCalls;
    assert(value);
    return xpc_bool_create(value);
}

static kern_return_t inspectTask(task_name_t task, task_flavor_t flavor, task_info_t info, mach_msg_type_number_t *count) {
    ++taskInfoCalls;
    assert(task == mach_task_self() && flavor == TASK_AUDIT_TOKEN && *count == TASK_AUDIT_TOKEN_COUNT);
    memcpy(info, &taskAudit, sizeof(taskAudit));
    *count = returnedCount;
    return taskInfoResult;
}

#define LSB_LOGGING_TESTING
#define xpc_copy_entitlement_for_token originalQuery
#define xpc_bool_create createBoolean
#define task_info inspectTask
#include "../Guest/LoggingCompatibility.c"
#undef xpc_copy_entitlement_for_token
#undef xpc_bool_create
#undef task_info

static void passthrough(const char *key, const audit_token_t *token) {
    originalCalls = booleanCalls = taskInfoCalls = 0;
    errno = EDOM;
    xpc_object_t result = LSBLoggingCopyEntitlementForToken(key, token);
    assert(result == originalResult && originalCalls == 1 && !booleanCalls && !taskInfoCalls &&
           seenKey == key && seenToken == token && errno == EACCES);
}

int main(void) {
    originalResult = xpc_bool_create(false);
    const char key[] = LSB_LOGGING_DISABLE_MACH_PORTS_ENTITLEMENT;
#ifdef LSB_GUEST_RUNTIME
    errno = EDOM;
    xpc_object_t result = LSBLoggingCopyEntitlementForToken(key, NULL);
    assert(result == XPC_BOOL_TRUE && !originalCalls && booleanCalls == 1 && errno == EDOM);
    xpc_release(result);
    char prefix[sizeof(key)];
    for (size_t length = 0; length < sizeof(key) - 1; ++length) {
        memcpy(prefix, key, length);
        prefix[length] = '\0';
        passthrough(prefix, NULL);
    }
#else
    // Compiling this source outside the guest build must never enable the override.
    passthrough(key, NULL);
    (void)createBoolean;
    (void)inspectTask;
#endif
    passthrough(NULL, NULL);
    passthrough("com.apple.private.disable-log-mach-ports.extra", NULL);
    passthrough("com.apple.private.disable-log-mach-port", NULL);
    passthrough("com.apple.private.Disable-log-mach-ports", NULL);
    passthrough("com.apple.private.task_for_pid-allow", NULL);
    passthrough(LSB_NOTIFY_RESTRICTION_ENTITLEMENT ".extra", NULL);
    passthrough("com.apple.developer.web-browser-engine.restrict.notify", NULL);
    audit_token_t target = {{0}};
    passthrough(key, &target);
    passthrough(LSB_NOTIFY_RESTRICTION_ENTITLEMENT, &target);
    target.val[5] = 1234;
    target.val[7] = 12;
    passthrough(key, &target);
    passthrough(key, (const audit_token_t *)(uintptr_t)1); // Target validation belongs to libxpc, never dereference or replace it here.
    passthrough((const char *)(uintptr_t)1, &target); // Even key inspection is skipped for explicit targets.
    xpc_release(originalResult);
    originalResult = NULL;
    passthrough("missing", NULL);
    passthrough(key, &target);
    originalResult = xpc_bool_create(true);
    passthrough("original-true", NULL);
    xpc_release(originalResult);
#ifdef LSB_GUEST_RUNTIME
    // An actual full kernel token selects the existing authenticated guest snapshot. Keep the original value and errors,
    // including false, a wrong plist type, or missing metadata; libnotify itself requires the exact true singleton.
    for (unsigned index = 0; index < sizeof(taskAudit.val) / sizeof(taskAudit.val[0]); ++index) taskAudit.val[index] = 100 + index;
    xpc_object_t values[] = {xpc_bool_create(true), xpc_bool_create(false), xpc_int64_create(1), NULL};
    captureForwardedAudit = true;
    for (unsigned index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        originalResult = values[index];
        originalCalls = booleanCalls = taskInfoCalls = 0;
        errno = EDOM;
        xpc_object_t result = LSBLoggingCopyEntitlementForToken(LSB_NOTIFY_RESTRICTION_ENTITLEMENT, NULL);
        assert(result == originalResult && originalCalls == 1 && taskInfoCalls == 1 && !booleanCalls && errno == EACCES);
        assert(!strcmp(seenKey, LSB_NOTIFY_RESTRICTION_ENTITLEMENT) && !memcmp(&forwardedAudit, &taskAudit, sizeof(taskAudit)));
    }
    for (unsigned index = 0; index < 3; ++index) xpc_release(values[index]);
    for (unsigned failure = 0; failure < 4; ++failure) {
        taskInfoResult = failure == 0 ? KERN_FAILURE : KERN_SUCCESS;
        returnedCount = failure == 1 ? TASK_AUDIT_TOKEN_COUNT - 1 : TASK_AUDIT_TOKEN_COUNT;
        taskAudit.val[5] = failure == 2 ? 0 : failure == 3 ? UINT32_MAX : 105;
        originalCalls = booleanCalls = taskInfoCalls = 0;
        errno = EDOM;
        assert(!LSBLoggingCopyEntitlementForToken(LSB_NOTIFY_RESTRICTION_ENTITLEMENT, NULL));
        assert(!originalCalls && !booleanCalls && taskInfoCalls == 1 && errno == EDOM);
    }
#else
    passthrough(LSB_NOTIFY_RESTRICTION_ENTITLEMENT, NULL);
#endif
    puts("LoggingCompatibility: exact key/self scope, early-call policy, native build and original error forwarding passed");
    return 0;
}

#endif
