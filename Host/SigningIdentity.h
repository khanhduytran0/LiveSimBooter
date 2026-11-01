#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSNotificationName const LSBSigningIdentityDidChangeNotification;
FOUNDATION_EXPORT const NSUInteger LSBSigningPKCS12MaximumSize;

// Host-only immutable snapshot. Pass secrets only to the authenticated native signer, never to simulator guests.
@interface LSBSigningCredentials : NSObject
@property (nonatomic, readonly, copy) NSData *pkcs12Data;
@property (nonatomic, readonly, copy) NSString *password, *label, *revision;
@end

BOOL LSBLazySigningEnabled(void);
void LSBSaveConfiguredLazySigningEnabled(BOOL enabled);
// Metadata queries do not retrieve the private-key payload. Missing/inaccessible credentials return nil.
NSString *_Nullable LSBSigningIdentitySummary(void);
NSString *_Nullable LSBSigningIdentityRevision(void);
LSBSigningCredentials *_Nullable LSBCopySigningCredentials(NSError *_Nullable *_Nullable error);
// Import validates one identity in memory, then atomically replaces one device-only, app-private keychain item.
BOOL LSBImportSigningIdentity(NSData *data, NSString *password, NSError *_Nullable *_Nullable error);
BOOL LSBRemoveSigningIdentity(NSError *_Nullable *_Nullable error);

NS_ASSUME_NONNULL_END
