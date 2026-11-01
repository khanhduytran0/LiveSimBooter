#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <assert.h>

// Exercise real PKCS12 parsing with a generated test identity; never read or write the user's keychain/defaults.
@interface SigningTestDefaults : NSObject
@property (class, readonly) SigningTestDefaults *standardUserDefaults;
- (BOOL)boolForKey:(NSString *)key;
- (void)setBool:(BOOL)value forKey:(NSString *)key;
@end
@implementation SigningTestDefaults {
    BOOL _enabled;
}
+ (instancetype)standardUserDefaults {
    static SigningTestDefaults *defaults;
    if (!defaults) defaults = [self new];
    return defaults;
}
- (BOOL)boolForKey:(NSString *)key { assert([key isEqual:@"LSBLazySigningEnabled"]); return _enabled; }
- (void)setBool:(BOOL)value forKey:(NSString *)key { assert([key isEqual:@"LSBLazySigningEnabled"]); _enabled = value; }
@end

static OSStatus testCopy(CFDictionaryRef query, CFTypeRef *result);
static OSStatus testAdd(CFDictionaryRef attributes, CFTypeRef *result);
static OSStatus testUpdate(CFDictionaryRef query, CFDictionaryRef attributes);
static OSStatus testDelete(CFDictionaryRef query);
static OSStatus testImport(CFDataRef data, CFDictionaryRef options, CFArrayRef *items);
#define SecItemCopyMatching testCopy
#define SecItemAdd testAdd
#define SecItemUpdate testUpdate
#define SecItemDelete testDelete
#define SecPKCS12Import testImport
#define NSUserDefaults SigningTestDefaults
#include "../Host/SigningIdentity.m"
#undef SecItemCopyMatching
#undef SecItemAdd
#undef SecItemUpdate
#undef SecItemDelete
#undef SecPKCS12Import
#undef NSUserDefaults

static NSMutableDictionary *stored;
static OSStatus injectedWriteStatus, injectedReadStatus;
static unsigned imports, writes, metadataReads, secretReads;

static NSDictionary *checkQuery(CFDictionaryRef queryRef) {
    NSDictionary *query = (__bridge NSDictionary *)queryRef;
    assert([query[(__bridge id)kSecClass] isEqual:(__bridge id)kSecClassGenericPassword]);
    assert([query[(__bridge id)kSecAttrService] isEqual:@"LiveSimBooter.SigningIdentity"]);
    assert([query[(__bridge id)kSecAttrAccount] isEqual:@"runtime-signing"]);
    assert(!query[(__bridge id)kSecAttrAccessGroup]);
    assert(query[(__bridge id)kSecAttrSynchronizable] == (__bridge id)kCFBooleanFalse);
    assert(query[(__bridge id)kSecUseDataProtectionKeychain] == (__bridge id)kCFBooleanTrue);
    return query;
}

static OSStatus testCopy(CFDictionaryRef queryRef, CFTypeRef *result) {
    NSDictionary *query = checkQuery(queryRef);
    assert(result);
    assert([query[(__bridge id)kSecMatchLimit] isEqual:(__bridge id)kSecMatchLimitOne]);
    if (query[(__bridge id)kSecReturnData]) ++secretReads;
    else { assert(query[(__bridge id)kSecReturnAttributes]); ++metadataReads; }
    *result = NULL;
    if (injectedReadStatus) return injectedReadStatus;
    if (!stored) return errSecItemNotFound;
    if (query[(__bridge id)kSecReturnData]) *result = CFBridgingRetain(stored[(__bridge id)kSecValueData]);
    else *result = CFBridgingRetain(@{(__bridge id)kSecAttrLabel: stored[(__bridge id)kSecAttrLabel],
                                    (__bridge id)kSecAttrGeneric: stored[(__bridge id)kSecAttrGeneric]});
    return errSecSuccess;
}

static OSStatus testAdd(CFDictionaryRef attributes, CFTypeRef *result) {
    assert(!result);
    NSDictionary *item = checkQuery(attributes);
    ++writes;
    if (injectedWriteStatus) return injectedWriteStatus;
    if (stored) return errSecDuplicateItem;
    stored = [item mutableCopy];
    return errSecSuccess;
}

static OSStatus testUpdate(CFDictionaryRef query, CFDictionaryRef attributes) {
    checkQuery(query);
    ++writes;
    if (injectedWriteStatus) return injectedWriteStatus;
    if (!stored) return errSecItemNotFound;
    [stored addEntriesFromDictionary:(__bridge NSDictionary *)attributes];
    return errSecSuccess;
}

static OSStatus testDelete(CFDictionaryRef query) {
    checkQuery(query);
    ++writes;
    if (injectedWriteStatus) return injectedWriteStatus;
    if (!stored) return errSecItemNotFound;
    stored = nil;
    return errSecSuccess;
}

static OSStatus testImport(CFDataRef data, CFDictionaryRef options, CFArrayRef *items) {
    ++imports;
    assert(CFDictionaryGetValue(options, kSecImportToMemoryOnly) == kCFBooleanTrue);
    return SecPKCS12Import(data, options, items);
}

