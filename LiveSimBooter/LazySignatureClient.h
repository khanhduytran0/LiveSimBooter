#ifndef LSB_LAZY_SIGNATURE_CLIENT_H
#define LSB_LAZY_SIGNATURE_CLIENT_H

#include <mach/mach.h>
#include <sys/fcntl.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

// Native startup only, once before mapping dyld_sim. Borrows a send right retained by the caller until process exit.
// A null endpoint disables lazy signing. No credentials or native XPC object cross the simulator handoff.
void LSBConfigureLazySignature(mach_port_t endpoint, const audit_token_t *hostAudit);

// Called only after genuine F_ADDFILESIGS_RETURN failure. Returns an errno value and preserves the caller's errno.
// On success a verified replacement is registered, installed at fd, and the signature receives the kernel's covered length.
// Uses app-resident code/direct syscalls only, including before guest TLS and constructors are initialized.
int LSBLazySignatureRetry(int fd, fsignatures_t *signature);

// dyld helper only: preflight before dyld captures its persistent inode/mtime. General guest stat/fstat remain native.
// Disabled mode forwards directly. A repair returns actual published metadata, never an alias of the previous inode.
int LSBLazySignatureDyldStat(const char *path, struct stat *info);

#ifdef __cplusplus
}
#endif

#endif
