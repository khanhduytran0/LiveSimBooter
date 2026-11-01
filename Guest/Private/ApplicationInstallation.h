#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Pinned 23A343 exports: MobileInstallation UUID 7617ADA2-8257-3C9A-8333-E9DA188A0728 and
// InstallCoordination UUID 05F74DE0-3DA5-37F1-B0B6-863219BDB84B. CoreSimulatorBridge uses this typed install path.
@interface MIInstallOptions : NSObject
@property(nonatomic) NSUInteger installTargetType;
@property(nonatomic) BOOL developerInstall;
@property(nonatomic) BOOL userInitiated;
@end

@interface IXAppInstallCoordinator : NSObject
+ (void)installApplication:(NSURL *)url consumeSource:(BOOL)consumeSource options:(MIInstallOptions *)options
                completion:(void (^)(NSString *_Nullable bundleIdentifier, NSError *_Nullable error))completion;
@end

// These are existing CoreServices classes; no subclassing or private instance layout is used here.
@interface LSApplicationRecord : NSObject
- (nullable instancetype)initWithBundleIdentifier:(NSString *)identifier allowPlaceholder:(BOOL)allowPlaceholder error:(NSError **)error;
@property(nonatomic, readonly) NSString *bundleIdentifier;
@property(nonatomic, readonly) NSURL *URL;
@property(nonatomic, readonly, nullable) NSURL *dataContainerURL;
@end

NS_ASSUME_NONNULL_END
