#import "../Host/HapticOptions.h"
#import "../Host/PrivateHaptics.h"
#import "../Host/PrivateIOSurface.h"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

@interface HapticTestToken : NSObject <NSSecureCoding>
@property(nonatomic) uint64_t value;
@end
@implementation HapticTestToken
+ (BOOL)supportsSecureCoding { return YES; }
- (void)encodeWithCoder:(NSCoder *)coder { [coder encodeInt64:(int64_t)self.value forKey:@"value"]; }
- (instancetype)initWithCoder:(NSCoder *)coder {
    if ((self = [super init])) self.value = (uint64_t)[coder decodeInt64ForKey:@"value"];
    return self;
}
@end

static NSXPCInterface *relayInterface(void) { return LSBHapticServerInterface(HapticTestToken.class); }


@interface HapticTestCallbacks : NSObject
@property(nonatomic) dispatch_semaphore_t done;
@end
@implementation HapticTestCallbacks
- (void)sequenceFinished:(uint64_t)sequence error:(NSError *)error {
    assert(sequence == 0x123456789abcdef0 && error.code == 11);
    dispatch_semaphore_signal(self.done);
}
@end

@interface HapticTestService : NSObject <NSXPCListenerDelegate>
@property(nonatomic) NSXPCConnection *connection;
@end
@implementation HapticTestService
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection {
    (void)listener;
    assert(!NSThread.isMainThread);
    audit_token_t own = {{0}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&own, &count));
    audit_token_t peer = connection.auditToken;
    assert(count == TASK_AUDIT_TOKEN_COUNT && !memcmp(&peer, &own, sizeof(peer)));
    self.connection = connection;
    connection.exportedInterface = relayInterface();
    connection.exportedObject = self;
    connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(LSBHapticClientProtocol)];
    [connection resume];
    return YES;
}
- (void)configureWithOptions:(NSDictionary *)options reply:(void (^)(uint64_t))reply {
    assert([options[@"AudioSessionID"] unsignedIntValue] == 123);
    assert(options[@"AudioSessionIsShared"] == (__bridge id)kCFBooleanTrue);
    assert(options[@"BypassAudioSession"] == (__bridge id)kCFBooleanFalse);
    HapticTestToken *token = options[@"ClientProcessTaskToken"];
    assert([token isKindOfClass:HapticTestToken.class] && token.value == 0xabcdef0123456789);
    reply(0x123456789abcdef0);
    id<LSBHapticClientProtocol> callbacks = [self.connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
        fprintf(stderr, "relay callback transport: %s\n", error.description.UTF8String); abort();
    }];
    [callbacks sequenceFinished:0x123456789abcdef0 error:[NSError errorWithDomain:@"LSBCallbackTest" code:11 userInfo:nil]];
}
- (void)allocateClientResources:(void (^)(xpc_object_t, uint32_t, NSError *))reply {
    size_t length = (size_t)getpagesize();
    void *bytes = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    assert(bytes != MAP_FAILED);
    memcpy(bytes, "haptic-ring", 12);
    xpc_object_t memory = xpc_shmem_create(bytes, length);
    assert(memory);
    reply(memory, (uint32_t)length, nil);
    munmap(bytes, length);
}
- (void)loadHapticSequenceFromData:(NSData *)data reply:(void (^)(uint64_t, double, uint64_t, NSError *))reply {
    assert(data.length == 3);
    reply(0xfedcba9876543210, 1.25, 0x123456789, [NSError errorWithDomain:@"LSBTest" code:7 userInfo:nil]);
}
@end

int main(void) {
    @autoreleasepool {
        NSObject *hostToken = [NSObject new];
        NSDictionary *guest = @{ @"AudioSessionID": @0, @"AudioSessionIsShared": @NO, @"BypassAudioSession": @YES,
            @"ClientProcessTaskToken": @"untrusted", @"MachServiceName": @"another-service", @"Locality": @"Default" };
        NSDictionary *native = LSBHapticHostOptions(guest, 123, hostToken);
        assert(native[@"ClientProcessTaskToken"] == hostToken && [native[@"AudioSessionID"] unsignedIntValue] == 123);
        assert(!native[@"MachServiceName"] && [native[@"Locality"] isEqual:@"Default"]);
        assert(native[@"AudioSessionIsShared"] == (__bridge id)kCFBooleanTrue && native[@"BypassAudioSession"] == (__bridge id)kCFBooleanFalse);
        assert([guest[@"ClientProcessTaskToken"] isEqual:@"untrusted"] && guest[@"MachServiceName"] && [guest[@"BypassAudioSession"] boolValue]);
        assert(!LSBHapticHostOptions(guest, 0, hostToken) && !LSBHapticHostOptions(guest, 123, nil));
        assert(!LSBHapticHostOptions((id)@"invalid", 123, hostToken));

        HapticTestService *service = [HapticTestService new];
        NSXPCListener *listener = [NSXPCListener anonymousListener];
        listener.delegate = service;
        [listener _setQueue:dispatch_queue_create("haptic-relay-test", DISPATCH_QUEUE_SERIAL)];
        xpc_connection_enable_sim2host_4sim([listener _xpcConnection]);
        [listener resume];
        NSXPCConnection *connection = [[NSXPCConnection alloc] initWithListenerEndpoint:listener.endpoint];
        HapticTestCallbacks *callbacks = [HapticTestCallbacks new];
        callbacks.done = dispatch_semaphore_create(0);
        connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(LSBHapticClientProtocol)];
        connection.exportedObject = callbacks;
        connection.remoteObjectInterface = relayInterface();
        xpc_connection_enable_sim2host_4sim([connection _xpcConnection]);
        [connection resume];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        id<LSBHapticServerProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
            fprintf(stderr, "relay test transport: %s\n", error.description.UTF8String); abort();
        }];
        // Exercise the actual reply block ABI, including 64-bit IDs and the native shared-memory object (not a copied byte buffer).
        HapticTestToken *wireToken = [HapticTestToken new];
        wireToken.value = 0xabcdef0123456789;
        [proxy configureWithOptions:LSBHapticHostOptions(guest, 123, wireToken) reply:^(uint64_t clientID) {
            assert(clientID == 0x123456789abcdef0);
            dispatch_semaphore_signal(done);
        }];
        assert(!dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)));
        assert(!dispatch_semaphore_wait(callbacks.done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)));
        [proxy allocateClientResources:^(xpc_object_t memory, uint32_t size, NSError *error) {
            void *mapping = NULL;
            assert(!error && xpc_get_type(memory) == XPC_TYPE_SHMEM);
            size_t mapped = xpc_shmem_map(memory, &mapping);
            assert(mapped == size && mapping && !memcmp(mapping, "haptic-ring", 12));
            munmap(mapping, mapped);
            dispatch_semaphore_signal(done);
        }];
        assert(!dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)));
        [proxy loadHapticSequenceFromData:[NSData dataWithBytes:"abc" length:3] reply:^(uint64_t sequence, double duration, uint64_t value, NSError *error) {
            assert(sequence == 0xfedcba9876543210 && duration == 1.25 && value == 0x123456789 && error.code == 7);
            dispatch_semaphore_signal(done);
        }];
        assert(!dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)));
        [connection invalidate];
        [service.connection invalidate];
        service.connection.exportedObject = nil;
        [listener invalidate];
        puts("Haptic relay options and NSXPC reply/shared-memory ABI tests passed");
    }
    return 0;
}
