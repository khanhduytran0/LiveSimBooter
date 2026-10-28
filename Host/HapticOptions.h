#pragma once
#import <Foundation/Foundation.h>

// Preserve caller event/audio policy, but never forward a guest identity or an invalid simulator AudioSession to the native daemon.
static inline NSDictionary *LSBHapticHostOptions(NSDictionary *options, uint32_t sessionID, id token) {
    if (![options isKindOfClass:NSDictionary.class] || !sessionID || !token) return nil;
    NSMutableDictionary *result = [options mutableCopy];
    result[@"AudioSessionID"] = @(sessionID);
    result[@"AudioSessionIsShared"] = @YES;
    result[@"BypassAudioSession"] = @NO;
    result[@"ClientProcessTaskToken"] = token;
    [result removeObjectForKey:@"MachServiceName"];
    return result;
}
