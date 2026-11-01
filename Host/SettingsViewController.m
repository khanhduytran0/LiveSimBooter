#import "SettingsViewController.h"
#import "BackgroundAudio.h"
#import "GuestDisplaySettings.h"
#import "GuestDeviceSettings.h"
#import "SigningIdentity.h"
#import "Broker.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <string.h>

static void reloadSettingsTable(UITableView *table, NSIndexSet *sections) {
    [UIView performWithoutAnimation:^{
        if (sections) [table reloadSections:sections withRowAnimation:UITableViewRowAnimationNone];
        else [table reloadData];
        [table layoutIfNeeded];
    }];
}

static void offerSimulatorRestart(UIViewController *controller, LSBGuestDeviceSettings *settings) {
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
        [controller dismissViewControllerAnimated:YES completion:^{ offerSimulatorRestart(controller, settings); }];
    else offerSimulatorRestart(controller, settings);
}

@interface LSBDeviceTypePickerViewController : UITableViewController
@end
@implementation LSBDeviceTypePickerViewController {
    LSBGuestDeviceSettings *_settings;
    NSString *_draftIdentifier;
    UIBarButtonItem *_applyItem, *_progressItem;
    UIActivityIndicatorView *_progress;
    id _observer;
}
- (instancetype)init {
    if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) return nil;
    self.title = NSLocalizedString(@"Simulator Device", @"Title of the simulated device picker.");
    return self;
}
- (void)dealloc { if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer]; }
- (void)viewDidLoad {
    [super viewDidLoad];
    _settings = LSBGuestDeviceSettings.sharedController;
    _draftIdentifier = [_settings.currentIdentifier copy];
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 60;
    _applyItem = [[UIBarButtonItem alloc] initWithTitle:NSLocalizedString(@"Apply", @"Save the selected simulated device.")
        style:UIBarButtonItemStyleProminent target:self action:@selector(applySelection)];
    _applyItem.accessibilityIdentifier = @"simulator-device-apply";
    _applyItem.accessibilityHint = NSLocalizedString(@"Save the selected device, then choose whether to restart now or later.",
        @"Accessibility hint for the device picker Apply button.");
    _progress = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _progress.isAccessibilityElement = YES;
    _progress.accessibilityLabel = NSLocalizedString(@"Applying simulator configuration", @"Accessibility label for the device settings progress indicator.");
    _progressItem = [[UIBarButtonItem alloc] initWithCustomView:_progress];
    __weak LSBDeviceTypePickerViewController *weakSelf = self;
    _observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification object:_settings
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf updateState];
        }];
    [self updateState];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [_settings refresh];
}
- (void)updateState {
    if (!_draftIdentifier.length) _draftIdentifier = [_settings.currentIdentifier copy];
    BOOL validSelection = NO;
    for (LSBGuestDeviceType *type in _settings.deviceTypes)
        if ([type.identifier isEqual:_draftIdentifier]) { validSelection = YES; break; }
    [UIView performWithoutAnimation:^{
        self->_applyItem.enabled = validSelection && ![self->_draftIdentifier isEqual:self->_settings.currentIdentifier] && self->_settings.canApply;
        if (self->_settings.pending) {
            [self->_progress startAnimating];
            self.navigationItem.rightBarButtonItems = @[self->_applyItem, self->_progressItem];
        } else {
            [self->_progress stopAnimating];
            self.navigationItem.rightBarButtonItems = @[self->_applyItem];
        }
        reloadSettingsTable(self.tableView, nil);
    }];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return _settings.deviceTypes.count;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    NSString *description = NSLocalizedString(@"Choose a device, then tap Apply to save its screen resolution. While running, choose Restart or Later.",
        @"Device picker instructions.");
    return _settings.statusText.length ? [NSString stringWithFormat:NSLocalizedString(@"%@\n\n%@",
        @"Settings footer: explanatory paragraph, then current status."), description, _settings.statusText] : description;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    LSBGuestDeviceType *type = _settings.deviceTypes[indexPath.row];
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.text = type.displayName;
    cell.detailTextLabel.text = type.detailText;
    cell.textLabel.numberOfLines = cell.detailTextLabel.numberOfLines = 0;
    cell.accessoryType = [type.identifier isEqual:_draftIdentifier] ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.userInteractionEnabled = _settings.canApply;
    cell.textLabel.enabled = cell.detailTextLabel.enabled = cell.userInteractionEnabled;
    cell.accessibilityIdentifier = [@"simulator-device-" stringByAppendingString:type.identifier];
    cell.accessibilityHint = [type.identifier isEqual:_settings.currentIdentifier] ?
        NSLocalizedString(@"Saved device.", @"Accessibility hint for the currently saved device choice.") :
        NSLocalizedString(@"Select, then tap Apply.", @"Accessibility hint for another device choice.");
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (!_settings.canApply || (NSUInteger)indexPath.row >= _settings.deviceTypes.count) return;
    _draftIdentifier = [_settings.deviceTypes[indexPath.row].identifier copy];
    [self updateState];
}
- (void)applySelection {
    if (!_applyItem.enabled) return;
    BOOL saved = [_settings saveDeviceIdentifier:_draftIdentifier];
    [self updateState];
    if (saved) offerSimulatorRestart(self, _settings);
}
@end

