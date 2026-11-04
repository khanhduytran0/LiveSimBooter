#include "../Guest/libLiveSimSpawn/CodeSigningCompatibility.h"
#include "../ProcessSupport/SpawnTransport.h"

#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int originalQuery(pid_t, unsigned int, void *, size_t, audit_token_t *);
static int selfQuery(pid_t, unsigned int, void *, size_t);
static int brokerQuery(const audit_token_t *, unsigned int, unsigned char **, size_t *);
static int brokerHashQuery(const audit_token_t *, unsigned char [LSB_GUEST_CDHASH_BYTES]);

#define csops_audittoken originalQuery
#define csops selfQuery
#define LSBTransportCopyEntitlements brokerQuery
#define LSBTransportCopyCodeDirectoryHash brokerHashQuery
#define LSB_CODE_SIGNING_TESTING
#include "../Guest/libLiveSimSpawn/CodeSigningCompatibility.c"
#undef csops_audittoken
#undef csops
#undef LSBTransportCopyEntitlements
#undef LSBTransportCopyCodeDirectoryHash

static struct {
    unsigned originalCalls, selfCalls, brokerCalls;
    audit_token_t brokerTarget;
    unsigned brokerOperation, malformedReply, profile;
    int brokerError;
    pid_t pid, selfPID;
    unsigned int operation, selfOperation;
    void *buffer, *selfBuffer;
    size_t size, selfSize;
    audit_token_t *token;
    int originalResult, originalError, selfResult, selfError, selfIncomingErrno;
    bool realSelf, selfWritesOnError;
    void *invalidateAfterSelf;
    size_t invalidateSize;
} calls;

static const unsigned int readOperations[] = {0, 5, 7, 11, 14, 16, 17};
static const unsigned char originalBytes[] = {0x51, 0x91};
static const unsigned char selfBytes[] = {0x04, 0x11, 0x52, 0x89, 0x21, 0x17, 0x69, 0xf0};

static uint32_t readBE32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

static bool entitlementOperation(unsigned int operation) {
    return operation == LSBCSOpEntitlementsBlob || operation == LSBCSOpDEREntitlementsBlob;
}

static int originalQuery(pid_t pid, unsigned int operation, void *buffer, size_t size, audit_token_t *token) {
    assert(!calls.originalCalls++ && !calls.selfCalls && !calls.brokerCalls); // Native paths query once, before any EPERM fallback.
    calls.pid = pid;
    calls.operation = operation;
    calls.buffer = buffer;
    calls.size = size;
    calls.token = token;
    if (calls.originalResult == 0 && buffer && size >= sizeof(originalBytes)) memcpy(buffer, originalBytes, sizeof(originalBytes));
    errno = calls.originalError;
    return calls.originalResult;
}

static int selfQuery(pid_t pid, unsigned int operation, void *buffer, size_t size) {
    assert(calls.originalCalls == 1 && !calls.selfCalls++); // A self fallback must not recurse through the audited wrapper.
    calls.selfPID = pid;
    calls.selfOperation = operation;
    calls.selfBuffer = buffer;
    calls.selfSize = size;
    calls.selfIncomingErrno = errno;
    if (calls.realSelf) {
        int result = csops(pid, operation, buffer, size);
        if (!result && calls.invalidateAfterSelf) assert(!mprotect(calls.invalidateAfterSelf, calls.invalidateSize, PROT_NONE));
        return result;
    }
    if ((calls.selfResult == 0 || calls.selfWritesOnError) && buffer && size)
        memcpy(buffer, selfBytes, size < sizeof(selfBytes) ? size : sizeof(selfBytes));
    if (calls.selfError) errno = calls.selfError;
    return calls.selfResult;
}

static void writeBE32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
}

static int brokerHashQuery(const audit_token_t *target, unsigned char hash[LSB_GUEST_CDHASH_BYTES]) {
    assert(calls.originalCalls == 1 && !calls.brokerCalls++ && !calls.selfCalls && target != calls.token);
    calls.brokerTarget = *target;
    calls.brokerOperation = LSBCSOpCDHash;
    errno = EINTR;
    memset(hash, 0, LSB_GUEST_CDHASH_BYTES);
    if (calls.brokerError) return calls.brokerError;
    for (unsigned i = 0; i < LSB_GUEST_CDHASH_BYTES; ++i) hash[i] = (unsigned char)(target->val[5] + i);
    return 0;
}

