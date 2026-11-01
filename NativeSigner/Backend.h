#ifndef LSB_NATIVE_SIGNER_BACKEND_H
#define LSB_NATIVE_SIGNER_BACKEND_H
#include <mach/mach.h>

// Only the broker-selected native role loads this backend. No guest TLS or simulator libraries are installed.
typedef int (*LSBNativeSignerPrepareFunction)(const audit_token_t *host, mach_port_t *endpoint);
typedef void (*LSBNativeSignerRunFunction)(void);
int LSBNativeSignerPrepare(const audit_token_t *host, mach_port_t *endpoint);
void LSBNativeSignerRun(void);
#endif