@interface LSBHUDSettingsViewController : UITableViewController
@end
@implementation LSBHUDSettingsViewController {
    id _observer, _deviceObserver;
}
- (instancetype)init {
    if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) return nil;
    self.title = NSLocalizedString(@"Performance HUD", @"Title of the performance heads-up display setting.");
    return self;
}
- (void)dealloc {
    if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer];
    if (_deviceObserver) [NSNotificationCenter.defaultCenter removeObserver:_deviceObserver];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    __weak LSBHUDSettingsViewController *weakSelf = self;
    _observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDisplaySettingsDidChangeNotification
        object:LSBGuestDisplaySettings.sharedController queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            reloadSettingsTable(weakSelf.tableView, nil);
        }];
    _deviceObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification
        object:LSBGuestDeviceSettings.sharedController queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            reloadSettingsTable(weakSelf.tableView, nil);
        }];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return LSBPerformanceHUDNames().count;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return LSBGuestDeviceSettings.sharedController.simulatorOff ?
        NSLocalizedString(@"Start the simulator to change its live display settings.", @"Settings footer while the simulator is off.") :
        LSBGuestDisplaySettings.sharedController.statusText;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    LSBGuestDisplaySettings *settings = LSBGuestDisplaySettings.sharedController;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.textLabel.text = [NSString stringWithFormat:NSLocalizedString(@"%ld · %@", @"Performance HUD row: numeric mode identifier, then localized mode name."),
        (long)indexPath.row, LSBPerformanceHUDNames()[indexPath.row]];
    cell.accessoryType = settings.hudMode == indexPath.row ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.userInteractionEnabled = LSBGuestDeviceSettings.sharedController.canControlSimulator &&
        !settings.pending && !settings.outcomeUnknown && settings.hudMode >= 0;
    cell.textLabel.enabled = cell.userInteractionEnabled;
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (!LSBGuestDeviceSettings.sharedController.canControlSimulator) return;
    [LSBGuestDisplaySettings.sharedController setHUDMode:indexPath.row];
    [self.navigationController popViewControllerAnimated:YES];
}
@end

@interface LSBSettingsViewController () <UIDocumentPickerDelegate>
@end

