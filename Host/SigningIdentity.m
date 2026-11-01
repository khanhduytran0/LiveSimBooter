#import "SigningIdentity.h"
#import <Security/Security.h>

NSNotificationName const LSBSigningIdentityDidChangeNotification = @"LSBSigningIdentityDidChange";
const NSUInteger LSBSigningPKCS12MaximumSize = 16 * 1024 * 1024;
static NSString *const LazySigningPreference = @"LSBLazySigningEnabled";
static NSString *const IdentityService = @"LiveSimBooter.SigningIdentity";
static NSString *const IdentityAccount = @"runtime-signing";

@interface LSBSigningCredentials ()
@property (nonatomic, readwrite, copy) NSData *pkcs12Data;
@property (nonatomic, readwrite, copy) NSString *password, *label, *revision;
@end
@implementation LSBSigningCredentials
@end

static void notifyIdentityChanged(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:LSBSigningIdentityDidChangeNotification object:nil];
    });
}

BOOL LSBLazySigningEnabled(void) { return [NSUserDefaults.standardUserDefaults boolForKey:LazySigningPreference]; }
void LSBSaveConfiguredLazySigningEnabled(BOOL enabled) {
    if (enabled == LSBLazySigningEnabled()) return;
    [NSUserDefaults.standardUserDefaults setBool:enabled forKey:LazySigningPreference];
    notifyIdentityChanged();
}

static NSDictionary *identityQuery(void) {
    // No access group: this stays in the host app's private keychain domain, independent of the runtime's store App Group.
    return @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword, (__bridge id)kSecAttrService: IdentityService,
             (__bridge id)kSecAttrAccount: IdentityAccount, (__bridge id)kSecAttrSynchronizable: @NO,
             (__bridge id)kSecUseDataProtectionKeychain: @YES};
}

static BOOL reportError(NSError **error, NSInteger code, NSString *description) {
    if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey: description}];
    return NO;
}

static BOOL keychainError(NSError **error, OSStatus status) {
    return reportError(error, status, NSLocalizedString(@"Could not access the signing identity in this app's keychain.",
        @"Signing identity error when app-private keychain storage is unavailable."));
}

static NSDictionary *copyIdentityMetadata(void) {
    NSMutableDictionary *query = [identityQuery() mutableCopy];
    query[(__bridge id)kSecReturnAttributes] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef result = NULL;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    id object = CFBridgingRelease(result);
    return status == errSecSuccess && [object isKindOfClass:NSDictionary.class] ? object : nil;
}

NSString *LSBSigningIdentitySummary(void) {
    id label = copyIdentityMetadata()[(__bridge id)kSecAttrLabel];
    return [label isKindOfClass:NSString.class] && [label length] ? label : nil;
}

NSString *LSBSigningIdentityRevision(void) {
    id revision = copyIdentityMetadata()[(__bridge id)kSecAttrGeneric];
    return [revision isKindOfClass:NSData.class] ? [[NSString alloc] initWithData:revision encoding:NSUTF8StringEncoding] : nil;
}

