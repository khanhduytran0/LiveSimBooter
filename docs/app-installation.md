# Installing guest applications

## Implemented flow

Native **Settings → Applications → Install IPA…** stages a selected document and submits one installation to the running
**iOS Simulator 26.0 / 23A343** guest. Installation through the reserved control request succeeded on iPhone 15 on
2026-11-01, with the helper verifying the installed LaunchServices record and physical bundle/data URLs. A fresh test
app then installed and launched in the running session without restarting SpringBoard after the notification adapter below
was enabled, and the user confirmed that its UI appears. Both probe icons are visible on the Home Screen. Its separate
application home and document preservation across a same-bundle update are also verified. Native document-picker submission reached guest validation; a successful install
selected through that UI has not yet been verified.

```text
Native document picker / GuestApplicationInstaller
  → existing launchctl-control queue: --lsb-install-ipa <physical archive path>
  → fixed guest /usr/local/bin/lsb-app-install
  → IXAppInstallCoordinator
  → com.apple.installcoordinationd
  → MobileInstallation / com.apple.mobile.installd
  → guest container manager and LaunchServices
```

[GuestApplicationInstaller](../Host/GuestApplicationInstaller.m) copies the security-scoped document into
`data/Library/LiveSimBooter/Imports/<32 lowercase hex digits>/payload.ipa` off the UI thread. It preserves the original
selected file and original runtime inputs. A persisted request ID and exclusive queue ownership prevent duplicate
submission. The reserved control verb accepts exactly one absolute source path; it cannot choose another executable.
No new native installation service or host MobileInstallation call is introduced.

[ApplicationInstaller](../Guest/ApplicationInstaller.m) requires simulator platform 7, matching 23A343 RuntimeRoot and
sibling data roots, and the exact staged-import layout. It walks the import components without following symlinks and
keeps a source FD to verify file identity/generation through completion. It calls the pinned typed API declared in
[ApplicationInstallation.h](../Guest/Private/ApplicationInstallation.h):

```objc
MIInstallOptions *options = [MIInstallOptions new];
options.installTargetType = 1;
options.developerInstall = YES;
options.userInitiated = YES;
[IXAppInstallCoordinator installApplication:sourceURL consumeSource:NO options:options
    completion:^(NSString *bundleIdentifier, NSError *error) {
        // Verify the returned LaunchServices record and canonical installed container URLs.
    }];
```

The stock simple installer extracts ZIP inputs through `extractZipArchiveAtURL:toURL:withError:` and handles Payload
packages. The host does not unpack the IPA or manually manufacture an installation record. `consumeSource:NO` matches
CoreSimulatorBridge and preserves the staged archive until the host verifies completion.

The helper resolves an LSApplicationRecord with `allowPlaceholder:NO`, requires its bundle identifier to match, and
checks its existing canonical bundle directory beneath `data/Containers/Bundle/Application`. A returned data-container
URL must stay beneath `data/Containers/Data/Application`. It emits one `LSB_INSTALL_RESULT ` line containing JSON:
`version:1`, a Boolean `success`, bundle identifier/URL on success, optional data URL, or error domain/code/description.
The native client requires a matching finished worker receipt and valid helper result before reporting success.

Build with [Tools/build-application-installer.sh](../Tools/build-application-installer.sh), sign its `lsb-app-install`
output and stage it at `RuntimeRoot/usr/local/bin/lsb-app-install`; see the [setup guide](agent-setup.md).
The same build emits `libLiveSimInstall.dylib`; sign it and stage it at `RuntimeRoot/usr/lib/libLiveSimInstall.dylib`.
The [control worker](guest-control.md) must be rebuilt and deployed with them. Direct guest invocation also supports
`lsb-app-install --inspect <bundle-id>` for read-only verification; that mode is not exposed by the reserved control verb.

## Services and interrupted installs

| Guest job | Executable | Mach services |
| --- | --- | --- |
| `com.apple.mobile.installd` | `/usr/libexec/installd` | `com.apple.mobile.installd` |
| `com.apple.installcoordinationd` | `/System/Library/PrivateFrameworks/InstallCoordination.framework/Support/installcoordinationd` | `com.apple.installcoordinationd`, `com.apple.installcoordinationd.CacheDelete`, `com.apple.InstallProgress.xpc` |