static int brokerQuery(const audit_token_t *target, unsigned int operation, unsigned char **blob, size_t *size) {
    assert(!calls.originalCalls && !calls.brokerCalls++ && !calls.selfCalls && target != calls.token);
    calls.brokerTarget = *target;
    calls.brokerOperation = operation;
    *blob = NULL;
    *size = 0;
    errno = EINTR; // The wrapper must restore the caller's errno independently of transport internals.
    if (calls.brokerError) return calls.brokerError;
    static const unsigned char derEmpty[] = {0x70, 5, 2, 1, 1, 0xb0, 0};
    static const unsigned char derPeer[] = {0x70, 16, 2, 1, 1, 0xb0, 11, 0x30, 9, 0x0c, 4, 'p', 'e', 'e', 'r', 1, 1, 0xff};
    static const char xmlEmpty[] = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict/></plist>";
    static const char xmlPeer[] = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>peer</key><true/></dict></plist>";
    bool peer = target->val[5] == 5678 || calls.profile;
    const void *payload;
    size_t payloadSize;
    if (operation == LSBCSOpDEREntitlementsBlob) {
        payload = peer ? derPeer : derEmpty;
        payloadSize = peer ? sizeof(derPeer) : sizeof(derEmpty);
    } else {
        assert(operation == LSBCSOpEntitlementsBlob);
        payload = peer ? xmlPeer : xmlEmpty;
        payloadSize = peer ? sizeof(xmlPeer) - 1 : sizeof(xmlEmpty) - 1;
    }
    size_t length = 8 + payloadSize;
    if (calls.malformedReply == 1) return 0;
    if (calls.malformedReply == 5)
        length = 1 + (operation == LSBCSOpEntitlementsBlob ? LSB_GUEST_ENTITLEMENTS_MAX_XML_BLOB_SIZE : LSB_GUEST_ENTITLEMENTS_MAX_BLOB_SIZE);
    unsigned char *data = calloc(1, length);
    assert(data);
    writeBE32(data, operation == LSBCSOpEntitlementsBlob ? 0xfade7171u : 0xfade7172u);
    writeBE32(data + 4, (uint32_t)length);
    memcpy(data + 8, payload, payloadSize);
    if (calls.malformedReply == 2) length = 7;
    if (calls.malformedReply == 3) data[3] ^= 3;
    if (calls.malformedReply == 4) data[7] ^= 1;
    *blob = data;
    *size = length;
    return 0;
}

static void resetCalls(void) {
    memset(&calls, 0, sizeof(calls));
    calls.originalResult = -1;
    calls.originalError = EPERM;
    errno = EDOM;
}

static void assertOriginal(pid_t pid, unsigned int operation, void *buffer, size_t size, audit_token_t *token) {
    assert(calls.originalCalls == 1 && calls.pid == pid && calls.operation == operation);
    assert(calls.buffer == buffer && calls.size == size && calls.token == token);
}

static void testDisabledAndOriginalResults(void) {
    audit_token_t token = {{1, 2, 3, 4, 5, 6, 7, 8}}, before = token;
    const char *disabled[] = {NULL, "", "0", "true", "01", "1 "};
    unsigned char buffer[32];
    for (size_t i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) {
        if (disabled[i]) assert(!setenv(LSB_CODE_SIGNING_ENABLE_ENV, disabled[i], 1));
        else assert(!unsetenv(LSB_CODE_SIGNING_ENABLE_ENV));
        for (size_t j = 0; j < sizeof(readOperations) / sizeof(*readOperations); ++j) {
            memset(buffer, 0xa5, sizeof(buffer));
            resetCalls();
            assert(LSBCSopsAuditToken(1234, readOperations[j], buffer, sizeof(buffer), &token) == -1 && errno == EPERM);
            assertOriginal(1234, readOperations[j], buffer, sizeof(buffer), &token);
            assert(!calls.selfCalls && buffer[0] == 0xa5 && buffer[31] == 0xa5 && !memcmp(&token, &before, sizeof(token)));
            resetCalls();
            calls.originalResult = 0;
            calls.originalError = EDOM;
            assert(!LSBCSopsAuditToken(1234, readOperations[j], buffer, sizeof(buffer), &token) && errno == EDOM);
            assertOriginal(1234, readOperations[j], buffer, sizeof(buffer), &token);
            assert(!calls.brokerCalls && !calls.selfCalls && !memcmp(buffer, originalBytes, sizeof(originalBytes)));
        }
    }
    assert(!setenv(LSB_CODE_SIGNING_ENABLE_ENV, "1", 1));
    const int untouchedErrors[] = {ESRCH, EINVAL, EFAULT, ERANGE, EACCES, ENOMEM, EINTR, 0};
    for (size_t operation = 0; operation < sizeof(readOperations) / sizeof(*readOperations); ++operation) {
        if (entitlementOperation(readOperations[operation])) continue;
        for (size_t i = 0; i < sizeof(untouchedErrors) / sizeof(*untouchedErrors); ++i) {
            resetCalls();
            calls.originalError = untouchedErrors[i];
            assert(LSBCSopsAuditToken(-9, readOperations[operation], NULL, SIZE_MAX, NULL) == -1 && errno == untouchedErrors[i]);
            assertOriginal(-9, readOperations[operation], NULL, SIZE_MAX, NULL);
            assert(!calls.selfCalls);
        }
    }
    const int originalResults[] = {0, 1, -2};
    for (size_t operation = 0; operation < sizeof(readOperations) / sizeof(*readOperations); ++operation) {
        if (entitlementOperation(readOperations[operation])) continue;
        for (size_t i = 0; i < sizeof(originalResults) / sizeof(*originalResults); ++i) {
            memset(buffer, 0xa5, sizeof(buffer));
            resetCalls();
            calls.originalResult = originalResults[i];
            assert(LSBCSopsAuditToken(1234, readOperations[operation], buffer, sizeof(buffer), &token) == originalResults[i] && errno == EPERM);
            assertOriginal(1234, readOperations[operation], buffer, sizeof(buffer), &token);
            assert(!calls.selfCalls);
            if (!originalResults[i]) assert(!memcmp(buffer, originalBytes, sizeof(originalBytes)) && buffer[2] == 0xa5);
        }
    }
}

