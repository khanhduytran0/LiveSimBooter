#import "HapticServer.h"
#import "PrivateHaptics.h"
#import "HapticOptions.h"
#import "PrivateIOSurface.h"
#import "../ProcessSupport/PrivateXPC.h"
#import <CoreHaptics/CoreHaptics.h>
#import <objc/runtime.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { LSBMaximumHapticPeers = 64 };
static _Atomic(unsigned) diagnosticCount;
static atomic_bool traceEnabled;
static void hapticDiagnostic(const char *stage, uint64_t first, uint64_t second) {
    if (atomic_fetch_add(&diagnosticCount, 1) < 32)
        fprintf(stderr, "[haptics] %s first=%llu second=%llu\n", stage, (unsigned long long)first, (unsigned long long)second);
}
static void hapticTrace(const char *stage, uint64_t first, uint64_t second) {
    if (atomic_load_explicit(&traceEnabled, memory_order_relaxed)) hapticDiagnostic(stage, first, second);
}


@interface LSBHapticPeer : NSObject <LSBHapticServerProtocol, LSBHapticClientProtocol> {
    NSXPCConnection *_guest;
    NSXPCConnection *_native;
    AVAudioSession *_session;
    AVHapticProcessTaskToken *_taskToken;
    atomic_bool _closed;
    atomic_bool _configured;
    dispatch_queue_t _ownerQueue;
    void (^_finished)(LSBHapticPeer *);
}
- (instancetype)initWithConnection:(NSXPCConnection *)connection queue:(dispatch_queue_t)queue finished:(void (^)(LSBHapticPeer *))finished;
- (void)resume;
- (void)close;
@end

@implementation LSBHapticPeer
- (instancetype)initWithConnection:(NSXPCConnection *)connection queue:(dispatch_queue_t)queue finished:(void (^)(LSBHapticPeer *))finished {
    if (!(self = [super init])) return nil;
    _ownerQueue = queue;
    _finished = [finished copy];
    _session = [AVAudioSession auxiliarySession];
    uint32_t sessionID = _session.opaqueSessionID;
    if (!sessionID) { hapticDiagnostic("peer-session-failed", _session != nil, 0); return nil; }
    mach_port_t identity = MACH_PORT_NULL;
    kern_return_t identityResult = task_create_identity_token(mach_task_self(), &identity);
    if (identityResult != KERN_SUCCESS || !MACH_PORT_VALID(identity)) {
        hapticDiagnostic("peer-identity-failed", identityResult, sessionID);
        return nil;
    }
    xpc_object_t dictionary = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_mach_send(dictionary, "XPC_ProcessTaskToken", identity);
    mach_port_deallocate(mach_task_self(), identity);
    // Stock CHHapticEngine createProcessTaskToken/createOptionsFromAudioSessionID: use this exact secure-coding wrapper.
    Class tokenClass = NSClassFromString(@"AVHapticProcessTaskToken");
    _taskToken = [(AVHapticProcessTaskToken *)[tokenClass alloc] initWithXPCDictionary:dictionary];
    if (!_taskToken) { hapticDiagnostic("peer-token-failed", tokenClass != Nil, sessionID); return nil; }
    _guest = connection;
    _native = [(id<LSBHapticNativeConnectionInitializer>)[NSXPCConnection alloc]
        initWithMachServiceName:@"com.apple.audio.hapticd" options:0x1000];
    if (!_guest || !_native) { hapticDiagnostic("peer-connection-failed", _guest != nil, _native != nil); return nil; }
    _guest.exportedInterface = LSBHapticServerInterface(NSClassFromString(@"AVHapticProcessTaskToken"));
    _guest.exportedObject = self;
    _guest.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(LSBHapticClientProtocol)];
    _native.remoteObjectInterface = LSBHapticServerInterface(NSClassFromString(@"AVHapticProcessTaskToken"));
    _native.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(LSBHapticClientProtocol)];
    _native.exportedObject = self;
    __weak LSBHapticPeer *weakSelf = self;
    _guest.interruptionHandler = ^{ hapticTrace("guest-interrupted", 0, 0); [weakSelf close]; };
    _guest.invalidationHandler = ^{ hapticTrace("guest-invalidated", 0, 0); [weakSelf close]; };
    _native.interruptionHandler = ^{ hapticTrace("native-interrupted", 0, 0); [weakSelf close]; };
    _native.invalidationHandler = ^{ hapticTrace("native-invalidated", 0, 0); [weakSelf close]; };
    return self;
}

