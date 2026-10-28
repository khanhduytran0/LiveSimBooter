#import <Foundation/Foundation.h>

// Main-thread control, AudioQueue-owned render callback. One ring per authenticated job; no microphone/input API.
@interface LSBAudioOutput : NSObject
- (instancetype)initWithError:(int *)error;
- (void *)sharedMemory; // Borrowed XPC object until invalidate; transport copies the underlying memory capability.
- (void)invalidate;
@end
