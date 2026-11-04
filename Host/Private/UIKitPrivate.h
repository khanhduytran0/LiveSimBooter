#pragma once

#import <UIKit/UIDevice.h>

@interface UIDevice (LSBHardware)
+ (BOOL)_hasHomeButton;
@end
