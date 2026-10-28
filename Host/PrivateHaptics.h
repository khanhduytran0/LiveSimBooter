#pragma once

#import <Foundation/Foundation.h>
#import <AVFAudio/AVFAudio.h>
#import <xpc/xpc.h>

// Protocol and reply block layouts from 23A343 CoreHaptics, UUID 2AFD3C06-3A36-35CE-8940-B381C1AFFC0B.
// AVHapticClient setupConnectionWithOptions:error: also sets reply argument 0 of allocateClientResources: to XPC_TYPE_SHMEM.
@protocol LSBHapticClientProtocol
- (void)sequenceFinished:(uint64_t)sequence error:(NSError *)error;
- (void)clientCompletedWithError:(NSError *)error;
- (void)clientStoppedForReason:(uint64_t)reason error:(NSError *)error;
- (void)clientDisconnectingForReason:(uint64_t)reason error:(NSError *)error;
@end

@protocol LSBHapticServerProtocol
- (void)configureWithOptions:(NSDictionary *)options reply:(void (^)(uint64_t))reply;
- (void)queryCapabilities:(NSDictionary *)request reply:(void (^)(NSDictionary *, NSError *))reply;
- (void)allocateClientResources:(void (^)(xpc_object_t, uint32_t, NSError *))reply;
- (void)releaseClientResources;
- (void)getHapticLatency:(void (^)(double, NSError *))reply;
- (void)setPlayerBehavior:(uint64_t)behavior reply:(void (^)(NSError *))reply;
- (void)releaseChannels;
- (void)requestChannels:(uint64_t)count reply:(void (^)(NSArray *, NSError *))reply;
- (void)removeChannel:(uint64_t)channel reply:(void (^)(NSError *))reply;
- (void)setChannelEventBehavior:(uint64_t)channel behavior:(uint64_t)behavior reply:(void (^)(NSError *))reply;
- (void)loadHapticEvent:(NSDictionary *)event reply:(void (^)(uint64_t, NSError *))reply;
- (void)loadHapticSequenceFromData:(NSData *)data reply:(void (^)(uint64_t, double, uint64_t, NSError *))reply;
- (void)loadHapticSequenceFromEvents:(NSArray *)events reply:(void (^)(uint64_t, double, uint64_t, NSError *))reply;
- (void)loadVibePattern:(NSDictionary *)pattern reply:(void (^)(uint64_t, NSError *))reply;
- (void)setSequenceEventBehavior:(uint64_t)sequence behavior:(uint64_t)behavior channelIndex:(uint64_t)channel reply:(void (^)(NSError *))reply;
- (void)createCustomAudioEvent:(NSData *)data format:(AVAudioFormat *)format frames:(uint64_t)frames options:(NSDictionary *)options
                       reply:(void (^)(uint64_t, NSError *))reply;
- (void)copyCustomAudioEvent:(uint64_t)event options:(NSDictionary *)options reply:(void (^)(uint64_t, NSError *))reply;
- (void)referenceCustomAudioEvent:(uint64_t)event reply:(void (^)(NSError *))reply;
- (void)releaseCustomAudioEvent:(uint64_t)event reply:(void (^)(NSError *))reply;
- (void)prepareHapticSequence:(uint64_t)sequence reply:(void (^)(NSError *))reply;
- (void)detachSequence:(uint64_t)sequence;
- (void)prewarm:(void (^)(NSError *))reply;
- (void)stopPrewarm;
- (void)startRunning:(void (^)(NSError *))reply;
- (void)stopRunning;
- (void)stopRunning:(void (^)(NSError *))reply;
- (void)debugExpectNotifyOnFinishAfter:(double)time reply:(void (^)(void))reply;
@optional
- (void)setIntendedSpatialExperience:(id)experience reply:(void (^)(NSError *))reply;
@end

@interface AVAudioSession (LSBHapticSession)
+ (instancetype)auxiliarySession;
- (uint32_t)opaqueSessionID;
@end

@interface AVHapticProcessTaskToken : NSObject <NSSecureCoding>
- (instancetype)initWithXPCDictionary:(xpc_object_t)dictionary;
@end

@interface NSXPCConnection (LSBHapticPeer)
- (audit_token_t)auditToken;
- (xpc_connection_t)_xpcConnection;
@end

@interface NSXPCListener (LSBHapticListener)
- (xpc_connection_t)_xpcConnection;
- (void)_setQueue:(dispatch_queue_t)queue;
@end

// The SDK hides this initializer on iOS although stock CoreHaptics calls it there.
@protocol LSBHapticNativeConnectionInitializer
- (NSXPCConnection *)initWithMachServiceName:(NSString *)name options:(NSUInteger)options;
@end

@interface NSXPCInterface (LSBHapticSharedMemory)
- (void)setXPCType:(xpc_type_t)type forSelector:(SEL)selector argumentIndex:(NSUInteger)index ofReply:(BOOL)reply;
@end

// Default Foundation container classes do not include AudioToolboxCore's custom token wrapper. Permit that exact class
// only inside configureWithOptions:, then replace the decoded guest token with the host's before forwarding.
static inline NSXPCInterface *LSBHapticServerInterface(Class taskTokenClass) {
    NSXPCInterface *interface = [NSXPCInterface interfaceWithProtocol:@protocol(LSBHapticServerProtocol)];
    [interface setXPCType:XPC_TYPE_SHMEM forSelector:@selector(allocateClientResources:) argumentIndex:0 ofReply:YES];
    if (taskTokenClass) {
        NSMutableSet *classes = [[interface classesForSelector:@selector(configureWithOptions:reply:) argumentIndex:0 ofReply:NO] mutableCopy];
        if (!classes) classes = [NSMutableSet set];
        [classes addObject:taskTokenClass];
        [interface setClasses:classes forSelector:@selector(configureWithOptions:reply:) argumentIndex:0 ofReply:NO];
    }
    // This optional SPI class is SDK-unavailable on iOS. Preserve its exact secure-decoding class without naming an unavailable type.
    Class spatialExperience = NSClassFromString(@"CASpatialAudioExperience");
    if (spatialExperience) [interface setClasses:[NSSet setWithObject:spatialExperience]
        forSelector:@selector(setIntendedSpatialExperience:reply:) argumentIndex:0 ofReply:NO];
    return interface;
}
