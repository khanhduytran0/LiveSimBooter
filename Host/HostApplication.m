#import <UIKit/UIKit.h>
#include "HostApplication.h"
#include "Broker.h"
#import "ViewController.h"
#include <stdio.h>
#include <string.h>

static int launchArgumentCount;
static const char **launchArguments;
static const char **launchEnvironment;
static int startupError;
static BOOL initialGuestStarted;

@interface LSBHostSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation LSBHostSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session;
    (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [LSBHostViewController new];
    [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
}
- (void)sceneWillResignActive:(UIScene *)scene {
    (void)scene;
    UIApplication.sharedApplication.idleTimerDisabled = NO;
}
@end

@interface LSBHostApplicationDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation LSBHostApplicationDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application;
    (void)options;
    return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session
                              options:(UISceneConnectionOptions *)options {
    (void)application;
    (void)options;
    UISceneConfiguration *configuration = [UISceneConfiguration configurationWithName:@"Runtime" sessionRole:session.role];
    configuration.delegateClass = LSBHostSceneDelegate.class;
    return configuration;
}
@end

bool LSBInitialGuestStartsAutomatically(void) {
    return LSBHostArgumentsRequestAutomaticStart(launchArgumentCount, launchArguments);
}

bool LSBInitialGuestStartupWasAttempted(void) { return initialGuestStarted; }
int LSBInitialGuestStartupError(void) { return startupError; }

int LSBStartInitialGuest(void) {
    if (!LSBHostShouldAttemptGuestStart(initialGuestStarted, startupError, LSBHostSimulatorIsOff())) return startupError;
    initialGuestStarted = YES;
    startupError = LSBStartHost(launchArgumentCount, launchArguments, launchEnvironment);
    if (startupError) fprintf(stderr, "LiveSimBooter: runtime startup failed: %s\n", strerror(startupError));
    return startupError;
}

int LSBRunHostApplication(int argc, const char *argv[], const char *envp[]) {
    // Capture the original guest arguments before UIKit starts its main-thread event loop.
    launchArgumentCount = argc;
    launchArguments = argv;
    launchEnvironment = envp;
    @autoreleasepool { return UIApplicationMain(argc, (char **)argv, nil, NSStringFromClass(LSBHostApplicationDelegate.class)); }
}
