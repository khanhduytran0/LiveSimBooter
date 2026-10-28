#import "DeviceType.h"
#include <errno.h>
#include <string.h>

typedef struct {
    const char *identifier, *name, *detail, *model, *product, *idiom;
    NSUInteger width, height, scale, pitch, subtype, homeButton;
    BOOL experimental, dynamicIsland;
} LSBDeviceTypeDescriptor;

// Metadata from Apple's installed CoreSimulator DeviceTypes profile.plist and capabilities.plist; no Apple assets are embedded.
// Use capabilities.displays[deviceName=primary]. iPhone 8 Plus renders at 1242x2208, unlike its physical ScreenDimensionsCapability.
static const LSBDeviceTypeDescriptor descriptors[] = {
    {"iphone-8", "iPhone 8", "Home button · 375 × 667 points · Experimental", "iPhone10,4", "D20", "phone",
        750, 1334, 2, 326, 569, 1, YES, NO},
    {"iphone-8-plus", "iPhone 8 Plus", "Home button · 414 × 736 points · Experimental", "iPhone10,5", "D21", "phone",
        1242, 2208, 3, 461, 570, 1, YES, NO},
    {"iphone-se-2", "iPhone SE (2nd generation)", "Home button · 375 × 667 points", "iPhone12,8", "D79", "phone",
        750, 1334, 2, 326, 569, 1, NO, NO},
    {"iphone-11", "iPhone 11", "Home indicator · 414 × 896 points", "iPhone12,1", "N104", "phone",
        828, 1792, 2, 326, 1792, 2, NO, NO},
    {"iphone-15-pro-max", "iPhone 15 Pro Max", "Dynamic Island · 430 × 932 points", "iPhone16,2", "D84", "phone",
        1290, 2796, 3, 460, 2796, 2, NO, YES},
    {"ipad-9", "iPad (9th generation)", "Home button · 810 × 1080 points", "iPad12,2", "J182", "pad",
        1620, 2160, 2, 264, 2160, 0, NO, NO},
    {"ipad-a16", "iPad (A16)", "Home indicator · 820 × 1180 points", "iPad15,7", "J481", "pad",
        1640, 2360, 2, 264, 2360, 2, NO, NO},
};

@interface LSBGuestDeviceType ()
- (instancetype)initWithDescriptor:(const LSBDeviceTypeDescriptor *)descriptor;
@end

@implementation LSBGuestDeviceType
- (instancetype)initWithDescriptor:(const LSBDeviceTypeDescriptor *)descriptor {
    self = [super init];
    if (!self) return nil;
    _identifier = @(descriptor->identifier);
    _displayName = @(descriptor->name);
    _detailText = @(descriptor->detail);
    _modelIdentifier = @(descriptor->model);
    _productClass = @(descriptor->product);
    _width = descriptor->width;
    _height = descriptor->height;
    _scale = descriptor->scale;
    _pitch = descriptor->pitch;
    _experimental = descriptor->experimental;
    _hasHomeButton = descriptor->homeButton == 0 || descriptor->homeButton == 1;
    _isPad = !strcmp(descriptor->idiom, "pad");
    return self;
}
- (NSURL *)capabilitiesURLInRuntimeRoot:(NSURL *)root {
    NSURL *directory = [root URLByAppendingPathComponent:@"Library/Developer/CoreSimulator/DeviceTypes" isDirectory:YES];
    return [[directory URLByAppendingPathComponent:self.identifier isDirectory:YES] URLByAppendingPathComponent:@"capabilities.plist"];
}
@end

NSArray<LSBGuestDeviceType *> *LSBGuestDeviceTypes(void) {
    static NSArray<LSBGuestDeviceType *> *catalog;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableArray<LSBGuestDeviceType *> *items = [NSMutableArray array];
        for (unsigned i = 0; i < sizeof(descriptors) / sizeof(*descriptors); ++i)
            [items addObject:[[LSBGuestDeviceType alloc] initWithDescriptor:&descriptors[i]]];
        catalog = [items copy];
    });
    return catalog;
}

