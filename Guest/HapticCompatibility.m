#import <Foundation/Foundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <xpc/xpc.h>
#include <string.h>

@interface CHHapticEngine (LSBRelayInitialization)
- (BOOL)doInit:(uint32_t)sessionID sessionIsShared:(BOOL)shared options:(NSDictionary *)options
    bypassAudioSession:(BOOL)bypass error:(NSError **)error;
@end

typedef BOOL (*LSBHapticDoInit)(id, SEL, uint32_t, BOOL, NSDictionary *, BOOL, NSError **);
static LSBHapticDoInit originalDoInit;
static atomic_bool relayEnabled;

// The simulator SDK hides this exported libxpc entry. Keep its normal symbol binding as the original side of interposition.
extern xpc_connection_t LSBHapticCreateMachService(const char *, dispatch_queue_t, uint64_t) __asm("_xpc_connection_create_mach_service");
extern void xpc_connection_enable_sim2host_4sim(xpc_connection_t);

static xpc_connection_t createHapticConnection(const char *name, dispatch_queue_t queue, uint64_t flags) {
    xpc_connection_t connection = LSBHapticCreateMachService(name, queue, flags);
    // A plain simulator connection uses wire mode1. The native relay uses mode2 on its listener and peers; both endpoints
    // must opt into that stock bridge mode. No TLS, environment or Objective-C access occurs before the constructor enables this.
    if (atomic_load_explicit(&relayEnabled, memory_order_relaxed) && connection && name &&
        !(flags & XPC_CONNECTION_MACH_SERVICE_LISTENER) && !strcmp(name, "com.apple.audio.hapticd"))
        xpc_connection_enable_sim2host_4sim(connection);
    return connection;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement;
    const void *original;
} hapticConnectionInterpose = {(const void *)createHapticConnection, (const void *)LSBHapticCreateMachService};

static BOOL relayDoInit(id engine, SEL selector, uint32_t sessionID, BOOL shared, NSDictionary *options, BOOL bypass, NSError **error) {
    NSString *service = options[@"MachServiceName"];
    BOOL usesRelay = !service || ([service isKindOfClass:NSString.class] && [service isEqualToString:@"com.apple.audio.hapticd"]);
    // Simulator AudioSession deliberately returns ID zero. This bypass is local to its constructor; the authenticated native
    // relay always replaces it with its own genuine auxiliary session and BypassAudioSession=NO before contacting audiomxd.
    return originalDoInit(engine, selector, sessionID, shared, options, usesRelay && sessionID == 0 ? YES : bypass, error);
}

static BOOL pinnedInitializer(IMP implementation) {
    static const unsigned char expectedUUID[16] = {
        0x2a, 0xfd, 0x3c, 0x06, 0x3a, 0x36, 0x35, 0xce, 0x89, 0x40, 0xb3, 0x81, 0xc1, 0xaf, 0xfc, 0x0b
    };
    // Original 23A343 CoreHaptics doInit:sessionIsShared:options:bypassAudioSession:error:, image +0x32a8c.
    static const uint32_t expectedInstructions[] = { 0xd10243ff, 0xa90467fa, 0xa9055ff8, 0xa90657f6 };
    Dl_info info = {0};
    if (!dladdr((const void *)implementation, &info) || !info.dli_fbase) return NO;
    const struct mach_header_64 *header = info.dli_fbase;
    if (header->magic != MH_MAGIC_64 || header->sizeofcmds > 1024 * 1024) return NO;
    const unsigned char *cursor = (const unsigned char *)(header + 1), *end = cursor + header->sizeofcmds;
    BOOL uuidMatches = NO;
    for (uint32_t index = 0; index < header->ncmds; index++) {
        if ((size_t)(end - cursor) < sizeof(struct load_command)) return NO;
        const struct load_command *command = (const void *)cursor;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(end - cursor)) return NO;
        if (command->cmd == LC_UUID && command->cmdsize == sizeof(struct uuid_command)) {
            uuidMatches = !memcmp(((const struct uuid_command *)command)->uuid, expectedUUID, sizeof(expectedUUID));
        }
        cursor += command->cmdsize;
    }
    return uuidMatches && !memcmp((const void *)implementation, expectedInstructions, sizeof(expectedInstructions));
}

__attribute__((constructor)) static void installHapticRelayCompatibility(void) {
    // This optional Objective-C dylib initializes after its Foundation/CoreHaptics dependencies, outside the core C shim's early hooks.
    // LiveProcess supplies this opt-in from the broker's immutable native session settings, alongside the fixed helper path.
    const char *enabled = getenv("LSB_EXPERIMENTAL_HAPTICS");
    if (!enabled || strcmp(enabled, "1")) return;
    atomic_store_explicit(&relayEnabled, true, memory_order_relaxed);
    Method method = class_getInstanceMethod(CHHapticEngine.class, @selector(doInit:sessionIsShared:options:bypassAudioSession:error:));
    if (!method || strcmp(method_getTypeEncoding(method), "B44@0:8I16B20@24B32^@36")) {
        fprintf(stderr, "[haptics] unsupported guest CoreHaptics initializer signature\n");
        return;
    }
    IMP implementation = method_getImplementation(method);
    if (!pinnedInitializer(implementation)) {
        fprintf(stderr, "[haptics] unsupported guest CoreHaptics UUID or initializer instructions\n");
        return;
    }
    originalDoInit = (LSBHapticDoInit)implementation;
    method_setImplementation(method, (IMP)relayDoInit);
}
