#pragma once

#import <Foundation/Foundation.h>
#include <mach/mach.h>

// Experimental, session-owned relay. Authorization runs off main; it must validate the complete native audit token.
@interface LSBHapticServer : NSObject
- (instancetype)initWithAuthorization:(BOOL (^)(const audit_token_t *))authorization error:(int *)error;
@property (nonatomic, readonly) mach_port_t registrationPort;
- (void)close;
@end