The graphical defaults in [Broker.m](../Host/Broker.m) now include both installer daemons and
`com.kdt.LiveSimBooter.launchctl-control`, using all **24 selected-service slots**. Explicit `--launchd-services LABEL...`
replaces that list; retain these three labels when using native IPA import. The generator adds its baseline jobs separately.
Selected jobs are not proof of service readiness, and generated jobs do not preserve every stock lifecycle policy, including
pressure-exit and InstallCoordination pending-work/unlock triggers.

The worker permits 180 seconds for this helper, then sends SIGKILL and allows five seconds to observe termination.
The native client polls up to 190 seconds. Killing the helper does **not** cancel an installation already accepted by a
daemon: timeout, missing/invalid reply, signal death or unresolved wait means **unknown outcome**, not safe-to-retry failure.
The worker preserves `active` on installer timeout and stops; the host retains its imported copy, request identity and
queue ownership. **Refresh reads the same request; it never resubmits it.** Inspect the actual application state and queue
evidence before manually resolving an interrupted operation. A simulator restart may archive interrupted queue entries,
but does not turn their outcome into success or permit automatic replay.

After a confirmed helper result or a known failure before the helper spawned, the host removes only its owned import
and releases its matching queue lock. Logs/status remain. If cleanup fails, it retains the receipt identity so Refresh can
try cleanup without installing again. Importing a newer IPA with the same bundle identifier uses the stock update path;
separate update/uninstall controls are not implemented.

## How macOS invokes it

The local CoreSimulator **1171.7** host binaries were traced further on 2026-11-01. This is static inspection of the actual
Mac command/client and simulator bridge, not an installation run or a claim about every Xcode version.

```text
macOS simctl install <device> <path>
  → CoreSimulator.framework: SimDevice installApplication:withOptions:error:
  → SimDevice installApplication:withOptions:installationInfo:error:
  → lookup the selected simulator's com.apple.CoreSimulator.host_support
  → host_support_mig_install_app_with_info (Mach request)
simulator CoreSimulatorBridge
  → IXAppInstallCoordinator installApplication:consumeSource:options:completion:
  → guest installcoordinationd / installd
```

The traced `simctl` handler expands `~`, creates a file NSURL from the supplied path, and invokes:

```objc
[device installApplication:sourceURL withOptions:nil error:&error];
```

That command handler does not check for a `.app` suffix or extract an IPA. The framework resolves symlinks and standardizes
the URL, adds `SimulatorRootPath`, `SimulatorUserPath` and default `PackageType=Developer`, and sends its absolute URL
string plus securely keyed-archived options.
It needs a running simulator session; this install path does not boot a shutdown device.

`hostSupportPortWithError:` looks up `com.apple.CoreSimulator.host_support` through the selected SimDevice and retains a
`SimMachPort`. `_sendBridgeRequest:caller:operationDescription:error:` supplies that port to the MIG client stub. The
operation sends an inline **source pathname/URL** and out-of-line archived metadata, not the app's bytes or a file descriptor. Mac and simulator
processes can access the same underlying source file; this is not a USB installation-proxy upload.

The receiving `/usr/libexec/CoreSimulatorBridge` is an **iOS Simulator executable**, supplied by CoreSimulator's platform
overlay. Its `com.apple.CoreSimulator.bridge` launchd job publishes the host-support Mach service. The inspected bridge has
`LC_BUILD_VERSION` platform IOSSIMULATOR, minimum OS 15.0 and SDK 27.0. It invokes MobileInstallation/InstallCoordination
inside the simulator environment, using guest libraries and the guest bootstrap namespace.

The bridge builds `MIInstallOptions`, sets target type 1, developer-install and user-initiated flags, optionally forwards
`SkipWatchAppInstall`, and invokes the typed guest method shown above with `consumeSource:NO`. Its dispatch-group wait uses
`DISPATCH_TIME_FOREVER` before producing the Mach reply. The reply contains separately archived installation information
and NSError objects. Installation information includes the bundle ID and, when available from LSApplicationRecord, its
install URL and persistent identifier. The MIG handler returns `KERN_SUCCESS` even when installation fails; the Mac client
must decode the error object. Successful Mach message delivery alone is not an installation success.

