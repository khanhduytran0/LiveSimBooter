#import "ViewController.h"
#include "HostApplication.h"
#include "Broker.h"
#import "TerminalSession.h"
#import "TerminalView.h"
#import "SimulatorDisplayView.h"
#import "FramebufferServer.h"
#import "SettingsViewController.h"
#import "BackgroundAudio.h"
#import "GuestDeviceSettings.h"
#import "IndigoHIDServer.h"
#include "SimulatorDisplayLayout.h"
#include <errno.h>
#include <string.h>

static __weak LSBSimulatorDisplayView *hostDisplay;
LSBSimulatorDisplayView *LSBHostSimulatorDisplay(void) { return hostDisplay; }

static void showStatus(UIViewController *controller, NSString *text) {
    UILabel *label = [UILabel new];
    label.text = text;
    label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    label.adjustsFontForContentSizeCategory = YES;
    label.numberOfLines = 0;
    label.textAlignment = NSTextAlignmentCenter;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [controller.view addSubview:label];
    UILayoutGuide *safe = controller.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
        [label.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
        [label.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor]
    ]];
}

@interface LSBVirtualHomeButton : UIButton
@property (nonatomic, copy) BOOL (^accessibilityPress)(void);
@end
@implementation LSBVirtualHomeButton
- (BOOL)accessibilityActivate { return self.enabled && self.accessibilityPress ? self.accessibilityPress() : NO; }
@end

@interface LSBSurfaceViewController : UIViewController
@property BOOL visible;
@property BOOL startupAttempted;
@property int startupError;
@property (nonatomic, copy) void (^startupResultHandler)(int);
@property (nonatomic, strong) UIStackView *startOverlay;
@property (nonatomic, strong) UILabel *startupStatus;
@property (nonatomic, strong) UIButton *startButton;
@property (nonatomic, strong) id indigoObserver;
@property (nonatomic, strong) id framebufferObserver;
@property (nonatomic, strong) id deviceSettingsObserver, inactiveObserver, activeObserver, sessionObserver;
@property (nonatomic, strong) LSBSimulatorDisplayView *simulatorDisplay;
@property (nonatomic, strong) LSBVirtualHomeButton *homeButton;
@property (nonatomic, strong) LSBIndigoHIDServer *pressedHomeServer;
@property (nonatomic, strong) LSBFramebufferServer *framebuffer;
@property uint64_t displayGeneration;
@end