LSBGuestDeviceType *LSBGuestDeviceTypeForIdentifier(NSString *identifier) {
    if (![identifier isKindOfClass:NSString.class]) return nil;
    for (LSBGuestDeviceType *device in LSBGuestDeviceTypes()) if ([device.identifier isEqualToString:identifier]) return device;
    return nil;
}

LSBGuestDeviceType *LSBDefaultGuestDeviceType(void) {
    return LSBGuestDeviceTypeForIdentifier(@"iphone-15-pro-max");
}

static BOOL matchesNumber(id value, NSUInteger expected) {
    return [value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID() &&
        [value doubleValue] == (double)expected;
}

static BOOL optionalString(NSDictionary *dictionary, NSString *key, NSString *expected) {
    id value = dictionary[key];
    return !value || ([value isKindOfClass:NSString.class] && [value isEqualToString:expected]);
}

static BOOL optionalNumber(NSDictionary *dictionary, NSString *key, NSUInteger expected) {
    return !dictionary[key] || matchesNumber(dictionary[key], expected);
}

static BOOL optionalBoolean(NSDictionary *dictionary, NSString *key, BOOL expected) {
    id value = dictionary[key];
    return !value || ([value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() &&
        [value boolValue] == expected);
}

static BOOL validateCapabilities(NSDictionary *root, LSBGuestDeviceType *device) {
    if (![root isKindOfClass:NSDictionary.class] || ![device isKindOfClass:LSBGuestDeviceType.class]) return NO;
    NSDictionary *capabilities = root[@"capabilities"];
    if (![capabilities isKindOfClass:NSDictionary.class]) return NO;
    NSUInteger index = [LSBGuestDeviceTypes() indexOfObjectIdenticalTo:device];
    if (index == NSNotFound) return NO;
    const LSBDeviceTypeDescriptor *descriptor = &descriptors[index];
    if (!optionalString(capabilities, @"modelIdentifier", device.modelIdentifier) ||
        !optionalString(capabilities, @"productClass", device.productClass) ||
        !optionalString(capabilities, @"idiom", @(descriptor->idiom)) ||
        !optionalBoolean(capabilities, @"ipad", device.isPad) ||
        !optionalBoolean(capabilities, @"DeviceSupportsDynamicIsland", descriptor->dynamicIsland) ||
        !optionalNumber(capabilities, @"HomeButtonType", descriptor->homeButton)) return NO;
    id artwork = capabilities[@"ArtworkTraits"];
    if (artwork && (![artwork isKindOfClass:NSDictionary.class] ||
        !optionalString(artwork, @"ArtworkDeviceIdiom", @(descriptor->idiom)) ||
        !optionalNumber(artwork, @"ArtworkDeviceScaleFactor", device.scale) ||
        !optionalNumber(artwork, @"ArtworkDeviceSubType", descriptor->subtype))) return NO;
    NSArray *displays = capabilities[@"displays"];
    if (![displays isKindOfClass:NSArray.class] || displays.count > 32) return NO;
    NSDictionary *primary = nil;
    for (id display in displays) {
        if (![display isKindOfClass:NSDictionary.class]) return NO;
        id name = display[@"deviceName"];
        if (![name isKindOfClass:NSString.class]) return NO;
        if (![name isEqualToString:@"primary"]) continue;
        if (primary) return NO;
        primary = display;
    }
    return primary && matchesNumber(primary[@"width"], device.width) && matchesNumber(primary[@"height"], device.height) &&
        matchesNumber(primary[@"scale"], device.scale) && matchesNumber(primary[@"hdpi"], device.pitch) &&
        matchesNumber(primary[@"vdpi"], device.pitch);
}

int LSBValidateGuestDeviceCapabilities(NSDictionary *profile, LSBGuestDeviceType *device) {
    int savedErrno = errno;
    int result = validateCapabilities(profile, device) ? 0 : EINVAL;
    errno = savedErrno;
    return result;
}