LiveSimBooter follows the same guest installation route through its existing control queue; the whole CoreSimulator
platform overlay is not a runtime dependency. Its host client adds a finite wait and explicit unknown-outcome handling.

## Destination and existing registration

The intended user-install destination is the guest data tree:

```text
<selected store App Group>/LSB/iOS/23A343/
  RuntimeRoot/                              supplied system code and resources
  data/Containers/Bundle/Application/<UUID>/<App>.app
  data/Containers/Data/Application/<UUID>/   application's writable home
```

The app and data container UUIDs are independent. Guest container services choose these paths; the caller should not
construct UUIDs or copy a bundle to `RuntimeRoot/Applications` to imitate installation. The host already sets
`SIMULATOR_SHARED_RESOURCES_DIRECTORY` and `IPHONE_SHARED_RESOURCES_DIRECTORY` to this `data` root. Verify the actual
physical URLs returned by each installation instead of inferring a container UUID.

The original simulator `libsystem_containermanager` contains these bundle/data relative roots. InstalledContentLibrary's
`MIBundleContainer` allocates the bundle through MIContainer/MCM, and `MIGlobalConfiguration` reads the shared-resources
directory separately from the runtime root. The first successful device installation confirmed this layout.

[ApplicationRegistration](../Guest/ApplicationRegistration.m) currently calls `LSApplicationWorkspace registerApplication:`
for pre-staged stock apps. Its allowed directories are deliberately restricted to RuntimeRoot's `/Applications`,
`/System/Applications`, and `/System/Library/CoreServices`. This registers an existing bundle with lsd; it does not run
the installation workflow. Keep that tool separate from a user-app installer.

## Temporary staging compatibility

The first device attempt reached InstallCoordination but failed before extraction: `IXErrorDomain` 1 contained
`NSPOSIXErrorDomain` 1, `mkdir of /private/var/tmp/com.apple.appinstall.temp failed: Operation not permitted`.
The bounded helper error log now includes up to six underlying NSError levels without dumping arbitrary userInfo.

The pinned MobileInstallationHelperService `MIStagingManager stagingRecordForSystemContentWithinSubsystem:error:`
hardcodes `/private/var/tmp` for subsystem 3. That branch does not consult `TMPDIR`, install options or a configurable
staging setter. The request originates in installcoordinationd's synchronous staging call; creating `data/var/tmp`
would not redirect the absolute native path.

[InstallationCompatibility](../Guest/InstallationCompatibility.m) is a separate Objective-C dylib inserted only into the
exact guest MobileInstallationHelperService executable. It validates helper UUID `3F7ABD1C-04D6-3A63-9C61-731176B12263`,
method ABI, offset and original instructions before exchanging the typed method. It obtains the image from the method's
`dladdr` owner and separately checks `_NSGetExecutablePath`; image index zero can be the inserted spawn shim. Subsystem 3 returns the same record
class with canonical `<data>/tmp` as its base and `com.apple.appinstall.temp` as its relative path. Other subsystems and
original errors are forwarded. It requires the existing no-symlink data/tmp directory and fails closed on a changed record.

Changing the producer keeps the helper, returned staging objects and sandbox-extension requests on the same physical
URL. It does not rewrite general filesystem calls or load Objective-C into the always-injected C shim. Local tests and
simulator compilation pass. With this normal adapter, the previously failing installation completed with helper exit 0.

## Live install notifications

The initial install created a valid LaunchServices record, but SpringBoard did not discover the app until it restarted.
The traced Mac client does not send an additional refresh after installation: CoreSimulatorBridge records the IX completion
and constructs its reply, while SimDevice only records analytics. The normal live update comes from guest lsd's installation
observer connection. `LSApplicationWorkspaceRemoteObserver applicationsDidInstall:` invalidates the client's LS context
and delivers the update to its local observers.

Before opening that connection, pinned CoreServices caches a sandbox preflight for `com.apple.lsd.xpc` in
`_LSProcessCanAccessProgressPort`. On iPhone 15, a C probe using the complete kernel `TASK_AUDIT_TOKEN` observed
`sandbox_check_by_audit_token` return **1** for that name while `bootstrap_look_up` in the guest namespace returned a valid
port. The controls behaved distinctly: `com.apple.cfprefsd.daemon` returned **0**, and an unregistered test name returned
**1**. This exposes the difference between the native extension's sandbox policy and the running guest service namespace.

