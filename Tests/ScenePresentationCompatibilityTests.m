#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <assert.h>

// Native macOS stubs exercise the transition and policy without loading UIKit or creating a simulator scene.
@interface UIColor : NSObject
@property(nonatomic, readonly) CGColorRef CGColor;
- (instancetype)initWithWhite:(CGFloat)white alpha:(CGFloat)alpha;
@end
@implementation UIColor
- (instancetype)initWithWhite:(CGFloat)white alpha:(CGFloat)alpha {
    if ((self = [super init])) _CGColor = CGColorCreateGenericGray(white, alpha);
    return self;
}
- (void)dealloc { CGColorRelease(_CGColor); }
@end

@class UIWindow;
@interface UIView : NSObject
@property(nonatomic, weak) UIView *superview;
@property(nonatomic, weak) UIWindow *window;
@property(nonatomic, copy) NSArray<UIView *> *subviews;
@property(nonatomic) BOOL hidden;
@property(nonatomic) CGFloat alpha;
@property(nonatomic, strong) UIColor *backgroundColor;
@end
@implementation UIView
- (instancetype)init { if ((self = [super init])) { _alpha = 1; _subviews = @[]; } return self; }
@end
@interface UIWindow : UIView @end
@implementation UIWindow @end
@interface SBRootSceneWindow : UIWindow @end
@implementation SBRootSceneWindow @end

#define LSB_SCENE_PRESENTATION_TESTING 1
#include "../Guest/libLiveSimScene/ScenePresentationCompatibility.m"

@interface FBScene ()
@property(nonatomic, readwrite) NSString *identifier;
@end
@implementation FBScene @end
@interface UIScenePresentationContext ()
@property(nonatomic, readwrite) UIColor *backgroundColorWhileNotHosting;
@end
@implementation UIScenePresentationContext @end
@interface _UIScenePresenter ()
@property(nonatomic, readwrite) BOOL isHosting;
@property(nonatomic, readwrite) NSString *identifier;
@property(nonatomic, readwrite) FBScene *scene;
@property(nonatomic, strong) _UIScenePresentationView *existingView;
@property(nonatomic) BOOL nextHosting;
@property(nonatomic) NSUInteger originalCalls;
@end
@implementation _UIScenePresenter
@synthesize existingView = _view;
- (void)updateHostingStateIfNecessary { ++_originalCalls; _isHosting = _nextHosting; errno = EDOM; }
@end
@interface _UIScenePresentationView ()
@property(nonatomic, readwrite) _UIScenePresenter *presenter;
@property(nonatomic, readwrite) UIView *hostContainerView;
@property(nonatomic, readwrite) UIScenePresentationContext *currentPresentationContext;
@property(nonatomic) NSUInteger refreshCalls;
@end
@implementation _UIScenePresentationView
- (void)_updateBackgroundColor { ++_refreshCalls; self.backgroundColor = self.currentPresentationContext.backgroundColorWhileNotHosting; errno = ERANGE; }
@end

static void pinTests(void) {
    unsigned char *image = calloc(1, 0x4000);
    assert(image);
    struct mach_header_64 *header = (void *)image;
    *header = (struct mach_header_64){.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .filetype = MH_DYLIB, .ncmds = 2,
        .sizeofcmds = sizeof(struct uuid_command) + sizeof(struct segment_command_64)};
    struct uuid_command *uuid = (void *)(header + 1);
    *uuid = (struct uuid_command){.cmd = LC_UUID, .cmdsize = sizeof(*uuid)};
    memcpy(uuid->uuid, UIKitUUID, sizeof(UIKitUUID));
    struct segment_command_64 *text = (void *)(uuid + 1);
    *text = (struct segment_command_64){.cmd = LC_SEGMENT_64, .cmdsize = sizeof(*text), .vmsize = 0x4000,
        .initprot = VM_PROT_READ | VM_PROT_EXECUTE};
    memcpy(text->segname, "__TEXT", 7);
    uint32_t *method = (void *)(image + 0x1000);
    memcpy(method, HostingInstructions, sizeof(HostingInstructions));
    size_t count = sizeof(HostingInstructions) / sizeof(uint32_t);
    assert(pinnedMethod(header, (IMP)method, HostingInstructions, count, YES));
    method[4] |= 0x1234; // Cache branch displacement adjustments are permitted.
    assert(pinnedMethod(header, (IMP)method, HostingInstructions, count, YES));
    method[5] ^= 1;
    assert(!pinnedMethod(header, (IMP)method, HostingInstructions, count, YES));
    method[5] ^= 1;
    uuid->uuid[0] ^= 1;
    assert(!pinnedMethod(header, (IMP)method, HostingInstructions, count, YES));
    uuid->uuid[0] ^= 1;
    text->vmsize = 0x1000;
    assert(!pinnedMethod(header, (IMP)method, HostingInstructions, count, YES));
    text->vmsize = 0x4000;
    text->initprot = VM_PROT_READ;
    assert(!pinnedMethod(header, (IMP)method, HostingInstructions, count, YES));
    text->initprot |= VM_PROT_EXECUTE;
    memcpy(method, BackgroundInstructions, sizeof(BackgroundInstructions));
    assert(pinnedMethod(header, (IMP)method, BackgroundInstructions, sizeof(BackgroundInstructions) / sizeof(uint32_t), NO));
    uuid->cmdsize = 0;
    assert(!pinnedMethod(header, (IMP)method, BackgroundInstructions, sizeof(BackgroundInstructions) / sizeof(uint32_t), NO));
    free(image);
}

