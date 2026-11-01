#ifndef LSB_NATIVE_SIGNING_PROTOCOL_H
#define LSB_NATIVE_SIGNING_PROTOCOL_H

// Native host/signer XPC protocol. Independent of the pre-TLS guest Mach signing ABI.
enum {
    LSB_NATIVE_SIGNING_VERSION = 2,
    LSB_NATIVE_SIGNING_MAX_SIGNATURE_BYTES = 128 * 1024 * 1024
};

#endif
