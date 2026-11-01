#ifndef LSB_NATIVE_SIGNER_LOADER_H
#define LSB_NATIVE_SIGNER_LOADER_H
#include "../NativeSigner/Backend.h"
int LSBLoadNativeSigner(const audit_token_t *host, mach_port_t *endpoint, LSBNativeSignerRunFunction *run);
#endif