static void pathTests(void) {
    char temporary[] = "/tmp/LSB-scene-presentation.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    NSString *base = @(canonical);
    NSString *runtime = [base stringByAppendingPathComponent:@"LSB/iOS/23A343/RuntimeRoot"];
    NSString *executable = [runtime stringByAppendingString:@(SpringBoardPath)];
    assert([NSFileManager.defaultManager createDirectoryAtPath:executable.stringByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:NULL]);
    assert([NSData.data writeToFile:executable atomically:NO]);
    assert(validatedSpringBoardPath(runtime.fileSystemRepresentation, executable.fileSystemRepresentation));
    assert(validatedSpringBoardPath(runtime.fileSystemRepresentation, SpringBoardPath));
    assert(!validatedSpringBoardPath(runtime.fileSystemRepresentation, "/Applications/Other.app/Other"));
    assert(!validatedSpringBoardPath(base.fileSystemRepresentation, executable.fileSystemRepresentation));
    assert(!validatedSpringBoardPath(NULL, SpringBoardPath));
    assert(!validatedSpringBoardPath(runtime.fileSystemRepresentation, NULL));
    assert([NSFileManager.defaultManager removeItemAtPath:base error:NULL]);
}

int main(void) {
    @autoreleasepool {
        pinTests();
        pathTests();
        assert(loadClasses(_UIScenePresenter.class));
        Method original = class_getInstanceMethod(_UIScenePresenter.class, @selector(updateHostingStateIfNecessary));
        assert(installAdapter(_UIScenePresenter.class, original));
        assert(!installAdapter(_UIScenePresenter.class, original));
        _UIScenePresenter *presenter = [_UIScenePresenter new];
        presenter.identifier = @"SBRootSceneWindow-SBMainDisplayRootWindowScenePresentationBinder-0x123";
        presenter.scene = [FBScene new];
        presenter.scene.identifier = @"sceneID:com.apple.PosterBoard-EC05D966-CBD5-45F1-85C3-0431FED2D984";
        _UIScenePresentationView *view = [_UIScenePresentationView new];
        SBRootSceneWindow *window = [SBRootSceneWindow new];
        view.window = window;
        view.superview = window;
        view.presenter = presenter;
        presenter.existingView = view;
        view.currentPresentationContext = [UIScenePresentationContext new];
        UIColor *black = [[UIColor alloc] initWithWhite:0 alpha:1];
        view.backgroundColor = black;
        assert(shouldRefreshBackground(presenter, view));
        presenter.isHosting = YES;
        [presenter updateHostingStateIfNecessary];
        assert(presenter.originalCalls == 1 && view.refreshCalls == 1 && !view.backgroundColor && errno == EDOM);
        view.backgroundColor = black;
        [presenter updateHostingStateIfNecessary]; // Already nonhosting: no mutation.
        assert(presenter.originalCalls == 2 && view.refreshCalls == 1 && view.backgroundColor == black);
        presenter.isHosting = YES;
        presenter.nextHosting = YES;
        [presenter updateHostingStateIfNecessary];
        assert(presenter.originalCalls == 3 && view.refreshCalls == 1);
        view.hostContainerView = [UIView new];
        assert(!shouldRefreshBackground(presenter, view));
        view.hostContainerView = nil;
        view.subviews = @[[UIView new]];
        assert(shouldRefreshBackground(presenter, view)); // Owner removes the relinquished container after this synchronous callback.
        view.subviews = @[];
        window.hidden = YES;
        assert(!shouldRefreshBackground(presenter, view));
        window.hidden = NO;
        view.alpha = 0;
        assert(!shouldRefreshBackground(presenter, view));
        view.alpha = 1;
        view.currentPresentationContext.backgroundColorWhileNotHosting = black;
        assert(!shouldRefreshBackground(presenter, view));
        view.currentPresentationContext.backgroundColorWhileNotHosting = nil;
        view.backgroundColor = [[UIColor alloc] initWithWhite:0 alpha:0.5];
        assert(!shouldRefreshBackground(presenter, view));
        view.backgroundColor = [[UIColor alloc] initWithWhite:1 alpha:1];
        assert(!shouldRefreshBackground(presenter, view));
        view.backgroundColor = black;
        NSString *identifier = presenter.identifier;
        presenter.identifier = @"CoverSheet";
        assert(!shouldRefreshBackground(presenter, view));
        presenter.identifier = identifier;
        presenter.scene.identifier = @"sceneID:com.apple.Contacts-EC05D966-CBD5-45F1-85C3-0431FED2D984";
        assert(!shouldRefreshBackground(presenter, view));
        presenter.scene.identifier = @"sceneID:com.apple.PosterBoard-not-a-scene-uuid";
        assert(!shouldRefreshBackground(presenter, view));
        presenter.existingView = nil;
        presenter.isHosting = YES;
        presenter.nextHosting = NO;
        [presenter updateHostingStateIfNecessary];
        assert(presenter.originalCalls == 4 && view.refreshCalls == 1);
        presenter.existingView = view;
        presenter.scene.identifier = @"sceneID:com.apple.PosterBoard-EC05D966-CBD5-45F1-85C3-0431FED2D984";
        presenter.isHosting = YES;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            [presenter updateHostingStateIfNecessary];
            dispatch_semaphore_signal(done);
        });
        assert(!dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
        assert(presenter.originalCalls == 5 && view.refreshCalls == 1 && view.backgroundColor == black);
        presenter.existingView = nil;
        view.presenter = nil;
        puts("ScenePresentationCompatibility tests passed");
    }
    return 0;
}