[LaunchServicesNotifications](../Guest/LaunchServicesNotifications.c) adapts only this self preflight. It requires simulator
platform 7, matching canonical runtime-root environment values, CoreServices UUID `73F411D9-84A9-33FE-84C9-8CD57CA633DC`,
the exact return address at image offset `0xcfa94`, and the original instruction window. It additionally requires:

- The full supplied audit token matches a fresh kernel self token, including PID version.
- Operation `mach-lookup`, filter `0x40000002`, and the exact service name `com.apple.lsd.xpc`.
- A successful lookup of that name in the current guest bootstrap namespace; the temporary send right is released.

A successful guest lookup satisfies this narrowly scoped guest preflight. The adapter changes no native sandbox state or
entitlements. Other calls, failed validation, and failed guest lookups forward to the original API with the original
variadic stack, argument registers and errno. An atomic readiness gate forwards pre-constructor calls before consulting
TLS or runtime helpers. The adapter lives in the C spawn shim, so rebuild and deploy `libLiveSimSpawn.dylib` with the
installer overlay.

The seven instruction pins and full image validator were checked against the rebuilt cache UUID
`73ED978A-FD25-3166-A57A-34E21BB7E5B8` at `0x110000000`; the `BL` at CoreServices offset `0xcfa90` is unchanged. Focused
native tests cover full-token matching, caller/image bounds, operation/filter/name controls, failed lookup and right
cleanup, startup forwarding, errno, and the arm64 variadic ABI. The focused sanitizer run and guest shim build passed.

With that shim, fresh bundle ID `com.kdt.LiveSimBooter.InstallProbeLive` installed on iPhone 15 in request
`d4600075d51b4565b1b74d28a163817b`: helper PID **14304** exited **0** and reported the new LS bundle/data URLs. The subsequent
open request completed successfully without a SpringBoard restart, and the app at logical PID **14320** wrote its launch
receipt from the installed bundle. The user confirmed that the probe UI appears. This verifies live discovery, process
launch and rendered application UI. Local captures are in `tmp/ipa-install-20261101/install-live15/`,
`open-live15/`, `live-proof15.json`, and `notification-result15.txt`.
The later device screenshot `tmp/metal-demand-20261101/device15-screen.png` also shows both “IPA Probe” and “IPA Live Probe”
on the second Home Screen page, confirming icon presentation.

## Installed executables and remaining validation

[GuestExecutablePath.h](../ProcessSupport/GuestExecutablePath.h) now classifies canonical executable paths inside
RuntimeRoot or this runtime's `data/Containers/Bundle/Application/<container>/<App>.app/` tree, including nested app
extensions/XPC/framework executables. Data homes, Imports staging, other runtimes and paths escaping through symlinks
are not executable roots. [ExecutablePath](../LiveProcess/ExecutablePath.c) and the [broker](../Host/Broker.m) use the
same containment rules and retain opened-FD identity checks. System roles retain virtual identities; installed apps retain
their physical paths, so adding data apps does not grant native helper roles.

