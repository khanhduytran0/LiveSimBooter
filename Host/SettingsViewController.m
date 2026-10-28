#import "SettingsViewController.h"
#import "BackgroundAudio.h"
#import "GuestDisplaySettings.h"
#import "GuestDeviceSettings.h"

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
    self.title = @"Simulator Device";
    return self;
}
- (void)dealloc { if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer]; }
- (void)viewDidLoad {
    [super viewDidLoad];
    _settings = LSBGuestDeviceSettings.sharedController;
    _draftIdentifier = [_settings.currentIdentifier copy];
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 60;
    _applyItem = [[UIBarButtonItem alloc] initWithTitle:@"Apply" style:UIBarButtonItemStyleProminent target:self action:@selector(applySelection)];
    _applyItem.accessibilityIdentifier = @"simulator-device-apply";
    _applyItem.accessibilityHint = @"Apply the selected device and restart the simulator.";
    _progress = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _progress.isAccessibilityElement = YES;
    _progress.accessibilityLabel = @"Applying simulator configuration";
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
    _applyItem.enabled = validSelection && ![_draftIdentifier isEqual:_settings.currentIdentifier] &&
        _settings.canApply && !_settings.pending && !_settings.outcomeUnknown;
    if (_settings.pending) {
        [_progress startAnimating];
        self.navigationItem.rightBarButtonItems = @[_applyItem, _progressItem];
    } else {
        [_progress stopAnimating];
        self.navigationItem.rightBarButtonItems = @[_applyItem];
    }
    [self.tableView reloadData];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return _settings.deviceTypes.count;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    NSString *description = @"Choose a device, then tap Apply to change its screen resolution and restart the simulator. Running apps will close.";
    return _settings.statusText.length ? [description stringByAppendingFormat:@"\n\n%@", _settings.statusText] : description;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    LSBGuestDeviceType *type = _settings.deviceTypes[indexPath.row];
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.text = type.displayName;
    cell.detailTextLabel.text = type.detailText;
    cell.textLabel.numberOfLines = cell.detailTextLabel.numberOfLines = 0;
    cell.accessoryType = [type.identifier isEqual:_draftIdentifier] ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.userInteractionEnabled = !_settings.pending && !_settings.outcomeUnknown;
    cell.textLabel.enabled = cell.detailTextLabel.enabled = cell.userInteractionEnabled;
    cell.accessibilityIdentifier = [@"simulator-device-" stringByAppendingString:type.identifier];
    cell.accessibilityHint = [type.identifier isEqual:_settings.currentIdentifier] ? @"Current device." : @"Select, then tap Apply.";
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (_settings.pending || _settings.outcomeUnknown || (NSUInteger)indexPath.row >= _settings.deviceTypes.count) return;
    _draftIdentifier = [_settings.deviceTypes[indexPath.row].identifier copy];
    [self updateState];
}
- (void)applySelection {
    if (!_applyItem.enabled) return;
    [_settings applyDeviceIdentifier:_draftIdentifier];
    [self updateState];
}
@end