- (void)resume { [_native resume]; [_guest resume]; }

- (void)close {
    if (atomic_exchange(&_closed, true)) return;
    // No synchronous dispatch: the broker may call close while an accept authorization is waiting for its main queue.
    dispatch_async(_ownerQueue, ^{
        [self->_guest invalidate];
        [self->_native invalidate];
        self->_guest.exportedObject = nil;
        self->_native.exportedObject = nil;
        if (self->_finished) self->_finished(self);
        self->_finished = nil;
        // Session/token remain alive until pending native replies release this peer. Destruction then uses native AVAudioSession cleanup.
    });
}

- (id<LSBHapticServerProtocol>)nativeProxy:(void (^)(NSError *))failure {
    if (atomic_load(&_closed)) {
        failure([NSError errorWithDomain:NSCocoaErrorDomain code:NSXPCConnectionInvalid userInfo:nil]);
        return nil;
    }
    return [_native remoteObjectProxyWithErrorHandler:^(NSError *error) {
        hapticDiagnostic("native-transport-error", error.code, 0); failure(error); [self close];
    }];
}

- (id<LSBHapticClientProtocol>)guestProxy {
    if (atomic_load(&_closed)) return nil;
    return [_guest remoteObjectProxyWithErrorHandler:^(NSError *error) { (void)error; [self close]; }];
}

- (void)configureWithOptions:(NSDictionary *)options reply:(void (^)(uint64_t))reply {
    NSDictionary *nativeOptions = LSBHapticHostOptions(options, _session.opaqueSessionID, _taskToken);
    if (!nativeOptions || atomic_exchange(&_configured, true)) { reply(0); [self close]; return; }
    [[self nativeProxy:^(NSError *error) { (void)error; reply(0); }] configureWithOptions:nativeOptions reply:reply];
}

