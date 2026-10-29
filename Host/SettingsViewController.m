#import "SettingsViewController.h"
#import "BackgroundAudio.h"
#import "GuestDisplaySettings.h"
#import "GuestDeviceSettings.h"

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

@implementation LSBSettingsViewController {
    LSBBackgroundAudio *_audio;
    LSBGuestDisplaySettings *_display;
    LSBGuestDeviceSettings *_device;
    id _audioObserver, _displayObserver, _deviceObserver;
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
                reloadSettingsTable(controller.tableView, [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(1, 5)]);
            }];
        }];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshDisplay];
}
- (void)refreshDisplay {
    [_device refresh];
    if (_device.canControlSimulator) [_display refresh];
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return 6; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
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
    return NSLocalizedString(@"Simulator session", @"Settings section heading for Power Off and Restart.");
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
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
    if (indexPath.section == 5 && _device.canControlSimulator) {
        if (indexPath.row == 0) [_device powerOffSimulator];
        else [_device restartSimulator];
    } else if (indexPath.section == 1 && indexPath.row == 0 && _device.canControlSimulator)
        [self.navigationController pushViewController:[LSBHUDSettingsViewController new] animated:YES];
    else if (indexPath.section == 2 && indexPath.row == 0)
        [self.navigationController pushViewController:[LSBDeviceTypePickerViewController new] animated:YES];
}
@end
