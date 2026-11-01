#import "SettingsChoiceControllers.h"
#import "GuestDeviceSettings.h"
#import "GuestDisplaySettings.h"

@implementation LSBDeviceTypePickerViewController {
    LSBGuestDeviceSettings *_settings;
    NSString *_draftIdentifier;
    UIBarButtonItem *_applyItem, *_progressItem;
    UIActivityIndicatorView *_progress;
    id _observer;
}
- (instancetype)init {
    if (!(self = [super init])) return nil;
    self.title = NSLocalizedString(@"Simulator Device", @"Title of the simulated device picker.");
    return self;
}
- (void)dealloc { if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer]; }
- (UITableViewStyle)tableViewStyle { return UITableViewStyleInsetGrouped; }
- (NSArray<PSSpecifier *> *)specifiers {
    NSArray<PSSpecifier *> *installed = [super unprotectedSpecifiers];
    if (installed) return installed;
    if (!_settings) _settings = LSBGuestDeviceSettings.sharedController;
    if (!_draftIdentifier.length) _draftIdentifier = [_settings.currentIdentifier copy];
    PSSpecifier *group = [PSSpecifier emptyGroupSpecifier];
    [group setProperty:@"device-options" forKey:@"id"];
    NSString *description = NSLocalizedString(@"Choose a device, then tap Apply to save its screen resolution. While running, choose Restart or Later.",
        @"Device picker instructions.");
    NSString *footer = _settings.statusText.length ? [NSString stringWithFormat:NSLocalizedString(@"%@\n\n%@",
        @"Settings footer: explanatory paragraph, then current status."), description, _settings.statusText] : description;
    [group setProperty:footer forKey:@"footerText"];
    NSMutableArray<PSSpecifier *> *specifiers = [NSMutableArray arrayWithObject:group];
    for (LSBGuestDeviceType *type in _settings.deviceTypes) {
        PSSpecifier *choice = [PSSpecifier preferenceSpecifierNamed:type.displayName target:self set:NULL get:NULL detail:Nil cell:PSButtonCell edit:Nil];
        [choice setProperty:[@"simulator-device-" stringByAppendingString:type.identifier] forKey:@"id"];
        [choice setProperty:type.identifier forKey:@"LSBDeviceIdentifier"];
        [choice setProperty:_settings.canApply ? @YES : @NO forKey:@"enabled"];
        [choice setProperty:PSSubtitleButtonCell.class forKey:PSCellClassKey];
        [choice setProperty:type.detailText forKey:@"cellSubtitleText"];
        [choice setProperty:UIColor.secondaryLabelColor forKey:@"cellSubtitleColor"];
        choice.buttonAction = @selector(selectDevice:);
        [specifiers addObject:choice];
    }
    installed = [specifiers copy];
    [super setSpecifiers:installed];
    return installed;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    if (!_settings) _settings = LSBGuestDeviceSettings.sharedController;
    if (!_draftIdentifier.length) _draftIdentifier = [_settings.currentIdentifier copy];
    self.table.rowHeight = UITableViewAutomaticDimension;
    self.table.estimatedRowHeight = 60;
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
        [self reloadSpecifiers];
    }];
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    PSSpecifier *specifier = [self specifierAtIndexPath:indexPath];
    LSBGuestDeviceType *type = LSBGuestDeviceTypeForIdentifier([specifier propertyForKey:@"LSBDeviceIdentifier"]);
    if (!type) return [super tableView:tableView cellForRowAtIndexPath:indexPath];
    // Let Preferences select its modern subtitle cell and install its layout constraints.
    UITableViewCell *cell = [super tableView:tableView cellForRowAtIndexPath:indexPath];
    cell.accessoryType = [type.identifier isEqual:_draftIdentifier] ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.userInteractionEnabled = _settings.canApply;
    cell.accessibilityIdentifier = [specifier propertyForKey:@"id"];
    cell.accessibilityHint = [type.identifier isEqual:_settings.currentIdentifier] ?
        NSLocalizedString(@"Saved device.", @"Accessibility hint for the currently saved device choice.") :
        NSLocalizedString(@"Select, then tap Apply.", @"Accessibility hint for another device choice.");
    return cell;
}
- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView; (void)indexPath;
    // PSListController otherwise uses the estimate as the actual row height, clipping multiline descriptions.
    return UITableViewAutomaticDimension;
}
- (void)selectDevice:(PSSpecifier *)specifier {
    NSString *identifier = [specifier propertyForKey:@"LSBDeviceIdentifier"];
    if (!_settings.canApply) return;
    for (LSBGuestDeviceType *type in _settings.deviceTypes) if ([type.identifier isEqual:identifier]) {
        _draftIdentifier = [identifier copy];
        [self updateState];
        return;
    }
}
- (void)applySelection {
    if (!_applyItem.enabled) return;
    BOOL saved = [_settings saveDeviceIdentifier:_draftIdentifier];
    [self updateState];
    if (saved) LSBOfferSimulatorRestart(self, _settings);
}
@end

