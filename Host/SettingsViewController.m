#import "SettingsViewController.h"
#import "SettingsChoiceControllers.h"
#import "BackgroundAudio.h"
#import "GuestDisplaySettings.h"
#import "GuestDeviceSettings.h"
#import "SigningIdentity.h"
#import "GuestApplicationInstaller.h"
#import "Broker.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <string.h>

static NSString *const DeviceGroup = @"device-group", *const SigningGroup = @"signing-group", *const DisplayGroup = @"display-group";
static NSString *const AudioGroup = @"audio-group", *const HapticsGroup = @"haptics-group", *const BackgroundGroup = @"background-group";
static NSString *const SessionGroup = @"session-group";
static NSString *const ApplicationsGroup = @"applications-group", *const InstallIPA = @"simulator-install-ipa";
static NSString *const Device = @"simulator-device-picker", *const FullWidth = @"simulator-full-width", *const LazySign = @"simulator-lazy-sign";
static NSString *const SigningIdentity = @"signing-identity-import", *const HUD = @"simulator-performance-hud", *const Touches = @"simulator-visualize-touches";
static NSString *const AudioOutput = @"simulator-audio-output", *const Haptics = @"simulator-haptic-passthrough";
static NSString *const BackgroundAudio = @"silent-background-audio", *const PowerOff = @"simulator-power-off", *const Restart = @"simulator-restart";

static BOOL setSpecifierProperty(PSSpecifier *specifier, NSString *key, id value) {
    id previous = [specifier propertyForKey:key];
    if (previous == value || [previous isEqual:value]) return NO;
    if (value) [specifier setProperty:value forKey:key];
    else [specifier removePropertyForKey:key];
    return YES;
}

void LSBOfferSimulatorRestart(UIViewController *controller, LSBGuestDeviceSettings *settings) {
    if (settings.simulatorOff || !settings.needsRestart || !settings.canControlSimulator || controller.presentedViewController) return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:
        NSLocalizedString(@"Restart Simulator?", @"Title of the prompt to apply saved simulator settings.")
        message:NSLocalizedString(@"Your changes are saved. Restart now to apply them? Running simulator apps will close.",
            @"Message for the saved-settings restart prompt.")
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Later", @"Keep saved simulator settings without restarting now.")
        style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Restart", @"Restart the simulator session.")
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        (void)action;
        [settings restartSimulator];
    }]];
    [controller presentViewController:alert animated:YES completion:nil];
}

static void offerSigningRestart(UIViewController *controller, LSBGuestDeviceSettings *settings) {
    if ([controller.presentedViewController isKindOfClass:UIAlertController.class])
        [controller dismissViewControllerAnimated:YES completion:^{ LSBOfferSimulatorRestart(controller, settings); }];
    else LSBOfferSimulatorRestart(controller, settings);
}

@interface LSBSettingsViewController () <UIDocumentPickerDelegate>
@end

