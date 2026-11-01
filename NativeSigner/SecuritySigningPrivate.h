#pragma once

#include <TargetConditionals.h>
#include <Security/Security.h>

#if TARGET_OS_OSX
#include <Security/SecStaticCode.h>
#include <Security/CMSEncoder.h>
#else
typedef uint32_t SecCSFlags;
typedef struct __SecStaticCode *SecStaticCodeRef;
typedef struct __CMSEncoder *CMSEncoderRef;
enum { kCMSAttrSigningTime = 0x8, kCMSAttrAppleCodesigningHashAgility = 0x10, kCMSAttrAppleCodesigningHashAgilityV2 = 0x20 };
enum { kCMSCertificateSignerOnly = 1 };
extern const CFStringRef kCMSEncoderDigestAlgorithmSHA256;
extern OSStatus SecStaticCodeCreateWithPathAndAttributes(CFURLRef path, SecCSFlags flags, CFDictionaryRef attributes, SecStaticCodeRef *code);
extern OSStatus CMSEncoderCreate(CMSEncoderRef *encoder);
extern OSStatus CMSEncoderAddSigners(CMSEncoderRef encoder, CFTypeRef signers);
extern OSStatus CMSEncoderAddSupportingCerts(CMSEncoderRef encoder, CFTypeRef certificates);
extern OSStatus CMSEncoderSetSignerAlgorithm(CMSEncoderRef encoder, CFStringRef algorithm);
extern OSStatus CMSEncoderSetHasDetachedContent(CMSEncoderRef encoder, Boolean detached);
extern OSStatus CMSEncoderSetCertificateChainMode(CMSEncoderRef encoder, unsigned mode);
extern OSStatus CMSEncoderAddSignedAttributes(CMSEncoderRef encoder, unsigned attributes);
extern OSStatus CMSEncoderUpdateContent(CMSEncoderRef encoder, const void *content, size_t length);
extern OSStatus CMSEncoderCopyEncodedContent(CMSEncoderRef encoder, CFDataRef *content);
#endif

// Security's iOS code-signing SPI. Declarations follow Apple's SecCodeSigner.h and CMSPrivate.h.
typedef struct __SecCodeSigner *SecCodeSignerRef;
extern const CFStringRef kSecCodeSignerIdentity, kSecCodeSignerIdentifier, kSecCodeSignerFlags, kSecCodeSignerPageSize;
extern const CFStringRef kSecCodeSignerDigestAlgorithm, kSecCodeSignerRequireTimestamp, kSecCodeSignerPreserveMetadata;
extern OSStatus SecCodeSignerCreate(CFDictionaryRef parameters, SecCSFlags flags, SecCodeSignerRef *signer);
extern OSStatus SecCodeSignerAddSignatureWithErrors(SecCodeSignerRef signer, SecStaticCodeRef code, SecCSFlags flags, CFErrorRef *error);
extern OSStatus CMSEncoderSetSigningTime(CMSEncoderRef encoder, CFAbsoluteTime time);
extern OSStatus CMSEncoderSetAppleCodesigningHashAgility(CMSEncoderRef encoder, CFDataRef agility);
extern OSStatus CMSEncoderSetAppleCodesigningHashAgilityV2(CMSEncoderRef encoder, CFDictionaryRef agility);