[ExecutableAccess](../Guest/ExecutableAccess.c), [PeerProcessCompatibility](../Guest/PeerProcessCompatibility.c) and
[ProcessInspection](../Guest/ProcessInspection.c) handle these physical installed-app paths without prepending RuntimeRoot
again. [MetalCompilerRegistration](../Guest/MetalCompilerRegistration.c) accepts any contained guest executable, including
installed apps and nested services, and imports the fixed compiler bundle only when that process requests its exact XPC
service. It no longer needs an APPL Info.plist check or a list of graphics processes; see [the demand hook](metal.md#guest-compiler-bundle-registration).

The existing lazy signer already accepts canonical paths under RuntimeRoot and data. Its native signature repair is
separate from installer acceptance. The pinned simulator `MICodeSigningVerifier performValidationWithError:` loads
simulator entitlement metadata and constructs an `Unvalidated` signer result instead of device CMS/profile verification.
This does not make the payload acceptable to native AMFI, guarantee replacement-signature space, or remove architecture
and platform requirements. Preserve original entitlement metadata; there is no blanket installer entitlement override.

On 2026-11-01, request `a17095e16c774b5f8e189959d215b87f` installed the disposable `LSBInstallProbe.app` on iPhone 15.
Native helper PID 13713 exited 0 and verified the LaunchServices record, whose physical paths ended in:

```text
data/Containers/Bundle/Application/2E6E6624-10E7-477E-B0F0-E147607A5709/LSBInstallProbe.app
data/Containers/Data/Application/1BDE06C0-4D2F-4532-B157-886179DB50F4
```

These UUIDs are evidence from that run, not setup defaults. Before a SpringBoard restart, the user saw no icon and
LaunchServices rejected activation with error 115. After `launchctl kickstart -k` of guest SpringBoard, LaunchServices
accepted activation without an error; live job `UIKitApplication:com.kdt.LiveSimBooter.InstallProbe[edf0][rb-legacy]`
had logical PID 13789, and a device screenshot showed the probe's “IPA installed in data” UI. This needed a **SpringBoard
restart**, not a full simulator reboot. That initial failure led to the notification adapter above; the later fresh-ID
installation and launch succeeded without either restart. The importer does not restart SpringBoard to refresh apps.

Per-app home selection and same-bundle update preservation are verified below. Native document-picker submission was
exercised by two Filza imports that reached the helper and exited 1 with a stock InstallCoordination validation error:
the embedded extension identifier did not have the renamed parent app's required prefix. That verifies picker submission
and guest rejection, not successful installation of that payload. A successful native-picker import remains unverified;
uninstall behavior is untested.
Retain the [container-metadata filename patch](launchd.md#container-metadata-filename-compatibility) in both the cache and
MobileInstallationHelperService.

Local coverage includes strict source paths and immutable-source checks, typed installation completion/failure, installed
URL bounds, JSON Boolean/result validation, control-command routing and timeout/no-replay behavior. These local tests are
separate from the successful device installation; they do not establish native signature acceptance of every imported app.

## Per-app homes

The initial installed probe's `NSHomeDirectory()` resolved to shared `data`, and its launch receipt appeared in shared
`data/Documents` instead of its allocated application container. A bounded temporary RunningBoard trace on iPhone 15,
native PID 14385, showed that
`_addAppPropertiesToData:forIdentity:context:actualIdentity:error:` already received the correct per-app
`EnvironmentVariables.HOME`. The traced `LSApplicationProxy.environmentVariables` merge supplies this value before
RunningBoard's separate pre-containerization lookup.

Forcing `containermanagerd/stop_container_pushdown` enabled that later lookup, which failed with
`NSPOSIXErrorDomain` 22, “Container did not have a token.” The method nevertheless returned success and preserved the
existing per-app HOME; it did not set `SandboxContainer`. The final fix needs neither a token bypass nor that feature
override. In pinned RunningBoard UUID `D86594F1-48F2-3A64-8F44-482F5674B70A`, the LS environment merge
is at `0x1a21c–0x1a244`, independent of the feature gate. `_generateDataWithIdentity:…` returns the same plist, which
`_createLaunchdJobWithIdentity:…` passes to `OSLaunchdJob initWithPlist:` through `jobWithPlist:`. Its return value is a
job object, so absent environment keys in that diagnostic return do not demonstrate that RunningBoard removed them.

The loss occurred in simulator launchd's environment merge. The traced Mac SimDevice configuration separates HOME,
CFFIXED_USER_HOME, TMPDIR and both DYLD_FALLBACK path variables into `Bootstrap.ChildDefaults`; LiveSimBooter's prior
configuration placed them in `Bootstrap.Child`. Pinned launchd UUID `C3EC5C45-8C85-3D11-B59C-62C4248F5EF9` rejects a job
environment entry whose name already occurs in Child (constructor `0x11f04`), while ChildDefaults is applied after the
job environment. [LaunchdConfiguration](../Host/LaunchdConfiguration.m) now matches that split and removes the temporary
RunningBoard feature override. Focused tests verify the defaults and unchanged launchd Self/initial environment.

Before guest handoff, [ApplicationEnvironment](../LiveProcess/ApplicationEnvironment.h) also aligns CFFIXED_USER_HOME and
TMPDIR with the HOME supplied by RunningBoard: 23A343 CoreFoundation prefers CFFIXED_USER_HOME over HOME. This applies
only to a canonical top-level installed `.app` executable whose HOME resolves to an existing
`data/Containers/Data/Application/<UUID>` directory in the same runtime. It does not infer a data-container UUID,
create a container, or change daemon, nested-service, native-helper or shared-root environments.

On iPhone 15, the normal configuration with the restored RunningBoard executable and no trace dylib launched probe
version 2 at logical PID **14783**. HOME, CFFIXED_USER_HOME and `NSHomeDirectory()` all resolved to its application
container `EEE9762B-91C2-4E3B-A31E-F6CA2419E669`; TMPDIR resolved to that container's `tmp/`, and the launch receipt was
written into its own `Documents/`. The verified capture is `tmp/ipa-install-20261101/verify-finalhome15.json`.
This establishes directory selection, not a separate native sandbox boundary between simulator applications.

The local trace is `tmp/ipa-install-20261101/rbs-home-trace15.jsonl`; static branch details are in the adjacent
`home-analysis/findings.md`. The temporary diagnostic changed no return values and was removed after testing.

## Same-bundle update verification

The version 2 probe was stopped, and a sentinel document was written into its existing application home. Importing
version 3 with the same bundle identifier through request `851f11e5aac94bf4a52a582b7e76237d` completed with helper
PID **14854**, exit **0**, without restarting SpringBoard. Stock installation allocated a new bundle container and preserved
the app's data from home `EEE9762B-91C2-4E3B-A31E-F6CA2419E669` in new home `E893E4AE-746D-46E1-87CD-C580DA2B7C9B`.
`Documents/update-sentinel.txt` retained its exact value. The updated app then launched at logical PID **14858**, reported
version **3**, and used the new container for HOME, CFFIXED_USER_HOME, `NSHomeDirectory()` and TMPDIR.

The receipts are `tmp/ipa-install-20261101/install-v315/`, `update-preserved15.json` and `proof-v315.json`.
This verifies the tested stopped-app update and one preserved document; it does not establish active-app replacement,
every payload migration behavior, or uninstall support. Container UUIDs may change during an update, so consumers must
resolve the current LaunchServices record instead of retaining historical paths.

## Evidence

The supplied 23A343 binaries and plists were inspected without modifying them. Relevant symbols include
`MIInstallerClient init` (connection to `com.apple.mobile.installd`), `IXServerConnection _onQueue_createXPCConnectionIfNecessary` (connection to
`com.apple.installcoordinationd`), `IXAppInstallCoordinator installApplication:consumeSource:options:completion:`,
`MIBundleContainer appBundleContainerWithIdentifier:createIfNeeded:created:error:`, and
`MICodeSigningVerifier performValidationWithError:`.

| Original arm64 image | UUID |
| --- | --- |
| installd | `969E017F-D02A-38FF-8426-678C61E69BA6` |
| MobileInstallation | `7617ADA2-8257-3C9A-8333-E9DA188A0728` |
| InstallCoordination | `05F74DE0-3DA5-37F1-B0B6-863219BDB84B` |
| installcoordinationd | `084C77FC-3523-3EFD-B32F-89A0A5A2C1AA` |
| InstalledContentLibrary | `DDC817B4-3524-3D31-B72A-F8C73B48FB25` |

Mac-side provenance: simctl UUID `A9F26CDD-1971-3BD1-8DD3-0D53B9D541C7`, CoreSimulator UUID
`EC42F41F-0F10-3F19-8161-4A3CEC5111C9`, and the platform overlay's CoreSimulatorBridge UUID
`BD5D20EA-0047-3DF6-900A-C39F6CC73C63`. The simctl install handler calls SimDevice at `0x100023b98`;
the framework calls `_host_support_mig_install_app_with_info` at `0x50244`; the bridge calls the guest coordinator at
`0x100021f44`. These are unslid addresses in the inspected artifacts, not patch offsets or deployment requirements.

Ignored disassembly and research notes are in `tmp/ipa-install-20261101/`; they are supporting local captures, not setup
dependencies. These static captures did not attach a debugger, start device processes, change the cache or test signature acceptance.
