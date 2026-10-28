#import <Foundation/Foundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import "../Host/PrivateHaptics.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void deadline(int value) { (void)value; _exit(124); }

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    signal(SIGALRM, deadline);
    alarm(20);
    if (argc != 3 || strcmp(argv[1], "--expected-host-pid")) {
        fprintf(stderr, "usage: %s --expected-host-pid CURRENT_NATIVE_HOST_PID\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    errno = 0;
    long hostPID = strtol(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || hostPID <= 0 || hostPID > INT_MAX) return 2;
    @autoreleasepool {
        // Confirm this is the known foreground host relay before the optional helper can apply its local simulator constructor adaptation.
        NSXPCConnection *connection = [(id<LSBHapticNativeConnectionInitializer>)[NSXPCConnection alloc]
            initWithMachServiceName:@"com.apple.audio.hapticd" options:0x1000];
        connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(LSBHapticServerProtocol)];
        [connection resume];
        __block NSError *transportError = nil;
        id<LSBHapticServerProtocol> proxy = [connection synchronousRemoteObjectProxyWithErrorHandler:^(NSError *error) { transportError = error; }];
        [proxy queryCapabilities:@{} reply:^(NSDictionary *capabilities, NSError *error) {
            printf("relay-probe preflight capabilities=%s error=%s\n", capabilities.description.UTF8String, error.description.UTF8String);
        }];
        pid_t peer = connection.processIdentifier;
        printf("relay-probe peer=%d expected=%ld transport=%s\n", peer, hostPID, transportError.description.UTF8String);
        [connection invalidate];
        if (transportError || peer != hostPID) return 3;
        NSError *error = nil;
        CHHapticEngine *engine = [[CHHapticEngine alloc] initAndReturnError:&error];
        printf("relay-probe public-init valid=%d error=%s\n", engine != nil, error.description.UTF8String);
        if (!engine) return 4;
        engine.playsHapticsOnly = YES;
        engine.isMutedForAudio = YES;
        engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { fprintf(stderr, "relay-probe stopped=%ld\n", (long)reason); };
        engine.resetHandler = ^{ fprintf(stderr, "relay-probe reset\n"); };
        if (![engine startAndReturnError:&error]) { fprintf(stderr, "relay-probe start-error=%s\n", error.description.UTF8String); return 5; }
        CHHapticEventParameter *intensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:0.75];
        CHHapticEventParameter *sharpness = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.8];
        CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient
                                                        parameters:@[intensity, sharpness] relativeTime:0];
        CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
        id<CHHapticPatternPlayer> player = pattern ? [engine createPlayerWithPattern:pattern error:&error] : nil;
        if (!player || ![player startAtTime:CHHapticTimeImmediate error:&error]) {
            fprintf(stderr, "relay-probe play-error=%s\n", error.description.UTF8String); return 6;
        }
        printf("relay-probe transient-accepted; physical output requires observer confirmation\n");
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.8]];
        dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
        __block NSError *stopError = nil;
        [engine stopWithCompletionHandler:^(NSError *error) { stopError = error; dispatch_semaphore_signal(stopped); }];
        if (dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC))) return 7;
        printf("relay-probe stop-error=%s\n", stopError.description.UTF8String);
        return stopError ? 8 : 0;
    }
}
