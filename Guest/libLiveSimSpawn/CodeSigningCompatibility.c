#include "CodeSigningCompatibility.h"
#include "../../ProcessSupport/GuestEntitlements.h"
#include "../../ProcessSupport/SpawnTransport.h"

#include <errno.h>
#include <limits.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Exported by libSystem, but the simulator SDK intentionally makes mach/mach_vm.h unavailable.
extern kern_return_t mach_vm_write(vm_map_t task, mach_vm_address_t address, vm_offset_t data, mach_msg_type_number_t size);

extern kern_return_t mach_vm_read_overwrite(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size,
                                            mach_vm_address_t destination, mach_vm_size_t *copied);

static int currentAuditToken(audit_token_t *token) {
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    kern_return_t result = task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)token, &count);
    if (result != KERN_SUCCESS || count != TASK_AUDIT_TOKEN_COUNT || !token->val[5] || token->val[5] > INT_MAX) return EIO;
    return 0;
}

static int entitlementTarget(pid_t pid, const audit_token_t *source, audit_token_t *target) {
    if (pid < 0) return EINVAL;
    if (source) {
        mach_vm_size_t copied = 0;
        kern_return_t result = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)source, sizeof(*target),
                                                     (mach_vm_address_t)(uintptr_t)target, &copied);
        if (result != KERN_SUCCESS || copied != sizeof(*target)) return EFAULT;
        if (!target->val[5] || target->val[5] > INT_MAX || (pid && (uint32_t)pid != target->val[5])) return EINVAL;
        if (pid) return 0; // The broker checks the complete identity, including PID version, against its running guest job.
    }
    audit_token_t self = {{0}};
    int error = currentAuditToken(&self);
    if (error) return error;
    if (source) return memcmp(target, &self, sizeof(self)) ? EINVAL : 0;
    if (pid && (uint32_t)pid != self.val[5]) return EINVAL;
    *target = self; // NULL token or pid zero can name only the actual kernel process, never a guessed remote identity.
    return 0;
}

static int copyBytes(void *buffer, const void *bytes, mach_msg_type_number_t size) {
    // Kernel copy semantics handle NULL, unmapped, read-only and page-crossing destinations without dereferencing them here.
    kern_return_t result = mach_vm_write(mach_task_self(), (mach_vm_address_t)(uintptr_t)buffer, (vm_offset_t)(uintptr_t)bytes, size);
    if (result != KERN_SUCCESS) { errno = EFAULT; return -1; }
    return 0;
}

static int copyEntitlements(const unsigned char *blob, size_t length, void *buffer, size_t size) {
    if (size < 8) { errno = ERANGE; return -1; }
    unsigned char shortHeader[8] = {0};
    bool shortBuffer = size < length;
    if (shortBuffer) {
        memcpy(shortHeader + 4, blob + 4, 4);
        blob = shortHeader;
        length = sizeof(shortHeader);
    }
    if (copyBytes(buffer, blob, (mach_msg_type_number_t)length)) return -1;
    if (shortBuffer) { errno = ERANGE; return -1; }
    return 0;
}

static int copyGuestEntitlements(pid_t pid, unsigned operation, void *buffer, size_t size, audit_token_t *source, int incomingErrno) {
    audit_token_t target = {{0}};
    int error = entitlementTarget(pid, source, &target);
    unsigned char *blob = NULL;
    size_t length = 0;
    if (!error) error = LSBTransportCopyEntitlements(&target, operation, &blob, &length);
    if (!error && !LSBGuestEntitlementsHasValidBlobHeaderForOperation(blob, length, operation)) error = EPROTO;
    int result = -1;
    if (!error) {
        result = copyEntitlements(blob, length, buffer, size);
        error = result ? errno : incomingErrno;
    }
    free(blob);
    errno = error;
    return result;
}

static int copyGuestCodeDirectoryHash(pid_t pid, void *buffer, size_t size, audit_token_t *source, int incomingErrno) {
    if (size != LSB_GUEST_CDHASH_BYTES) { errno = EINVAL; return -1; }
    audit_token_t target = {{0}};
    int error = entitlementTarget(pid, source, &target);
    unsigned char hash[LSB_GUEST_CDHASH_BYTES];
    if (!error) error = LSBTransportCopyCodeDirectoryHash(&target, hash);
    if (error) { errno = error; return -1; }
    if (copyBytes(buffer, hash, sizeof(hash))) return -1;
    errno = incomingErrno;
    return 0;
}

static bool supportedRead(unsigned int operation) {
    switch (operation) {
        case LSBCSOpStatus:
        case LSBCSOpCDHash:
        case LSBCSOpIdentity:
        case LSBCSOpTeamID:
        case LSBCSOpValidationCategory:
            return true;
        default:
            return false;
    }
}

int LSBCSopsAuditToken(pid_t pid, unsigned int operation, void *buffer, size_t size, audit_token_t *token) {
    int incomingErrno = errno;
    if (operation == LSBCSOpEntitlementsBlob || operation == LSBCSOpDEREntitlementsBlob) {
        const char *enabled = getenv(LSB_CODE_SIGNING_ENABLE_ENV);
        // Native self reads describe LiveProcess, not the mapped guest. Security caches EINVAL/empty success as no entitlements,
        // so these two opted-in operations must use the registered image snapshot even when the kernel would not return EPERM.
        // The broker authenticates the complete target audit token; missing, stale or unknown guest identities never fall back.
        if (enabled && !strcmp(enabled, "1")) return copyGuestEntitlements(pid, operation, buffer, size, token, incomingErrno);
        errno = incomingErrno;
    }
    int result = csops_audittoken(pid, operation, buffer, size, token);
    int originalErrno = errno;
    if (result != -1 || originalErrno != EPERM || !supportedRead(operation)) return result;

    const char *enabled = getenv(LSB_CODE_SIGNING_ENABLE_ENV);
    if (!enabled || strcmp(enabled, "1")) {
        errno = originalErrno;
        return result;
    }
    // XNU STATUS otherwise writes four bytes regardless of usersize. Do not extend a caller's declared destination.
    if (operation == LSBCSOpStatus && buffer && size < sizeof(uint32_t)) {
        errno = ERANGE;
        return -1;
    }

    // Zero selects the actual kernel process, independently of the guest getpid interposition. Restore the caller's
    // errno before this syscall so a successful fallback does not leak the preceding, handled EPERM.
    errno = incomingErrno;
    if (operation == LSBCSOpCDHash) return copyGuestCodeDirectoryHash(pid, buffer, size, token, incomingErrno);
    result = csops(0, operation, buffer, size);
    if (result == 0 && operation == LSBCSOpStatus && buffer) {
        // Revalidate through a kernel copy: another thread may invalidate the destination after the successful self query.
        // NULL remains a successful no-copy STATUS query.
        uint32_t flags = LSB_CODE_SIGNING_STATUS;
        return copyBytes(buffer, &flags, sizeof(flags));
    }
    return result;
}

#ifndef LSB_CODE_SIGNING_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBCSopsAuditToken, (const void *)&csops_audittoken}
};
#endif
