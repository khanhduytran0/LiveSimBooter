#pragma once
#import <Foundation/Foundation.h>
#include <mach/mach.h>
@class LSBSigningCredentials;

// One session owns the credential snapshot, private receive right and isolated native signer connection.
@interface LSBLazySigning : NSObject
@property (nonatomic, readonly) mach_port_t endpoint;
@property (nonatomic, readonly, copy) NSString *identityRevision;
- (instancetype)initWithRuntimeRoot:(NSString *)root credentials:(LSBSigningCredentials *)credentials identifier:(NSString *)identifier
                         authorize:(BOOL (^)(const audit_token_t *audit))authorize error:(int *)error;
// Main-thread only. Copies the service send right; the broker retains ownership of the supplied reference.
- (int)connectService:(mach_port_t)port audit:(const audit_token_t *)audit;
- (void)invalidate;
@end