@implementation LSBSurfaceViewController
- (void)dealloc {
    [_pressedHomeServer releaseHomeButton];
    if (_indigoObserver) [NSNotificationCenter.defaultCenter removeObserver:_indigoObserver];
    if (_framebufferObserver) [NSNotificationCenter.defaultCenter removeObserver:_framebufferObserver];
    for (id observer in @[_deviceSettingsObserver ?: NSNull.null, _inactiveObserver ?: NSNull.null, _activeObserver ?: NSNull.null,
                         _sessionObserver ?: NSNull.null])
        if (observer != NSNull.null) [NSNotificationCenter.defaultCenter removeObserver:observer];
    _framebuffer.frameHandler = nil;
}
- (BOOL)pressHomeButton {
    if (!self.visible || !self.homeButton.enabled || self.homeButton.hidden || self.pressedHomeServer ||
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return NO;
    LSBIndigoHIDServer *server = self.simulatorDisplay.indigoServer;
    if (!server) return NO;
    int error = [server sendHomeButtonPressed:YES];
    if (error) { fprintf(stderr, "[input] Simulator Home down: %s\n", strerror(error)); return NO; }
    self.pressedHomeServer = server;
    return YES;
}
- (void)releaseHomeButton {
    LSBIndigoHIDServer *server = self.pressedHomeServer;
    self.pressedHomeServer = nil;
    int error = [server releaseHomeButton];
    if (error && error != EAGAIN && error != ENOTCONN) fprintf(stderr, "[input] Simulator Home release: %s\n", strerror(error));
}
- (void)homeButtonDown:(UIButton *)button { (void)button; [self pressHomeButton]; }
- (void)homeButtonUp:(UIButton *)button { (void)button; [self releaseHomeButton]; }
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (!self.simulatorDisplay) return;
    LSBGuestDeviceType *device = LSBHostCurrentDeviceType();
    LSBFramebufferGeometry geometry = self.framebuffer ? self.framebuffer.geometry : (LSBFramebufferGeometry){0};
    CGSize pixels = self.framebuffer ? CGSizeMake(geometry.width, geometry.height) : CGSizeMake(device.width, device.height);
    BOOL needsHome = device.hasHomeButton;
    LSBDisplayLayout layout;
    BOOL fits = LSBComputeSimulatorDisplayLayout(self.view.safeAreaLayoutGuide.layoutFrame, pixels, needsHome || device.isPad,
                                                 LSBGuestDeviceSettings.sharedController.fullWidth, needsHome, &layout);
    if (!fits || !needsHome || !CGRectEqualToRect(self.homeButton.frame, layout.homeButtonFrame)) [self releaseHomeButton];
    self.simulatorDisplay.frame = fits ? layout.displayFrame : CGRectZero;
    self.homeButton.hidden = !fits || !needsHome || LSBHostSimulatorIsOff();
    self.homeButton.enabled = !self.homeButton.hidden && self.visible && self.simulatorDisplay.indigoServer != nil &&
                              UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    self.homeButton.frame = fits ? layout.homeButtonFrame : CGRectZero;
}
- (void)bindFramebuffer {
    LSBFramebufferServer *server = LSBHostFramebufferServer();
    if (_framebuffer == server) return;
    [self releaseHomeButton];
    [self.simulatorDisplay resetPresentation];
    _framebuffer.frameHandler = nil;
    _framebuffer = server;
    uint64_t generation = ++_displayGeneration;
    [self.view setNeedsLayout];
    if (!server) return;
    __weak LSBSurfaceViewController *weakSelf = self;
    __weak LSBFramebufferServer *weakServer = server;
    server.frameHandler = ^(IOSurfaceRef surface, CGRect dirtyRect, void (^completion)(int)) {
        LSBSurfaceViewController *controller = weakSelf;
        if (!controller || controller.framebuffer != weakServer || controller.displayGeneration != generation) { completion(ECANCELED); return; }
        LSBSimulatorDisplayView *display = controller.simulatorDisplay;
        // Hidden presentation still consumes dirty updates into its owned baseline without requesting a drawable.
        if (display) [display presentSurface:surface dirtyRect:dirtyRect completion:completion];
        else completion(ENODEV);
    };
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    self.visible = YES;
    [self updateSessionState];
    [self.view setNeedsLayout];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (!LSBHostSimulatorIsOff()) [self.simulatorDisplay becomeFirstResponder];
}
- (void)viewWillDisappear:(BOOL)animated {
    self.visible = NO;
    [self releaseHomeButton];
    self.homeButton.enabled = NO;
    self.simulatorDisplay.userInteractionEnabled = NO; // Cancels active contacts before another tab can receive touches.
    self.simulatorDisplay.presentationEnabled = NO;
    [super viewWillDisappear:animated];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.startupError = LSBInitialGuestStartupError();
    UILabel *status = [UILabel new];
    status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    status.adjustsFontForContentSizeCategory = YES;
    status.numberOfLines = 0;
    status.textAlignment = NSTextAlignmentCenter;
    status.hidden = YES;
    UIButtonConfiguration *configuration = UIButtonConfiguration.filledButtonConfiguration;
    configuration.title = NSLocalizedString(@"Start", @"Button that starts the simulator runtime");
    configuration.buttonSize = UIButtonConfigurationSizeLarge;
    __weak LSBSurfaceViewController *weakSelf = self;
    UIButton *button = [UIButton buttonWithConfiguration:configuration primaryAction:[UIAction actionWithHandler:^(UIAction *action) {
        (void)action;
        [weakSelf startInitialRuntime];
    }]];
    button.accessibilityIdentifier = @"runtime-start";
    UIStackView *overlay = [[UIStackView alloc] initWithArrangedSubviews:@[status, button]];
    overlay.axis = UILayoutConstraintAxisVertical;
    overlay.alignment = UIStackViewAlignmentCenter;
    overlay.spacing = 16;
    overlay.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:overlay];
    self.startOverlay = overlay;
    self.startupStatus = status;
    self.startButton = button;
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [overlay.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
        [overlay.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
        [overlay.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor]
    ]];
    self.sessionObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostSessionStateDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf updateSessionState]; }];
    [self updateSessionState];
}
- (void)updateSessionState {
    BOOL off = LSBHostSimulatorIsOff();
    self.simulatorDisplay.hidden = off;
    self.simulatorDisplay.presentationEnabled = self.visible && !off;
    self.simulatorDisplay.userInteractionEnabled = self.visible && !off;
    self.startOverlay.hidden = !off && !self.startupError;
    if (self.startupError) {
        self.startupStatus.text = [NSString stringWithFormat:
            NSLocalizedString(@"Could not start runtime: %s. Close and reopen the app to try again.", @"Runtime startup error; %s is a system error"),
            strerror(self.startupError)];
        self.startupStatus.hidden = NO;
        self.startButton.hidden = YES;
    } else if (off) {
        [self releaseHomeButton];
        [self.simulatorDisplay resignFirstResponder];
        self.startupStatus.text = NSLocalizedString(@"Simulator is powered off.", @"Simulator display status after power off");
        self.startupStatus.hidden = !LSBInitialGuestStartupWasAttempted();
        self.startButton.hidden = NO;
        self.startButton.enabled = YES;
        [self.view bringSubviewToFront:self.startOverlay];
    }
    [self.view setNeedsLayout];
}
- (void)startInitialRuntime {
    if (self.startupAttempted && !LSBHostSimulatorIsOff()) return;
    self.startupAttempted = YES;
    self.startButton.enabled = NO;
    int error = LSBStartInitialGuest();
    self.startupError = error;
    if (self.startupResultHandler) self.startupResultHandler(error);
    [self updateSessionState];
    if (error || self.simulatorDisplay) return;
    if (!LSBHostUsesSimulatorDisplay()) {
        showStatus(self, NSLocalizedString(@"This runtime has no simulator display.", @"Status for a runtime without a graphical display"));
        return;
    }
    LSBSimulatorDisplayView *display = [[LSBSimulatorDisplayView alloc] initWithFrame:CGRectZero];
    display.indigoServer = LSBHostIndigoHIDServer();
    display.presentationEnabled = self.visible;
    display.userInteractionEnabled = self.visible;
    [self.view addSubview:display];
    hostDisplay = display;
    self.simulatorDisplay = display;
    UIButtonConfiguration *homeConfiguration = UIButtonConfiguration.grayButtonConfiguration;
    homeConfiguration.image = [UIImage systemImageNamed:@"square"];
    homeConfiguration.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration configurationWithPointSize:18 weight:UIImageSymbolWeightRegular];
    homeConfiguration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    homeConfiguration.baseForegroundColor = UIColor.labelColor;
    UIBackgroundConfiguration *background = homeConfiguration.background;
    CGFloat inset = (LSBDisplayHomeButtonHitSize - LSBDisplayHomeButtonVisibleSize) / 2.0;
    background.backgroundInsets = NSDirectionalEdgeInsetsMake(inset, inset, inset, inset);
    background.strokeColor = UIColor.secondaryLabelColor;
    background.strokeWidth = 1.5;
    homeConfiguration.background = background;
    LSBVirtualHomeButton *home = [[LSBVirtualHomeButton alloc] initWithFrame:CGRectZero];
    home.configuration = homeConfiguration;
    home.accessibilityLabel = NSLocalizedString(@"Simulator Home", @"Accessibility label for the virtual Home button");
    home.accessibilityHint = NSLocalizedString(@"Press the simulated device's Home button.", @"Accessibility hint for the virtual Home button");
    home.accessibilityIdentifier = @"simulator-home-button";
    [home addTarget:self action:@selector(homeButtonDown:) forControlEvents:UIControlEventTouchDown];
    [home addTarget:self action:@selector(homeButtonUp:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
        UIControlEventTouchCancel | UIControlEventTouchDragExit];
    [self.view addSubview:home];
    self.homeButton = home;
    [self bindFramebuffer];
    __weak LSBSimulatorDisplayView *weakDisplay = display;
    __weak LSBSurfaceViewController *weakSelf = self;
    home.accessibilityPress = ^BOOL {
        LSBSurfaceViewController *controller = weakSelf;
        BOOL sent = [controller pressHomeButton];
        [controller releaseHomeButton];
        return sent;
    };
    self.indigoObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostIndigoDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf releaseHomeButton];
            weakDisplay.indigoServer = LSBHostIndigoHIDServer();
            [weakSelf.view setNeedsLayout];
        }];
    self.framebufferObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostFramebufferDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf bindFramebuffer]; }];
    self.deviceSettingsObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification
        object:LSBGuestDeviceSettings.sharedController queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification; [weakSelf.view setNeedsLayout];
        }];
    self.inactiveObserver = [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillResignActiveNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification; [weakSelf releaseHomeButton]; weakSelf.homeButton.enabled = NO;
        }];
    self.activeObserver = [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf.view setNeedsLayout]; }];
    [self.view setNeedsLayout];
    if (self.visible) [display becomeFirstResponder];
}
@end

