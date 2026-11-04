#include "../Host/SimulatorDisplayLayout.h"
#include <assert.h>
#include <float.h>
#include <stdio.h>

static bool near(CGFloat left, CGFloat right) { return fabs(left - right) <= 1e-8 * fmax(1, fmax(fabs(left), fabs(right))); }

static CGPoint midpoint(CGRect rect) {
    return CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
}

static void checkLayout(CGRect available, CGRect viewport, CGSize pixels, bool fullWidth, bool home, bool tabs) {
    CGPoint center = midpoint(tabs ? viewport : available);
    LSBDisplayLayout layout;
    assert(LSBComputeSimulatorDisplayLayout(available, viewport, pixels, fullWidth, home, tabs ? LSBSurfaceWithTabButton : LSBSurfaceWithTabs, &layout));
    CGRect display = layout.displayFrame, button = layout.homeButtonFrame;
    assert(display.size.width > 0 && display.size.height > 0);
    CGFloat aspect = pixels.width / pixels.height;
    assert(near(display.size.width / display.size.height, aspect));
    CGRect roundedBounds = CGRectInset(available, -1e-7, -1e-7);
    assert(CGRectContainsRect(roundedBounds, display));
    assert(near(CGRectGetMidX(display), center.x));
    if (home) {
        assert(button.size.width == 48 && button.size.height == 48 && CGRectContainsRect(roundedBounds, button));
        assert(near(CGRectGetMidX(button), center.x));
        assert(near(button.origin.y - CGRectGetMaxY(display), 8) && !CGRectIntersectsRect(display, button));
        assert(CGRectInset(button, 2, 2).size.width == 44);
    } else assert(CGRectEqualToRect(button, CGRectZero));

    if (tabs) {
        assert(near(CGRectGetMidY(display), center.y));
        CGRect tabButton = LSBShowTabsButtonFrame(available, viewport);
        assert(tabButton.size.width == 44 && tabButton.size.height == 44);
        assert(CGRectContainsRect(CGRectInset(viewport, -1e-7, -1e-7), tabButton));
        assert(CGRectGetMinX(tabButton) >= CGRectGetMinX(available) - 1e-7 && CGRectGetMaxX(tabButton) <= CGRectGetMaxX(available) + 1e-7);
        CGRect padded = CGRectInset(tabButton, -LSBDisplayTabButtonGap + 1e-7, -LSBDisplayTabButtonGap + 1e-7);
        assert(!CGRectIntersectsRect(display, padded) && (!home || !CGRectIntersectsRect(button, padded)));

        // A larger centered framebuffer must either clip a control or exceed the safe area.
        CGRect larger = CGRectInset(display, -0.001 * aspect, -0.001);
        CGRect lowerHome = CGRectOffset(button, 0, 0.001);
        assert(!CGRectContainsRect(available, larger) || CGRectIntersectsRect(larger, padded) ||
               (home && (!CGRectContainsRect(available, lowerHome) || CGRectIntersectsRect(lowerHome, padded))));
    } else {
        CGFloat groupBottom = home ? CGRectGetMaxY(button) : CGRectGetMaxY(display);
        CGFloat fraction = fullWidth ? 1 : 0.94;
        assert(near(display.origin.y - available.origin.y, CGRectGetMaxY(available) - groupBottom));
        assert(near(display.size.width, available.size.width * fraction) || near(groupBottom - display.origin.y, available.size.height * fraction));
    }
}

static void deviceAndOrientationTests(void) {
    const struct { CGSize pixels; bool home; } devices[] = {
        {{750, 1334}, true}, {{1242, 2208}, true}, {{750, 1334}, true}, {{828, 1792}, false},
        {{1290, 2796}, false}, {{1620, 2160}, true}, {{1640, 2360}, false}
    };
    const CGRect available[] = {
        {{0, 59}, {430, 790}}, {{59, 0}, {780, 350}}, {{20, 24}, {984, 1300}}, {{24, 20}, {1300, 900}},
        {{-50, -30}, {400, 900}}, {{12.25, 34.75}, {393.333333333, 761.666666667}},
        {{0, 57.333333333333336}, {932.6666666666666, 1013}},
        {{9.666666666666666, 47.666666666666664}, {100.66666666666667, 289.3333333333333}}
    };
    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i) {
        for (unsigned j = 0; j < sizeof(available) / sizeof(available[0]); ++j) {
            for (unsigned mode = 0; mode < 3; ++mode) {
                checkLayout(available[j], available[j], devices[i].pixels, mode > 0, devices[i].home, mode == 2);
                // A guest surface can rotate independently of the native phone's orientation.
                checkLayout(available[j], available[j], CGSizeMake(devices[i].pixels.height, devices[i].pixels.width),
                            mode > 0, devices[i].home, mode == 2);
            }
        }
    }
}