static void testEntitlementSnapshotPriority(void) {
    audit_token_t target = {{1, 2, 3, 4, 5, 777, 7, 8}};
    const unsigned operations[] = {LSBCSOpEntitlementsBlob, LSBCSOpDEREntitlementsBlob};
    const struct { int result, error; } nativeResults[] = {{0, 0}, {0, EDOM}, {-1, EINVAL}, {-1, ERANGE}, {-1, EPERM}, {-1, ESRCH}};
    for (size_t op = 0; op < sizeof(operations) / sizeof(*operations); ++op) {
        for (size_t native = 0; native < sizeof(nativeResults) / sizeof(*nativeResults); ++native) {
            unsigned char header[8], blob[512];
            resetCalls();
            calls.originalResult = nativeResults[native].result;
            calls.originalError = nativeResults[native].error;
            calls.profile = 1;
            // Security's eight-byte probe must negotiate the guest blob, never cache an empty native self result.
            assert(LSBCSopsAuditToken(777, operations[op], header, sizeof(header), &target) == -1 && errno == ERANGE);
            uint32_t length = readBE32(header + 4);
            assert(!calls.originalCalls && calls.brokerCalls == 1 && !readBE32(header) && length > 8 && length <= sizeof(blob));
            assert(!memcmp(&calls.brokerTarget, &target, sizeof(target)));
            resetCalls();
            calls.originalResult = nativeResults[native].result;
            calls.originalError = nativeResults[native].error;
            calls.profile = 1;
            assert(!LSBCSopsAuditToken(777, operations[op], blob, length, &target) && errno == EDOM);
            assert(!calls.originalCalls && calls.brokerCalls == 1 && !calls.selfCalls && readBE32(blob + 4) == length);
            assert(LSBGuestEntitlementsHasValidBlobHeaderForOperation(blob, length, operations[op]));
            if (operations[op] == LSBCSOpEntitlementsBlob) {
                CFDataRef data = CFDataCreate(NULL, blob + 8, length - 8);
                CFPropertyListRef value = CFPropertyListCreateWithData(NULL, data, 0, NULL, NULL);
                assert(value && CFGetTypeID(value) == CFDictionaryGetTypeID());
                assert(CFDictionaryGetValue(value, CFSTR("peer")) == kCFBooleanTrue);
                CFRelease(value);
                CFRelease(data);
            }
        }
    }
    unsigned char empty[512];
    resetCalls();
    calls.originalResult = 0;
    assert(!LSBCSopsAuditToken(777, LSBCSOpEntitlementsBlob, empty, sizeof(empty), &target));
    CFDataRef data = CFDataCreate(NULL, empty + 8, readBE32(empty + 4) - 8);
    CFPropertyListRef value = CFPropertyListCreateWithData(NULL, data, 0, NULL, NULL);
    assert(value && CFGetTypeID(value) == CFDictionaryGetTypeID() && !CFDictionaryGetCount(value)); // An empty snapshot grants nothing.
    CFRelease(value);
    CFRelease(data);
}

