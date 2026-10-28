#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface LSBGuestDeviceType : NSObject
@property (nonatomic, copy, readonly) NSString *identifier;
@property (nonatomic, copy, readonly) NSString *displayName;
@property (nonatomic, copy, readonly) NSString *detailText;
@property (nonatomic, copy, readonly) NSString *modelIdentifier;
@property (nonatomic, copy, readonly) NSString *productClass;
@property (nonatomic, readonly) NSUInteger width, height, scale, pitch;
@property (nonatomic, readonly) BOOL experimental;
@property (nonatomic, readonly) BOOL hasHomeButton, isPad;
- (NSURL *)capabilitiesURLInRuntimeRoot:(NSURL *)root;
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
@end

NSArray<LSBGuestDeviceType *> *LSBGuestDeviceTypes(void);
LSBGuestDeviceType * _Nullable LSBGuestDeviceTypeForIdentifier(NSString * _Nullable identifier);
LSBGuestDeviceType *LSBDefaultGuestDeviceType(void);

// Validate an original capabilities dictionary against the selected catalog entry. No fallback, mutation, or global selected state.
// Returns EINVAL for malformed/mismatched input. Caller errno is preserved.
int LSBValidateGuestDeviceCapabilities(NSDictionary * _Nullable profile, LSBGuestDeviceType * _Nullable device);

NS_ASSUME_NONNULL_END
