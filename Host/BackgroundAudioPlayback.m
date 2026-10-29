#import "BackgroundAudioInternal.h"
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <libkern/OSByteOrder.h>
#include <string.h>

@interface LSBNativeSilentPlayback : NSObject <LSBBackgroundAudioPlayback, AVAudioPlayerDelegate>
@property (nonatomic, weak) LSBBackgroundAudio *controller;
@property (nonatomic, strong) AVAudioPlayer *player;
@property (nonatomic, strong) NSArray *observers;
@property BOOL needsDeactivation;
@end

// A one-second mono PCM loop, kept in memory. No temporary audio file or recording session is needed.
static NSData *silentWave(void) {
    const uint32_t sampleRate = 48000, samples = sampleRate, dataBytes = samples * sizeof(int16_t);
    struct __attribute__((packed)) {
        char riff[4]; uint32_t riffBytes; char wave[4], format[4]; uint32_t formatBytes;
        uint16_t encoding, channels; uint32_t sampleRate, bytesPerSecond; uint16_t blockAlign, bitsPerSample;
        char data[4]; uint32_t dataBytes;
    } header = {{'R', 'I', 'F', 'F'}, OSSwapHostToLittleInt32(36 + dataBytes), {'W', 'A', 'V', 'E'}, {'f', 'm', 't', ' '},
        OSSwapHostToLittleInt32(16), OSSwapHostToLittleInt16(1), OSSwapHostToLittleInt16(1), OSSwapHostToLittleInt32(sampleRate),
        OSSwapHostToLittleInt32(sampleRate * 2), OSSwapHostToLittleInt16(2), OSSwapHostToLittleInt16(16),
        {'d', 'a', 't', 'a'}, OSSwapHostToLittleInt32(dataBytes)};
    _Static_assert(sizeof(header) == 44, "PCM WAVE header layout");
    NSMutableData *wave = [NSMutableData dataWithLength:sizeof(header) + dataBytes];
    memcpy(wave.mutableBytes, &header, sizeof(header));
    return wave;
}

@implementation LSBNativeSilentPlayback
- (instancetype)initWithController:(LSBBackgroundAudio *)controller {
    if (!(self = [super init])) return nil;
    _controller = controller;
    __weak LSBBackgroundAudio *weakController = controller;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    _observers = @[
        [center addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification *notification) {
                AVAudioSessionInterruptionType type = [notification.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
                if (type == AVAudioSessionInterruptionTypeBegan) [weakController interruptionBegan];
                else if (type == AVAudioSessionInterruptionTypeEnded) {
                    NSUInteger options = [notification.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
                    [weakController interruptionEndedShouldResume:(options & AVAudioSessionInterruptionOptionShouldResume) != 0];
                }
            }],
        [center addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification *notification) { (void)notification; [weakController mediaServicesWereReset]; }],
        [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification *notification) { (void)notification; [weakController resumeIfEnabled]; }]
    ];
    return self;
}
- (void)dealloc {
    for (id observer in _observers) [NSNotificationCenter.defaultCenter removeObserver:observer];
    [_player stop];
}
- (BOOL)startWithError:(NSError **)error {
    AVAudioSession *session = AVAudioSession.sharedInstance;
    if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault
        options:AVAudioSessionCategoryOptionMixWithOthers error:error]) return NO;
    if (!self.player) {
        self.player = [[AVAudioPlayer alloc] initWithData:silentWave() fileTypeHint:AVFileTypeWAVE error:error];
        if (!self.player) return NO;
        self.player.numberOfLoops = -1;
        self.player.delegate = self;
    }
    if (![session setActive:YES error:error]) return NO;
    self.needsDeactivation = YES;
    if ([self.player play]) return YES;
    if (error) *error = [NSError errorWithDomain:@"LSBBackgroundAudio" code:1
        userInfo:@{NSLocalizedDescriptionKey: NSLocalizedString(@"The audio player did not start.", @"Background audio error")}];
    return NO;
}
- (BOOL)stopWithError:(NSError **)error {
    [self.player stop];
    self.player = nil;
    if (!self.needsDeactivation) return YES;
    if (![AVAudioSession.sharedInstance setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:error]) return NO;
    self.needsDeactivation = NO;
    return YES;
}
- (void)suspend { [self.player pause]; }
- (void)reset {
    self.player = nil;
    self.needsDeactivation = NO; // The media server reset invalidated the old player/session activation.
}
- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer *)player error:(NSError *)error {
    __weak LSBNativeSilentPlayback *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        LSBNativeSilentPlayback *playback = weakSelf;
        if (playback.player == player) [playback.controller playbackFailed:error];
    });
}
@end

id<LSBBackgroundAudioPlayback> LSBCreateBackgroundAudioPlayback(LSBBackgroundAudio *controller) {
    return [[LSBNativeSilentPlayback alloc] initWithController:controller];
}