@implementation LSBHUDSettingsViewController {
    id _observer, _deviceObserver;
}
- (instancetype)init {
    if (!(self = [super init])) return nil;
    self.title = NSLocalizedString(@"Performance HUD", @"Title of the performance heads-up display setting.");
    return self;
}
- (void)dealloc {
    if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer];
    if (_deviceObserver) [NSNotificationCenter.defaultCenter removeObserver:_deviceObserver];
}
- (UITableViewStyle)tableViewStyle { return UITableViewStyleInsetGrouped; }
- (NSArray<PSSpecifier *> *)specifiers {
    NSArray<PSSpecifier *> *installed = [super unprotectedSpecifiers];
    if (installed) return installed;
    LSBGuestDeviceSettings *device = LSBGuestDeviceSettings.sharedController;
    LSBGuestDisplaySettings *display = LSBGuestDisplaySettings.sharedController;
    PSSpecifier *group = [PSSpecifier emptyGroupSpecifier];
    [group setProperty:@"hud-options" forKey:@"id"];
    [group setProperty:device.simulatorOff ?
        NSLocalizedString(@"Start the simulator to change its live display settings.", @"Settings footer while the simulator is off.") :
        display.statusText forKey:@"footerText"];
    NSMutableArray<PSSpecifier *> *specifiers = [NSMutableArray arrayWithObject:group];
    NSArray<NSString *> *names = LSBPerformanceHUDNames();
    BOOL enabled = device.canControlSimulator && !display.pending && !display.outcomeUnknown && display.hudMode >= 0;
    for (NSUInteger index = 0; index < names.count; ++index) {
        NSString *name = [NSString stringWithFormat:NSLocalizedString(@"%ld · %@",
            @"Performance HUD row: numeric mode identifier, then localized mode name."), (long)index, names[index]];
        PSSpecifier *choice = [PSSpecifier preferenceSpecifierNamed:name target:self set:NULL get:NULL detail:Nil cell:PSButtonCell edit:Nil];
        [choice setProperty:[NSString stringWithFormat:@"simulator-hud-%lu", (unsigned long)index] forKey:@"id"];
        [choice setProperty:@(index) forKey:@"LSBHUDMode"];
        [choice setProperty:enabled ? @YES : @NO forKey:@"enabled"];
        choice.buttonAction = @selector(selectHUDMode:);
        [specifiers addObject:choice];
    }
    installed = [specifiers copy];
    [super setSpecifiers:installed];
    return installed;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    __weak LSBHUDSettingsViewController *weakSelf = self;
    _observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDisplaySettingsDidChangeNotification
        object:LSBGuestDisplaySettings.sharedController queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf reloadChoices];
        }];
    _deviceObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDeviceSettingsDidChangeNotification
        object:LSBGuestDeviceSettings.sharedController queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf reloadChoices];
        }];
}
- (void)reloadChoices {
    [UIView performWithoutAnimation:^{ [self reloadSpecifiers]; }];
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [super tableView:tableView cellForRowAtIndexPath:indexPath];
    PSSpecifier *specifier = [self specifierAtIndexPath:indexPath];
    LSBGuestDisplaySettings *settings = LSBGuestDisplaySettings.sharedController;
    cell.textLabel.textColor = UIColor.labelColor;
    cell.accessoryType = settings.hudMode == [[specifier propertyForKey:@"LSBHUDMode"] integerValue] ?
        UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.userInteractionEnabled = LSBGuestDeviceSettings.sharedController.canControlSimulator &&
        !settings.pending && !settings.outcomeUnknown && settings.hudMode >= 0;
    cell.textLabel.enabled = cell.userInteractionEnabled;
    cell.accessibilityIdentifier = [specifier propertyForKey:@"id"];
    return cell;
}
- (void)selectHUDMode:(PSSpecifier *)specifier {
    LSBGuestDisplaySettings *settings = LSBGuestDisplaySettings.sharedController;
    if (!LSBGuestDeviceSettings.sharedController.canControlSimulator || settings.pending || settings.outcomeUnknown || settings.hudMode < 0) return;
    NSNumber *value = [specifier propertyForKey:@"LSBHUDMode"];
    if (![value isKindOfClass:NSNumber.class] || value.integerValue < 0 || (NSUInteger)value.integerValue >= LSBPerformanceHUDNames().count) return;
    [settings setHUDMode:value.integerValue];
    [self.navigationController popViewControllerAnimated:YES];
}
@end
