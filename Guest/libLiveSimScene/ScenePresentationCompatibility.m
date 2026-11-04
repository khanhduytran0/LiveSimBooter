#import "ScenePresentation.h"
#import "../../ProcessSupport/RuntimeLayout.h"
#import <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern uint32_t dyld_get_active_platform(void);
static Class presentationClass, contextClass, sceneClass, rootWindowClass;
static Ivar existingView;
static const char SpringBoardPath[] = "/System/Library/CoreServices/SpringBoard.app/SpringBoard";
static const unsigned char UIKitUUID[] = {0xf3, 0x92, 0x1c, 0x0a, 0x13, 0x32, 0x37, 0x79, 0x97, 0xc7, 0x62, 0xd9, 0xf3, 0xfa, 0x6b, 0x77};
// Original 23A343 UIKitCore: -[_UIScenePresenter updateHostingStateIfNecessary] +0xb73608;
// -[_UIScenePresentationView _updateBackgroundColor] +0xb724c8. Cache construction can relocate these methods.
static const uint32_t HostingInstructions[] = {
    0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0x94000000, 0x39418268, 0x6b00011f,
    0x54000081, 0xa9417bfd, 0xa8c24ff4, 0xd65f03c0, 0xaa0003f4, 0xaa1303e0, 0xaa1403e2,
    0x94000000, 0x39018274, 0xaa1303e0, 0xaa1403e2, 0xa9417bfd, 0xa8c24ff4, 0x14000000
};
static const uint32_t BackgroundInstructions[] = {0xd10103ff, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};

static BOOL pinnedMethod(const struct mach_header_64 *header, IMP implementation, const uint32_t *words, size_t count, BOOL hosting) {
    if (!header || !implementation || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64 || header->filetype != MH_DYLIB ||
        header->sizeofcmds > 1024 * 1024 || count > SIZE_MAX / sizeof(*words)) return NO;
    const unsigned char *cursor = (const unsigned char *)(header + 1), *end = cursor + header->sizeofcmds;
    BOOL uuidMatches = NO, textContainsMethod = NO;
    uintptr_t offset = (uintptr_t)implementation - (uintptr_t)header;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if ((size_t)(end - cursor) < sizeof(struct load_command)) return NO;
        const struct load_command *command = (const void *)cursor;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(end - cursor)) return NO;
        if (command->cmd == LC_UUID && command->cmdsize == sizeof(struct uuid_command))
            uuidMatches = !memcmp(((const struct uuid_command *)command)->uuid, UIKitUUID, sizeof(UIKitUUID));
        if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment = (const void *)command;
            // Cache construction can move text and rewrite external branch displacements. UUID, instructions and executable ownership remain pinned.
            if (!memcmp(segment->segname, "__TEXT\0", 7) && (segment->initprot & VM_PROT_EXECUTE) &&
                offset <= segment->vmsize && count * sizeof(*words) <= segment->vmsize - offset) textContainsMethod = YES;
        }
        cursor += command->cmdsize;
    }
    if (!uuidMatches || !textContainsMethod) return NO;
    const uint32_t *actual = (const void *)implementation;
    for (size_t i = 0; i < count; ++i) {
        uint32_t mask = hosting && (i == 4 || i == 14 || i == 20) ? 0xfc000000 : UINT32_MAX;
        if ((actual[i] & mask) != words[i]) return NO;
    }
    return YES;
}

static BOOL validatedSpringBoardPath(const char *runtimeBytes, const char *executable) {
    if (!runtimeBytes || runtimeBytes[0] != '/' || !executable || executable[0] != '/') return NO;
    char runtime[PATH_MAX], expected[PATH_MAX], canonical[PATH_MAX];
    if (!realpath(runtimeBytes, runtime)) return NO;
    static const char suffix[] = "/" LSB_STORAGE_BUILD_RELATIVE_PATH "/" LSB_RUNTIME_DIRECTORY;
    size_t length = strlen(runtime);
    if (length < sizeof(suffix) - 1 || strcmp(runtime + length - sizeof(suffix) + 1, suffix)) return NO;
    int count = snprintf(expected, sizeof(expected), "%s%s", runtime, SpringBoardPath);
    if (count < 0 || (size_t)count >= sizeof(expected) || !realpath(expected, canonical) || strcmp(expected, canonical)) return NO;
    struct stat metadata;
    if (stat(canonical, &metadata) || !S_ISREG(metadata.st_mode)) return NO;
    // dyld may report either the guest logical name or its physical path beneath the selected runtime.
    return !strcmp(executable, SpringBoardPath) || (realpath(executable, canonical) && !strcmp(canonical, expected));
}

static BOOL opaqueBlack(UIColor *color) {
    CGColorRef value = color.CGColor;
    if (!value || CGColorGetAlpha(value) != 1) return NO;
    CGColorSpaceModel model = CGColorSpaceGetModel(CGColorGetColorSpace(value));
    size_t count = CGColorGetNumberOfComponents(value);
    const CGFloat *components = CGColorGetComponents(value);
    if (model == kCGColorSpaceModelMonochrome && count == 2) return components[0] == 0;
    return model == kCGColorSpaceModelRGB && count == 4 && components[0] == 0 && components[1] == 0 && components[2] == 0;
}

