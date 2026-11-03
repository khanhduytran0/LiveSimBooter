#pragma once

#include <fcntl.h>

// Bash 3.2.57 make_redirection (+0x11abc) allocates 40 bytes. do_redirection_internal
// (+0x47b88) dispatches opcode 7 to +0x47e40, which consumes the integer member of the redirectee union.
typedef struct {
    uintptr_t next;
    int32_t fd, flags, instruction;
    union { int32_t fd; uintptr_t word; } target;
    uintptr_t hereDoc;
} BashRedirect;
_Static_assert(sizeof(BashRedirect) == 40 && offsetof(BashRedirect, instruction) == 0x10 && offsetof(BashRedirect, target) == 0x18, "Bash REDIRECT");

static int addBashRedirectPages(TrialPlan *plan, uintptr_t header, uintptr_t redirects, size_t pageSize) {
    if (!redirects) return 0;
    // check_bash_input (+0x38984) may move a buffered script's input FD before dup2.
    // The caller already excluded default_buffered_input > 0. Also exclude a buffered stream actually using stdout/stderr.
    enum { BashInput = 0xa6c78, BufferedStream = 4, DuplicatingOutput = 7 };
    int32_t inputType, inputFD;
    if (!forkTrialRead(header + BashInput, &inputType, sizeof(inputType)) ||
        !forkTrialRead(header + BashInput + 0x10, &inputFD, sizeof(inputFD))) return EFAULT;
    for (int fd = STDOUT_FILENO; fd <= STDERR_FILENO; ++fd) {
        // The observer is launched by posix_spawn; a CLOEXEC descriptor would be missing in it.
        int flags = fcntl(fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) || (inputType == BufferedStream && inputFD == fd)) return ENOTSUP;
    }
    for (unsigned count = 0; redirects; ++count) {
        BashRedirect redirect;
        if (count == LSB_MAX_ACTIONS) return E2BIG;
        int error = addBashObject(plan, redirects, &redirect, sizeof(redirect), pageSize);
        if (error) return error;
        if (redirect.instruction != DuplicatingOutput || redirect.flags || redirect.hereDoc ||
            redirect.fd < STDOUT_FILENO || redirect.fd > STDERR_FILENO ||
            redirect.target.fd < STDOUT_FILENO || redirect.target.fd > STDERR_FILENO) return ENOTSUP;
        redirects = redirect.next;
    }
    return 0;
}