static void testReadAllowlistAndIdentity(void) {
    audit_token_t token = {{0x91, 0x32, 0x13, 0x44, 0x55, 9988, 0x77, 0x88}}, before = token;
    for (unsigned int operation = 0; operation <= 64; ++operation) {
        bool allowed = false;
        for (size_t i = 0; i < sizeof(readOperations) / sizeof(*readOperations); ++i) allowed |= operation == readOperations[i];
        unsigned char buffer[512];
        memset(buffer, 0xa5, sizeof(buffer));
        resetCalls();
        size_t size = operation == LSBCSOpCDHash ? LSB_GUEST_CDHASH_BYTES : sizeof(buffer);
        int result = LSBCSopsAuditToken(9988, operation, buffer, size, &token);
        if (!entitlementOperation(operation)) assertOriginal(9988, operation, buffer, size, &token);
        else assert(!calls.originalCalls);
        assert(!memcmp(&token, &before, sizeof(token)));
        if (!allowed) {
            assert(result == -1 && errno == EPERM && !calls.selfCalls && buffer[0] == 0xa5);
            continue;
        }
        if (entitlementOperation(operation)) {
            assert(!result && errno == EDOM && !calls.selfCalls && calls.brokerCalls == 1);
            assert(calls.brokerOperation == operation && !memcmp(&calls.brokerTarget, &token, sizeof(token)));
            uint32_t length = readBE32(buffer + 4);
            assert(length > 8 && length < sizeof(buffer) && buffer[length] == 0xa5);
            assert(readBE32(buffer) == (operation == LSBCSOpEntitlementsBlob ? 0xfade7171u : 0xfade7172u));
            continue;
        }
        if (operation == LSBCSOpCDHash) {
            assert(!result && errno == EDOM && !calls.selfCalls && calls.brokerCalls == 1);
            assert(calls.brokerOperation == operation && !memcmp(&calls.brokerTarget, &token, sizeof(token)));
            for (unsigned i = 0; i < LSB_GUEST_CDHASH_BYTES; ++i) assert(buffer[i] == (unsigned char)(token.val[5] + i));
            assert(buffer[LSB_GUEST_CDHASH_BYTES] == 0xa5);
            continue;
        }
        assert(!result && errno == EDOM && calls.selfCalls == 1 && calls.selfIncomingErrno == EDOM);
        assert(calls.selfPID == 0 && calls.selfOperation == operation && calls.selfBuffer == buffer && calls.selfSize == sizeof(buffer));
        if (operation == LSBCSOpStatus) {
            uint32_t status;
            memcpy(&status, buffer, sizeof(status));
            assert(status == UINT32_C(0x20000001));
            assert(!memcmp(buffer + 4, selfBytes + 4, sizeof(selfBytes) - 4));
        } else assert(!memcmp(buffer, selfBytes, sizeof(selfBytes)));
        assert(buffer[sizeof(selfBytes)] == 0xa5 && buffer[31] == 0xa5);
    }
    resetCalls();
    assert(LSBCSopsAuditToken(9988, UINT_MAX, NULL, 0, &token) == -1 && errno == EPERM && !calls.selfCalls);
}

static void testFallbackErrorsAndStatusBounds(void) {
    const int errors[] = {EPERM, ESRCH, EINVAL, EFAULT, ERANGE, ENOENT};
    unsigned char buffer[32];
    for (size_t i = 0; i < sizeof(readOperations) / sizeof(*readOperations); ++i) {
        if (entitlementOperation(readOperations[i]) || readOperations[i] == LSBCSOpCDHash) continue;
        for (size_t j = 0; j < sizeof(errors) / sizeof(*errors); ++j) {
            memset(buffer, 0xa5, sizeof(buffer));
            resetCalls();
            calls.selfResult = -1;
            calls.selfError = errors[j];
            assert(LSBCSopsAuditToken(5, readOperations[i], buffer, sizeof(buffer), NULL) == -1 && errno == errors[j]);
            assert(calls.selfCalls == 1 && buffer[0] == 0xa5 && buffer[31] == 0xa5);
        }
    }
    for (size_t size = 0; size < sizeof(uint32_t); ++size) {
        memset(buffer, 0xa5, sizeof(buffer));
        resetCalls();
        assert(LSBCSopsAuditToken(5, LSBCSOpStatus, buffer, size, NULL) == -1 && errno == ERANGE);
        assert(calls.originalCalls == 1 && !calls.selfCalls && buffer[0] == 0xa5);
    }
    const size_t sizes[] = {0, 1, 4, SIZE_MAX};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        resetCalls();
        assert(!LSBCSopsAuditToken(5, LSBCSOpStatus, NULL, sizes[i], NULL) && errno == EDOM);
        assert(calls.selfCalls == 1 && !calls.selfBuffer && calls.selfSize == sizes[i]);
    }
    // Non-STATUS operations retain their syscall-specific NULL/size negotiation, including partial error responses.
    resetCalls();
    calls.selfResult = -1;
    calls.selfError = ERANGE;
    assert(LSBCSopsAuditToken(5, LSBCSOpIdentity, NULL, 0, NULL) == -1 && errno == ERANGE && calls.selfCalls == 1);
    resetCalls();
    calls.selfResult = -1;
    calls.selfError = ERANGE;
    calls.selfWritesOnError = true;
    memset(buffer, 0xa5, sizeof(buffer));
    assert(LSBCSopsAuditToken(5, LSBCSOpIdentity, buffer, sizeof(selfBytes), NULL) == -1 && errno == ERANGE);
    assert(!memcmp(buffer, selfBytes, sizeof(selfBytes)) && buffer[sizeof(selfBytes)] == 0xa5);
    resetCalls();
    calls.selfError = E2BIG;
    assert(!LSBCSopsAuditToken(5, LSBCSOpStatus, buffer + 1, 4, NULL) && errno == E2BIG); // Unaligned output is supported.
    uint32_t flags;
    memcpy(&flags, buffer + 1, 4);
    assert(flags == LSB_CODE_SIGNING_STATUS);
}

