#import <Foundation/Foundation.h>

// The 23A343 _LSDReadProtocol method implemented by _LSDReadClient at CoreServices+0xc6cb4.
// Its reply block contains one unsigned 32-bit session-status value; zero is a legitimate reply.
@protocol LSBLaunchServicesReadProtocol
- (void)getServerStatusWithCompletionHandler:(void (^)(uint32_t status))completion;
@end

// This normal Objective-C initializer exists in simulator Foundation although the public SDK marks it macOS-only.
@protocol LSBMachServiceConnectionInitializer
- (instancetype)initWithMachServiceName:(NSString *)name options:(NSXPCConnectionOptions)options;
@end
