#import <Foundation/Foundation.h>
#include "DisplaySettingsControl.h"
#include "PrivateDisplaySettings.h"
#include "../../ProcessSupport/PrivateXPC.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern unsigned dyld_get_active_platform(void);
enum { PerformanceHUDFlag = 0x10000000 };

static int readHUD(mach_port_t port, int32_t *mode) {
    uint32_t flags = 0;
    int error = LSBRenderServerGetFlags(port, &flags), value = 0;
    if (error) return error;
    if (flags & PerformanceHUDFlag) {
        int32_t raw = 0;
        error = LSBRenderServerGetHUDValue(port, &raw);
        if (error) return error;
        value = raw + 1;
    }
    *mode = value;
    return 0;
}

void LSBDisplaySettingsApply(uint32_t operation, int32_t value, LSBDSettingsResponse *response) {
    response->operation = operation;
    response->error = 0;
    response->hudMode = response->touches = -1;
    response->hudError = response->touchesError = ENOTSUP;
    if (dyld_get_active_platform() != 7) { response->error = ENOTSUP; return; }
    if ((operation == LSBDSettingsRead && value) || (operation == LSBDSettingsHUD && (value < 0 || value > 9)) ||
        (operation == LSBDSettingsTouches && (value < 0 || value > 1)) || operation < LSBDSettingsRead || operation > LSBDSettingsTouches) {
        response->error = EINVAL;
        return;
    }
    @autoreleasepool {
        mach_port_t server = MACH_PORT_NULL;
        kern_return_t lookup = bootstrap_look_up(bootstrap_port, "com.apple.CARenderServer", &server);
        response->hudError = lookup || !MACH_PORT_VALID(server) ? ENOTCONN : readHUD(server, &response->hudMode);
        if (operation == LSBDSettingsHUD && !response->hudError) {
            if (value) response->hudError = LSBRenderServerSetHUDValue(server, value - 1);
            // The mask updates only this bit. Unrelated render-server diagnostics remain untouched.
            if (!response->hudError) response->hudError = LSBRenderServerSetFlags(server, PerformanceHUDFlag, value ? PerformanceHUDFlag : 0);
            if (!response->hudError) response->hudError = readHUD(server, &response->hudMode);
            response->error = response->hudError ?: (response->hudMode == value ? 0 : EIO);
        } else if (operation == LSBDSettingsHUD) response->error = response->hudError;
        if (MACH_PORT_VALID(server)) mach_port_deallocate(mach_task_self(), server);

        @try {
            // Same suite/store as BKSLocalDefaults. Stock backboardd observes this Boolean through NSUserDefaults KVO.
            NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:@"com.apple.backboardd"];
            if (!defaults) response->touchesError = ENOMEM;
            else {
                if (operation == LSBDSettingsTouches) [defaults setBool:value != 0 forKey:@"BKDigitizerVisualizeTouches"];
                BOOL synchronized = [defaults synchronize];
                id observed = [defaults objectForKey:@"BKDigitizerVisualizeTouches"];
                response->touchesError = !synchronized ? EIO : observed && CFGetTypeID((__bridge CFTypeRef)observed) != CFBooleanGetTypeID() ? EINVAL : 0;
                if (!response->touchesError) response->touches = observed ? [observed boolValue] : 0;
            }
        } @catch (NSException *exception) {
            (void)exception;
            response->touchesError = EIO;
        }
        if (operation == LSBDSettingsTouches) response->error = response->touchesError ?: (response->touches == value ? 0 : EIO);
        if (operation == LSBDSettingsRead) response->error = response->hudError ?: response->touchesError;
        if (response->hudError) response->hudMode = -1;
        if (response->touchesError) response->touches = -1;
    }
}

#ifndef LSB_DISPLAY_SETTINGS_TESTING
int main(int argc, const char *argv[]) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &signals, NULL);
    signal(SIGALRM, SIG_DFL);
    if (argc == 2 && !strcmp(argv[1], "--serve")) {
        int error = LSBDisplaySettingsServe();
        fprintf(stderr, "display-settings: worker stopped: %s\n", strerror(error));
        return error ? 1 : 0;
    }
    uint32_t operation = 0;
    int32_t value = 0;
    if (argc == 2 && !strcmp(argv[1], "read")) operation = LSBDSettingsRead;
    else if (argc == 3 && argv[2][0] >= '0' && argv[2][0] <= '9' && !argv[2][1]) {
        operation = !strcmp(argv[1], "hud") ? LSBDSettingsHUD : !strcmp(argv[1], "touches") ? LSBDSettingsTouches : 0;
        value = argv[2][0] - '0';
    }
    if (!operation) { fprintf(stderr, "usage: display-settings read | hud 0..9 | touches 0..1\n"); return 64; }
    // Bound the diagnostic CLI even if a private synchronous RPC or preferences service never replies.
    alarm(10);
    LSBDSettingsResponse result = {0};
    LSBDisplaySettingsApply(operation, value, &result);
    alarm(0);
    printf("{\"pid\":%d,\"operation\":%u,\"requested\":%d,\"error\":%d,\"hud\":%d,\"hud_error\":%d,\"touches\":%d,\"touches_error\":%d}\n",
           getpid(), operation, value, result.error, result.hudMode, result.hudError, result.touches, result.touchesError);
    return result.error ? 1 : 0;
}
#endif
