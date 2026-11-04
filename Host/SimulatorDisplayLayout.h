#pragma once

#include <CoreGraphics/CGGeometry.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>

static const CGFloat LSBDisplayHomeButtonVisibleSize = 44, LSBDisplayHomeButtonHitSize = 48, LSBDisplayHomeButtonGap = 8;
static const CGFloat LSBDisplayTabButtonSize = 44, LSBDisplayTabButtonGap = 4, LSBDisplayTabButtonInset = 12;
typedef struct { CGRect displayFrame, homeButtonFrame; } LSBDisplayLayout;
typedef enum { LSBSurfaceWithTabs, LSBSurfaceWithTabButton, LSBSurfaceGuidedFullscreen } LSBSurfacePresentation;

static inline LSBSurfacePresentation LSBSelectSurfacePresentation(bool activeGuest, bool normallyHideTabs, bool guidedAccess,
                                                                 bool hostHasHomeIndicator, bool guestHasHomeIndicator) {
    if (!activeGuest) return LSBSurfaceWithTabs;
    if (guidedAccess && hostHasHomeIndicator && guestHasHomeIndicator) return LSBSurfaceGuidedFullscreen;
    return normallyHideTabs ? LSBSurfaceWithTabButton : LSBSurfaceWithTabs;
}

static inline CGRect LSBShowTabsButtonFrame(CGRect safe, CGRect viewport) {
    if (CGRectIsInfinite(viewport) || !CGRectContainsRect(viewport, safe) || CGRectIsInfinite(safe) || !isfinite(safe.size.width) || !isfinite(safe.size.height) ||
        safe.size.width < LSBDisplayTabButtonSize || safe.size.height < LSBDisplayTabButtonSize) return CGRectZero;
    CGFloat x = fmin(CGRectGetMaxX(safe), CGRectGetMaxX(viewport) - LSBDisplayTabButtonInset);
    x = fmax(x, CGRectGetMinX(safe) + LSBDisplayTabButtonSize);
    CGFloat bottom = CGRectGetMaxY(safe);
    // The home indicator occupies the middle of the bottom inset. The trailing quarter can use this shallow strip.
    // Stay above larger obstructions and inside the side safe area in landscape.
    if (CGRectGetMaxY(viewport) - bottom <= LSBDisplayTabButtonSize &&
        x - LSBDisplayTabButtonSize >= CGRectGetMaxX(viewport) - viewport.size.width / 4) bottom = CGRectGetMaxY(viewport);
    CGFloat y = fmax(CGRectGetMinY(safe) + LSBDisplayTabButtonSize, fmin(bottom, CGRectGetMaxY(viewport) - LSBDisplayTabButtonInset));
    if (!isfinite(x) || !isfinite(y) || x - LSBDisplayTabButtonSize >= x || y - LSBDisplayTabButtonSize >= y) return CGRectZero;
    return CGRectMake(x - LSBDisplayTabButtonSize, y - LSBDisplayTabButtonSize, LSBDisplayTabButtonSize, LSBDisplayTabButtonSize);
}