@implementation LSBSettingsViewController {
    LSBBackgroundAudio *_audio;
    LSBGuestDisplaySettings *_display;
    LSBGuestDeviceSettings *_device;
    id _audioObserver, _displayObserver, _deviceObserver, _signingObserver;
    NSString *_signingSummary, *_signingError;
    BOOL _importingIdentity;
}
- (instancetype)init {
    if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) return nil;
    self.title = NSLocalizedString(@"Settings", @"Title of the native Settings tab.");
    return self;
}
- (void)dealloc {
    if (_audioObserver) [NSNotificationCenter.defaultCenter removeObserver:_audioObserver];
    if (_displayObserver) [NSNotificationCenter.defaultCenter removeObserver:_displayObserver];
    if (_deviceObserver) [NSNotificationCenter.defaultCenter removeObserver:_deviceObserver];
    if (_signingObserver) [NSNotificationCenter.defaultCenter removeObserver:_signingObserver];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    _audio = LSBBackgroundAudio.sharedController;
    _display = LSBGuestDisplaySettings.sharedController;
    _device = LSBGuestDeviceSettings.sharedController;
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 54;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
        target:self action:@selector(refreshDisplay)];
    self.navigationItem.rightBarButtonItem.accessibilityLabel =
        NSLocalizedString(@"Refresh simulator settings", @"Accessibility label for the Settings refresh button.");
    __weak LSBSettingsViewController *weakSelf = self;
    _audioObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBBackgroundAudioDidChangeNotification object:_audio
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            reloadSettingsTable(weakSelf.tableView, [NSIndexSet indexSetWithIndex:0]);
        }];
    _displayObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDisplaySettingsDidChangeNotification object:_display
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            LSBSettingsViewController *controller = weakSelf;
            if (!controller) return;
            [UIView performWithoutAnimation:^{
                controller.navigationItem.rightBarButtonItem.enabled = !controller->_display.pending && !controller->_device.pending;
                reloadSettingsTable(controller.tableView, [NSIndexSet indexSetWithIndex:1]);
            }];
        }];
    _deviceObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification object:_device
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            LSBSettingsViewController *controller = weakSelf;
            if (!controller) return;
            [UIView performWithoutAnimation:^{
                controller.navigationItem.rightBarButtonItem.enabled = !controller->_display.pending && !controller->_device.pending;
                reloadSettingsTable(controller.tableView, [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(1, 6)]);
            }];
        }];
    _signingObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBSigningIdentityDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf refreshSigningIdentity];
        }];
    [self refreshSigningIdentity];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshDisplay];
}
- (void)refreshDisplay {
    [_device refresh];
    if (_device.canControlSimulator) [_display refresh];
    [self refreshSigningIdentity];
}
- (void)refreshSigningIdentity {
    _signingSummary = LSBSigningIdentitySummary();
    [_device refresh];
    reloadSettingsTable(self.tableView, [NSIndexSet indexSetWithIndex:6]);
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return 7; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    if (section == 6) return 2;
    if (section == 2) {
        LSBGuestDeviceType *type = LSBGuestDeviceTypeForIdentifier(_device.currentIdentifier);
        return type.hasHomeButton || type.isPad ? 2 : 1;
    }
    return section == 1 || section == 5 ? 2 : 1;
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView;
    if (section == 0) return NSLocalizedString(@"Background execution", @"Settings section heading for native background audio.");
    if (section == 1) return NSLocalizedString(@"Simulator display", @"Settings section heading for live display controls.");
    if (section == 2) return NSLocalizedString(@"Simulator device", @"Settings section heading for device selection and display sizing.");
    if (section == 3) return NSLocalizedString(@"Experimental haptics", @"Settings section heading for optional haptic passthrough.");
    if (section == 4) return NSLocalizedString(@"Experimental audio", @"Settings section heading for optional simulator audio output.");
    if (section == 6) return NSLocalizedString(@"Code signing", @"Settings section heading for lazy runtime code signing.");
    return NSLocalizedString(@"Simulator session", @"Settings section heading for Power Off and Restart.");
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    if (section == 6) {
        NSString *description = NSLocalizedString(@"Off by default. Sign a runtime binary only when its signature registration fails. "
            @"The P12 and password stay in this app's device-only keychain. Changes apply on the next simulator start or restart.",
            @"Description of lazy signing and where its credentials are stored.");
        return _signingError.length ? [NSString stringWithFormat:NSLocalizedString(@"%@\n\n%@",
            @"Settings footer: explanatory paragraph, then current status."), description, _signingError] : description;
    }
    if (section == 5) return _device.sessionStatusText;
    if (section == 4) return _device.audioOutputForced ? NSLocalizedString(@"Enabled by the host launch environment.",
        @"Settings footer when a feature is forced on by the native host environment.") : _device.audioOutputStatusText;
    if (section == 3) return _device.hapticsForced ? NSLocalizedString(@"Enabled by the host launch environment.",
        @"Settings footer when a feature is forced on by the native host environment.") : _device.hapticsStatusText;
    if (section == 1) return _device.simulatorOff ?
        NSLocalizedString(@"Start the simulator to change its live display settings.", @"Settings footer while the simulator is off.") : _display.statusText;
    if (section == 2) {
        LSBGuestDeviceType *type = LSBGuestDeviceTypeForIdentifier(_device.currentIdentifier);
        if (!type.hasHomeButton && !type.isPad) return _device.statusText;
        return [NSString stringWithFormat:NSLocalizedString(@"%@\n\nFull width preserves the screen’s aspect ratio and fits within the available height.",
            @"Device settings footer: current status followed by the Full Width explanation."), _device.statusText];
    }
    NSString *description = NSLocalizedString(@"Play silence to keep the host's audio session active in the background. Off by default. "
        @"This may increase battery use. Other audio can continue playing.",
        @"Description of native silent background audio, independent of simulator audio output.");
    NSString *status = _audio.statusDetail;
    if (status.length) return [NSString stringWithFormat:NSLocalizedString(@"%@\n\n%@",
        @"Settings footer: explanatory paragraph, then current status."), description, status];
    return description;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    if (indexPath.section == 6) {
        if (indexPath.row == 0) {
            cell.textLabel.text = NSLocalizedString(@"Lazy sign", @"Switch to sign runtime binaries on signature-registration failure.");
            UISwitch *toggle = [UISwitch new];
            toggle.on = LSBLazySigningEnabled();
            toggle.enabled = _device.canApply && !_importingIdentity && (toggle.on || _signingSummary.length);
            toggle.accessibilityLabel = cell.textLabel.text;
            toggle.accessibilityIdentifier = @"simulator-lazy-sign";
            toggle.accessibilityHint = NSLocalizedString(@"Import a P12 signing identity first. Save this choice, then restart now or later.",
                @"Accessibility hint for enabling lazy signing.");
            cell.textLabel.enabled = toggle.enabled;
            __weak LSBSettingsViewController *weakSelf = self;
            [toggle addAction:[UIAction actionWithHandler:^(UIAction *action) {
                LSBSettingsViewController *controller = weakSelf;
                if (!controller) return;
                UISwitch *sender = (UISwitch *)action.sender;
                BOOL requested = sender.on;
                sender.on = LSBLazySigningEnabled();
                int error = LSBHostSaveLazySigningEnabled(requested);
                controller->_signingError = error ? [NSString stringWithFormat:
                    NSLocalizedString(@"Could not save lazy signing: %s (%d).", @"Lazy signing settings error: description and numeric code."),
                    strerror(error), error] : nil;
                [controller refreshSigningIdentity];
                if (!error) offerSimulatorRestart(controller, controller->_device);
            }] forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = toggle;
        } else {
            cell.textLabel.text = _importingIdentity ? NSLocalizedString(@"Importing…", @"P12 import is in progress.") :
                _signingSummary.length ? NSLocalizedString(@"Imported", @"A P12 signing identity has been imported; tap to replace it.") :
                NSLocalizedString(@"Import P12…", @"Choose a PKCS12 signing identity document.");
            cell.textLabel.numberOfLines = 1;
            cell.textLabel.lineBreakMode = NSLineBreakByTruncatingTail;
            cell.userInteractionEnabled = _device.canApply && !_importingIdentity;
            cell.textLabel.enabled = cell.userInteractionEnabled;
            cell.textLabel.textColor = cell.userInteractionEnabled ? self.view.tintColor : UIColor.secondaryLabelColor;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            cell.accessibilityIdentifier = @"signing-identity-import";
            if (_signingSummary.length) {
                cell.accessibilityValue = _signingSummary;
                UIButton *remove = [UIButton buttonWithType:UIButtonTypeSystem];
                remove.frame = CGRectMake(0, 0, 32, 32);
                [remove setImage:[UIImage systemImageNamed:@"xmark.circle.fill"] forState:UIControlStateNormal];
                remove.enabled = cell.userInteractionEnabled;
                remove.accessibilityIdentifier = @"signing-identity-remove";
                remove.accessibilityLabel = NSLocalizedString(@"Remove signing identity", @"Delete the imported signing identity from this app's keychain.");
                __weak LSBSettingsViewController *weakSelf = self;
                [remove addAction:[UIAction actionWithHandler:^(UIAction *action) {
                    (void)action;
                    [weakSelf confirmRemoveSigningIdentity];
                }] forControlEvents:UIControlEventTouchUpInside];
                cell.accessoryView = remove;
            }
        }
        return cell;
    }
    if (indexPath.section == 5) {
        BOOL powerOff = indexPath.row == 0;
        cell.textLabel.text = powerOff ? NSLocalizedString(@"Power Off", @"Stop the simulator session without deleting its data.") :
            NSLocalizedString(@"Restart", @"Restart the simulator session.");
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        cell.userInteractionEnabled = _device.canControlSimulator;
        cell.textLabel.enabled = cell.userInteractionEnabled;
        cell.textLabel.textColor = cell.userInteractionEnabled ? (powerOff ? UIColor.systemRedColor : self.view.tintColor) : UIColor.secondaryLabelColor;
        cell.accessibilityIdentifier = powerOff ? @"simulator-power-off" : @"simulator-restart";
        return cell;
    }
    if (indexPath.section == 3 || indexPath.section == 4) {
        BOOL audioOutput = indexPath.section == 4;
        cell.textLabel.text = audioOutput ? NSLocalizedString(@"Audio output", @"Switch for simulator audio playback on the physical device.") :
            NSLocalizedString(@"Haptic passthrough", @"Switch for simulator haptic feedback on the physical device.");
        UISwitch *toggle = [UISwitch new];
        toggle.on = audioOutput ? _device.audioOutputEnabled : _device.hapticsEnabled;
        toggle.enabled = _device.canApply && !(audioOutput ? _device.audioOutputForced : _device.hapticsForced);
        toggle.accessibilityLabel = cell.textLabel.text;
        toggle.accessibilityIdentifier = audioOutput ? @"simulator-audio-output" : @"simulator-haptic-passthrough";
        toggle.accessibilityHint = NSLocalizedString(@"Save this choice; restart now or later if the simulator is running.",
            @"Accessibility hint for optional simulator features.");
        cell.textLabel.enabled = toggle.enabled;
        __weak LSBSettingsViewController *weakSelf = self;
        [toggle addAction:[UIAction actionWithHandler:^(UIAction *action) {
            LSBSettingsViewController *controller = weakSelf;
            if (!controller) return;
            UISwitch *sender = (UISwitch *)action.sender;
            BOOL requested = sender.on;
            sender.on = audioOutput ? controller->_device.audioOutputEnabled : controller->_device.hapticsEnabled;
            BOOL saved = audioOutput ? [controller->_device saveAudioOutputEnabled:requested] : [controller->_device saveHapticsEnabled:requested];
            if (saved) offerSimulatorRestart(controller, controller->_device);
        }] forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = toggle;
        return cell;
    }
    if (indexPath.section == 2 && indexPath.row == 1) {
        cell.textLabel.text = NSLocalizedString(@"Use Full Width", @"Switch to expand the simulator display within the available view.");
        UISwitch *toggle = [UISwitch new];
        toggle.on = _device.fullWidth;
        toggle.accessibilityLabel = cell.textLabel.text;
        toggle.accessibilityIdentifier = @"simulator-full-width";
        __weak LSBSettingsViewController *weakSelf = self;
        [toggle addAction:[UIAction actionWithHandler:^(UIAction *action) {
            LSBSettingsViewController *controller = weakSelf;
            if (controller) [controller->_device setFullWidth:((UISwitch *)action.sender).on];
        }] forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = toggle;
        return cell;
    }
    if (indexPath.section == 2) {
        cell.textLabel.text = NSLocalizedString(@"Device", @"Settings row that opens the simulated device picker.");
        cell.detailTextLabel.text = NSLocalizedString(@"Choose…", @"Placeholder when no simulated device name is available.");
        for (LSBGuestDeviceType *type in _device.deviceTypes) if ([type.identifier isEqual:_device.currentIdentifier]) {
            cell.detailTextLabel.text = type.displayName;
            break;
        }
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        cell.accessibilityIdentifier = @"simulator-device-picker";
        return cell;
    }
    if (indexPath.section == 1 && indexPath.row == 0) {
        cell.textLabel.text = NSLocalizedString(@"Performance HUD", @"Title of the performance heads-up display setting.");
        cell.detailTextLabel.text = _display.hudMode >= 0 ? LSBPerformanceHUDNames()[_display.hudMode] :
            NSLocalizedString(@"Unavailable", @"A simulator display setting could not be read.");
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        cell.userInteractionEnabled = _device.canControlSimulator && !_display.pending && !_display.outcomeUnknown && _display.hudMode >= 0;
        cell.textLabel.enabled = cell.userInteractionEnabled;
        return cell;
    }
    BOOL audio = indexPath.section == 0;
    cell.textLabel.text = audio ? NSLocalizedString(@"Silent background audio", @"Switch to keep the native host active by playing silence.") :
        NSLocalizedString(@"Visualize touches", @"Switch to show simulator touch markers.");
    UISwitch *toggle = [UISwitch new];
    toggle.on = audio ? _audio.enabled : _display.touches == 1;
    toggle.enabled = audio || (_device.canControlSimulator && !_display.pending && !_display.outcomeUnknown && _display.touches >= 0);
    cell.textLabel.enabled = toggle.enabled;
    if (!audio && _display.touches < 0) cell.detailTextLabel.text = NSLocalizedString(@"Unavailable", @"A simulator display setting could not be read.");
    toggle.accessibilityLabel = cell.textLabel.text;
    __weak LSBSettingsViewController *weakSelf = self;
    [toggle addAction:[UIAction actionWithHandler:^(UIAction *action) {
        LSBSettingsViewController *controller = weakSelf;
        if (!controller) return;
        UISwitch *sender = (UISwitch *)action.sender;
        if (audio) [controller->_audio setEnabled:sender.on];
        else {
            if (!controller->_device.canControlSimulator) { sender.on = controller->_display.touches == 1; return; }
            BOOL requested = sender.on;
            sender.on = controller->_display.touches == 1; // Show only confirmed readback while the request is pending.
            [controller->_display setVisualizeTouches:requested];
        }
    }] forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 6 && _device.canApply && !_importingIdentity) {
        if (indexPath.row == 1) [self chooseSigningIdentity];
        return;
    }
    if (indexPath.section == 5 && _device.canControlSimulator) {
        if (indexPath.row == 0) [_device powerOffSimulator];
        else [_device restartSimulator];
    } else if (indexPath.section == 1 && indexPath.row == 0 && _device.canControlSimulator)
        [self.navigationController pushViewController:[LSBHUDSettingsViewController new] animated:YES];
    else if (indexPath.section == 2 && indexPath.row == 0)
        [self.navigationController pushViewController:[LSBDeviceTypePickerViewController new] animated:YES];
}
- (void)chooseSigningIdentity {
    if (self.presentedViewController) return;
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
        if (!self || !self->_device.canApply) return;
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
    if (self.presentedViewController) return;
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
        if (!self || !self->_device.canApply) return;
        NSError *error = nil;
        BOOL removed = LSBRemoveSigningIdentity(&error);
        self->_signingError = removed ? nil : error.localizedDescription;
        [self refreshSigningIdentity];
        if (removed) offerSigningRestart(self, self->_device);
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
@end