static BOOL shouldRefreshBackground(_UIScenePresenter *presenter, _UIScenePresentationView *view) {
    if (![view isKindOfClass:presentationClass] || view.presenter != presenter || view.hostContainerView ||
        ![view.window isMemberOfClass:rootWindowClass] || !opaqueBlack(view.backgroundColor)) return NO;
    if (![presenter.identifier hasPrefix:@"SBRootSceneWindow-SBMainDisplayRootWindowScenePresentationBinder-"] ||
        ![presenter.scene isKindOfClass:sceneClass]) return NO;
    NSString *identifier = presenter.scene.identifier;
    static NSString * const prefix = @"sceneID:com.apple.PosterBoard-";
    if (![identifier hasPrefix:prefix] || ![[NSUUID alloc] initWithUUIDString:[identifier substringFromIndex:prefix.length]]) return NO;
    UIScenePresentationContext *context = view.currentPresentationContext;
    if (![context isKindOfClass:contextClass] || context.backgroundColorWhileNotHosting) return NO;
    // The old shared container may still be a subview: the owner removes it immediately after updating hosting state.
    // Its nil hostContainerView already relinquishes ownership. Refresh synchronously in that transaction; never create a lazy presentationView.
    UIView *ancestor = view;
    for (NSUInteger depth = 0; ancestor && depth < 64; ++depth, ancestor = ancestor.superview) {
        if (ancestor.hidden || ancestor.alpha <= 0) return NO;
        if (ancestor == view.window) return YES;
    }
    return NO;
}

@interface LSBScenePresentationAdapter : NSObject
- (void)lsb_updateHostingStateIfNecessary;
@end

@implementation LSBScenePresentationAdapter
- (void)lsb_updateHostingStateIfNecessary {
    _UIScenePresenter *presenter = (_UIScenePresenter *)self;
    BOOL mainThread = pthread_main_np() != 0;
    BOOL wasHosting = mainThread && presenter.isHosting;
    [presenter lsb_updateHostingStateIfNecessary];
    int savedError = errno;
    if (wasHosting && !presenter.isHosting) {
        _UIScenePresentationView *view = object_getIvar(presenter, existingView);
        if (shouldRefreshBackground(presenter, view)) [view _updateBackgroundColor];
    }
    errno = savedError;
}
@end

static BOOL loadClasses(Class presenter) {
    presentationClass = objc_lookUpClass("_UIScenePresentationView");
    contextClass = objc_lookUpClass("UIScenePresentationContext");
    sceneClass = objc_lookUpClass("FBScene");
    rootWindowClass = objc_lookUpClass("SBRootSceneWindow");
    existingView = class_getInstanceVariable(presenter, "_view");
    // The private header defines the call signatures. Resolve the existing ivar by name to avoid the lazy presentationView getter.
    return presenter && presentationClass && contextClass && sceneClass && rootWindowClass && existingView;
}

static BOOL installAdapter(Class presenter, Method original) {
    SEL alias = @selector(lsb_updateHostingStateIfNecessary);
    if (!presenter || !original || class_getInstanceMethod(presenter, alias)) return NO;
    Method replacement = class_getInstanceMethod(LSBScenePresentationAdapter.class, alias);
    if (!replacement || !class_addMethod(presenter, alias, method_getImplementation(replacement), method_getTypeEncoding(replacement))) return NO;
    method_exchangeImplementations(original, class_getInstanceMethod(presenter, alias));
    return YES;
}

#ifndef LSB_SCENE_PRESENTATION_TESTING
__attribute__((constructor)) static void installScenePresentationCompatibility(void) {
    // Role-only guest dylib: never loaded into the native host or the early C-only loader.
    // 23A343 _UIScenePresenterOwner clears the old container before updating cached hosting, leaving its hosting background behind.
    // Recompute only the relinquished PosterBoard root presentation after true->false; preserve CoverSheet's actual hosting view and policy.
    @autoreleasepool {
        char executable[PATH_MAX];
        uint32_t capacity = sizeof(executable);
        if (dyld_get_active_platform() != 7 || _NSGetExecutablePath(executable, &capacity) ||
            !validatedSpringBoardPath(getenv("DYLD_ROOT_PATH"), executable)) return;
        Class presenter = objc_lookUpClass("_UIScenePresenter");
        if (!loadClasses(presenter)) return;
        Method method = class_getInstanceMethod(presenter, @selector(updateHostingStateIfNecessary));
        Method background = class_getInstanceMethod(presentationClass, @selector(_updateBackgroundColor));
        IMP implementation = method ? method_getImplementation(method) : NULL;
        IMP backgroundImplementation = background ? method_getImplementation(background) : NULL;
        Dl_info owner = {0}, backgroundOwner = {0};
        if (!implementation || !backgroundImplementation || !dladdr((const void *)implementation, &owner) ||
            !dladdr((const void *)backgroundImplementation, &backgroundOwner) || owner.dli_fbase != backgroundOwner.dli_fbase ||
            !pinnedMethod(owner.dli_fbase, implementation, HostingInstructions, sizeof(HostingInstructions) / sizeof(uint32_t), YES) ||
            !pinnedMethod(owner.dli_fbase, backgroundImplementation, BackgroundInstructions, sizeof(BackgroundInstructions) / sizeof(uint32_t), NO) ||
            !installAdapter(presenter, method)) fprintf(stderr, "[scene-presentation] unsupported SpringBoard UIKit hosting transition\n");
    }
}
#endif