@interface LSBFishViewController : UIViewController
@property int startupError;
@property BOOL visible;
@property BOOL receivedTerminalOutput;
@property (nonatomic, strong) id terminalObserver;
@property (nonatomic, strong) id closeObserver;
@property (nonatomic, strong) id sessionObserver;
@property (nonatomic, strong) LSBTerminalView *terminal;
@property (nonatomic, strong) LSBTerminalSession *session;
@end

@implementation LSBFishViewController
- (void)dealloc {
    if (_terminalObserver) [NSNotificationCenter.defaultCenter removeObserver:_terminalObserver];
    if (_closeObserver) [NSNotificationCenter.defaultCenter removeObserver:_closeObserver];
    if (_sessionObserver) [NSNotificationCenter.defaultCenter removeObserver:_sessionObserver];
    _session.outputHandler = nil;
}
- (NSString *)waitingStatus {
    if (self.startupError) return [NSString stringWithFormat:
        NSLocalizedString(@"Runtime startup failed: %s. Close and reopen the app to try again.", @"Terminal startup error; %s is a system error"),
        strerror(self.startupError)];
    return LSBHostSimulatorIsOff() ?
        NSLocalizedString(@"Start the simulator from the IOSurface tab.", @"Terminal status when the simulator is powered off; IOSurface is a tab name") :
        NSLocalizedString(@"Waiting for launchd to start Fish…", @"Terminal status while the simulator starts the Fish shell; preserve launchd and Fish");
}
- (void)runtimeStartupFinished:(int)error {
    self.startupError = error;
    if (!self.session) self.terminal.statusText = [self waitingStatus];
}
- (void)bindTerminal {
    LSBTerminalSession *session = LSBHostTerminal();
    if (_session == session) {
        if (session.closed) [self terminalClosed];
        else if (!session) self.terminal.statusText = [self waitingStatus];
        return;
    }
    _session.outputHandler = nil;
    _session = session;
    self.receivedTerminalOutput = NO;
    self.terminal.inputHandler = nil;
    self.terminal.resizeHandler = nil;
    self.terminal.statusText = session ? NSLocalizedString(@"Terminal attached.", @"Terminal status after connecting to the shell") : [self waitingStatus];
    if (!session) return;
    __weak LSBFishViewController *weakSelf = self;
    self.terminal.inputHandler = ^(NSData *data) {
        LSBFishViewController *controller = weakSelf;
        // Hidden tabs cannot receive UIKit input, but the terminal parser must still answer device/cursor queries from Fish.
        if (!controller || controller.session != session || session.closed) return;
        int error = [session sendInput:data];
        if (error) fprintf(stderr, "[terminal] input: %s\n", strerror(error));
    };
    self.terminal.resizeHandler = ^(unsigned short rows, unsigned short columns) { [session resizeRows:rows columns:columns]; };
    __weak LSBTerminalSession *weakSession = session;
    session.outputHandler = ^(NSData *data) {
        LSBFishViewController *controller = weakSelf;
        if (!controller || controller.session != weakSession) return;
        if (data.length) controller.receivedTerminalOutput = YES;
        [controller.terminal appendOutput:data];
    };
    [self.terminal setNeedsLayout];
    if (session.closed) [self terminalClosed];
    else if (self.visible) [self.terminal becomeFirstResponder];
}
- (void)terminalClosed {
    self.terminal.inputHandler = nil;
    self.terminal.resizeHandler = nil;
    [self.terminal resignFirstResponder];
    self.terminal.statusText = self.receivedTerminalOutput ? NSLocalizedString(@"Terminal closed.", @"Terminal status after the shell exits") :
        NSLocalizedString(@"Terminal closed without output.", @"Terminal status when the shell exits without producing output");
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.startupError = LSBInitialGuestStartupError();
    LSBTerminalView *terminal = [[LSBTerminalView alloc] initWithFrame:CGRectZero];
    terminal.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:terminal];
    self.terminal = terminal;
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint *preferredBottom = [terminal.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor];
    preferredBottom.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [terminal.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [terminal.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [terminal.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [terminal.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor],
        [terminal.bottomAnchor constraintLessThanOrEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor],
        preferredBottom
    ]];
    terminal.statusText = [self waitingStatus];
    [self bindTerminal];
    __weak LSBFishViewController *weakSelf = self;
    self.terminalObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostTerminalDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf bindTerminal]; }];
    self.closeObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBTerminalSessionDidCloseNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            LSBFishViewController *controller = weakSelf;
            if (controller && notification.object == controller.session) [controller terminalClosed];
        }];
    self.sessionObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostSessionStateDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf bindTerminal]; }];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    self.visible = YES;
    // A tab/scene can become visible after attachment and EOF; recover the current session even if a notification was missed.
    [self bindTerminal];
    self.terminal.presentationEnabled = YES;
    if (self.session && !self.session.closed) [self.terminal becomeFirstResponder];
}
- (void)viewWillDisappear:(BOOL)animated {
    self.visible = NO;
    self.terminal.presentationEnabled = NO;
    [self.view endEditing:YES];
    [super viewWillDisappear:animated];
}
@end

