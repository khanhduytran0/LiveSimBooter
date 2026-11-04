#pragma once

#import <Foundation/Foundation.h>
#include "ExtensionLauncher.h"
#include "../ProcessSupport/SpawnTransport.h"

typedef int (*LSBPoolPeerValidator)(const LSBPeerProcessIdentity *, const audit_token_t *);

// Main queue only. Each slot is a fresh native extension blocked in its synchronous CHECKIN. Consuming a slot is one-shot.
@interface LSBLiveProcessPool : NSObject
- (instancetype)initWithCapacity:(NSUInteger)capacity validatePeer:(LSBPoolPeerValidator)validatePeer;
- (void)start;
// YES consumes the message, either holding CHECKIN or replying to it. Called only for native CHECKIN without an existing guest job.
- (BOOL)handleCheckIn:(LSBReceivedMessage *)message coldPID:(pid_t)coldPID;
// Zero bounds select any parked worker. A fork hint prefers a worker with a disjoint stack. The observer still checks the complete offer.
// On success the caller owns the held message and the rekeyed launcher session. On a miss it can launch the job normally.
- (LSBReceivedMessage *)takeForRequest:(uint64_t)jobID donorStackBottom:(uint64_t)bottom top:(uint64_t)top event:(LSBExtensionEvent)event context:(void *)context;
// Prevents further refills. A successful completion means all outstanding warm sessions have exited.
- (void)drain:(void (^)(int error))completion;
@end