static LSBSigningCredentials *credentialsForPayload(NSData *payload) {
    if (payload.length > LSBSigningPKCS12MaximumSize + 16384) return nil;
    id object = [NSPropertyListSerialization propertyListWithData:payload options:NSPropertyListImmutable format:NULL error:nil];
    if (![object isKindOfClass:NSDictionary.class]) return nil;
    NSDictionary *values = object;
    id data = values[@"pkcs12"], password = values[@"password"], label = values[@"label"], revision = values[@"revision"];
    if (![data isKindOfClass:NSData.class] || ![data length] || [data length] > LSBSigningPKCS12MaximumSize ||
        ![password isKindOfClass:NSString.class] || [password lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 4096 ||
        ![label isKindOfClass:NSString.class] || ![label length] || [label length] > 4096 ||
        ![revision isKindOfClass:NSString.class] || ![[NSUUID alloc] initWithUUIDString:revision]) return nil;
    LSBSigningCredentials *credentials = [LSBSigningCredentials new];
    credentials.pkcs12Data = data;
    credentials.password = password;
    credentials.label = label;
    credentials.revision = revision;
    return credentials;
}

LSBSigningCredentials *LSBCopySigningCredentials(NSError **error) {
    NSMutableDictionary *query = [identityQuery() mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef result = NULL;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    id payload = CFBridgingRelease(result);
    if (status != errSecSuccess) { keychainError(error, status); return nil; }
    LSBSigningCredentials *credentials = [payload isKindOfClass:NSData.class] ? credentialsForPayload(payload) : nil;
    if (!credentials) reportError(error, errSecDecode, NSLocalizedString(@"The stored signing identity is invalid. Import the P12 again.",
        @"Signing identity error when a keychain payload is malformed."));
    return credentials;
}

BOOL LSBImportSigningIdentity(NSData *data, NSString *password, NSError **error) {
    if (!data.length || data.length > LSBSigningPKCS12MaximumSize || !password || [password lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 4096)
        return reportError(error, errSecParam, NSLocalizedString(@"Choose a P12 or PFX file smaller than 16 MB.", @"Signing import input size error."));
    CFArrayRef imported = NULL;
    NSDictionary *options = @{(__bridge id)kSecImportExportPassphrase: password, (__bridge id)kSecImportToMemoryOnly: @YES};
    OSStatus status = SecPKCS12Import((__bridge CFDataRef)data, (__bridge CFDictionaryRef)options, &imported);
    NSArray *items = CFBridgingRelease(imported);
    if (status != errSecSuccess) return reportError(error, status,
        NSLocalizedString(@"The P12 could not be opened. Check its password and that the file contains a signing identity.",
            @"Signing import error for a wrong password or malformed PKCS12 file."));
    if (items.count != 1 || ![items.firstObject isKindOfClass:NSDictionary.class]) return reportError(error, errSecParam,
        NSLocalizedString(@"The P12 must contain exactly one certificate with its private key.", @"Signing import error for ambiguous or missing identities."));
    id identityValue = items.firstObject[(__bridge id)kSecImportItemIdentity];
    if (!identityValue || CFGetTypeID((__bridge CFTypeRef)identityValue) != SecIdentityGetTypeID()) return reportError(error, errSecParam,
        NSLocalizedString(@"The P12 must contain exactly one certificate with its private key.", @"Signing import error for ambiguous or missing identities."));
    SecIdentityRef identity = (__bridge SecIdentityRef)identityValue;
    SecCertificateRef certificate = NULL;
    SecKeyRef key = NULL;
    status = SecIdentityCopyCertificate(identity, &certificate);
    if (!status) status = SecIdentityCopyPrivateKey(identity, &key);
    NSString *label = certificate ? CFBridgingRelease(SecCertificateCopySubjectSummary(certificate)) : nil;
    if (certificate) CFRelease(certificate);
    if (key) CFRelease(key);
    if (status || !label.length || label.length > 4096) return reportError(error, status ?: errSecParam,
        NSLocalizedString(@"The P12 must contain exactly one certificate with its private key.", @"Signing import error for ambiguous or missing identities."));
    NSString *revision = NSUUID.UUID.UUIDString;
    NSDictionary *values = @{ @"pkcs12": data, @"password": password, @"label": label, @"revision": revision };
    NSData *payload = [NSPropertyListSerialization dataWithPropertyList:values format:NSPropertyListBinaryFormat_v1_0 options:0 error:error];
    if (!payload) return NO;
    NSDictionary *attributes = @{(__bridge id)kSecValueData: payload, (__bridge id)kSecAttrLabel: label,
        (__bridge id)kSecAttrGeneric: [revision dataUsingEncoding:NSUTF8StringEncoding],
        (__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    status = SecItemUpdate((__bridge CFDictionaryRef)identityQuery(), (__bridge CFDictionaryRef)attributes);
    if (status == errSecItemNotFound) {
        NSMutableDictionary *item = [identityQuery() mutableCopy];
        [item addEntriesFromDictionary:attributes];
        status = SecItemAdd((__bridge CFDictionaryRef)item, NULL);
    }
    if (status) return keychainError(error, status);
    notifyIdentityChanged();
    return YES;
}

BOOL LSBRemoveSigningIdentity(NSError **error) {
    OSStatus status = SecItemDelete((__bridge CFDictionaryRef)identityQuery());
    if (status != errSecSuccess && status != errSecItemNotFound) return keychainError(error, status);
    LSBSaveConfiguredLazySigningEnabled(NO);
    notifyIdentityChanged();
    return YES;
}
