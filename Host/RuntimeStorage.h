#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NSURL * _Nullable (^LSBAppGroupContainerProvider)(NSString *identifier);

// Resolves once without creating or moving files. All native consumers use the same process-lifetime selection.
// storageURL is <selected group>/LSB/iOS/23A343; runtimeRootURL and dataURL are its RuntimeRoot and data children.
@interface LSBRuntimeStorage : NSObject
@property (class, nonatomic, readonly) LSBRuntimeStorage *sharedStorage;
@property (nonatomic, readonly, copy, nullable) NSString *appGroupIdentifier;
@property (nonatomic, readonly, copy, nullable) NSURL *containerURL, *storageURL, *runtimeRootURL, *dataURL;
@property (nonatomic, readonly, nullable) NSError *error;
// Production candidates come from signed entitlements, including identifiers rewritten by a sideloading store.
- (instancetype)initWithAppGroupIdentifiers:(NSArray<NSString *> *)identifiers containerProvider:(LSBAppGroupContainerProvider)provider;
- (instancetype)init NS_UNAVAILABLE;
@end

NS_ASSUME_NONNULL_END