- (void)queryCapabilities:(NSDictionary *)request reply:(void (^)(NSDictionary *, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(nil, error); }] queryCapabilities:request reply:reply];
}
- (void)allocateClientResources:(void (^)(xpc_object_t, uint32_t, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(nil, 0, error); }] allocateClientResources:reply];
}
- (void)releaseClientResources { [[self nativeProxy:^(NSError *error) { (void)error; }] releaseClientResources]; }
- (void)getHapticLatency:(void (^)(double, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(0, error); }] getHapticLatency:reply];
}
- (void)setPlayerBehavior:(uint64_t)behavior reply:(void (^)(NSError *))reply {
    [[self nativeProxy:reply] setPlayerBehavior:behavior reply:reply];
}
- (void)releaseChannels { [[self nativeProxy:^(NSError *error) { (void)error; }] releaseChannels]; }
- (void)requestChannels:(uint64_t)count reply:(void (^)(NSArray *, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(nil, error); }] requestChannels:count reply:reply];
}
- (void)removeChannel:(uint64_t)channel reply:(void (^)(NSError *))reply {
    [[self nativeProxy:reply] removeChannel:channel reply:reply];
}
- (void)setChannelEventBehavior:(uint64_t)channel behavior:(uint64_t)behavior reply:(void (^)(NSError *))reply {
    [[self nativeProxy:reply] setChannelEventBehavior:channel behavior:behavior reply:reply];
}
- (void)loadHapticEvent:(NSDictionary *)event reply:(void (^)(uint64_t, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(0, error); }] loadHapticEvent:event reply:reply];
}
- (void)loadHapticSequenceFromData:(NSData *)data reply:(void (^)(uint64_t, double, uint64_t, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(0, 0, 0, error); }] loadHapticSequenceFromData:data reply:reply];
}
- (void)loadHapticSequenceFromEvents:(NSArray *)events reply:(void (^)(uint64_t, double, uint64_t, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(0, 0, 0, error); }] loadHapticSequenceFromEvents:events reply:reply];
}
- (void)loadVibePattern:(NSDictionary *)pattern reply:(void (^)(uint64_t, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(0, error); }] loadVibePattern:pattern reply:reply];
}
- (void)setSequenceEventBehavior:(uint64_t)sequence behavior:(uint64_t)behavior channelIndex:(uint64_t)channel reply:(void (^)(NSError *))reply {
    [[self nativeProxy:reply] setSequenceEventBehavior:sequence behavior:behavior channelIndex:channel reply:reply];
}
- (void)createCustomAudioEvent:(NSData *)data format:(AVAudioFormat *)format frames:(uint64_t)frames options:(NSDictionary *)options
                       reply:(void (^)(uint64_t, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(0, error); }] createCustomAudioEvent:data format:format frames:frames options:options reply:reply];
}
- (void)copyCustomAudioEvent:(uint64_t)event options:(NSDictionary *)options reply:(void (^)(uint64_t, NSError *))reply {
    [[self nativeProxy:^(NSError *error) { reply(0, error); }] copyCustomAudioEvent:event options:options reply:reply];
}
- (void)referenceCustomAudioEvent:(uint64_t)event reply:(void (^)(NSError *))reply {
    [[self nativeProxy:reply] referenceCustomAudioEvent:event reply:reply];
}
- (void)releaseCustomAudioEvent:(uint64_t)event reply:(void (^)(NSError *))reply {
    [[self nativeProxy:reply] releaseCustomAudioEvent:event reply:reply];
}
- (void)prepareHapticSequence:(uint64_t)sequence reply:(void (^)(NSError *))reply {
    [[self nativeProxy:reply] prepareHapticSequence:sequence reply:reply];
}
- (void)detachSequence:(uint64_t)sequence { [[self nativeProxy:^(NSError *error) { (void)error; }] detachSequence:sequence]; }
- (void)prewarm:(void (^)(NSError *))reply { [[self nativeProxy:reply] prewarm:reply]; }
- (void)stopPrewarm { [[self nativeProxy:^(NSError *error) { (void)error; }] stopPrewarm]; }
- (void)startRunning:(void (^)(NSError *))reply { [[self nativeProxy:reply] startRunning:reply]; }
- (void)stopRunning { [[self nativeProxy:^(NSError *error) { (void)error; }] stopRunning]; }
- (void)stopRunning:(void (^)(NSError *))reply { [[self nativeProxy:reply] stopRunning:reply]; }
- (void)debugExpectNotifyOnFinishAfter:(double)time reply:(void (^)(void))reply {
    [[self nativeProxy:^(NSError *error) { (void)error; }] debugExpectNotifyOnFinishAfter:time reply:reply];
}
- (void)setIntendedSpatialExperience:(id)experience reply:(void (^)(NSError *))reply {
    [[self nativeProxy:reply] setIntendedSpatialExperience:experience reply:reply];
}

- (void)sequenceFinished:(uint64_t)sequence error:(NSError *)error { [[self guestProxy] sequenceFinished:sequence error:error]; }
- (void)clientCompletedWithError:(NSError *)error { [[self guestProxy] clientCompletedWithError:error]; }
- (void)clientStoppedForReason:(uint64_t)reason error:(NSError *)error { [[self guestProxy] clientStoppedForReason:reason error:error]; }
- (void)clientDisconnectingForReason:(uint64_t)reason error:(NSError *)error {
    [[self guestProxy] clientDisconnectingForReason:reason error:error];
}
@end

@interface LSBHapticServer () <NSXPCListenerDelegate> {
    NSXPCListener *_listener;
    dispatch_queue_t _queue;
    NSMutableSet<LSBHapticPeer *> *_peers;
    BOOL (^_authorization)(const audit_token_t *);
    atomic_bool _closed;
    _Atomic(mach_port_t) _registration;
}
@end

