#pragma once

#import <Foundation/Foundation.h>

FOUNDATION_EXPORT NSNotificationName const LSBBackgroundAudioDidChangeNotification;

// Main-thread state for the native host only. A missing preference means off.
@interface LSBBackgroundAudio : NSObject
@property (class, nonatomic, readonly) LSBBackgroundAudio *sharedController;
@property (nonatomic, readonly, getter=isEnabled) BOOL enabled;
@property (nonatomic, readonly, getter=isPlaying) BOOL playing;
@property (nonatomic, readonly, copy) NSString *statusText;
// An interruption or error to append to the Settings explanation; nil for ordinary Off/Playing states.
@property (nonatomic, readonly, copy) NSString *statusDetail;
- (void)setEnabled:(BOOL)enabled;
@end
