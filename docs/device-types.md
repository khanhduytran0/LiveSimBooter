# Simulator device types

The native **Settings → Simulator Device** picker selects a fixed device profile. Selecting a row is a draft; **Apply**
commits it. Before Start, Apply saves the selection for the next boot. During a graphical session, Apply closes its running
guest apps, drains the host's owned guest and native-helper processes, installs the selected capabilities, and starts a fresh
simulator session. The `data` root is preserved. The native IOSurface, Fish, and Settings tabs remain in place.

The default remains **iPhone 15 Pro Max**. The simulated device is independent of the physical host iPhone.

For a Home-button profile or either iPad, **Settings → Use Full Width** removes the normal presentation inset. This saved
option takes effect immediately without rebooting or changing the guest's resolution. The display keeps its aspect ratio
and fits the native safe area; if height is limiting, it uses the largest width that still fits. Home-indicator iPhones keep
the existing inset layout. Switching profiles preserves the saved option but applies it only to eligible devices.

A virtual **Home** button appears below the guest screen for iPhone 8, 8 Plus, SE2 and iPad (9th generation). It reserves a
48-point touch target outside the framebuffer, so it does not cover simulator content. The layout keeps full width when
the screen and button fit vertically, otherwise reducing the screen just enough to fit both. There is no extra button for
Home-indicator profiles.

The button sends stock Indigo Home down/up events to the authenticated simulator backboardd, never to the physical phone.
Releases on drag-out, cancellation, tab/window inactivity and session replacement target the server that accepted the down.
Queued releases are retried within that connection generation and cannot leak into a replacement session. VoiceOver activation
sends one press/release pair. The wire format follows SimulatorKit's `IndigoHIDMessageForButton(0, operation, 51)` and
SimulatorHID 1171.7's consumer-page Menu/Home mapping; the implementation records the inspected framework UUID and offsets.

| Device | Identifier | Framebuffer pixels | Scale | Interface |
| --- | --- | --- | ---: | --- |
| iPhone 8 | `iphone-8` | 750 × 1334 | 2 | Home button; experimental |
| iPhone 8 Plus | `iphone-8-plus` | 1242 × 2208 | 3 | Home button; experimental |
| iPhone SE (2nd generation) | `iphone-se-2` | 750 × 1334 | 2 | Home button |
| iPhone 11 | `iphone-11` | 828 × 1792 | 2 | Home indicator |
| iPhone 15 Pro Max | `iphone-15-pro-max` | 1290 × 2796 | 3 | Dynamic Island |
| iPad (9th generation) | `ipad-9` | 1620 × 2160 | 2 | Home button |
| iPad (A16) | `ipad-a16` | 1640 × 2360 | 2 | Home indicator |

iPhone 8 and 8 Plus are deliberately offered although their original device profiles exclude iOS 26. Their experimental
label is not a claim that they boot this runtime. The 8 Plus uses its simulator rendering size, not its physical panel's
1080 × 1920 dimensions. Selecting a profile also selects its original artwork traits, model/product identity, display pitch,
and device capabilities; changing only the framebuffer dimensions would leave these inconsistent.

## Stage original profiles

Apple capabilities files are separately supplied setup inputs. They are not embedded in the app or committed to Git.
Use the fixed **iOS 26.0 / 23A343** runtime from the [setup guide](agent-setup.md); this picker does not select another OS version.

With `LSB_WORK` from that guide, validate and stage all seven original CoreSimulator device profiles into a **new** directory:

```sh
python3 Tools/setup-ios26-device-types.py \
  --device-types-root /Library/Developer/CoreSimulator/Profiles/DeviceTypes \
  --output-dir "$LSB_WORK/device-types"
```

The source directory may differ with the Xcode installation. The tool validates each original bundle identifier, model,
architecture, runtime support range, primary display, and relevant capability types before writing anything. `--dry-run`
prints the inventory without creating the output. Existing output directories and symlinked source files are refused.
The manifest records provenance and hashes; the copied `capabilities.plist` bytes are unchanged.

Transfer the staged `RuntimeRoot/Library/Developer/CoreSimulator/DeviceTypes/` subtree into the corresponding App Group
`RuntimeRoot`, following the [migration/copy procedure](migration.md). Its layout is:

```text
RuntimeRoot/Library/Developer/CoreSimulator/DeviceTypes/<identifier>/capabilities.plist
```

Keep these staged originals. On activation, the host atomically copies the selected bytes to the singular active path:

```text
RuntimeRoot/Library/Developer/CoreSimulator/DeviceType/capabilities.plist
```

`SIMULATOR_CAPABILITIES` continues to point to that singular file. The host replaces it only before guest startup or after
all old session processes have exited, because guest frameworks cache capabilities. Original profile files remain unchanged.
For an older single-profile setup, only the default device may use the existing singular file as its source; other selections
require their staged originals. Missing, malformed, mismatched, or symlinked profiles fail preflight before stopping the session.