static void expectedGeometryTests(void) {
    LSBDisplayLayout full, fit;
    CGRect tall = CGRectMake(20, 50, 400, 900);
    CGSize phone = CGSizeMake(750, 1334);
    assert(LSBComputeSimulatorDisplayLayout(tall, tall, phone, true, true, LSBSurfaceWithTabs, &full));
    assert(full.displayFrame.origin.x == 20 && full.displayFrame.size.width == 400);
    assert(near(full.displayFrame.size.height, 400.0 * 1334 / 750));
    assert(near(full.displayFrame.origin.y, 50 + (900 - 400.0 * 1334 / 750 - 56) / 2));
    assert(LSBComputeSimulatorDisplayLayout(tall, tall, phone, false, true, LSBSurfaceWithTabs, &fit));
    assert(near(fit.displayFrame.size.width, 376));
    CGRect shortArea = CGRectMake(59, 0, 780, 350);
    assert(LSBComputeSimulatorDisplayLayout(shortArea, shortArea, phone, true, true, LSBSurfaceWithTabs, &full));
    assert(full.displayFrame.origin.y == 0 && full.displayFrame.size.height == 294);
    assert(near(full.displayFrame.size.width, 294.0 * 750 / 1334));
    assert(full.homeButtonFrame.origin.y == 302 && CGRectGetMaxY(full.homeButtonFrame) == 350);
    assert(LSBComputeSimulatorDisplayLayout(CGRectMake(0, 0, 100, 200), CGRectMake(0, 0, 100, 200), CGSizeMake(1, 2), false, false, LSBSurfaceWithTabs, &fit));
    assert(CGRectEqualToRect(fit.displayFrame, CGRectMake(3, 6, 94, 188))); // Original 94% fit, with no Home row.
}

static void tabButtonTests(void) {
    LSBDisplayLayout withTabs;
    // Measured host bounds and safe area from the iPhone 17 screenshot. Both physical margins must be equal.
    CGRect portrait = CGRectMake(0, 62, 440, 860);
    CGRect viewport = CGRectMake(0, 0, 440, 956);
    CGSize homeBarPhone = CGSizeMake(1290, 2796);
    assert(CGRectEqualToRect(LSBShowTabsButtonFrame(portrait, viewport), CGRectMake(384, 900, 44, 44)));
    assert(LSBComputeSimulatorDisplayLayout(portrait, viewport, homeBarPhone, true, false, LSBSurfaceWithTabButton, &withTabs));
    assert(near(withTabs.displayFrame.origin.y, 62) && near(956 - CGRectGetMaxY(withTabs.displayFrame), 62));
    assert(near(CGRectGetMidX(withTabs.displayFrame), 220) && near(withTabs.displayFrame.size.width, 832.0 * 1290 / 2796));

    // In landscape the horizontal cutout can make the safe area's center differ from the viewport's center.
    CGRect landscape = CGRectMake(60, 0, 800, 400);
    checkLayout(landscape, CGRectMake(0, 0, 880, 400), CGSizeMake(8, 3), true, true, true);

    // The largest centered image may fit beside the reveal button or above it, depending on the guest aspect.
    CGRect square = CGRectMake(0, 0, 300, 300);
    assert(CGRectEqualToRect(LSBShowTabsButtonFrame(square, square), CGRectMake(244, 244, 44, 44)));
    assert(LSBComputeSimulatorDisplayLayout(square, square, CGSizeMake(4, 5), true, false, LSBSurfaceWithTabButton, &withTabs));
    assert(near(withTabs.displayFrame.size.width, 180) && near(withTabs.displayFrame.size.height, 225));
    assert(LSBComputeSimulatorDisplayLayout(square, square, CGSizeMake(5, 4), true, false, LSBSurfaceWithTabButton, &withTabs));
    assert(near(withTabs.displayFrame.size.width, 225) && near(withTabs.displayFrame.size.height, 180));

    // A large bottom obstruction, or an arrow whose hit box enters the middle gesture region, cannot use the bottom inset.
    CGRect obstructed = CGRectMake(0, 62, 440, 849); // 45pt of bottom inset on the 956pt viewport.
    assert(near(CGRectGetMaxY(LSBShowTabsButtonFrame(obstructed, viewport)), CGRectGetMaxY(obstructed)));
    CGRect narrow = CGRectMake(0, 0, 200, 266), narrowViewport = CGRectMake(0, 0, 200, 300);
    assert(near(CGRectGetMaxY(LSBShowTabsButtonFrame(narrow, narrowViewport)), CGRectGetMaxY(narrow)));
    CGRect cutout = CGRectMake(50, 0, 800, 390), landscapeViewport = CGRectMake(0, 0, 900, 424);
    assert(CGRectEqualToRect(LSBShowTabsButtonFrame(cutout, landscapeViewport), CGRectMake(806, 368, 44, 44)));

    CGRect small = CGRectMake(0, 0, 90, 240);
    checkLayout(small, small, CGSizeMake(1, 3), true, true, true);
    CGRect cramped = CGRectMake(0, 0, 90, 200);
    assert(!LSBComputeSimulatorDisplayLayout(cramped, cramped, CGSizeMake(1, 3), true, true, LSBSurfaceWithTabButton, &withTabs));
    assert(CGRectEqualToRect(withTabs.displayFrame, CGRectZero) && CGRectEqualToRect(withTabs.homeButtonFrame, CGRectZero));
    assert(CGRectEqualToRect(LSBShowTabsButtonFrame(CGRectMake(0, 0, 43, 900), viewport), CGRectZero));
    assert(CGRectEqualToRect(LSBShowTabsButtonFrame(CGRectMake(0, 0, 900, 43), CGRectMake(0, 0, 900, 43)), CGRectZero));
    CGRect minimal = CGRectMake(0, 0, 44, 44);
    assert(CGRectEqualToRect(LSBShowTabsButtonFrame(minimal, minimal), minimal));
    assert(!LSBComputeSimulatorDisplayLayout(minimal, minimal, homeBarPhone, true, false, LSBSurfaceWithTabButton, &withTabs));
    assert(CGRectEqualToRect(withTabs.displayFrame, CGRectZero) && CGRectEqualToRect(withTabs.homeButtonFrame, CGRectZero));
}