@implementation LSBHostViewController
- (void)viewDidLoad {
    [super viewDidLoad];
    // The scene keeps the display and terminal connections. Tab selection never starts or stops a process.
    LSBSurfaceViewController *surface = [LSBSurfaceViewController new];
    surface.tabBarItem = [[UITabBarItem alloc] initWithTitle:NSLocalizedString(@"IOSurface", @"Simulator display tab; preserve the IOSurface API name")
        image:[UIImage systemImageNamed:@"iphone"] tag:0];
    LSBFishViewController *fish = [LSBFishViewController new];
    __weak LSBFishViewController *weakFish = fish;
    surface.startupResultHandler = ^(int error) { [weakFish runtimeStartupFinished:error]; };
    fish.tabBarItem = [[UITabBarItem alloc] initWithTitle:NSLocalizedString(@"Fish", @"Simulator shell tab; preserve the Fish shell name")
        image:[UIImage systemImageNamed:@"terminal"] tag:1];
    LSBSettingsViewController *settings = [LSBSettingsViewController new];
    UINavigationController *settingsNavigation = [[UINavigationController alloc] initWithRootViewController:settings];
    settingsNavigation.tabBarItem = [[UITabBarItem alloc] initWithTitle:NSLocalizedString(@"Settings", @"Native host settings tab")
        image:[UIImage systemImageNamed:@"gearshape"] tag:2];
    self.viewControllers = @[surface, fish, settingsNavigation];
    // Restoring an explicit opt-in is the only startup path that may activate audio.
    (void)LSBBackgroundAudio.sharedController;
    self.selectedIndex = 0;
    // Install both consumers before asynchronous check-in/output, without focusing the hidden terminal.
    [surface loadViewIfNeeded];
    [fish loadViewIfNeeded];
    BOOL attempted = LSBInitialGuestStartupWasAttempted();
    if ((!attempted && LSBInitialGuestStartsAutomatically()) || (attempted && !LSBHostSimulatorIsOff())) [surface startInitialRuntime];
}
@end