## Session and rendering boundaries

[DeviceType.m](../Host/DeviceType.m) owns the catalog; [DeviceConfiguration.m](../Host/DeviceConfiguration.m) validates staged
data and installs the active copy. A saved unknown preference falls back to the default catalog entry. No preference value
is interpreted as a file path or shell command.

The broker snapshots one profile for each session and uses it for guest environment and framebuffer construction. The
[framebuffer server](../Host/FramebufferServer.h) copies width, height, scale, and pitch into immutable geometry. Swapchains,
surface dimensions, and dirty rectangles must match that session. On replacement, the IOSurface tab clears the previous
presentation and recomputes its fitted display frame; rendering and touch coordinates already follow the actual surface viewport.

Apply is unavailable while startup or another restart is in progress. Preflight, drain, and synchronous setup failures are
reported. Successful Apply means the replacement session was queued and is starting; it does not confirm later guest boot
or readiness. It uses the same owned-session teardown as [guest userspace reboot](guest-control.md), without erasing guest
preferences, containers, or app data.

## Validation scope

Local tests cover profile lookup, typed capability validation, default-only legacy fallback, no-follow directory/file opens,
nonregular inputs, and atomic installation preserving staged originals and existing readers. Framebuffer tests exercise
alternate geometries through real Mach messages and IOSurfaces, including rejection of stale-size swapchains and surfaces.
These checks do not establish that every listed device boots, lays out every app correctly, or survives a device-type switch
on an iPhone; those require separate device verification.

### Device verification (2026-10-27)

Pinned iOS Simulator 26.0 / 23A343 on iPhone18,2 running iOS 27.0 / 24A435, without LLDB. The native host cache remained
loaded. The existing simulator cache was unchanged: base `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`.
The rebuilt signed host UUID was `307AFFE7-FBD7-3439-9565-01F6B3BA2953`; the extension and guest shim remained the versions
recorded in [userspace reboot verification](guest-control.md#device-verification-2026-10-27). Seven original capabilities
files were staged, totaling 29,986 bytes. The previous active capabilities and host package were preserved for rollback.
Transfers followed the requested no-checksum policy; the host validated the selected plist before each Apply.

The user selected each following model through native Settings and pressed Apply, keeping native host PID **13888**:

| Session | Native launchd PID | Observed framebuffer | Result |
| --- | ---: | --- | --- |
| Initial iPhone 15 Pro Max | 13892 | 1290 × 2796 | Existing graphical baseline |
| Apply iPhone 8 | 13954 | 750 × 1334 | User confirmed boot into Home-button layout |
| Apply iPad (9th generation) | 14017 | 1620 × 2160 | User confirmed usable tablet layout |
| Apply iPad (A16) | 14084 | 1640 × 2360 | User confirmed usable tablet layout |

Each replacement registered a new backboardd client and swapchain of the expected size. Launchd Self/Child environment
readback matched the selected model, product, resolution, scale and pitch. The iPhone 8 and final iPad process inventories
shared only the native host PID, and no owned target was stopped. The final session was left on iPad (A16). This verifies
the three device switches above; iPhone 8 Plus, SE2 and iPhone 11 have local profile/configuration coverage but were not
separately boot-tested in this run. iPhone 8 compatibility remains experimental even though this boot passed.

The integrated `sh Tests/run.sh` suite, iPhoneOS build/signature checks and `git diff --check` passed. Evidence is under
ignored `tmp/device-picker/`, with console output in `tmp/userspace-reboot/device-picker-baseline-console.log`.

### Full-width presentation and virtual Home verification (2026-10-27)

The follow-up signed host UUID was `A13D0D48-6F67-3BE5-8453-A1D2CA5EBAA2`, installed on the same iPhone18,2 with the
same pinned runtime and cache. No guest binary or cache replacement was needed. Native host PID **14327** booted the saved
iPad (A16) profile, then remained alive through the user's Apply to iPhone 8 Plus. The replacement session registered a
1242 × 2208 framebuffer and reported model `iPhone10,5`, product `D21`, scale 3 and pitch 461.

The user confirmed that full-width presentation and the virtual Home action both work. The iPhone 8 Plus screenshot shows
the Home button below the framebuffer and above the native tabs; the screen is slightly width-limited to fit that combined
height. A reported double-press failure was attributed by the user to pressing too slowly; the stock guest timing remains
unchanged. No special double-click packet or host-side app-switcher action was added.

The integrated local suite passed, including seven-profile layout coverage, fractional safe-area bounds, actual-Mach Home
packet/authentication tests, queued-release retries and session replacement. The iPhoneOS build, signature verification and
`git diff --check` passed. No LLDB was attached, and the final owned-process inventory contained no stopped process. Evidence
is under ignored `tmp/full-width-home/` and `tmp/userspace-reboot/full-width-home-baseline-console.log`.