static void guidedAccessTests(void) {
    for (unsigned hideTabs = 0; hideTabs < 2; ++hideTabs) {
        LSBSurfacePresentation ordinary = hideTabs ? LSBSurfaceWithTabButton : LSBSurfaceWithTabs;
        for (unsigned flags = 0; flags < 8; ++flags) {
            bool guided = flags & 1, hostIndicator = flags & 2, guestIndicator = flags & 4;
            assert(LSBSelectSurfacePresentation(false, hideTabs, guided, hostIndicator, guestIndicator) == LSBSurfaceWithTabs);
            LSBSurfacePresentation expected = flags == 7 ? LSBSurfaceGuidedFullscreen : ordinary;
            assert(LSBSelectSurfacePresentation(true, hideTabs, guided, hostIndicator, guestIndicator) == expected);
        }

        CGRect portrait = CGRectMake(0, 0, 440, 956), safe = CGRectMake(0, 62, 440, 860);
        CGSize pixels = CGSizeMake(1290, 2796);
        LSBDisplayLayout before, guided, after;
        LSBSurfacePresentation mode = LSBSelectSurfacePresentation(true, hideTabs, false, true, true);
        assert(LSBComputeSimulatorDisplayLayout(safe, portrait, pixels, hideTabs, false, mode, &before));
        mode = LSBSelectSurfacePresentation(true, hideTabs, true, true, true);
        assert(LSBComputeSimulatorDisplayLayout(safe, portrait, pixels, hideTabs, false, mode, &guided));
        assert(near(guided.displayFrame.size.width, 440) && near(guided.displayFrame.size.height, 440.0 * 2796 / 1290));
        assert(CGPointEqualToPoint(midpoint(guided.displayFrame), midpoint(portrait)) && CGRectIsEmpty(guided.homeButtonFrame));
        assert(!CGRectContainsRect(safe, guided.displayFrame));
        mode = LSBSelectSurfacePresentation(true, hideTabs, false, true, true);
        assert(LSBComputeSimulatorDisplayLayout(safe, portrait, pixels, hideTabs, false, mode, &after));
        assert(CGRectEqualToRect(before.displayFrame, after.displayFrame) && CGRectContainsRect(safe, after.displayFrame));
    }

    CGRect landscape = CGRectMake(12, 18, 956, 440), safe = CGRectInset(landscape, 62, 20);
    LSBDisplayLayout layout;
    assert(LSBComputeSimulatorDisplayLayout(safe, landscape, CGSizeMake(2000, 1000), false, false, LSBSurfaceGuidedFullscreen, &layout));
    assert(CGRectEqualToRect(layout.displayFrame, CGRectMake(50, 18, 880, 440)) && CGRectIsEmpty(layout.homeButtonFrame));
    assert(!LSBComputeSimulatorDisplayLayout(safe, landscape, CGSizeMake(750, 1334), true, true, LSBSurfaceGuidedFullscreen, &layout));
    assert(CGRectEqualToRect(layout.displayFrame, CGRectZero) && CGRectEqualToRect(layout.homeButtonFrame, CGRectZero));
}