static void testKernelOutputValidation(void) {
    uint32_t nativeFlags = 0;
    assert(!csops(0, LSBCSOpStatus, &nativeFlags, sizeof(nativeFlags)));
    long pageSize = sysconf(_SC_PAGESIZE);
    assert(pageSize > 0);
    unsigned char *pages = mmap(NULL, (size_t)pageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(pages != MAP_FAILED && !mprotect(pages + pageSize, (size_t)pageSize, PROT_NONE));
    void *badOutputs[] = {(void *)(uintptr_t)1, pages + pageSize, pages + pageSize - 2};
    for (size_t i = 0; i < sizeof(badOutputs) / sizeof(*badOutputs); ++i) {
        resetCalls();
        calls.realSelf = true;
        assert(LSBCSopsAuditToken(999, LSBCSOpStatus, badOutputs[i], 4, NULL) == -1 && errno == EFAULT);
        assert(calls.selfCalls == 1);
    }
    assert(!mprotect(pages, (size_t)pageSize, PROT_READ));
    resetCalls();
    calls.realSelf = true;
    assert(LSBCSopsAuditToken(999, LSBCSOpStatus, pages, 4, NULL) == -1 && errno == EFAULT);
    assert(!mprotect(pages, (size_t)pageSize, PROT_READ | PROT_WRITE));
    resetCalls();
    calls.realSelf = true;
    assert(!LSBCSopsAuditToken(999, LSBCSOpStatus, pages + 1, 4, NULL) && errno == EDOM);
    uint32_t flags;
    memcpy(&flags, pages + 1, sizeof(flags));
    assert(flags == LSB_CODE_SIGNING_STATUS);
    resetCalls();
    calls.realSelf = true;
    assert(!LSBCSopsAuditToken(999, LSBCSOpStatus, NULL, 0, NULL) && errno == EDOM);
    // Deterministically model a racing thread revoking the destination after the self syscall validated and wrote it.
    resetCalls();
    calls.realSelf = true;
    calls.invalidateAfterSelf = pages;
    calls.invalidateSize = (size_t)pageSize;
    assert(LSBCSopsAuditToken(999, LSBCSOpStatus, pages, 4, NULL) == -1 && errno == EFAULT && calls.selfCalls == 1);
    assert(!mprotect(pages, (size_t)pageSize, PROT_READ | PROT_WRITE));
    assert(!munmap(pages, (size_t)pageSize * 2));
}

static void testDistinctPeersAndUncachedSnapshots(void) {
    const unsigned int operations[] = {LSBCSOpEntitlementsBlob, LSBCSOpDEREntitlementsBlob};
    audit_token_t targets[] = {{{1, 2, 3, 4, 5, 1234, 7, 8}}, {{11, 12, 13, 14, 15, 5678, 17, 18}}};
    for (size_t operation = 0; operation < sizeof(operations) / sizeof(*operations); ++operation) {
        unsigned char profiles[2][512] = {{0}};
        for (size_t target = 0; target < 2; ++target) {
            audit_token_t before = targets[target];
            resetCalls();
            assert(!LSBCSopsAuditToken((pid_t)before.val[5], operations[operation], profiles[target], sizeof(profiles[target]), &targets[target]));
            assert(!calls.originalCalls);
            assert(!calls.selfCalls && calls.brokerCalls == 1 && errno == EDOM && !memcmp(&before, &targets[target], sizeof(before)));
            assert(!memcmp(&calls.brokerTarget, &before, sizeof(before)) && calls.brokerOperation == operations[operation]);
            assert(LSBGuestEntitlementsHasValidBlobHeaderForOperation(profiles[target], readBE32(profiles[target] + 4), operations[operation]));
        }
        assert(memcmp(profiles[0], profiles[1], sizeof(profiles[0])));
        unsigned char updated[512] = {0};
        resetCalls();
        calls.profile = 1;
        assert(!LSBCSopsAuditToken(1234, operations[operation], updated, sizeof(updated), &targets[0]));
        assert(calls.brokerCalls == 1 && !memcmp(updated, profiles[1], sizeof(updated))); // No stale local profile cache.
        resetCalls();
        calls.brokerError = ESRCH;
        targets[0].val[7]++;
        memset(updated, 0xa5, sizeof(updated));
        assert(LSBCSopsAuditToken(1234, operations[operation], updated, sizeof(updated), &targets[0]) == -1 && errno == ESRCH);
        assert(calls.brokerCalls == 1 && calls.brokerTarget.val[7] == targets[0].val[7] && updated[0] == 0xa5);
    }
}

static void testBrokerErrorsAndMalformedReplies(void) {
    audit_token_t target = {{1, 2, 3, 4, 5, 777, 7, 8}};
    const unsigned operations[] = {LSBCSOpEntitlementsBlob, LSBCSOpDEREntitlementsBlob};
    const int errors[] = {ESRCH, ENOENT, EACCES, ENOTCONN, EPROTO, ENOMEM, EINVAL};
    for (size_t op = 0; op < sizeof(operations) / sizeof(*operations); ++op) {
        for (size_t i = 0; i < sizeof(errors) / sizeof(*errors); ++i) {
            unsigned char output[512];
            memset(output, 0xa5, sizeof(output));
            resetCalls();
            calls.originalResult = 0; // A native success must never hide an unavailable or unauthenticated guest snapshot.
            calls.brokerError = errors[i];
            assert(LSBCSopsAuditToken(777, operations[op], output, sizeof(output), &target) == -1 && errno == errors[i]);
            assert(calls.brokerCalls == 1 && !calls.selfCalls);
            for (size_t byte = 0; byte < sizeof(output); ++byte) assert(output[byte] == 0xa5);
        }
        for (unsigned malformed = 1; malformed <= 5; ++malformed) {
            unsigned char output[512];
            memset(output, 0xa5, sizeof(output));
            resetCalls();
            calls.malformedReply = malformed;
            assert(LSBCSopsAuditToken(777, operations[op], output, sizeof(output), &target) == -1 && errno == EPROTO);
            assert(calls.brokerCalls == 1 && !calls.selfCalls);
            for (size_t byte = 0; byte < sizeof(output); ++byte) assert(output[byte] == 0xa5);
        }
    }
}

static void testAuditTokenValidation(void) {
    audit_token_t actualSelf = {{0}}, peer = {{1, 2, 3, 4, 5, 777, 7, 8}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&actualSelf, &count) == KERN_SUCCESS);
    assert(count == TASK_AUDIT_TOKEN_COUNT && actualSelf.val[5] > 0 && actualSelf.val[5] <= INT_MAX);
    unsigned char output[512];
    const unsigned operations[] = {LSBCSOpEntitlementsBlob, LSBCSOpDEREntitlementsBlob};
    for (size_t op = 0; op < sizeof(operations) / sizeof(*operations); ++op) {
        audit_token_t *selfTokens[] = {NULL, &actualSelf};
        const pid_t selfPIDs[] = {0, (pid_t)actualSelf.val[5]};
        for (size_t token = 0; token < 2; ++token) {
            for (size_t pid = 0; pid < 2; ++pid) {
                resetCalls();
                assert(!LSBCSopsAuditToken(selfPIDs[pid], operations[op], output, sizeof(output), selfTokens[token]) && errno == EDOM);
                assert(calls.brokerCalls == 1 && !memcmp(&calls.brokerTarget, &actualSelf, sizeof(actualSelf)));
            }
        }
        audit_token_t invalid[] = {peer, peer, peer, actualSelf};
        invalid[0].val[5] = 0;
        invalid[1].val[5] = UINT32_MAX;
        invalid[2].val[5]++;
        invalid[3].val[7]++;
        const pid_t invalidPIDs[] = {777, 777, 777, 0};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
            resetCalls();
            assert(LSBCSopsAuditToken(invalidPIDs[i], operations[op], output, sizeof(output), &invalid[i]) == -1 && errno == EINVAL);
            assert(!calls.brokerCalls && !calls.selfCalls);
        }
        resetCalls();
        assert(LSBCSopsAuditToken(-1, operations[op], output, sizeof(output), &peer) == -1 && errno == EINVAL && !calls.brokerCalls);
        resetCalls();
        pid_t remote = actualSelf.val[5] == 777 ? 778 : 777;
        assert(LSBCSopsAuditToken(remote, operations[op], output, sizeof(output), NULL) == -1 && errno == EINVAL && !calls.brokerCalls);
        resetCalls();
        assert(LSBCSopsAuditToken(0, operations[op], output, sizeof(output), &peer) == -1 && errno == EINVAL && !calls.brokerCalls);
    }
    long pageSize = sysconf(_SC_PAGESIZE);
    assert(pageSize > (long)sizeof(peer));
    unsigned char *pages = mmap(NULL, (size_t)pageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(pages != MAP_FAILED && !mprotect(pages + pageSize, (size_t)pageSize, PROT_NONE));
    audit_token_t *badTokens[] = {(void *)(uintptr_t)1, (void *)(pages + pageSize), (void *)(pages + pageSize - 16)};
    for (size_t i = 0; i < sizeof(badTokens) / sizeof(*badTokens); ++i) {
        resetCalls();
        assert(LSBCSopsAuditToken(777, LSBCSOpDEREntitlementsBlob, output, sizeof(output), badTokens[i]) == -1 && errno == EFAULT);
        assert(!calls.brokerCalls && !calls.selfCalls);
        resetCalls();
        calls.originalResult = 0; // Enabled virtualization still validates the token even if a native query would succeed.
        assert(LSBCSopsAuditToken(777, LSBCSOpDEREntitlementsBlob, output, sizeof(output), badTokens[i]) == -1 && errno == EFAULT);
        assert(!calls.brokerCalls && !calls.originalCalls);
        assert(!unsetenv(LSB_CODE_SIGNING_ENABLE_ENV));
        resetCalls();
        calls.originalResult = 0; // Opt-out forwards the original pointer without reading it in the adapter.
        assert(!LSBCSopsAuditToken(777, LSBCSOpDEREntitlementsBlob, output, sizeof(output), badTokens[i]) && !calls.brokerCalls);
        assertOriginal(777, LSBCSOpDEREntitlementsBlob, output, sizeof(output), badTokens[i]);
        assert(!setenv(LSB_CODE_SIGNING_ENABLE_ENV, "1", 1));
    }
    memcpy(pages + 1, &peer, sizeof(peer));
    assert(!mprotect(pages, (size_t)pageSize, PROT_READ));
    resetCalls();
    assert(!LSBCSopsAuditToken(777, LSBCSOpDEREntitlementsBlob, output, sizeof(output), (void *)(pages + 1)) && errno == EDOM);
    assert(!memcmp(&calls.brokerTarget, &peer, sizeof(peer))); // Read-only, unaligned input requires no writes or direct loads.
    assert(!munmap(pages, (size_t)pageSize * 2));
}

