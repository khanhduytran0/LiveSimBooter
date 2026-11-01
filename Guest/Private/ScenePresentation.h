#pragma once
#import <Foundation/Foundation.h>
#ifndef LSB_SCENE_PRESENTATION_TESTING
#import <UIKit/UIKit.h>
#endif

NS_ASSUME_NONNULL_BEGIN

// Private declarations verified against the original arm64 23A343 UIKitCore/FrontBoard Objective-C metadata.
@class _UIScenePresentationView;
@interface FBScene : NSObject
@property(nonatomic, readonly) NSString *identifier;
@end

@interface UIScenePresentationContext : NSObject
@property(nonatomic, readonly, nullable) UIColor *backgroundColorWhileNotHosting;
@end

@interface _UIScenePresenter : NSObject
@property(nonatomic, readonly) BOOL isHosting;
@property(nonatomic, readonly) NSString *identifier;
@property(nonatomic, readonly) FBScene *scene;
- (void)updateHostingStateIfNecessary;
@end

@interface _UIScenePresentationView : UIView
@property(nonatomic, readonly) _UIScenePresenter *presenter;
@property(nonatomic, readonly, nullable) UIView *hostContainerView;
@property(nonatomic, readonly) UIScenePresentationContext *currentPresentationContext;
- (void)_updateBackgroundColor;
@end

@interface _UIScenePresenter (LSBScenePresentation)
// Installed after pinned UIKit validation; these declarations supply the method signatures.
// Exchanges implementations with updateHostingStateIfNecessary.
- (void)lsb_updateHostingStateIfNecessary;
@end

NS_ASSUME_NONNULL_END
