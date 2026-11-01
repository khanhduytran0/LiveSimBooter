#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// These classes belong to the pinned 23A343 MobileInstallationHelperService executable, not a linked public framework.
@interface MIStagingRecord : NSObject
- (instancetype)initWithStagingBase:(NSURL *)base relativePath:(NSString *)relativePath;
@property(nonatomic, readonly) NSURL *stagingBase;
@property(nonatomic, readonly) NSString *relativePath;
@end

@interface MIStagingManager : NSObject
- (nullable MIStagingRecord *)stagingRecordForSystemContentWithinSubsystem:(NSUInteger)subsystem error:(NSError **)error;
@end

@interface MIStagingManager (LSBInstallationStaging)
// Installed only after exact-image/method validation; exchanged with the original selector for normal Objective-C forwarding.
- (nullable MIStagingRecord *)lsb_stagingRecordForSystemContentWithinSubsystem:(NSUInteger)subsystem error:(NSError **)error;
@end

NS_ASSUME_NONNULL_END