@interface LSBHUDSettingsViewController : UITableViewController
@end
@implementation LSBHUDSettingsViewController {
    id _observer, _deviceObserver;
}
- (instancetype)init {
    if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) return nil;
    self.title = @"Performance HUD";
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
            [weakSelf.tableView reloadData];
        }];
    _deviceObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification
        object:LSBGuestDeviceSettings.sharedController queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf.tableView reloadData];
        }];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return LSBPerformanceHUDNames().count;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return LSBGuestDisplaySettings.sharedController.statusText;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    LSBGuestDisplaySettings *settings = LSBGuestDisplaySettings.sharedController;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.textLabel.text = [NSString stringWithFormat:@"%ld · %@", (long)indexPath.row, LSBPerformanceHUDNames()[indexPath.row]];
    cell.accessoryType = settings.hudMode == indexPath.row ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.userInteractionEnabled = !LSBGuestDeviceSettings.sharedController.pending && !settings.pending && !settings.outcomeUnknown && settings.hudMode >= 0;
    cell.textLabel.enabled = cell.userInteractionEnabled;
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (LSBGuestDeviceSettings.sharedController.pending) return;
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
    self.title = @"Settings";
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
    self.navigationItem.rightBarButtonItem.accessibilityLabel = @"Refresh simulator settings";
    __weak LSBSettingsViewController *weakSelf = self;
    _audioObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBBackgroundAudioDidChangeNotification object:_audio
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf.tableView reloadData];
        }];
    _displayObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDisplaySettingsDidChangeNotification object:_display
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            LSBSettingsViewController *controller = weakSelf;
            if (!controller) return;
            controller.navigationItem.rightBarButtonItem.enabled = !controller->_display.pending && !controller->_device.pending;
            [controller.tableView reloadData];
        }];
    _deviceObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification object:_device
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            LSBSettingsViewController *controller = weakSelf;
            if (!controller) return;
            controller.navigationItem.rightBarButtonItem.enabled = !controller->_display.pending && !controller->_device.pending;
            [controller.tableView reloadData];
        }];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshDisplay];
}
- (void)refreshDisplay {
    [_device refresh];
    if (!_device.pending) [_display refresh];
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return 5; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    if (section == 2) {
        LSBGuestDeviceType *type = LSBGuestDeviceTypeForIdentifier(_device.currentIdentifier);
        return type.hasHomeButton || type.isPad ? 2 : 1;
    }
    return section == 1 ? 2 : 1;
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView;
    return section == 0 ? @"Background execution" : section == 1 ? @"Simulator display" : section == 2 ? @"Simulator device" :
        section == 3 ? @"Experimental haptics" : @"Experimental audio";
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    if (section == 4) return _device.audioOutputForced ? @"Enabled by the host launch environment." : _device.audioOutputStatusText;
    if (section == 3) return _device.hapticsForced ? @"Enabled by the host launch environment." : _device.hapticsStatusText;
    if (section == 1) return _display.statusText;
    if (section == 2) {
        LSBGuestDeviceType *type = LSBGuestDeviceTypeForIdentifier(_device.currentIdentifier);
        if (!type.hasHomeButton && !type.isPad) return _device.statusText;
        return [_device.statusText stringByAppendingString:@"\n\nFull width preserves the screen’s aspect ratio and fits within the available height."];
    }
    NSString *description = @"Play silence to keep the host's audio session active in the background. Off by default. "
        "This may increase battery use. Other audio can continue playing.";
    NSString *status = _audio.statusText;
    if (![status isEqualToString:@"Off"] && ![status isEqualToString:@"Playing silence"])
        return [description stringByAppendingFormat:@"\n\n%@", status];
    return description;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    if (indexPath.section == 3 || indexPath.section == 4) {
        BOOL audioOutput = indexPath.section == 4;
        cell.textLabel.text = audioOutput ? @"Audio output" : @"Haptic passthrough";
        UISwitch *toggle = [UISwitch new];
        toggle.on = audioOutput ? _device.audioOutputEnabled : _device.hapticsEnabled;
        toggle.enabled = _device.canApply && !(audioOutput ? _device.audioOutputForced : _device.hapticsForced) && !_display.pending && !_display.outcomeUnknown;
        toggle.accessibilityLabel = cell.textLabel.text;
        toggle.accessibilityIdentifier = audioOutput ? @"simulator-audio-output" : @"simulator-haptic-passthrough";
        toggle.accessibilityHint = audioOutput ? @"Restart the simulator to apply audio output." : @"Restart the simulator to apply haptic passthrough.";
        cell.textLabel.enabled = toggle.enabled;
        __weak LSBSettingsViewController *weakSelf = self;
        [toggle addAction:[UIAction actionWithHandler:^(UIAction *action) {
            LSBSettingsViewController *controller = weakSelf;
            if (!controller) return;
            UISwitch *sender = (UISwitch *)action.sender;
            BOOL requested = sender.on;
            sender.on = audioOutput ? controller->_device.audioOutputEnabled : controller->_device.hapticsEnabled;
            if (audioOutput) [controller->_device applyAudioOutputEnabled:requested];
            else [controller->_device applyHapticsEnabled:requested];
        }] forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = toggle;
        return cell;
    }
    if (indexPath.section == 2 && indexPath.row == 1) {
        cell.textLabel.text = @"Use Full Width";
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
        cell.textLabel.text = @"Device";
        cell.detailTextLabel.text = @"Choose…";
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
        cell.textLabel.text = @"Performance HUD";
        cell.detailTextLabel.text = _display.hudMode >= 0 ? LSBPerformanceHUDNames()[_display.hudMode] : @"Unavailable";
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        cell.userInteractionEnabled = !_device.pending && !_display.pending && !_display.outcomeUnknown && _display.hudMode >= 0;
        cell.textLabel.enabled = cell.userInteractionEnabled;
        return cell;
    }
    BOOL audio = indexPath.section == 0;
    cell.textLabel.text = audio ? @"Silent background audio" : @"Visualize touches";
    UISwitch *toggle = [UISwitch new];
    toggle.on = audio ? _audio.enabled : _display.touches == 1;
    toggle.enabled = audio || (!_device.pending && !_display.pending && !_display.outcomeUnknown && _display.touches >= 0);
    cell.textLabel.enabled = toggle.enabled;
    if (!audio && _display.touches < 0) cell.detailTextLabel.text = @"Unavailable";
    toggle.accessibilityLabel = cell.textLabel.text;
    __weak LSBSettingsViewController *weakSelf = self;
    [toggle addAction:[UIAction actionWithHandler:^(UIAction *action) {
        LSBSettingsViewController *controller = weakSelf;
        if (!controller) return;
        UISwitch *sender = (UISwitch *)action.sender;
        if (audio) [controller->_audio setEnabled:sender.on];
        else {
            if (controller->_device.pending) { sender.on = controller->_display.touches == 1; return; }
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
    if (indexPath.section == 1 && indexPath.row == 0 && !_device.pending)
        [self.navigationController pushViewController:[LSBHUDSettingsViewController new] animated:YES];
    else if (indexPath.section == 2 && indexPath.row == 0)
        [self.navigationController pushViewController:[LSBDeviceTypePickerViewController new] animated:YES];
}
@end
