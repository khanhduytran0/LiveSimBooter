# Backboardd and the simulator UI cache

The current native services cover Indigo, framebuffer, IOSurface and an isolated Metal helper. Stock backboardd now produces
a real frame that reaches the host image view: a spinner on black at 1290 × 2796. Following the Indigo target and boot-generation
fixes, physical touches produce readable stock touch markers in that display. An earlier partial-update trial reached 300
frames before aborting on the then-invalid HID target; that failure is retained below as history. An offscreen probe also compiled
shader source through the stock guest compiler and rendered a triangle with verified pixels. Apple-logo/progress rendering
and a complete simulator boot remain unverified. See [the first stock frame](#first-stock-backboardd-frame) and
[the Metal results](metal.md#latest-phone-results).
The earlier backboardd traces remain chronological evidence of the separate boot/resource failures.

## Cache coverage

The earlier iOS 26 simulator cache contains **219 images**. It includes CoreGraphics and QuartzCore but does not include
UIKit or UIKitCore. The initial backboardd staging added 137 required loose libraries and two more libraries for an AVFAudio
weak re-export. The currently deployed expanded cache replaces those loose dependencies. These are simulator runtime
contents; the native foreground host separately uses the device's UIKit.

On 2026-10-25, a new cache was built from original iOS 26 simulator **23A343** libraries. Its inputs combine the existing
cache, UIKit's dependencies, and backboardd's dependencies, recursively including every weak dependency available in that
runtime. Install-name aliases, including the Cryptex WebKit paths, are resolved before traversal. The result is **545
images**: 542 original runtime libraries plus the three host support libraries supplied by Apple's simulator cache builder.
All expected images are present, including UIKit, UIKitCore, AudioSession, AVFAudio, and every required backboardd dependency.

The only unresolved names in this traversal are weak dependencies absent from the original runtime:

- `/usr/lib/libobjc-env.dylib`
- `/System/Library/PrivateFrameworks/RemoteProcessingBlock.framework/RemoteProcessingBlock`
- `/System/Library/PrivateFrameworks/MLCompilerRuntime.framework/Libraries/libmlc_rt.dylib`
- `/usr/appleinternal/lib/liblinkguard.dylib`

This is a dependency subset for UIKit and backboardd, not a complete simulator installation. Dynamic library loads,
framework resources, service configuration, and the services needed to bring up a user interface still require separate
validation. In particular, including UIKit in a cache does not establish that its initialization succeeds on the device.

## Measured address-space use

These measurements come from the cache headers and every mapping in every subcache, rather than filesystem sizes. The
reserved span includes internal gaps and the final 16 KiB reservation beyond the last mapping.

| Cache | Images | Reserved span | Base | Reserved end |
| --- | ---: | ---: | --- | --- |
| Earlier iOS 26 subset | 219 | 341.03125 MiB (`0x15508000`) | `0x120000000` | `0x135508000` |
| Expanded UIKit/backboardd subset | 545 | 1,067.265625 MiB (`0x42b44000`) | `0x120000000` | `0x162b44000` |
| Full stock iOS 26 cache | 3,221 | 4,076.03125 MiB (`0xfec08000`) | `0x180000000` | `0x27ec08000` |
| Full stock iOS 27 cache | 1,826 | 3,772.796875 MiB (`0xebccc000`) | `0x1c0000000` | `0x2abccc000` |

The expanded cache fits below the native shared-region base and leaves **468.734375 MiB** between its reserved end and
`0x180000000`. Its six mappings occupy 1,003.25 MiB; two internal gaps total 64 MiB. Its last mapping ends at `0x162b40000`.
The unsigned builder output occupies 1,054,048,256 bytes on disk. Signing expands the signature allocation, producing a
1,060,225,024-byte file without changing any mapping or the reserved VM span.

The full iOS 26 cache has 12 mappings across its main file and `.01` subcache. They span
`[0x180000000, 0x27ec04000)`: 4,076.015625 MiB, including 128 MiB of internal gaps. At `0x120000000`, its complete reservation
would end at `0x21ec08000`, well past the native shared-region boundary. Its executable mappings alone occupy
2,380.171875 MiB, so moving only data and LINKEDIT elsewhere would still not fit the unchanged executable mappings in the
entire theoretical 2 GiB low window. The stock builder also omitted SpringBoard.framework because its full cache overflowed;
even that full cache does not contain every runtime image.

The full iOS 27 cache similarly does not fit: at `0x120000000`, its reserved end would be `0x20bccc000`. Its relocation
metadata limitations are documented in [iOS 27 relocation](ios27-relocation.md).

## Why deallocating page zero does not provide another 4 GiB

Placing the full iOS 26 cache at `0x01000000` would mathematically end at `0xffc08000`, below the app's preferred text address.
However, the native LiveProcess executable has `__PAGEZERO = [0, 0x100000000)`. XNU implements this segment by raising the
task VM map's minimum address, rather than inserting an ordinary removable mapping. The arm64 executable loader requires
that minimum to cover at least 4 GiB, and PIE can extend it further. Removing mappings therefore does not open this range;
the kernel's minimum-address update also rejects lowering the bound. These are source findings, not a new device mapping
experiment. See Apple's [Mach-O loader](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/mach_loader.c)
(`load_machfile`, `load_segment`) and [VM map implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_map.c)
(`vm_map_raise_min_offset`).

## Build and signature validation

The existing patched 23A343 builder was reused from `tmp/minimal-cache-compare/tools/update_dyld_sim_shared_cache`. Its base
selection remains `0x120000000`; the existing dyld_sim cache-base and cleanup-size patches do not depend on the image count.
The source libraries were selected individually from the original runtime, with no extracted-cache libraries or patched
loose host-support copies used as builder inputs. Absolute symlinks to external inputs were not enumerated by the builder,
so the staging script copies only the 542 selected originals and dyld_sim.

```sh
WORK="$PWD/tmp/backboardd/cache-ui"
./tmp/minimal-cache-compare/tools/update_dyld_sim_shared_cache \
  -root "$WORK/root" -cache_dir "$WORK/unsigned" -iOS -arch arm64 -force -verbose
```

The completed cache was signed with identity `D7610EDC80D34A9B1A3D3C8563D66A5CD4716293`, identifier
`com.kdt.LiveSimBooter`, and team `UU7W52U79B`, using [the cache signer](../Tools/sign-dyld-cache.py). Independent inspection
confirmed:

- CodeDirectory version `0x20400`, SHA-256, 16 KiB pages, 64,208 valid code-page hashes, no scatter table, and no entitlements.
- Executable-segment flags are zero, and its extent matches the executable cache mapping.
- Signature offset 1,051,983,872 plus capacity 8,241,152 equals the signed file size. The 2,060,041-byte signature superblob
  fits, with unused signature capacity zero-filled.
- The cache payload matches the unsigned output byte for byte except for the header's signature-size field. Mapping tables
  and the reserved range are unchanged.
- `codesign --verify --strict --verbose=4` passes with host trust-service access. A sandboxed verification first reported
  `CSSMERR_TP_NOT_TRUSTED`; verification outside the sandbox succeeded.

Signed cache SHA-256: `493ad467f2032d283608678002b888edb618f4736f174458f64c3be38384ea24`.
CDHash: `2f91969b23198c4c8d31b0d876aea7f3441a8704`.

Local evidence is under ignored `tmp/backboardd/cache-ui/`: `stage.py`, `stage-manifest.json`, `build.log`, `sign.log`,
`validation.json`, and the `unsigned/` and `signed/` outputs. The full iOS 26 measurements use the original cache under
`/Library/Developer/CoreSimulator/Caches/dyld/26A434/com.apple.CoreSimulator.SimRuntime.iOS-26-0.23A343/`; the iOS 27 measurements
use the 24A434 runtime's `RuntimeRoot/System/Library/Caches/com.apple.dyld/`.

## Device validation of the expanded cache

The signed 545-image cache is now installed on the iOS 26.1 test phone, with its SHA-256 verified after transfer. The 139
loose backboardd dependencies were moved out of RuntimeRoot into the reversible App Group backup
`Library/CacheTrials/UIKit545`. This makes the cache responsible for those dependencies during the test.

LLDB attached to simulator launchd PID 80819 and observed executable cache TEXT at `[0x120000000, 0x146148000)`, with `r-x`
protection and zero dirty pages. The native shared region `[0x180000000, 0x300000000)` remained inaccessible. Detaching that
debugger caused the already documented native-workqueue SIGBUS after exception-port restoration; this was not a failure to
execute the expanded cache. See [the detach limitation](launchd.md#final-lldb-validation-and-startup-storage).

A fresh run used launchd 80841, xpcproxy/guest PID 80844, and replacement df native PID 80847. LLDB caught df calling the
cache's `getmntinfo` at `0x145ef2984` and exiting 0. The broker published wait status 0 and submitted SIGCHLD to launchd;
this trial did not stop on the parent's wait. This establishes mapping and execution of the expanded signed cache with the
loose dependencies absent. It does not establish successful UIKit initialization or a complete backboardd boot.
Evidence: `activate.log`, `lldb-launchd.txt`, `console-df-fresh.log`, and `lldb-df.txt` under `tmp/backboardd/cache-ui/`.

## Backboardd startup and the first service failure

`--launchd-backboardd` creates an isolated RunAtLoad job from the stock 23A343 `/usr/libexec/backboardd`. The generated plist
imports the stock job's 21 MachServices entries, uses `KeepAlive = false` and `EnableTransactions = false`, and omits scheduler
and identity attributes that the broker does not implement. It writes to `data/Library/Logs/backboardd.log` under the selected
group's `LSB/iOS/23A343` directory; the historical trials below used the old `SimulatorData` root name.
The stock executable's `LC_MAIN` offset is `0xb828`; its UUID is `385892A3-EF4B-362A-BAFF-02FE702C41DD`.
The current configuration also loads the matching `com.apple.cfprefsd.xpc.daemon` job and atomically enables
`BKDigitizerVisualizeTouches` in the isolated simulator preferences. See [cfprefsd staging and validation](cfprefsd.md).
The earlier startup trials below predate that addition.

The initial loose-library run first failed to resolve `_OBJC_CLASS_$_AVAudioSession`. AVFAudio re-exports that symbol through
its weak AudioSession dependency; importing only required load commands therefore missed a real dependency. Adding
AudioSession and libSessionUtility let backboardd reach main. The expanded cache now includes available weak dependencies
recursively. The original staging manifests are `tmp/backboardd/dependencies.json` and `reexport-dependencies.json`.

Backboardd then exposed two native-identity assumptions after SETEXEC. Firehose queried `proc_pidinfo` using the retired
guest PID, and libxpc compared `getpid()` with a native audit token. The adapters now translate self process-info queries to
the current native PID and give direct libxpc `getpid()` calls that native identity. Ordinary guest code retains its stable
guest PID. Kernel audit tokens and unique process identities remain unchanged; see [the identity boundary](launchd.md#native-identity-and-service-ownership).

With those fixes on the phone, native PID 80379 retained guest PID 80376 and progressed to the following failure:

```text
IOHIDEventSystemCreate:
  bootstrap_check_in("com.apple.iohideventsystem") -> 1100 (BOOTSTRAP_NOT_PRIVILEGED)

-[BKHIDSystem startHIDSystem] + 476
backboardd + 0xb530
backboardd + 0xc1e8
backboardd + 0xb530
backboardd + 0xbb08
```

The fatal message was `IOHIDEventSystem creation failed`. Stock launchd's check-in handler reads the caller PID directly
from the audit token and compares it with the job PID it stored before xpcproxy's replacement. The broker's logical PID
preservation alone did not update that launchd ownership check. The signed owner-translation adapter documented below now
addresses this mismatch without changing kernel audit tokens.
Evidence is in `tmp/backboardd/lldb-identity-fixed.txt`, `lldb-hid-checkin.txt`, and `lldb-hid-failure.txt`.
No IOSurface or Metal service request was observed before this failure.

## Host Indigo input and presentation preparation

The native host now owns an `IndigoHIDRegistrationPort` receiver for the backboardd mode. It registers that endpoint with
the isolated simulator launchd through a bounded MIG 701 transaction and authenticates the reply against launchd's full
native audit token. The broker authorizes only the selected backboardd instance's exact native audit token before guest
entry. Registration accepts the simulator's header-only event-port handoff, retains that send right, and bounds message
parsing and queue draining. It does not open or capture the phone's native IOHID event system.

`SIMULATOR_HID_SYSTEM_MANAGER` points to `RuntimeRoot/Library/Frameworks/SimulatorHID.framework`. The staged plugin comes
from the installed CoreSimulator framework, version 1171.7, built for iOS Simulator with the iOS 27 SDK. Its arm64 imports
were checked against the iOS 26 libraries and it was signed with the same identity and identifier as the runtime. This
static check does not establish full cross-version runtime compatibility. No matching older plugin was found in the
installed Xcode locations. Staging evidence is in `tmp/backboardd/hid-plugin/manifest.json`.

Each prepared simulator boot supplies `SIMULATOR_BOOT_TIME` to launchd and its children, replacing any inherited stale value.
It follows CoreSimulator 1171.7's `createLaunchdJobWithBinpref:...` implementation: the current Unix timestamp truncated to
unsigned integer seconds and formatted with `%lu`. SimulatorHID reads that string as its boot generation and persists it
with dynamic HID services in `data/var/run/SimHIDDynamicServices.plist`. Without it, the first successful built-in
digitizer creation reached `ScreenTouchService.PurpleMain`, then raised an exception while inserting a nil generation into
the persistence dictionary. Supplying the normal boot environment addresses this prerequisite; no custom SimulatorHID
library patch is needed. Evidence: `tmp/metal-native/guest-cfprefsd-indigo.log` and
`tmp/backboardd/hid-plugin/coresimulator-environment-disassembly.txt` (producer `0x3aad4`–`0x3ab08`; receiver
`_currentBootGeneration` and `_locked_saveDynamicServicesToDisk`).

The subsequent stock touch-visualization path also needed matching `usr/lib/libobjc-trampolines.dylib` plus the Helvetica
and LastResort font data under `System/Library/Fonts/Core`. These files have been staged and hash-verified. Physical touches
produced visualizer label/dirty updates, and the final phone screenshot confirms readable stock marker text after the font copy.
See [the minimal visualization assets and measured preferences result](cfprefsd.md#touch-visualization-runtime-assets).

`LSBSimulatorDisplayView` can snapshot a completed nonplanar 32BGRA IOSurface into a native UIImage. It checks the format,
stride and allocation size, copies under an IOSurface read lock, and retains no surface after the call. The view aspect-fits
that image. Only touches beginning inside the displayed image on that view are forwarded as normalized Indigo digitizer
events, with stable touch identities; letterbox and unrelated view touches are ignored. Clearing or detaching the view,
changing its geometry, and app inactivity cancel active contacts. If the Mach queue is full, the server retains the pending
cancellation, retries nonblocking on its serial queue, and rejects new touches until cancellation succeeds or the client
disconnects.

Native tests cover endpoint wire layout, audit authentication, malformed requests/replies, deadlines and Mach-right cleanup;
Indigo tests cover touch sequences, backpressure, cancellation retries and reauthorization. Display geometry and surface
validation tests also pass. The complete host suite passed in `tmp/backboardd/tests-final.log`; focused Indigo tests passed
ASan/UBSan and the server/helper compile for arm64 iPhoneOS.

On the phone, native host 80851 successfully registered the endpoint with simulator launchd 80852:
`[Indigo] registration error=0 bootstrap=0`. Its proxy 80856 then prepared backboardd replacement 80860. This confirms the
host-to-launchd endpoint registration. In this run, the earlier `com.apple.iohideventsystem` ownership failure still blocked
the guest event-port handshake and view-to-guest touch delivery. Evidence is in `tmp/backboardd/cache-ui/console-indigo.log`.

The cache-only backboardd 80860 run then reproduced that failure under hardware-only LLDB: `bootstrap_check_in` returned
1100 at `IOKit` address `0x12708fa64`, and the fatal stop was
`0x1304df830 BackBoardHIDEventFoundation\`-[BKHIDSystem startHIDSystem] + 476`. The next frames were backboardd offsets
`0xb530`, `0xc1e8`, `0xb530`, `0xbb08`, then `dyld_sim\`start_sim + 20`. The crash message remained
`IOHIDEventSystem creation failed`. Cache TEXT was executable with zero dirty pages, and the native shared region remained
unmapped. The SSH amfid/TXM capture contained no rejection or TXM event. Evidence is in
`tmp/backboardd/cache-ui/lldb-backboardd.txt` and `tmp/backboardd/security.log`.

After installing the final host build, a fresh run without a debugger again registered Indigo successfully. Host 80877 and
simulator launchd 80878 remained running after replacement backboardd 80880 exited with signal status 5. The broker
published its exit under guest PID 80879, and launchd still answered the authenticated health lookup
(`error=0 bootstrap=1102`). This confirms the remaining failure also occurs without LLDB. The final installed build includes
the explicit ENOTSUP guard for exec by an Indigo-authorized process. Evidence: `tmp/backboardd/console-final-normal.log`.

## Signed owner translation and HID client identity

The first phone trial of the [signed launchd adapter](launchd.md#reproducing-the-signed-launchd-adapter) used host 80924,
simulator launchd 80925, proxy/guest PID 80928, and replacement backboardd native PID 80931. The broker authenticated the
native audit identity and resolved it to guest PID 80928. Both HID `bootstrap_check_in` calls returned 0 under hardware-only
LLDB, and `IOHIDEventSystemCreate` returned the nonnull object `0x104707010`. This confirms that owner translation passed the
previous service check-in failure while preserving the original audit tokens.

Backboardd then stopped at a new assertion in `IOHIDEventSystemClient.c:589`:

```text
__IOHIDEventSystemClientRefresh: kern_result == KERN_SUCCESS

__assert_rtn
__IOHIDEventSystemClientRefresh.cold.5 + 40
__IOHIDEventSystemClientRefresh + 1528
IOHIDEventSystemClientCreateWithType + 800
-[BKHIDSystem startHIDSystem] + 84
backboardd + 0xb530
backboardd + 0xc1e8
backboardd + 0xb530
backboardd + 0xbb08
dyld_sim`start_sim + 20
```

The failing native operation was `task_name_for_pid(mach_task_self(), getpid(), &port)`: ordinary guest `getpid()` still
returned the stable proxy PID, which is not the running replacement's kernel PID. The new `LSBTaskNameForPID` wrapper in
`Guest/ProcessIdentity.c` translates only a positive guest-self PID when the target is the current task. It calls the real
kernel API with the native PID and preserves that API's returned task-name right and result; other targets and PIDs are
unchanged. This does not grant access to another process or replace a kernel-generated identity.

The wrapper passed native unit tests, ASan/UBSan, and a live macOS self-task lookup. In the second phone run, backboardd 80959
passed both HID check-ins and the self task-name lookup with result 0. The host then logged
`[indigo] guest event port registered native=80959`. This establishes the guest event-port handshake, but not view-to-guest
touch delivery. The first run's evidence is in `tmp/launchd-identity/lldb-first-assert.txt` and `console-lldb.log`; the second
run's host log is `console-self-task.log`. The earlier HID initialization failures preceded SimulatorHID startup and did not
demonstrate plugin incompatibility.

## Initial simulator device profile

After the Indigo handshake, backboardd advanced through `AWStartStandaloneServer` into CMCapture.
`FigCaptureGetModelSpecificName` passed the result of `getenv("SIMULATOR_PRODUCT_CLASS")` to NSString, raising an exception
because the environment value was absent. The host now supplies `SIMULATOR_PRODUCT_CLASS=D84` and
`SIMULATOR_MODEL_IDENTIFIER=iPhone16,2` for the initial iPhone 15 Pro Max guest profile. These exact values come from
`iPhone 15 Pro Max.simdevicetype/Contents/Resources/profile.plist` under
`/Library/Developer/CoreSimulator/Profiles/DeviceTypes/`; CoreSimulator's `-[SimDeviceType environment]` copies the profile's
productClass directly into that environment key. The value is `D84`, not a guessed board identifier with an `AP` suffix.

Explicit inherited values for either key override its default in both launchd and child environments. The first occurrence
wins, matching getenv; empty or invalid UTF-8 values fail configuration with EINVAL before any configuration is published.
Focused tests cover defaults, overrides, duplicate handling and these failures. This supplies only the two identity keys;
display dimensions and capability resources are not added.

The next normal phone launch used host 81006, simulator launchd 81007, guest/proxy PID 81008, and replacement backboardd
native PID 81009. It passed the model-name failure and HID initialization, and the host logged
`[indigo] guest event port registered native=81009`. Backboardd then reached QuartzCore's simulator display discovery.
This confirms the initial profile and the Indigo handshake together in a run started without LLDB. It does not establish
display output or touch delivery. Evidence: `tmp/launchd-identity/console-profile-normal.log` and `backboardd-profile-normal.log`.

## Framebuffer provider selection

Backboardd 81009 consumed a CPU while entering `CAWindowServer`. Attaching LLDB found this repeating tail branch:

```text
0x13abcbcc8  SimFramebufferClient`SFBConnectionCreate + 56
0x13abcbccc  br x2
x2 = 0x13abcbc90  SimFramebufferClient`SFBConnectionCreate
```

The cached framework is a dispatch trampoline. Its initializer calls
`dlopen(getenv("SIMULATOR_FRAMEBUFFER_FRAMEWORK"), RTLD_LAZY)` without checking for a missing environment value.
The unset value became `dlopen(NULL)`, so `dlsym` found the trampoline's own exports and every call branched back to itself.
This failure did not require incorrect loader interposition. The stack continued through `-[CAWindowServer _detectSimDisplays]`
and `+[CAWindowServer serverWithOptions:]`. Evidence: `tmp/launchd-identity/lldb-framebuffer.txt`.

Backboardd configuration now supplies an absolute path beneath the canonical RuntimeRoot in both the launchd `Self` and
`Child` environments:

```text
SIMULATOR_FRAMEBUFFER_FRAMEWORK=<RuntimeRoot>/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer
```

The value is controlled by the host; inherited duplicates cannot redirect it to the cached SimFramebufferClient or clear it.
Other launchd modes do not add this setting. Focused configuration tests cover canonicalization, the two scopes, duplicate
removal and the mode boundary.

The provider to deploy at that path is the real arm64 simulator framework from
`/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A/Resources/Platforms/iphoneos/Library/PrivateFrameworks/SimFramebuffer.framework`.
Its version is 1171.7, minimum OS 15.0, built with the iOS 27 SDK. It implements the trampoline's exports and connects to the
bootstrap service `com.apple.CoreSimulator.SimFramebufferServer`. In the phone trial with backboardd 81070, the dispatch table
resolved to the real provider, and `SFBConnectionCreate` returned object `0x106b228c0`. `SFBConnectionConnect` returned false
with error object `0x1075041b0`; the exact bootstrap lookup result was not captured. Backboardd continued past this failure,
so an unavailable framebuffer connection is nonfatal on this startup path. Evidence: `tmp/launchd-identity/lldb-framebuffer-provider.txt`.

The stock client's Mach exchange, packed structures, error replies and implemented host lifetime rules are recorded in
[framebuffer transport](framebuffer.md).

## Runtime resources after framebuffer discovery

Backboardd 81070 next raised `NSInternalInconsistencyException` while reading the absent
`/System/Library/BackBoard/EventProcessorConfiguration.plist`, at `BKHIDCreatePrimaryEventProcessor + 496`. The matching stock
23A343 resource was copied into RuntimeRoot. A subsequent normal launch used host 81093, simulator launchd 81094, guest PID
81095 and backboardd native PID 81096. It registered the Indigo event port and remained alive after approximately 22 seconds
with reported CPU use 0.0%. This is a process-liveness observation, not proof of a working display or a complete simulator.
Evidence: `tmp/launchd-identity/backboardd-framebuffer-provider.log`, `console-resources-normal.log`, and `resources-normal-state.log`.

Attaching LLDB to 81096 confirmed that its main thread reached `CFRunLoopRun`, through backboardd offset `0xbf90` and
`dyld_sim`'s `start_sim`. The render-server thread was waiting in `CA::Render::Server::server_thread`. A separate worker was
waiting for a synchronous DataMigration XPC reply in `_DMPerformMigration + 292`; startup is therefore not fully complete.
The transcript is `tmp/launchd-identity/lldb-backboardd-idle.txt`. The debugged process was terminated after capture.
The matching stock `System/Library/CoreServices/SpringBoard.app/US.uchr` resource was also copied after its missing-file
diagnostic. Both resource files come from the iOS 26.0 23A343 RuntimeRoot; they are data and require no executable signature.

The complete `sh Tests/run.sh` regression suite and signed device build passed after these fixes. The SSH amfid/TXM capture
contained no rejection or TXM event (`tmp/launchd-identity/security-test.log`). LLDB's internal image-notification breakpoint
can loop during handoff: the test used hardware-only breakpoints, paused the guest, disabled the internal image/thread
breakpoints, then stepped past the notifier before continuing. This workaround does not fix LLDB's notification handling.

The host now connects its view to the framebuffer service and uses Apple's native `IOSurfaceRemoteServer` for guest
surface allocation. See [the IOSurface service](iosurface.md) and [framebuffer transport](framebuffer.md) for their tests and
limits. A separate native Metal service has also completed a shader-based triangle and shared pixel readback, as recorded below. The 545-image
cache remains deployed; the earlier 219-image cache and the 139 loose dependencies are retained for reversal.

## Native Metal service trial

`--launchd-backboardd` now starts a host-selected native LiveProcess helper before simulator launchd. This role retains the
native cache, loads the fixed bundled backend, and returns its listener send right in authenticated READY. The broker
registers `com.apple.metal.simulator` and `com.apple.metal.simulator.backboardd` alongside Indigo, IOSurface and framebuffer.
The xpcproxy instance may run while registration completes; its backboardd SETEXEC replacement waits for all five results.

The helper receives a read-only authorization file at fd 3. The broker retains the writable mapping and publishes only the
selected backboardd instance's complete native audit token before releasing its launch plan. The native Metal listener
uses mode-2 XPC framing and admits exact-token matches. These operations preserve the real kernel security identity;
launchd's stable guest PID does not replace the native audit token.

The 2026-10-25 trial temporarily substituted the offscreen Metal/IOSurface test for RuntimeRoot's backboardd executable.
Host **81448** launched helper **81449**, simulator launchd **81450**, xpcproxy **81451**, and replacement **81452**. All five
endpoint registrations returned error 0/bootstrap 0. The replacement retained stable guest PID **81451** while native
audit PID **81452**, version **202629**, authorized all three Metal connections. The guest created a 256 × 256 BGRA surface,
obtained `Apple iOS simulator GPU`, and successfully called `newCommandQueue`.

`newLibraryWithSource` then returned `MTLLibraryErrorDomain` code 3 with
`Compiler encountered XPC_ERROR_CONNECTION_INVALID (is the OS shutting down?)`. The guest exited 1, recorded as wait
status `0x100`. The broker revoked authorization and terminated helper 81449 as designed. No render pipeline, GPU command
submission, triangle pixels or framebuffer presentation was established by this trial. It tests the dedicated probe's
Metal path, not completion of stock backboardd boot.

A later shader-free probe passed with host **81535**, helper **81536**, simulator launchd **81537**, stable guest PID **81538**
and native guest **81539**. Only the native helper requested assertion flags **39** (`PreventThrottleDownUI` plus the existing
flags 7). Its GPU clear completed successfully; the guest read back center/corner BGRA **64,128,191,255**, wrote
`SimulatorData/metal-iosurface.png` and exited 0. This demonstrates offscreen GPU execution through the isolated helper,
not a stock backboardd frame. At this stage, source and precompiled shader probes were still blocked on compiler XPC despite
deployment of the complete matching stock compiler bundle. The successful clear logs are
`tmp/metal-native/console-clear-gpu-assertion.log` and the PID 81539 section of `tmp/metal-iosurface/guest-clear-gpu-assertion.log`.

The subsequent source trial with host **81793**, helper **81794**, launchd **81795**, native guest **81797** and stock compiler
**81799** passed compilation and pipeline creation after explicit bundle registration and the sixth launchd identity fix.
GPU rendering completed; CPU readback found center BGRA **127,65,63,255** and corner **0,0,0,255**. The recovered PNG visibly
contains the triangle and the guest exited 0. Evidence is `tmp/metal-native/console-compiler-sixth.log`, the **81797** section
of `tmp/metal-iosurface/guest-compiler-sixth.log`, and `tmp/metal-iosurface/phone-triangle.png`. This verifies the probe's
offscreen shader path; it does not establish a stock backboardd frame or framebuffer presentation.

Evidence: `tmp/metal-native/console-service.log` and the native PID 81452 section of
`tmp/metal-iosurface/guest-service.log`. See [Metal](metal.md) for the earlier successful construction-only probe, driver
discovery, compiler fixes and remaining tests; [NativeMetal/README.md](../NativeMetal/README.md) covers preparation,
signing and packaging. Each native helper is permanently limited to one owner/generation. Owner exit/cancellation or
helper failure requires a new host/session; an already-authorized graphics owner cannot currently exec again.

## Matching QuartzCore resources

Stock backboardd **81599** passed the prior SystemWake assertion with the [system power adapter](system-power.md), created
CAWindowServer and a swapchain, then aborted because `fixed_frag_lpf_cpf` was absent from its selected Metal library.
A phone diagnostic established that CALayer's normal NSBundle lookup read the native QuartzCore `default.metallib`
(23,192,352 bytes, 189 functions), while the matching RuntimeRoot library contained the required function
(1,662,256 bytes, 175 functions). Merely deploying the simulator resource did not change that absolute bundle lookup.

The backboardd configuration now enables the pure-C `CFBundleCreate` compatibility adapter before bundle initialization.
It redirects eligible system-framework URLs only to existing contained framework bundles under RuntimeRoot, with readable
bundle metadata; missing or ineligible candidates retain their original paths. In the phone trial with host **81917**,
helper **81919**, launchd **81920**, and native diagnostic guest **81923** (logical **81922**), normal CALayer bundle/resource
lookup and explicit RuntimeRoot lookup selected the same simulator library, both found the required function, and the
guest exited **0**. The framework executable remains cache-only, so the resource bundle's `executableURL` is nil.
This diagnostic proves corrected resource/library selection. The later stock run below additionally verifies its first
frame. See [the mechanism, scope and evidence](metal.md#simulator-framework-resource-lookup).

## First stock backboardd frame

The normal phone run used host **82139**, native Metal helper **82140**, simulator launchd **82141**, and native stock
backboardd **82143**, retaining logical PID **82142**. With the matching framework resources and compiler fixes, backboardd
passed its earlier shader-specialization abort and submitted a real framebuffer request. The host completed operation
**1** from IOSurface **45**, assigned the copied image to its UIImageView, and captured the same immutable image.

The recovered `tmp/framebuffer-server/phone-first-frame.png` is **1290 × 2796** and visibly contains a spinner on black.
Its SHA-256 is `174efa36e14dd9edf5f7e765c2fd772adfbe2d89b0825c59eddd59f876f6b212`. This establishes a guest-rendered frame
through the native presentation path. The image contains no Apple logo or progress bar, and it does not establish complete
simulator boot or a working SpringBoard session. Evidence is `tmp/metal-native/console-stock-framebuffer.log` and
`tmp/metal-native/guest-stock-framebuffer.log`; the scoped `security-stock-framebuffer.log` capture contains no amfid rejection
or TXM event.

Subsequent requests used matching source/destination rectangles **`597,1350,96,96`**, layer 0 and flags **`0x18`**. The
then-full-frame-only server rejected those with `ENOTSUP`, so this capture does not prove continuing spinner animation.
The server and view now support bounded composition of matching dirty rectangles after a successful full-frame baseline.
Focused tests, the full regression suite and the device build pass; the subsequent phone trial below also verifies partial updates.
See [framebuffer behavior and limits](framebuffer.md#host-implementation-and-limits).

### Partial frames and the subsequent input abort

The next normal run used host **82181**, native helper **82182**, simulator launchd **82183**, and native backboardd
**82185**, retaining logical PID **82184**. The host completed full-frame operation **1** from surface **302**, partial
operation **5** from surface **303**, and operation **300** from surface **304**, with **300** completed frames. The partial
rectangle was the observed **`597,1350,96,96`**. The first-frame PNG matched the earlier spinner image's SHA-256. This verifies
continued accepted composition on the phone, beyond the first full frame. Evidence is `tmp/metal-native/console-stock-dirty.log`.

This run ended about **21 seconds** after startup with **SIGABRT (6)**. An uncaught `NSInternalInconsistencyException`
reported `Encountered HID event with unexpected target 0 not in known targets`. The stack passes through
`-[SimHIDVirtualServiceManager serviceForIndigoHIDData:]` and the `SimHIDSystem initUsingHIDService:error:` event block.
The exception and full stack are in `tmp/metal-native/guest-stock-dirty-exit.log`. The broker recorded wait status `0x6`,
revoked the graphics owner's authorization and terminated its native Metal helper. This is a separate Indigo packet failure;
the successful frame count does not mean that earlier backboardd remained alive or finished booting. The later input result is below.

### Readable stock touch markers

After the Indigo target/creation fixes, normal boot-generation environment and missing runtime assets were supplied, a
fresh normal run used host **82455**, helper **82456**, launchd **82457**, backboardd logical **82458** / native **82460**,
and cfprefsd logical **82459** / native **82461**. Physical touches reached the stock visualizer and produced dirty updates.
The phone screenshot `tmp/framebuffer-server/phone-display-inset.png` shows readable `t3 p1`, `in range`, `touch down`,
and `ctx:C488CF8E` labels plus the spinner, with the native status bar unobstructed. Its SHA-256 is
`11177bacf9c2d2914ca1b4f00b7e63574f4d7cabdbffb491fbf7c3b397e14476`.
The normal production shim was restored before this run. Evidence is `tmp/metal-native/console-display-inset.log` and
`tmp/cfprefsd/final-phone-state.log`; [the preferences notes](cfprefsd.md) retain the separate, still-unproven daemon IPC question.
This establishes stock touch-marker rendering and the font correction, not a complete simulator boot or SpringBoard session.

A separate LLDB run used host **82160** and backboardd **82168** and stalled around debugger image notification. It is not
the normal-run first-frame evidence, and no debugger continuation is needed to explain the captured frame. Its artifacts
are `tmp/metal-native/console-stock-framebuffer-lldb.log` and `tmp/framebuffer-server/lldb-stock-framebuffer-notifier.txt`.
## Physical keyboard forwarding

The native IOSurface view forwards physical keyboard/keypad usages through the authenticated Indigo event port. The host
does not open or alter the system HID event service. The transport uses the stock SimulatorKit 1171.7
`IndigoHIDMessageForHIDArbitrary(target, page, usage, operation)` format: payload type 2, generic subtype 10001, keyboard
target 100, usage page 7, and operation 1 for down / 2 for up. The 148-byte payload includes a Mach-tick timestamp and the
modern timestamp fields. This path deliberately avoids the keyboard-specific subtype's `_IndigoHIDUpdateKeyboardType`
call, which changes keyboard-attachment notification state. Existing physical keyboard or Mirroring state is preserved.

Only successfully queued downs are tracked. Modifier usages `0xe0` through `0xe7` follow the same path as letters,
Backspace, Return, and arrows. Duplicate downs return `EALREADY`; guest keyboard handling owns held-key repeat. Key-up
queue pressure schedules a nonblocking retry and temporarily rejects new keyboard input. Releasing all keys on focus
loss includes ordinary keys before modifiers. Authorization changes, reconnection, and close discard the old generation,
so a delayed release cannot reach a new client.

Focused packet/lifecycle tests and existing touch tests pass, including failed-down state, queued releases, all modifiers,
and reconnection. A local probe fed actual host-generated packets to the unchanged 1171.7 decoder at offset `0x5d98`,
verifying page, usage, down/up, and timestamp for letters, Return, Backspace, arrows, Shift, and Command. Its stub for the
GraphicsServices attachment setter aborts if called; the probe completed without that call. Evidence is in
`tmp/shm-write-measure/keyboard/stock-decode.log`.

On the phone, a diagnostic probe sent 12 down/up edges through the production IOSurface view routing path to backboardd
native **11306**. All 12 returned error 0, including letters, Backspace, and a digit; the intended empty-field result was
`tes1`. This verifies acceptance through the view and Indigo send path, not text insertion into a guest app. Contacts
aborted during the tests, so actual text delivery and end-to-end physical typing remain unverified. A hardware-only LLDB
reproduction in Contacts **11553**, triggered by the user's Search tap and one typed letter, caught
`SystemAUsSoftLink` asserting `lib != nullptr` in `AudioToolboxSoftLink.h:58`. `dlerror()` identified the missing matching
`AudioToolbox.framework/libEmbeddedSystemAUs.dylib`; the stack was preparing audio units through `AudioConverterPrepare`,
consistent with keyboard press sound initialization. No assertion was bypassed; LLDB detached after capture.

The stock registration path also dynamically loads `libAudioDSP.dylib` and `AudioCodecs`; the DSP library requires the
uncached `libAudioDSPCore.dylib`. All four matching binaries were signed and published with their original platform/type/code
intact, totaling 22,458,464 bytes. Their remaining strong dependencies are cached. Fresh Contacts **11647** no longer stopped
at the audio assertion, but a later sample established the separate freeze below. End-to-end text insertion remains
unverified. Evidence: [probe transcript](../tmp/cpu-investigation/final-console.log),
[LLDB assertion and dlerror](../tmp/cpu-investigation/lldb-contacts-keyboard.txt), and
[Contacts crash findings](../tmp/cpu-investigation/contacts-keyboard-analysis.md).

## Dictation service dependency

A later Contacts freeze was sampled as native **11647** on iOS 26.1. The three-second sample showed a **2.9 GB** physical
footprint and a repeating AssistantServices dictation-availability callback → UIKit keyboard-dock update → forced-offline
dictation query path. This is separate from the earlier audio-plugin assertion. The missing assistantd
dependency also appears in the [simslim service findings](springboard-services.md#services-simslim-deliberately-preserves).
See [the process sample](../tmp/cpu-investigation/contacts-11647-spin-sample.txt).

The host's default service selection now includes stock `com.apple.assistantd`, running
`/System/Library/PrivateFrameworks/AssistantServices.framework/assistantd`. The generated job retains its 27 Mach service
declarations, including `com.apple.assistant.dictation`, with the existing bounded daemon startup policy: RunAtLoad true,
KeepAlive false, and transactions disabled. It does not import native user, resource, or event-trigger policy.
The focused Broker test verifies the generated service and defaults; its
[test log](../tmp/cpu-investigation/assistant-broker-tests.log) passed.

The live trial signed the matching 23A343 daemon and its missing strong dependency closure: 22 code files, with the other
strong imports already cached. Publication added 17 absent files and preserved six existing entries. The stock
`RuntimeRoot/System/Library/LaunchDaemons/com.apple.assistantd.plist` is present and matches the original parsed plist;
all 968 non-code resources in the staged frameworks are present with matching sizes. Post-copy checksums were skipped.
See the [stage receipt](../tmp/cpu-investigation/assistantd-stage/README.md) and
[resource audit](../tmp/cpu-investigation/assistantd-stage/resource-audit.json).

Bootstrap returned 0, and assistantd logical **11826** / native **11827** remained running with
`com.apple.assistant.dictation` active. Fresh Contacts **11854** did not reproduce the CPU/memory growth after a Search tap
and 12 keyboard edges: the subsequent three-second sample measured **35.7 MB** physical footprint, while `ps` reported
**0.0% CPU** for Contacts and assistantd. This did not establish text insertion: the screenshot still showed the Search
placeholder. Every main-thread sample waited in `AFNetworkAvailability isAvailable`, and its worker waited for the
`nwi_state_copy` XPC reply. See [the sample](../tmp/cpu-investigation/contacts-11854-after-assistant-sample.txt).

The awaited guest `com.apple.SystemConfiguration.NetworkInformation_sim` endpoint belongs to `configd_sim`. Its job was
in `spawn scheduled` state after 17 consecutive SIGABRTs; its log reported `store_host != NULL` in
`load_SimulatorSupport`, `simulator_support.c:283`. The stock plugin creates a dynamic store with
`kSCDynamicStoreUseHostServer` before mirroring native network state. The native host can look up
`com.apple.SystemConfiguration.configd` successfully, but that endpoint was absent from the simulator host bridge.
The host now imports that endpoint while leaving the simulator NWI/DNS services owned by stock `configd_sim`.
On the next boot, bridge lookup and registration returned 0, and configd_sim **12090** passed the host-store assertion,
mirrored the native interface/DNS state, and remained running alongside assistantd **12086**. Contacts text entry still
needs verification after this bridge fix. No dictation availability result or network response has been fabricated. Evidence:
[configd state](../tmp/cpu-investigation/configd-control/0e21212cdad1492495f7be8afde357d0.log),
[configd assertion](../tmp/cpu-investigation/configd-current-log.txt),
[fresh daemon startup](../tmp/cpu-investigation/fresh-daemon-startup.txt), and
[Apple's SimulatorSupport source](https://github.com/apple-oss-distributions/configd/blob/main/Plugins/SimulatorSupport/simulator_support.c).


The next first/last-character delay was a separate shader-discovery failure. Contacts native **12247** waited in RenderBox
shader specialization and `MTLCompilerScheduler`, while Metal retried the missing `com.apple.MTLCompilerService` endpoint
once per second. Simulator launchd rejected the cache's virtual XPC bundle path against the physical Contacts.app origin.
The app-domain registration fix is described in [Metal app compiler registration](metal.md#app-compiler-registration).
On the updated build, Contacts logical **12413** / native **12414** displayed the keyboard; the user repeatedly typed the
first character and deleted it, confirming both operations were responsive. The user also confirmed the Metal-rendered
swipe control worked. This verifies those interactions, without claiming all keyboard or wallpaper paths are complete.