static void testEntitlementBufferSemantics(void) {
    audit_token_t token = {{1, 2, 3, 4, 5, 777, 7, 8}};
    const unsigned int operations[] = {LSBCSOpEntitlementsBlob, LSBCSOpDEREntitlementsBlob};
    for (size_t operation = 0; operation < sizeof(operations) / sizeof(*operations); ++operation) {
        unsigned char complete[512], output[512];
        resetCalls();
        assert(!LSBCSopsAuditToken(777, operations[operation], complete, sizeof(complete), &token));
        uint32_t length = readBE32(complete + 4);
        for (size_t size = 0; size < length; ++size) {
            memset(output, 0xa5, sizeof(output));
            resetCalls();
            assert(LSBCSopsAuditToken(777, operations[operation], output, size, &token) == -1 && errno == ERANGE);
            assert(!calls.selfCalls);
            if (size >= 8) assert(readBE32(output) == 0 && readBE32(output + 4) == length);
            for (size_t i = size >= 8 ? 8 : 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
        }
        const size_t sizes[] = {length, length + 1, sizeof(output) - 1, SIZE_MAX};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
            memset(output, 0xa5, sizeof(output));
            resetCalls();
            assert(!LSBCSopsAuditToken(777, operations[operation], output + 1, sizes[i], &token) && errno == EDOM && !calls.selfCalls);
            assert(output[0] == 0xa5 && !memcmp(output + 1, complete, length));
            for (size_t offset = length + 1; offset < sizeof(output); ++offset) assert(output[offset] == 0xa5);
        }

        long pageSize = sysconf(_SC_PAGESIZE);
        assert(pageSize > 0);
        unsigned char *pages = mmap(NULL, (size_t)pageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(pages != MAP_FAILED && !mprotect(pages + pageSize, (size_t)pageSize, PROT_NONE));
        const size_t failingSizes[] = {8, length, SIZE_MAX};
        void *badOutputs[] = {NULL, (void *)(uintptr_t)1, pages + pageSize, pages + pageSize - 4};
        for (size_t bad = 0; bad < sizeof(badOutputs) / sizeof(*badOutputs); ++bad) {
            for (size_t size = 0; size < sizeof(failingSizes) / sizeof(*failingSizes); ++size) {
                resetCalls();
                assert(LSBCSopsAuditToken(777, operations[operation], badOutputs[bad], failingSizes[size], &token) == -1 && errno == EFAULT);
                assert(!calls.selfCalls);
            }
            resetCalls();
            assert(LSBCSopsAuditToken(777, operations[operation], badOutputs[bad], 7, &token) == -1 && errno == ERANGE);
        }
        assert(!mprotect(pages, (size_t)pageSize, PROT_READ));
        resetCalls();
        assert(LSBCSopsAuditToken(777, operations[operation], pages, length, &token) == -1 && errno == EFAULT);
        assert(!munmap(pages, (size_t)pageSize * 2));
    }
}

static void testCodeDirectoryHashes(void) {
    audit_token_t first = {{1, 2, 3, 4, 5, 777, 7, 8}}, second = {{1, 2, 3, 4, 5, 778, 7, 9}};
    unsigned char a[LSB_GUEST_CDHASH_BYTES + 2], b[LSB_GUEST_CDHASH_BYTES];
    memset(a, 0xa5, sizeof(a));
    resetCalls();
    assert(!LSBCSopsAuditToken(777, LSBCSOpCDHash, a + 1, sizeof(b), &first) && errno == EDOM);
    assert(!calls.selfCalls && calls.brokerCalls == 1 && !memcmp(&calls.brokerTarget, &first, sizeof(first)));
    assert(a[0] == 0xa5 && a[sizeof(a) - 1] == 0xa5);
    resetCalls();
    assert(!LSBCSopsAuditToken(778, LSBCSOpCDHash, b, sizeof(b), &second) && memcmp(a + 1, b, sizeof(b)));
    const int errors[] = {ENOENT, EINVAL, EACCES, ESRCH, EIO};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(*errors); ++i) {
        memset(b, 0xa5, sizeof(b));
        resetCalls();
        calls.brokerError = errors[i];
        assert(LSBCSopsAuditToken(777, LSBCSOpCDHash, b, sizeof(b), &first) == -1 && errno == errors[i]);
        assert(!calls.selfCalls && b[0] == 0xa5 && b[sizeof(b) - 1] == 0xa5);
    }
    const size_t sizes[] = {0, 1, LSB_GUEST_CDHASH_BYTES - 1, LSB_GUEST_CDHASH_BYTES + 1, SIZE_MAX};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        resetCalls();
        assert(LSBCSopsAuditToken(777, LSBCSOpCDHash, b, sizes[i], &first) == -1 && errno == EINVAL);
        assert(!calls.brokerCalls && !calls.selfCalls);
    }
    resetCalls();
    assert(LSBCSopsAuditToken(778, LSBCSOpCDHash, b, sizeof(b), &first) == -1 && errno == EINVAL && !calls.brokerCalls);
    long pageSize = sysconf(_SC_PAGESIZE);
    assert(pageSize > 0);
    unsigned char *pages = mmap(NULL, (size_t)pageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(pages != MAP_FAILED && !mprotect(pages + pageSize, (size_t)pageSize, PROT_NONE));
    void *invalid[] = {NULL, (void *)(uintptr_t)1, pages + pageSize, pages + pageSize - 10};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        resetCalls();
        assert(LSBCSopsAuditToken(777, LSBCSOpCDHash, invalid[i], sizeof(b), &first) == -1 && errno == EFAULT);
    }
    assert(!mprotect(pages, (size_t)pageSize, PROT_READ));
    resetCalls();
    assert(LSBCSopsAuditToken(777, LSBCSOpCDHash, pages, sizeof(b), &first) == -1 && errno == EFAULT);
    assert(!munmap(pages, (size_t)pageSize * 2));
}

int main(void) {
    testDisabledAndOriginalResults();
    testEntitlementSnapshotPriority();
    testReadAllowlistAndIdentity();
    testFallbackErrorsAndStatusBounds();
    testKernelOutputValidation();
    testDistinctPeersAndUncachedSnapshots();
    testBrokerErrorsAndMalformedReplies();
    testAuditTokenValidation();
    testEntitlementBufferSemantics();
    testCodeDirectoryHashes();
    puts("CodeSigningCompatibility: XML/DER snapshot priority, EPERM-only other reads, audit validation, opt-out and kernel copies passed");
    return 0;
}