@implementation LSBHapticServer
- (instancetype)initWithAuthorization:(BOOL (^)(const audit_token_t *))authorization error:(int *)error {
    if (error) *error = 0;
    if (!(self = [super init])) { if (error) *error = ENOMEM; return nil; }
    if (!authorization) { if (error) *error = EINVAL; return nil; }
    // Loading CoreHaptics makes its secure-coded task-token class available. Do not initialize a guest engine in the host.
    BOOL tokenBeforeInitialization = NSClassFromString(@"AVHapticProcessTaskToken") != Nil;
    (void)[CHHapticEngine capabilitiesForHardware];
    BOOL engineClass = [CHHapticEngine class] != Nil;
    BOOL tokenClass = NSClassFromString(@"AVHapticProcessTaskToken") != Nil;
    BOOL peerInitializer = [NSXPCListener instancesRespondToSelector:@selector(_xpcConnection)] &&
        [NSXPCListener instancesRespondToSelector:@selector(_setQueue:)] && [NSXPCConnection instancesRespondToSelector:@selector(auditToken)];
    if (!engineClass || !tokenClass || !peerInitializer) {
        fprintf(stderr, "[haptics] relay unavailable engine=%d token=%d token-before=%d peer-init=%d\n",
                engineClass, tokenClass, tokenBeforeInitialization, peerInitializer);
        if (error) *error = ENOTSUP;
        return nil;
    }
    const char *trace = getenv("LSB_HAPTIC_TRACE");
    atomic_store_explicit(&traceEnabled, trace && !strcmp(trace, "1"), memory_order_relaxed);
    _authorization = [authorization copy];
    _queue = dispatch_queue_create("com.kdt.LiveSimBooter.haptics", DISPATCH_QUEUE_SERIAL);
    _peers = [NSMutableSet set];
    _listener = [NSXPCListener anonymousListener];
    if (!_listener) { if (error) *error = ENOMEM; return nil; }
    [_listener _setQueue:_queue];
    _listener.delegate = self;
    // Stock NSXPCListener owns peer construction. Its internal initializer is objc_direct, not a messageable private selector.
    // The listener's simulator-to-host mode is inherited by accepted peers; outbound native connections keep native mode.
    xpc_connection_t listenerConnection = [_listener _xpcConnection];
    if (!listenerConnection) { if (error) *error = ENOTSUP; [self close]; return nil; }
    xpc_connection_enable_sim2host_4sim(listenerConnection);
    xpc_endpoint_t endpoint = xpc_endpoint_create(listenerConnection);
    mach_port_t registration = endpoint ? xpc_endpoint_copy_listener_port_4sim(endpoint) : MACH_PORT_NULL;
    if (!MACH_PORT_VALID(registration)) { if (error) *error = EIO; [self close]; return nil; }
    atomic_store(&_registration, registration);
    [_listener resume];
    return self;
}

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection {
    (void)listener;
    audit_token_t audit = connection.auditToken;
    if (atomic_load(&_closed) || _peers.count >= LSBMaximumHapticPeers) {
        hapticDiagnostic("listener-unavailable", audit.val[5], _peers.count); return NO;
    }
    if (!_authorization(&audit)) { hapticDiagnostic("listener-unauthorized", audit.val[5], audit.val[7]); return NO; }
    hapticTrace("listener-authorized", audit.val[5], audit.val[7]);
    __weak LSBHapticServer *weakSelf = self;
    LSBHapticPeer *peer = [[LSBHapticPeer alloc] initWithConnection:connection queue:_queue finished:^(LSBHapticPeer *finished) {
        LSBHapticServer *server = weakSelf;
        if (server) [server->_peers removeObject:finished];
    }];
    if (!peer) return NO;
    hapticTrace("peer-ready", audit.val[5], 0);
    [_peers addObject:peer];
    [peer resume];
    return YES;
}

- (mach_port_t)registrationPort { return atomic_load(&_registration); }
- (void)close {
    if (atomic_exchange(&_closed, true)) return;
    mach_port_t registration = atomic_exchange(&_registration, MACH_PORT_NULL);
    if (MACH_PORT_VALID(registration)) mach_port_deallocate(mach_task_self(), registration);
    NSXPCListener *listener = _listener;
    NSMutableSet<LSBHapticPeer *> *peers = _peers;
    if (_queue) dispatch_async(_queue, ^{
        [listener invalidate];
        for (LSBHapticPeer *peer in peers) [peer close];
        [peers removeAllObjects];
    });
}
- (void)dealloc { [self close]; }
@end
