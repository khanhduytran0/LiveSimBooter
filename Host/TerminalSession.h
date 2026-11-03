#pragma once

#import <Foundation/Foundation.h>
#include "SpawnTransport.h"

extern NSString *const LSBTerminalSessionDidCloseNotification;

// Native host only. The guest endpoint uses the pure-C FakePTY shim and ordinary inherited file descriptors.
@interface LSBTerminalSession : NSObject
@property (nonatomic, copy) void (^outputHandler)(NSData *data);
@property (nonatomic, copy) int (^signalHandler)(pid_t foreground, int signalNumber);
@property (nonatomic, readonly) NSString *statePath;
@property (nonatomic, readonly, getter=isClosed) BOOL closed;
- (instancetype)initWithDirectory:(NSString *)directory error:(int *)error;
- (int)attachToMessage:(LSBReceivedMessage *)message;
// Attach only to the broker-authorized launchd terminal helper. Preserve its actions, then override stdio with this terminal.
// Adds one inherited staging FD and four final actions; consumes the session's slave only after complete validation succeeds.
// On failure the caller's message and all existing FD ownership remain unchanged.
- (int)attachToMessageReplacingStandardIO:(LSBReceivedMessage *)message;
- (void)start;
// Consume all output already enqueued on the master under the current output flags before acknowledging TCSADRAIN.
- (int)drainOutput;
- (int)checkAccess:(LSBTerminalAccess)access forGroup:(pid_t)group background:(BOOL *)background;
- (int)setForegroundGroup:(pid_t)group;
- (int)sendInput:(NSData *)data;
- (void)resizeRows:(unsigned short)rows columns:(unsigned short)columns;
- (void)close;
@end

// Main-thread UI access; nil until the configured terminal job is attached.
LSBTerminalSession *LSBHostTerminal(void);
