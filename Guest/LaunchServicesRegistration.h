#pragma once

#import <Foundation/Foundation.h>

// Matching 23A343 CoreServices implements registerApplication: by calling LSRegisterURL (not a fabricated install dictionary).
@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplication:(NSURL *)url;
// The 23A343 package-app route accepts a minimal dictionary containing CFBundleIdentifier and the physical Path.
- (BOOL)registerApplicationDictionary:(NSDictionary *)application;
- (BOOL)unregisterApplication:(NSURL *)url;
@end

// 23A343 CoreServices: _LSEnumeratorFireErrorHandler calls the block synchronously with its NSError.
@interface LSEnumerator : NSEnumerator
+ (instancetype)enumeratorForApplicationProxiesWithOptions:(NSUInteger)options;
@property(copy) void (^errorHandler)(NSError *);
@end

@interface LSApplicationProxy : NSObject
+ (instancetype)applicationProxyForIdentifier:(NSString *)identifier;
@property(nonatomic, readonly) NSString *bundleIdentifier;
@property(nonatomic, readonly) NSURL *bundleURL;
@property(nonatomic, readonly, getter=isInstalled) BOOL installed;
@end