// In Full Width with the tabs hidden, center the guest on the host viewport. With native tabs visible,
// center the guest and its optional Home button as a group in the available safe area.
static inline bool LSBComputeSimulatorDisplayLayout(CGRect available, CGRect viewport, CGSize pixels, bool fullWidth, bool needsHomeButton,
                                                    LSBSurfacePresentation presentation, LSBDisplayLayout *output) {
    if (!output) return false;
    *output = (LSBDisplayLayout){CGRectZero, CGRectZero};
    if (presentation == LSBSurfaceGuidedFullscreen) {
        if (needsHomeButton) return false;
        // The guest supplies the status area and home indicator. Fit to the entire host view, including cutout insets.
        available = viewport;
        fullWidth = true;
    }
    if (!isfinite(available.origin.x) || !isfinite(available.origin.y) || !isfinite(available.size.width) ||
        !isfinite(available.size.height) || available.size.width <= 0 || available.size.height <= 0 ||
        !isfinite(pixels.width) || !isfinite(pixels.height) || pixels.width <= 0 || pixels.height <= 0) return false;
    CGFloat right = CGRectGetMaxX(available), bottom = CGRectGetMaxY(available);
    CGFloat aspect = pixels.width / pixels.height;
    if (!isfinite(right) || !isfinite(bottom) || right <= available.origin.x || bottom <= available.origin.y ||
        !isfinite(aspect) || aspect <= 0) return false;

    CGFloat fraction = fullWidth ? 1 : 0.94;
    CGFloat homeRow = needsHomeButton ? LSBDisplayHomeButtonHitSize + LSBDisplayHomeButtonGap : 0;
    CGFloat maximumWidth = available.size.width * fraction, maximumHeight = available.size.height * fraction - homeRow;
    CGPoint center = CGPointMake(CGRectGetMidX(available), CGRectGetMidY(available) - homeRow / 2);
    if (presentation == LSBSurfaceWithTabButton) {
        CGRect tabs = LSBShowTabsButtonFrame(available, viewport);
        if (!fullWidth || CGRectIsEmpty(tabs)) return false;
        center = CGPointMake(CGRectGetMidX(viewport), CGRectGetMidY(viewport));
        if (!isfinite(center.x) || !isfinite(center.y)) return false;
        maximumWidth = 2 * fmin(center.x - CGRectGetMinX(available), right - center.x);
        maximumHeight = 2 * fmin(center.y - CGRectGetMinY(available), bottom - homeRow - center.y);

        // A centered guest can pass either above or beside the arrow. Keep whichever fit gives it more height.
        CGFloat widthBeside = 2 * (CGRectGetMinX(tabs) - LSBDisplayTabButtonGap - center.x);
        CGFloat heightAbove = 2 * (CGRectGetMinY(tabs) - LSBDisplayTabButtonGap - center.y);
        if (needsHomeButton && widthBeside < LSBDisplayHomeButtonHitSize) {
            // Home also occupies the arrow's column, so its full touch target has to fit above the arrow.
            maximumHeight = fmin(maximumHeight, heightAbove - 2 * homeRow);
        } else {
            maximumHeight = fmin(maximumHeight, fmax(widthBeside / aspect, heightAbove));
        }
    }
    if (needsHomeButton && maximumWidth < LSBDisplayHomeButtonHitSize) return false;
    CGFloat width = maximumWidth, height = width / aspect;
    if (height > maximumHeight) { height = maximumHeight; width = height * aspect; }
    if (!isfinite(width) || !isfinite(height) || width <= 0 || height <= 0) return false;
    CGRect display = CGRectMake(center.x - width / 2, center.y - height / 2, width, height);
    CGRect home = CGRectZero;
    if (needsHomeButton) {
        home = CGRectMake(center.x - LSBDisplayHomeButtonHitSize / 2, CGRectGetMaxY(display) + LSBDisplayHomeButtonGap,
                          LSBDisplayHomeButtonHitSize, LSBDisplayHomeButtonHitSize);
    }
    // Fractional safe-area coordinates can round the last endpoint or 8-point gap by one ulp. Allow only that rounding,
    // capped far below a display pixel; still reject unrepresentable coordinates and overflow at extreme finite inputs.
    CGFloat magnitude = fmax(1, fmax(fmax(fabs(available.origin.x), fabs(right)), fmax(fabs(available.origin.y), fabs(bottom))));
    CGFloat tolerance = fmin(1e-7, 8 * DBL_EPSILON * magnitude);
    if (!isfinite(display.origin.x) || !isfinite(display.origin.y) || available.origin.x - display.origin.x > tolerance ||
        available.origin.y - display.origin.y > tolerance || CGRectGetMaxX(display) - right > tolerance || CGRectGetMaxY(display) - bottom > tolerance ||
        CGRectGetMaxX(display) <= display.origin.x || CGRectGetMaxY(display) <= display.origin.y) return false;
    if (needsHomeButton && (!isfinite(home.origin.x) || !isfinite(home.origin.y) || available.origin.x - home.origin.x > tolerance ||
        CGRectGetMaxX(home) - right > tolerance || CGRectGetMaxY(home) - bottom > tolerance ||
        home.origin.y - CGRectGetMaxY(display) + tolerance < LSBDisplayHomeButtonGap)) return false;
    *output = (LSBDisplayLayout){display, home};
    return true;
}
