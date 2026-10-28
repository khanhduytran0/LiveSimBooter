#ifndef LSB_BROKER_H
#define LSB_BROKER_H
// Start once on the original main thread. The application's event loop continues processing broker callbacks.
// Returns zero after submitting the initial guest, or an errno value if startup fails.
int LSBStartHost(int argc, const char *argv[], const char *envp[]);
#ifdef __OBJC__
@class LSBIndigoHIDServer;
@class LSBFramebufferServer;
@class NSString;
@class LSBGuestDeviceType;
// Main-thread native UI only. Apply stages a catalog profile and uses the existing owned-process drain before replacement.
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