@implementation LSBSettingsViewController {
    LSBBackgroundAudio *_audio;
    LSBGuestDisplaySettings *_display;
    LSBGuestDeviceSettings *_device;
    LSBGuestApplicationInstaller *_installer;
    __weak UIDocumentPickerViewController *_ipaPicker;
    id _audioObserver, _displayObserver, _deviceObserver, _signingObserver, _installerObserver;
    NSString *_signingSummary, *_signingError;
    BOOL _importingIdentity;
}
- (instancetype)init {
    if (!(self = [super init])) return nil;
    _audio = LSBBackgroundAudio.sharedController;
    _display = LSBGuestDisplaySettings.sharedController;
    _device = LSBGuestDeviceSettings.sharedController;
    _installer = LSBGuestApplicationInstaller.sharedController;
    _signingSummary = LSBSigningIdentitySummary();
    self.title = NSLocalizedString(@"Settings", @"Title of the native Settings tab.");
    return self;
}
- (UITableViewStyle)tableViewStyle { return UITableViewStyleInsetGrouped; }
- (void)dealloc {
    if (_audioObserver) [NSNotificationCenter.defaultCenter removeObserver:_audioObserver];
    if (_displayObserver) [NSNotificationCenter.defaultCenter removeObserver:_displayObserver];
    if (_deviceObserver) [NSNotificationCenter.defaultCenter removeObserver:_deviceObserver];
    if (_signingObserver) [NSNotificationCenter.defaultCenter removeObserver:_signingObserver];
    if (_installerObserver) [NSNotificationCenter.defaultCenter removeObserver:_installerObserver];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.table.rowHeight = UITableViewAutomaticDimension;
    self.table.estimatedRowHeight = 54;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
        target:self action:@selector(refreshDisplay)];
    self.navigationItem.rightBarButtonItem.accessibilityLabel =
        NSLocalizedString(@"Refresh simulator settings", @"Accessibility label for the Settings refresh button.");
    __weak LSBSettingsViewController *weakSelf = self;
    _audioObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBBackgroundAudioDidChangeNotification object:_audio
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf updateSettings]; }];
    _displayObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDisplaySettingsDidChangeNotification object:_display
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf updateSettings]; }];
    _deviceObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification object:_device
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf updateSettings]; }];
    _signingObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBSigningIdentityDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf refreshSigningIdentity]; }];
    _installerObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestApplicationInstallerDidChangeNotification object:_installer
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf updateSettings]; }];
    [self updateSettings];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshDisplay];
}
- (void)refreshDisplay {
    [_device refresh];
    if (_device.canControlSimulator) [_display refresh];
    [_installer refresh];
    [self refreshSigningIdentity];
}
- (void)refreshSigningIdentity {
    _signingSummary = LSBSigningIdentitySummary();
    [_device refresh];
    [self updateSettings];
}
- (PSSpecifier *)group:(NSString *)identifier title:(NSString *)title {
    PSSpecifier *specifier = [PSSpecifier groupSpecifierWithName:title];
    specifier.identifier = identifier;
    return specifier;
}
- (PSSpecifier *)setting:(NSString *)identifier title:(NSString *)title cell:(PSSpecifierType)cell detail:(Class)detail {
    PSSpecifier *specifier = [PSSpecifier preferenceSpecifierNamed:title target:self
        set:cell == PSSwitchCell ? @selector(setSetting:specifier:) : NULL
        get:@selector(readSetting:) detail:detail cell:cell edit:Nil];
    specifier.identifier = identifier;
    return specifier;
}
- (NSArray<PSSpecifier *> *)specifiers {
    NSArray<PSSpecifier *> *existing = [super unprotectedSpecifiers];
    if (existing) return existing;
    NSArray<PSSpecifier *> *items = @[
        [self group:DeviceGroup title:NSLocalizedString(@"Simulator device", @"Settings section heading for device selection and display sizing.")],
        [self setting:Device title:NSLocalizedString(@"Device", @"Settings row that opens the simulated device picker.")
            cell:PSLinkCell detail:LSBDeviceTypePickerViewController.class],
        [self setting:FullWidth title:NSLocalizedString(@"Use Full Width", @"Switch to expand the simulator display within the available view.")
            cell:PSSwitchCell detail:Nil],
        [self group:SigningGroup title:NSLocalizedString(@"Code signing", @"Settings section heading for lazy runtime code signing.")],
        [self setting:LazySign title:NSLocalizedString(@"Lazy sign", @"Switch to sign runtime binaries on signature-registration failure.")
            cell:PSSwitchCell detail:Nil],
        [self setting:SigningIdentity title:NSLocalizedString(@"Import P12…", @"Choose a PKCS12 signing identity document.") cell:PSButtonCell detail:Nil],
        [self group:ApplicationsGroup title:NSLocalizedString(@"Applications", @"Settings section heading for simulator app installation.")],
        [self setting:InstallIPA title:NSLocalizedString(@"Install IPA…", @"Choose an IPA document to install in the simulator.") cell:PSButtonCell detail:Nil],
        [self group:DisplayGroup title:NSLocalizedString(@"Simulator display", @"Settings section heading for live display controls.")],
        [self setting:HUD title:NSLocalizedString(@"Performance HUD", @"Title of the performance heads-up display setting.")
            cell:PSLinkCell detail:LSBHUDSettingsViewController.class],
        [self setting:Touches title:NSLocalizedString(@"Visualize touches", @"Switch to show simulator touch markers.") cell:PSSwitchCell detail:Nil],
        [self group:AudioGroup title:NSLocalizedString(@"Experimental audio", @"Settings section heading for optional simulator audio output.")],
        [self setting:AudioOutput title:NSLocalizedString(@"Audio output", @"Switch for simulator audio playback on the physical device.")
            cell:PSSwitchCell detail:Nil],
        [self group:HapticsGroup title:NSLocalizedString(@"Experimental haptics", @"Settings section heading for optional haptic passthrough.")],
        [self setting:Haptics title:NSLocalizedString(@"Haptic passthrough", @"Switch for simulator haptic feedback on the physical device.")
            cell:PSSwitchCell detail:Nil],
        [self group:BackgroundGroup title:NSLocalizedString(@"Background execution", @"Settings section heading for native background audio.")],
        [self setting:BackgroundAudio title:NSLocalizedString(@"Silent background audio", @"Switch to keep the native host active by playing silence.")
            cell:PSSwitchCell detail:Nil],
        [self group:SessionGroup title:NSLocalizedString(@"Simulator session", @"Settings section heading for Power Off and Restart.")],
        [self setting:PowerOff title:NSLocalizedString(@"Power Off", @"Stop the simulator session without deleting its data.") cell:PSButtonCell detail:Nil],
        [self setting:Restart title:NSLocalizedString(@"Restart", @"Restart the simulator session.") cell:PSButtonCell detail:Nil],
    ];
    for (PSSpecifier *specifier in items) {
        if (specifier.cellType == PSButtonCell) specifier.buttonAction = @selector(performSettingAction:);
        [self updateSpecifier:specifier];
    }
    [super setSpecifiers:items];
    return items;
}
- (id)readSetting:(PSSpecifier *)specifier {
    NSString *identifier = specifier.identifier;
    if ([identifier isEqual:Device]) return LSBGuestDeviceTypeForIdentifier(_device.currentIdentifier).displayName ?:
        NSLocalizedString(@"Choose…", @"Placeholder when no simulated device name is available.");
    if ([identifier isEqual:HUD]) return _display.hudMode >= 0 && _display.hudMode < (NSInteger)LSBPerformanceHUDNames().count ?
        LSBPerformanceHUDNames()[_display.hudMode] : NSLocalizedString(@"Unavailable", @"A simulator display setting could not be read.");
    if ([identifier isEqual:FullWidth]) return @(_device.fullWidth);
    if ([identifier isEqual:LazySign]) return @(LSBLazySigningEnabled());
    if ([identifier isEqual:Touches]) return @(_display.touches == 1);
    if ([identifier isEqual:AudioOutput]) return @(_device.audioOutputEnabled);
    if ([identifier isEqual:Haptics]) return @(_device.hapticsEnabled);
    if ([identifier isEqual:BackgroundAudio]) return @(_audio.enabled);
    return nil;
}
- (BOOL)settingEnabled:(PSSpecifier *)specifier {
    NSString *identifier = specifier.identifier;
    if ([identifier isEqual:InstallIPA]) return _device.canControlSimulator && !_installer.pending && !_installer.outcomeUnknown;
    if ([identifier isEqual:LazySign]) return _device.canApply && !_importingIdentity && (LSBLazySigningEnabled() || _signingSummary.length);
    if ([identifier isEqual:SigningIdentity]) return _device.canApply && !_importingIdentity;
    if ([identifier isEqual:HUD] || [identifier isEqual:Touches]) return _device.canControlSimulator && !_display.pending &&
        !_display.outcomeUnknown && ([identifier isEqual:HUD] ? _display.hudMode >= 0 : _display.touches >= 0);
    if ([identifier isEqual:AudioOutput]) return _device.canApply && !_device.audioOutputForced;
    if ([identifier isEqual:Haptics]) return _device.canApply && !_device.hapticsForced;
    if ([identifier isEqual:PowerOff] || [identifier isEqual:Restart]) return _device.canControlSimulator;
    return YES;
}
- (NSString *)footerForGroup:(NSString *)identifier {
    if ([identifier isEqual:ApplicationsGroup]) {
        if (!_device.canControlSimulator && !_installer.pending && !_installer.outcomeUnknown)
            return NSLocalizedString(@"Start the simulator to install an IPA.", @"Applications settings footer while no controllable simulator is running.");
        return _installer.statusText;
    }
    if ([identifier isEqual:SigningGroup]) {
        NSString *description = NSLocalizedString(@"Off by default. Sign a runtime binary only when its signature registration fails. "
            @"The P12 and password stay in this app's device-only keychain. Changes apply on the next simulator start or restart.",
            @"Description of lazy signing and where its credentials are stored.");
        return _signingError.length ? [NSString stringWithFormat:NSLocalizedString(@"%@\n\n%@",
            @"Settings footer: explanatory paragraph, then current status."), description, _signingError] : description;
    }
    if ([identifier isEqual:SessionGroup]) return _device.sessionStatusText;
    if ([identifier isEqual:AudioGroup]) return _device.audioOutputForced ? NSLocalizedString(@"Enabled by the host launch environment.",
        @"Settings footer when a feature is forced on by the native host environment.") : _device.audioOutputStatusText;
    if ([identifier isEqual:HapticsGroup]) return _device.hapticsForced ? NSLocalizedString(@"Enabled by the host launch environment.",
        @"Settings footer when a feature is forced on by the native host environment.") : _device.hapticsStatusText;
    if ([identifier isEqual:DisplayGroup]) return _device.simulatorOff ?
        NSLocalizedString(@"Start the simulator to change its live display settings.", @"Settings footer while the simulator is off.") : _display.statusText;
    if ([identifier isEqual:DeviceGroup]) {
        return [NSString stringWithFormat:NSLocalizedString(@"%@\n\nFull width hides the tab bar on IOSurface. Tap the arrow below the display to show it again."
            @"\n\nIf this device and the simulated device both have a home indicator, Guided Access hides the host status bar and controls for a full-screen view. "
            @"End Guided Access to restore the previous layout.", @"Simulator Device footer explaining Full Width and Guided Access after the current status."),
            _device.statusText];
    }
    NSString *description = NSLocalizedString(@"Play silence to keep the host's audio session active in the background. Off by default. "
        @"This may increase battery use. Other audio can continue playing.",
        @"Description of native silent background audio, independent of simulator audio output.");
    return _audio.statusDetail.length ? [NSString stringWithFormat:NSLocalizedString(@"%@\n\n%@",
        @"Settings footer: explanatory paragraph, then current status."), description, _audio.statusDetail] : description;
}
- (BOOL)updateSpecifier:(PSSpecifier *)specifier {
    if (specifier.cellType == PSGroupCell) return setSpecifierProperty(specifier, @"footerText", [self footerForGroup:specifier.identifier]);
    BOOL changed = setSpecifierProperty(specifier, @"enabled", [self settingEnabled:specifier] ? @YES : @NO);
    // Track rendered state separately from Preferences' value cache; getters always use the existing controllers.
    changed |= setSpecifierProperty(specifier, @"LSBRenderedValue", [self readSetting:specifier]);
    if ([specifier.identifier isEqual:SigningIdentity]) {
        NSString *name = _importingIdentity ? NSLocalizedString(@"Importing…", @"P12 import is in progress.") :
            _signingSummary.length ? NSLocalizedString(@"Imported", @"A P12 signing identity has been imported; tap to replace it.") :
            NSLocalizedString(@"Import P12…", @"Choose a PKCS12 signing identity document.");
        if (![specifier.name isEqual:name]) { specifier.name = name; changed = YES; }
        changed |= setSpecifierProperty(specifier, @"LSBIdentitySummary", _signingSummary);
    }
    return changed;
}
- (void)updateSettings {
    if (!self.isViewLoaded) return;
    [UIView performWithoutAnimation:^{
        self.navigationItem.rightBarButtonItem.enabled = !self->_display.pending && !self->_device.pending;
        for (PSSpecifier *specifier in self.specifiers)
            if ([self updateSpecifier:specifier]) [self reloadSpecifier:specifier animated:NO];
        [self configureVisibleCells];
        [self.table layoutIfNeeded];
    }];
}
- (void)setSetting:(NSNumber *)value specifier:(PSSpecifier *)specifier {
    if (![self settingEnabled:specifier]) { [self reloadSpecifier:specifier animated:NO]; return; }
    NSString *identifier = specifier.identifier;
    BOOL saved = NO;
    if ([identifier isEqual:FullWidth]) [_device setFullWidth:value.boolValue];
    else if ([identifier isEqual:BackgroundAudio]) [_audio setEnabled:value.boolValue];
    else if ([identifier isEqual:Touches]) [_display setVisualizeTouches:value.boolValue];
    else if ([identifier isEqual:AudioOutput]) saved = [_device saveAudioOutputEnabled:value.boolValue];
    else if ([identifier isEqual:Haptics]) saved = [_device saveHapticsEnabled:value.boolValue];
    else if ([identifier isEqual:LazySign]) {
        int error = LSBHostSaveLazySigningEnabled(value.boolValue);
        _signingError = error ? [NSString stringWithFormat:
            NSLocalizedString(@"Could not save lazy signing: %s (%d).", @"Lazy signing settings error: description and numeric code."),
            strerror(error), error] : nil;
        [self refreshSigningIdentity];
        saved = !error;
    }
    // Restore confirmed values after a failed save or while live guest readback is pending.
    [self reloadSpecifier:specifier animated:NO];
    [self configureVisibleCells];
    if (saved) LSBOfferSimulatorRestart(self, _device);
}
- (void)performSettingAction:(PSSpecifier *)specifier {
    if (![self settingEnabled:specifier]) return;
    if ([specifier.identifier isEqual:InstallIPA]) [self chooseIPA];
    else if ([specifier.identifier isEqual:SigningIdentity]) [self chooseSigningIdentity];
    else if ([specifier.identifier isEqual:PowerOff]) [_device powerOffSimulator];
    else if ([specifier.identifier isEqual:Restart]) [_device restartSimulator];
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [super tableView:tableView cellForRowAtIndexPath:indexPath];
    [self configureCell:cell specifier:[self specifierAtIndexPath:indexPath]];
    return cell;
}
- (void)configureVisibleCells {
    // Preferences refreshes cached cells in place, so update our custom accessories after specifier reloads too.
    for (NSIndexPath *path in self.table.indexPathsForVisibleRows)
        [self configureCell:[self.table cellForRowAtIndexPath:path] specifier:[self specifierAtIndexPath:path]];
}
- (void)configureCell:(UITableViewCell *)cell specifier:(PSSpecifier *)specifier {
    NSString *identifier = specifier.identifier;
    cell.accessibilityIdentifier = identifier;
    cell.textLabel.numberOfLines = 0;
    NSString *hint = nil;
    if ([identifier isEqual:LazySign]) hint = NSLocalizedString(@"Import a P12 signing identity first. Save this choice, then restart now or later.",
        @"Accessibility hint for enabling lazy signing.");
    else if ([identifier isEqual:AudioOutput] || [identifier isEqual:Haptics]) hint =
        NSLocalizedString(@"Save this choice; restart now or later if the simulator is running.", @"Accessibility hint for optional simulator features.");
    cell.accessibilityHint = hint;
    if ([cell.accessoryView isKindOfClass:UISwitch.class]) {
        cell.accessoryView.accessibilityIdentifier = identifier;
        cell.accessoryView.accessibilityLabel = specifier.name;
        cell.accessoryView.accessibilityHint = hint;
    }
    if ([identifier isEqual:PowerOff]) cell.textLabel.textColor = [self settingEnabled:specifier] ? UIColor.systemRedColor : UIColor.secondaryLabelColor;
    if ([identifier isEqual:InstallIPA]) {
        cell.accessoryView = nil;
        if (_installer.pending) {
            UIActivityIndicatorView *progress = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
            [progress startAnimating];
            cell.accessoryView = progress;
        }
    }
    if ([identifier isEqual:SigningIdentity]) {
        cell.textLabel.numberOfLines = 1;
        cell.textLabel.lineBreakMode = NSLineBreakByTruncatingTail;
        cell.accessibilityValue = _signingSummary;
        cell.accessoryView = nil;
        if (_signingSummary.length) {
            UIButton *remove = [UIButton buttonWithType:UIButtonTypeSystem];
            remove.frame = CGRectMake(0, 0, 32, 32);
            [remove setImage:[UIImage systemImageNamed:@"xmark.circle.fill"] forState:UIControlStateNormal];
            remove.enabled = [self settingEnabled:specifier];
            remove.accessibilityIdentifier = @"signing-identity-remove";
            remove.accessibilityLabel = NSLocalizedString(@"Remove signing identity", @"Delete the imported signing identity from this app's keychain.");
            [remove addTarget:self action:@selector(confirmRemoveSigningIdentity) forControlEvents:UIControlEventTouchUpInside];
            cell.accessoryView = remove;
        }
    }
}
- (void)chooseIPA {
    if (self.presentedViewController || !_device.canControlSimulator || _installer.pending || _installer.outcomeUnknown) return;
    UTType *type = [UTType typeWithFilenameExtension:@"ipa"] ?: UTTypeData;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[type] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    _ipaPicker = picker;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)chooseSigningIdentity {
    if (self.presentedViewController || _importingIdentity || !_device.canApply) return;
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *extension in @[@"p12", @"pfx"]) {
        UTType *type = [UTType typeWithFilenameExtension:extension];
        if (type && ![types containsObject:type]) [types addObject:type];
    }
    if (!types.count) [types addObject:UTTypeData];
    // Open in place and read only: avoid leaving a second private-key document in the app's Inbox.
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (controller == _ipaPicker) {
        _ipaPicker = nil;
        [_device refresh];
        if (urls.count == 1 && _device.canControlSimulator) [_installer installIPAFromURL:urls.firstObject];
        return;
    }
    (void)controller;
    if (urls.count != 1 || _importingIdentity) return;
    NSURL *url = urls.firstObject;
    _importingIdentity = YES;
    _signingError = nil;
    [self refreshSigningIdentity];
    __weak LSBSettingsViewController *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL scoped = [url startAccessingSecurityScopedResource];
        NSError *error = nil;
        NSFileHandle *file = [NSFileHandle fileHandleForReadingFromURL:url error:&error];
        NSData *data = [file readDataUpToLength:LSBSigningPKCS12MaximumSize + 1 error:&error];
        [file closeAndReturnError:nil];
        if (scoped) [url stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            LSBSettingsViewController *self = weakSelf;
            if (!self) return;
            self->_importingIdentity = NO;
            if (error || !data.length || data.length > LSBSigningPKCS12MaximumSize) {
                self->_signingError = error.localizedDescription ?: NSLocalizedString(@"Choose a P12 or PFX file smaller than 16 MB.",
                    @"Signing import input size error.");
                [self refreshSigningIdentity];
                return;
            }
            [self refreshSigningIdentity];
            if (self.presentedViewController == controller)
                [self dismissViewControllerAnimated:YES completion:^{ [self requestSigningPasswordForData:data]; }];
            else [self requestSigningPasswordForData:data];
        });
    });
}
- (void)requestSigningPasswordForData:(NSData *)data {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:NSLocalizedString(@"Import signing identity", @"Title of the P12 password prompt.")
        message:NSLocalizedString(@"Enter the P12 password. Leave it empty if the file has no password.", @"P12 password prompt instructions.")
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.secureTextEntry = YES;
        field.textContentType = UITextContentTypePassword;
        field.placeholder = NSLocalizedString(@"Password", @"Placeholder for the P12 password field.");
        field.autocorrectionType = UITextAutocorrectionTypeNo;
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Cancel the current dialog.") style:UIAlertActionStyleCancel handler:nil]];
    __weak LSBSettingsViewController *weakSelf = self;
    __weak UIAlertController *weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Import", @"Import the selected P12 signing identity.")
        style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        (void)action;
        LSBSettingsViewController *self = weakSelf;
        NSString *password = [weakAlert.textFields.firstObject.text copy] ?: @"";
        weakAlert.textFields.firstObject.text = @"";
        if (!self || !self->_device.canApply || self->_importingIdentity) return;
        self->_importingIdentity = YES;
        [self refreshSigningIdentity];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSError *error = nil;
            BOOL imported = LSBImportSigningIdentity(data, password, &error);
            dispatch_async(dispatch_get_main_queue(), ^{
                LSBSettingsViewController *controller = weakSelf;
                if (!controller) return;
                controller->_importingIdentity = NO;
                controller->_signingError = imported ? nil : error.localizedDescription;
                [controller refreshSigningIdentity];
                if (imported) offerSigningRestart(controller, controller->_device);
            });
        });
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)confirmRemoveSigningIdentity {
    if (self.presentedViewController || _importingIdentity || !_device.canApply) return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:
        NSLocalizedString(@"Remove signing identity?", @"Confirmation before deleting the app's stored P12 identity.")
        message:NSLocalizedString(@"Lazy sign will be turned off. A running simulator keeps its current identity until restarted.",
            @"Effect of removing the stored signing identity.") preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Cancel the current dialog.") style:UIAlertActionStyleCancel handler:nil]];
    __weak LSBSettingsViewController *weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Remove", @"Confirm deletion of a stored signing identity.")
        style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        (void)action;
        LSBSettingsViewController *self = weakSelf;
        if (!self || !self->_device.canApply || self->_importingIdentity) return;
        NSError *error = nil;
        BOOL removed = LSBRemoveSigningIdentity(&error);
        self->_signingError = removed ? nil : error.localizedDescription;
        [self refreshSigningIdentity];
        if (removed) offerSigningRestart(self, self->_device);
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
@end
