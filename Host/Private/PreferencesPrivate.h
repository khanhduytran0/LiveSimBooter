#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// Minimal native Preferences ABI, checked against the supplied 23A343 runtime and 24A435 device symbols.
// Keep the real UIViewController superclass chain; PSListController owns its table instead of inheriting UITableViewController.
typedef NS_ENUM(NSInteger, PSSpecifierType) {
    PSGroupCell = 0,
    PSLinkCell = 1,
    PSLinkListCell = 2,
    PSListItemCell = 3,
    PSTitleValueCell = 4,
    PSSliderCell = 5,
    PSSwitchCell = 6,
    PSStaticTextCell = 7,
    PSEditTextCell = 8,
    PSSegmentCell = 9,
    PSGiantIconCell = 10,
    PSGiantCell = 11,
    PSSecureEditTextCell = 12,
    PSButtonCell = 13,
    PSEditTextViewCell = 14
};

FOUNDATION_EXPORT NSString *const PSEnabledKey;
FOUNDATION_EXPORT NSString *const PSFooterTextGroupKey;
FOUNDATION_EXPORT NSString *const PSIDKey;
FOUNDATION_EXPORT NSString *const PSCellClassKey;
FOUNDATION_EXPORT NSString *const PSKeyNameKey;
FOUNDATION_EXPORT NSString *const PSValueKey;
FOUNDATION_EXPORT NSString *const PSDefaultValueKey;

@interface PSSpecifier : NSObject
@property (nonatomic, strong, nullable) NSString *name;
@property (nonatomic, strong, nullable) NSString *identifier;
@property (nonatomic, weak, nullable) id target;
@property (nonatomic) PSSpecifierType cellType;
@property (nonatomic, nullable) SEL buttonAction;
@property (nonatomic, nullable) Class detailControllerClass;
@property (nonatomic, strong) NSDictionary *properties;
+ (instancetype)preferenceSpecifierNamed:(nullable NSString *)name target:(nullable id)target set:(nullable SEL)setter
                                      get:(nullable SEL)getter detail:(nullable Class)detail cell:(PSSpecifierType)cell edit:(nullable Class)edit;
+ (instancetype)emptyGroupSpecifier;
+ (instancetype)groupSpecifierWithName:(nullable NSString *)name;
- (nullable id)propertyForKey:(NSString *)key;
- (void)setProperty:(id)value forKey:(NSString *)key;
- (void)removePropertyForKey:(NSString *)key;
@end

@interface PSViewController : UIViewController
@property (nonatomic, strong, nullable) PSSpecifier *specifier;
- (nullable id)readPreferenceValue:(PSSpecifier *)specifier;
- (void)setPreferenceValue:(id)value specifier:(PSSpecifier *)specifier;
@end

@interface PSListController : PSViewController <UITableViewDelegate, UITableViewDataSource>
@property (nonatomic, readonly, nullable) UITableView *table;
@property (nonatomic, strong, nullable) NSArray<PSSpecifier *> *specifiers;
@property (nonatomic, readonly, nullable) NSArray<PSSpecifier *> *unprotectedSpecifiers;
- (instancetype)init;
- (UITableViewStyle)tableViewStyle;
- (nullable PSSpecifier *)specifierAtIndexPath:(NSIndexPath *)indexPath;
- (nullable PSSpecifier *)specifierForID:(NSString *)identifier;
- (void)reloadSpecifiers;
- (void)reloadSpecifier:(PSSpecifier *)specifier;
- (void)reloadSpecifier:(PSSpecifier *)specifier animated:(BOOL)animated;
- (void)reloadSpecifierID:(NSString *)identifier animated:(BOOL)animated;
- (void)insertSpecifier:(PSSpecifier *)specifier afterSpecifierID:(NSString *)identifier animated:(BOOL)animated;
- (void)removeSpecifier:(PSSpecifier *)specifier animated:(BOOL)animated;
- (BOOL)containsSpecifier:(PSSpecifier *)specifier;
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath;
@end

@interface PSTableCell : UITableViewCell
@property (nonatomic, strong) PSSpecifier *specifier;
- (instancetype)initWithStyle:(UITableViewCellStyle)style reuseIdentifier:(nullable NSString *)identifier specifier:(PSSpecifier *)specifier;
- (void)refreshCellContentsWithSpecifier:(PSSpecifier *)specifier;
- (void)reloadWithSpecifier:(PSSpecifier *)specifier animated:(BOOL)animated;
- (void)setCellEnabled:(BOOL)enabled;
@end

// Specifier properties: cellSubtitleText (NSString) and optional cellSubtitleColor (UIColor).
@interface PSSubtitleButtonCell : PSTableCell
@end

NS_ASSUME_NONNULL_END
