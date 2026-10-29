#ifndef LSB_BROKER_H
#define LSB_BROKER_H
// Start on the original main thread, initially or after confirmed power off. The existing broker receiver is reused.
// Returns zero after submitting the initial guest, or an errno value if startup fails.
int LSBStartHost(int argc, const char *argv[], const char *envp[]);
#ifdef __OBJC__
@class LSBIndigoHIDServer;
@class LSBFramebufferServer;
@class NSString;
@class LSBGuestDeviceType;
// Main-thread native UI only. Saved choices do not change an active session until its next restart.
BOOL LSBHostSimulatorIsOff(void);
BOOL LSBHostCanControlSimulator(void);
BOOL LSBHostConfigurationNeedsRestart(void);
int LSBHostSaveDeviceType(LSBGuestDeviceType *type);
int LSBHostSaveHapticsEnabled(BOOL enabled);
int LSBHostSaveAudioOutputEnabled(BOOL enabled);
// A current child launch reaches authenticated CHECKIN before draining. Restart completes after old-process exit and new-session submission;
// power off completes only after confirmed owned-process exit.
void LSBHostRestartSimulator(void (^completion)(int error));
void LSBHostPowerOffSimulator(void (^completion)(int error));
// Compatibility apply operations stage a catalog profile and drain the owned session before replacement.
LSBGuestDeviceType *LSBHostCurrentDeviceType(void);
BOOL LSBHostCanApplyDeviceType(void);
void LSBHostApplyDeviceType(LSBGuestDeviceType *type, void (^completion)(int error, BOOL restarted));
BOOL LSBHostCurrentHapticsEnabled(void);
BOOL LSBHostCurrentAudioOutputEnabled(void);
void LSBHostApplyAudioOutputEnabled(BOOL enabled, void (^completion)(int error, BOOL restarted));
void LSBHostApplyHapticsEnabled(BOOL enabled, void (^completion)(int error, BOOL restarted));
extern NSString *const LSBHostIndigoDidChangeNotification;
extern NSString *const LSBHostFramebufferDidChangeNotification;
extern NSString *const LSBHostTerminalDidChangeNotification;
extern NSString *const LSBHostSessionStateDidChangeNotification;
int LSBHostUsesSimulatorDisplay(void);
// Main-thread access. The display view forwards only its own touches to this simulator-local endpoint.
LSBIndigoHIDServer *LSBHostIndigoHIDServer(void);
// Main-thread access. Bind the presentation callback before the next backboardd authorization.
LSBFramebufferServer *LSBHostFramebufferServer(void);
#endif
#endif
