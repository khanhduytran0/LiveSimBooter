#import "BackgroundAudioInternal.h"

NSNotificationName const LSBBackgroundAudioDidChangeNotification = @"LSBBackgroundAudioDidChangeNotification";
static NSString *const EnabledPreference = @"LSBSilentBackgroundAudioEnabled";

@implementation LSBBackgroundAudio {
    NSUserDefaults *_defaults;
    id<LSBBackgroundAudioPlayback> _playback;
    BOOL _enabled;
    BOOL _playing;
    BOOL _interrupted;
    NSString *_failure;
}

+ (instancetype)sharedController {
    static LSBBackgroundAudio *controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        controller = [[self alloc] initWithDefaults:NSUserDefaults.standardUserDefaults playback:nil];
        [controller resumeIfEnabled];
    });
    return controller;
}
- (instancetype)initWithDefaults:(NSUserDefaults *)defaults playback:(id<LSBBackgroundAudioPlayback>)playback {
    if (!(self = [super init])) return nil;
    _defaults = defaults;
    _playback = playback;
    _enabled = [defaults boolForKey:EnabledPreference];
    return self;
}
- (BOOL)isEnabled { return _enabled; }
- (BOOL)isPlaying { return _playing; }
- (NSString *)statusText {
    if (_failure) return _failure;
    if (!_enabled) return NSLocalizedString(@"Off", @"Background audio is disabled");
    if (_interrupted) return NSLocalizedString(@"Paused by an audio interruption", @"Background audio status");
    return _playing ? NSLocalizedString(@"Playing silence", @"Background audio status") :
        NSLocalizedString(@"Waiting to resume", @"Background audio status");
}
- (NSString *)statusDetail { return _failure ?: (_enabled && (_interrupted || !_playing) ? self.statusText : nil); }
- (void)publish {
    [NSNotificationCenter.defaultCenter postNotificationName:LSBBackgroundAudioDidChangeNotification object:self];
}
- (void)setEnabled:(BOOL)enabled {
    NSAssert(NSThread.isMainThread, @"Background audio state belongs to the main thread");
    _enabled = enabled;
    [_defaults setBool:enabled forKey:EnabledPreference];
    _failure = nil;
    if (enabled) { [self resumeIfEnabled]; return; }
    _interrupted = NO;
    _playing = NO;
    NSError *error = nil;
    // Do not even construct the native backend when this preference has never been enabled.
    if (_playback && ![_playback stopWithError:&error])
        _failure = [NSString stringWithFormat:NSLocalizedString(@"Playback stopped; audio session could not deactivate: %@", @"Background audio error"),
            error.localizedDescription ?: NSLocalizedString(@"Unknown error", @"Fallback when the system provides no error description")];
    [self publish];
}
- (void)resumeIfEnabled {
    NSAssert(NSThread.isMainThread, @"Background audio state belongs to the main thread");
    if (!_enabled || _interrupted || _playing) return;
    if (!_playback) _playback = LSBCreateBackgroundAudioPlayback(self);
    NSError *error = nil;
    _playing = [_playback startWithError:&error];
    _failure = nil;
    if (!_playing) {
        NSError *stopError = nil;
        [_playback stopWithError:&stopError];
        NSString *reason = error.localizedDescription ?: NSLocalizedString(@"Audio unavailable", @"Fallback background audio failure reason");
        _failure = [NSString stringWithFormat:NSLocalizedString(@"Unable to play silence: %@", @"Background audio error"), reason];
        if (stopError) _failure = [NSString stringWithFormat:
            NSLocalizedString(@"Unable to play silence: %1$@. Deactivation failed: %2$@", @"Background audio playback and cleanup failure reasons"),
            reason, stopError.localizedDescription];
    }
    [self publish];
}
- (void)interruptionBegan {
    if (!_enabled) return;
    _interrupted = YES;
    _playing = NO;
    [_playback suspend];
    [self publish];
}
- (void)interruptionEndedShouldResume:(BOOL)shouldResume {
    if (!_enabled) return;
    _interrupted = NO;
    if (shouldResume) [self resumeIfEnabled];
    else [self publish];
}
- (void)mediaServicesWereReset {
    [_playback reset];
    _playing = NO;
    _interrupted = NO;
    if (_enabled) [self resumeIfEnabled];
}
- (void)playbackFailed:(NSError *)error {
    if (!_enabled) return;
    _playing = NO;
    [_playback stopWithError:NULL];
    _failure = [NSString stringWithFormat:NSLocalizedString(@"Audio playback stopped: %@", @"Background audio error"),
        error.localizedDescription ?: NSLocalizedString(@"Unknown error", @"Fallback when the system provides no error description")];
    [self publish];
}
@end