static void testCredentialStorage(NSData *p12) {
    assert(!LSBLazySigningEnabled());
    assert(!LSBSigningIdentitySummary() && !LSBSigningIdentityRevision());
    assert(!secretReads && metadataReads == 2);
    NSError *error = nil;
    assert(!LSBCopySigningCredentials(&error) && error.code == errSecItemNotFound);
    unsigned previousWrites = writes;
    assert(!LSBImportSigningIdentity(p12, @"wrong password", &error));
    assert(!stored && writes == previousWrites);
    assert(!LSBImportSigningIdentity([@"not a P12" dataUsingEncoding:NSUTF8StringEncoding], @"", &error));
    assert(!stored && writes == previousWrites);
    assert(LSBImportSigningIdentity(p12, @"fixture-only", &error));
    assert(stored[(__bridge id)kSecAttrAccessible] == (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    unsigned beforeMetadata = secretReads;
    NSString *label = LSBSigningIdentitySummary(), *revision = LSBSigningIdentityRevision();
    assert(label.length && [[NSUUID alloc] initWithUUIDString:revision]);
    assert(secretReads == beforeMetadata);
    LSBSigningCredentials *snapshot = LSBCopySigningCredentials(&error);
    assert([snapshot.pkcs12Data isEqual:p12] && [snapshot.password isEqual:@"fixture-only"]);
    assert([snapshot.label isEqual:label] && [snapshot.revision isEqual:revision]);
    LSBSaveConfiguredLazySigningEnabled(YES);
    assert(LSBLazySigningEnabled());
    injectedWriteStatus = errSecInteractionNotAllowed;
    assert(!LSBImportSigningIdentity(p12, @"fixture-only", &error));
    assert(error.code == errSecInteractionNotAllowed && [revision isEqual:LSBSigningIdentityRevision()]);
    assert(!LSBRemoveSigningIdentity(&error) && LSBLazySigningEnabled());
    injectedWriteStatus = 0;
    assert(LSBImportSigningIdentity(p12, @"fixture-only", &error));
    assert(![revision isEqual:LSBSigningIdentityRevision()]);
    assert([snapshot.revision isEqual:revision]); // An active session's immutable identity does not change after reimport.
    injectedReadStatus = errSecInteractionNotAllowed;
    assert(!LSBSigningIdentitySummary() && !LSBSigningIdentityRevision());
    assert(!LSBCopySigningCredentials(&error) && error.code == errSecInteractionNotAllowed);
    injectedReadStatus = 0;
    stored[(__bridge id)kSecValueData] = [@"malformed payload" dataUsingEncoding:NSUTF8StringEncoding];
    assert(!LSBCopySigningCredentials(&error) && error.code == errSecDecode);
    assert(LSBRemoveSigningIdentity(&error) && !LSBLazySigningEnabled() && !stored);
    assert(LSBRemoveSigningIdentity(&error));
    assert([snapshot.pkcs12Data isEqual:p12] && [snapshot.password isEqual:@"fixture-only"]);
}

static void testPayloadValidation(void) {
    NSDictionary *valid = @{ @"pkcs12": [@"fixture" dataUsingEncoding:NSUTF8StringEncoding], @"password": @"", @"label": @"Fixture",
                            @"revision": NSUUID.UUID.UUIDString };
    NSData *(^serialize)(id) = ^NSData *(id value) {
        return [NSPropertyListSerialization dataWithPropertyList:value format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
    };
    assert(credentialsForPayload(serialize(valid)));
    for (NSString *key in valid) {
        NSMutableDictionary *invalid = [valid mutableCopy];
        [invalid removeObjectForKey:key];
        assert(!credentialsForPayload(serialize(invalid)));
        invalid[key] = @123;
        assert(!credentialsForPayload(serialize(invalid)));
    }
    NSMutableDictionary *invalid = [valid mutableCopy];
    invalid[@"revision"] = @"not a UUID";
    assert(!credentialsForPayload(serialize(invalid)));
    invalid = [valid mutableCopy];
    invalid[@"password"] = [@"x" stringByPaddingToLength:4097 withString:@"x" startingAtIndex:0];
    assert(!credentialsForPayload(serialize(invalid)));
    assert(!credentialsForPayload(serialize(@[@"not a dictionary"])));
    unsigned beforeImports = imports, beforeWrites = writes;
    NSError *error = nil;
    assert(!LSBImportSigningIdentity([NSData data], @"", &error));
    assert(!LSBImportSigningIdentity([NSData dataWithBytes:"x" length:1], invalid[@"password"], &error));
    assert(imports == beforeImports && writes == beforeWrites);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        assert(argc == 2);
        NSData *p12 = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
        assert(p12.length);
        testCredentialStorage(p12);
        testPayloadValidation();
        puts("SigningIdentity: memory-only P12 validation, private device-only keychain storage, atomic replacement and secret-free metadata passed");
    }
    return 0;
}
