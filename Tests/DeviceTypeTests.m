#import <Foundation/Foundation.h>
#import "../Host/DeviceType.h"
#include <assert.h>
#include <errno.h>

static NSMutableDictionary *capabilities(LSBGuestDeviceType *device) {
    return [@{@"capabilities": [@{
        @"modelIdentifier": device.modelIdentifier, @"productClass": device.productClass,
        @"displays": [NSMutableArray arrayWithObject:[@{@"deviceName": @"primary", @"width": @(device.width), @"height": @(device.height),
            @"scale": @(device.scale), @"hdpi": @(device.pitch), @"vdpi": @(device.pitch)} mutableCopy]]
    } mutableCopy]} mutableCopy];
}

static void validate(id document, LSBGuestDeviceType *device, int expected) {
    errno = EDOM;
    assert(LSBValidateGuestDeviceCapabilities(document, device) == expected && errno == EDOM);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSArray<LSBGuestDeviceType *> *catalog = LSBGuestDeviceTypes();
        assert(catalog.count == 7 && catalog == LSBGuestDeviceTypes());
        const struct {
            const char *identifier, *model, *product; NSUInteger width, height, scale, pitch; BOOL experimental, home, pad;
        } expected[] = {
            {"iphone-8", "iPhone10,4", "D20", 750, 1334, 2, 326, YES, YES, NO},
            {"iphone-8-plus", "iPhone10,5", "D21", 1242, 2208, 3, 461, YES, YES, NO},
            {"iphone-se-2", "iPhone12,8", "D79", 750, 1334, 2, 326, NO, YES, NO},
            {"iphone-11", "iPhone12,1", "N104", 828, 1792, 2, 326, NO, NO, NO},
            {"iphone-15-pro-max", "iPhone16,2", "D84", 1290, 2796, 3, 460, NO, NO, NO},
            {"ipad-9", "iPad12,2", "J182", 1620, 2160, 2, 264, NO, YES, YES},
            {"ipad-a16", "iPad15,7", "J481", 1640, 2360, 2, 264, NO, NO, YES},
        };
        NSURL *root = [NSURL fileURLWithPath:@"/fixture/RuntimeRoot" isDirectory:YES];
        for (unsigned i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
            LSBGuestDeviceType *device = catalog[i];
            assert([device.identifier isEqual:@(expected[i].identifier)] && [device.modelIdentifier isEqual:@(expected[i].model)]);
            assert([device.productClass isEqual:@(expected[i].product)] && device.displayName.length && device.detailText.length);
            assert(device.width == expected[i].width && device.height == expected[i].height && device.scale == expected[i].scale &&
                   device.pitch == expected[i].pitch && device.experimental == expected[i].experimental);
            assert(device.hasHomeButton == expected[i].home && device.isPad == expected[i].pad);
            assert(LSBGuestDeviceTypeForIdentifier(device.identifier) == device);
            NSString *path = [NSString stringWithFormat:@"/fixture/RuntimeRoot/Library/Developer/CoreSimulator/DeviceTypes/%@/capabilities.plist", device.identifier];
            assert([[device capabilitiesURLInRuntimeRoot:root].path isEqual:path]);
            validate(capabilities(device), device, 0);
        }
        assert(LSBDefaultGuestDeviceType() == catalog[4]);
        for (id identifier in @[@"", @"iphone-15-pro-max/..", @"iPhone-15-Pro-Max", @"unknown", @1])
            assert(!LSBGuestDeviceTypeForIdentifier(identifier));
        assert(!LSBGuestDeviceTypeForIdentifier(nil));
        LSBGuestDeviceType *device = LSBDefaultGuestDeviceType();
        validate(@[], device, EINVAL);
        validate(@{}, device, EINVAL);
        validate(@{@"capabilities": @[]}, device, EINVAL);
        validate(@{@"capabilities": @{}}, device, EINVAL);
        for (NSString *key in @[@"width", @"height", @"scale", @"hdpi", @"vdpi"]) {
            for (id bad in @[@0, @1, @YES, @"3", @(-1), @3.5]) {
                NSMutableDictionary *document = capabilities(device);
                document[@"capabilities"][@"displays"][0][key] = bad;
                validate(document, device, EINVAL);
            }
        }
        for (NSString *key in @[@"modelIdentifier", @"productClass", @"idiom"]) {
            NSMutableDictionary *document = capabilities(device);
            document[@"capabilities"][key] = @"wrong";
            validate(document, device, EINVAL);
        }
        for (NSString *key in @[@"ipad", @"DeviceSupportsDynamicIsland"]) {
            NSMutableDictionary *document = capabilities(device);
            document[@"capabilities"][key] = @1; // Policy fields retain actual plist Boolean types.
            validate(document, device, EINVAL);
        }
        NSMutableDictionary *document = capabilities(device);
        document[@"capabilities"][@"displays"] = @[@{@"deviceName": @"external"}];
        validate(document, device, EINVAL);
        document = capabilities(device);
        [document[@"capabilities"][@"displays"] addObject:[document[@"capabilities"][@"displays"][0] copy]];
        validate(document, device, EINVAL);
        document = capabilities(device);
        document[@"capabilities"][@"ArtworkTraits"] = @{@"ArtworkDeviceIdiom": @"pad"};
        validate(document, device, EINVAL);
        document = capabilities(device);
        document[@"capabilities"][@"HomeButtonType"] = @1;
        validate(document, device, EINVAL);
        device = LSBGuestDeviceTypeForIdentifier(@"iphone-8-plus");
        document = capabilities(device);
        // Stock physical-panel dimensions differ and must not invalidate the correct primary simulator display.
        document[@"capabilities"][@"ScreenDimensionsCapability"] = @{@"main-screen-width": @1080, @"main-screen-height": @1920};
        validate(document, device, 0);
        validate(document, LSBDefaultGuestDeviceType(), EINVAL);
        assert(LSBValidateGuestDeviceCapabilities(nil, device) == EINVAL);
        // Optional local integration check: supply a staged RuntimeRoot, without requiring Apple assets in the test checkout.
        if (argc == 2) for (LSBGuestDeviceType *entry in catalog) {
            NSURL *runtime = [NSURL fileURLWithPath:@(argv[1]) isDirectory:YES];
            NSDictionary *original = [NSDictionary dictionaryWithContentsOfURL:[entry capabilitiesURLInRuntimeRoot:runtime] error:NULL];
            assert(original && !LSBValidateGuestDeviceCapabilities(original, entry));
        }
        puts("Device type catalog tests passed");
    }
    return 0;
}