static void assertInvalid(CGRect available, CGSize pixels, bool home) {
    LSBDisplayLayout output = {CGRectMake(1, 2, 3, 4), CGRectMake(5, 6, 7, 8)};
    assert(!LSBComputeSimulatorDisplayLayout(available, available, pixels, true, home, LSBSurfaceWithTabs, &output));
    assert(CGRectEqualToRect(output.displayFrame, CGRectZero) && CGRectEqualToRect(output.homeButtonFrame, CGRectZero));
}

static void invalidAndExtremeTests(void) {
    CGRect available = CGRectMake(0, 0, 430, 790);
    CGSize pixels = CGSizeMake(750, 1334);
    assert(!LSBComputeSimulatorDisplayLayout(available, available, pixels, true, true, LSBSurfaceWithTabs, NULL));
    LSBDisplayLayout output;
    assert(!LSBComputeSimulatorDisplayLayout(available, available, pixels, false, false, LSBSurfaceWithTabButton, &output));
    const CGRect invalidViewports[] = {
        {{0, 0}, {NAN, 790}}, {{0, 0}, {430, INFINITY}}, {{0, 0}, {0, 790}}, {{-431, 0}, {430, 790}}, {{0, 791}, {430, 790}}
    };
    for (unsigned i = 0; i < sizeof(invalidViewports) / sizeof(invalidViewports[0]); ++i) {
        assert(!LSBComputeSimulatorDisplayLayout(available, invalidViewports[i], pixels, true, false, LSBSurfaceWithTabButton, &output));
        assert(CGRectEqualToRect(output.displayFrame, CGRectZero) && CGRectEqualToRect(output.homeButtonFrame, CGRectZero));
    }
    const CGRect invalid[] = {
        {{0, 0}, {0, 100}}, {{0, 0}, {100, 0}}, {{0, 0}, {-1, 100}}, {{0, 0}, {100, -1}},
        {{NAN, 0}, {100, 100}}, {{0, NAN}, {100, 100}}, {{INFINITY, 0}, {100, 100}}, {{0, 0}, {INFINITY, 100}},
        {{0, 0}, {100, NAN}}, {{DBL_MAX, 0}, {DBL_MAX, 100}}, {{0, DBL_MAX}, {100, DBL_MAX}}, {{1e30, 0}, {1, 100}}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assertInvalid(invalid[i], pixels, false);
    const CGSize invalidPixels[] = {{0, 1}, {1, 0}, {-1, 1}, {1, -1}, {NAN, 1}, {1, INFINITY}, {DBL_MAX, DBL_MIN}, {DBL_MIN, DBL_MAX}};
    for (unsigned i = 0; i < sizeof(invalidPixels) / sizeof(invalidPixels[0]); ++i) assertInvalid(available, invalidPixels[i], false);
    assertInvalid(CGRectMake(0, 0, 47, 900), pixels, true);
    assertInvalid(CGRectMake(0, 0, 400, 56), pixels, true);
    CGRect smallestWithHome = CGRectMake(0, 0, 48, 57);
    checkLayout(smallestWithHome, smallestWithHome, pixels, true, true, false); // Minimum button width and a nonzero guest image.
    CGRect tiny = CGRectMake(0, 0, 0.01, 0.02), large = CGRectMake(-1e6, 1e6, 1e6, 2e6);
    checkLayout(tiny, tiny, CGSizeMake(1, 2), true, false, false);
    checkLayout(large, large, pixels, true, false, false);
}

int main(void) {
    deviceAndOrientationTests();
    expectedGeometryTests();
    tabButtonTests();
    guidedAccessTests();
    invalidAndExtremeTests();
    puts("SimulatorDisplayLayoutTests passed: seven profiles, Guided Access transitions, viewport centering, controls, and invalid bounds");
    return 0;
}
