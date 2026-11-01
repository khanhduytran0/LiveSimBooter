#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSNotificationName const LSBGuestApplicationInstallerDidChangeNotification;

// Native UI/control-queue client only. Installation runs in a fixed guest helper through simulator launchd.
@interface LSBGuestApplicationInstaller : NSObject
@property (class, nonatomic, readonly) LSBGuestApplicationInstaller *sharedController;
@property (nonatomic, readonly) BOOL pending, outcomeUnknown;
@property (nonatomic, readonly, copy) NSString *statusText;
- (void)installIPAFromURL:(NSURL *)url;
// Read back the existing request only; never republishes an operation whose outcome is unknown.
- (void)refresh;
- (instancetype)initWithDataRoot:(nullable NSURL *)dataRoot defaults:(NSUserDefaults *)defaults timeout:(NSTimeInterval)timeout;
@end

NS_ASSUME_NONNULL_END
