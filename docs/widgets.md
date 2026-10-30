# Home Screen widgets

The widget path is pinned to iOS Simulator **26.0, build 23A343**. SpringBoard's `WidgetRenderer_Default` running by itself
does not establish that a widget provider launched or produced a timeline.

The default guest service selection includes `com.apple.pluginkit.pkd` and `com.apple.chronod`, with `com.apple.navd`
and `com.apple.geod` for the Maps widget's dependencies. They use their original runtime executables and Mach service
declarations. The existing job generator keeps these jobs inside guest launchd,
starts them at boot, and omits native user, pressure-exit, scheduling and launch-event policies. Policies remain plist Booleans.
These are guest services, separate from the native device's PlugInKit daemon that launches LiveProcess.

## Compatibility boundaries

The initial iPhone 15 investigation found two independent failures:

1. `pkd` cancelled clients because native `proc_pidpath` returned `EPERM`. In 23A343 PlugInKitDaemon,
   `-[PKDXpcClient initWithConnection:server:]` requires this path before constructing the audit-backed SecTask.
   The guest adapter handles only a denied query from that framework in exact `/usr/libexec/pkd`. It asks the broker for
   the current simulator peer's immutable identity using the **native** PID supplied by XPC, verifies the full audit token,
   and returns its physical runtime path. It grants no task-control, process-list, short-BSD-info or sandbox-policy access.
   Other callers and native errors retain their original behavior; PlugInKit's entitlement checks still run.
2. `chronod` could discover widget bundles but could not resolve ExtensionKit's `Launch` endpoint. Its host-selected job
   now opts into registering the stock `extensionkitservice.xpc` in **chronod's own PID domain**, as SpringBoard and
   PosterBoard already do. Both the job label/executable check and guest canonical-path check must match. This option is
   not placed in launchd's global environment.

After these fixes, launchd reached the spawn adapter and rejected widget extension jobs with process type `0x200`, role `0`,
flags `0x400c`. This is `POSIX_SPAWN_PROC_TYPE_APP_NONUI`, an application scheduling hint. Calendar's intent extension
then requested `POSIX_SPAWN_PROC_TYPE_DAEMON_INTERACTIVE` (`0x400`), also with role `0`. The adapter accepts these two
observed hints while retaining the native extension's scheduler policy. Unsupported roles, suspension and session flags
remain rejected. Definitions come from the pinned [XNU spawn header](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/sys/spawn_internal.h#L328-L345).

Provider discovery also exposed the former 64-child ledger limit: the session reached exactly launchd plus 64 guests and
could no longer launch Contacts. [Ledger ABI 5](multiprocess.md) provides 120 bounded slots in the same 4 KiB mapping.
All three producers/consumers must be rebuilt and deployed together. The outer transport ABI is unchanged.

## Device evidence, 2026-10-30

The test used iPhone16,2, native iOS 26.1 build 23B50, and guest 23A343 with the relocated cache at `0x110000000`, UUID
`73ED978A-FD25-3166-A57A-34E21BB7E5B8`. Existing runtime executables and cache were retained; the native development
signature and application identifier were used for rebuilt host/extension/shim artifacts. Native cache detachment remained
selected by the existing non-production MobileGestalt policy.

CalendarWidgetExtension started, and the screenshot and user both confirmed Calendar's date and “No events today” content.
This establishes provider/rendering startup, not account syncing or event-data correctness. With ledger ABI 5, Contacts then
opened and responded while more than 64 simulator guests were alive. The full local test suite passed with native framework
access, including the new scope, spawn-policy and ledger regressions.

An attempted LLDB attachment to the busy Maps widget did not finish; the known client was terminated, the widget was
independently verified running, and no debugserver remained before restarting. Successful widget rendering and Contacts
verification used no debugger. Subsequent bounded native `sample` captures resolved guest library stacks without LLDB.
Logs, captures and signed test outputs are under ignored `tmp/widgets-20261030/`; setup does not depend on those files.

## Maps retry CPU

Starting widget providers exposed missing guest services: GeneralMapsWidget used about 89% CPU, MapKit SnapshotService
about 36%, and launchd about 150%. A bounded native sample placed Maps in Navigation's
`MNNavigationServiceRemoteProxy` disconnect/reconnect path. It repeatedly looked up `com.apple.navigationService`,
whose stock owner is `MapsSupport.framework/navd`. Its retry path resets interruption history when opening a new
connection, so the retry window did not stop the missing-service loop.

Starting stock navd brought GeneralMapsWidget to idle, but navd and SnapshotService then retried `com.apple.geod`.
The diagnostic navd log named that exact missing service. After starting stock geod, all four processes measured 0%
CPU and launchd measured about 8%; the device reported thermal-pressure state 0. This is a sampled idle result, not a
guaranteed CPU ceiling.
Both service declarations are now included in normal startup without modifying their executables or broadening entitlements.

The final cold boot used the normal shim and default service selection, without diagnostic logging or a debugger. Calendar
rendered again. Maps showed a plain pale-green tile, so map imagery, location and routing remain unverified. Maps, navd,
geod and SnapshotService all measured 0% CPU, while launchd measured 24.7–25.3%. A two-second native sample showed the same
dispatch timer/workqueue redirection pattern as the [existing process-source polling cost](multiprocess.md#managed-process-source-polling-cost),
including native `start_wqthread` at `0x1ebcb78b8`. This suggests the existing polling overhead remains; it does not assign
all launchd CPU to that path. The final check found 72 host-owned processes, no debugger, and thermal-pressure state 0.

## Diagnosing provider startup

Use the [guest launchctl bridge](guest-control.md) to inspect `user/501/com.apple.chronod` and
`user/501/com.apple.pluginkit.pkd`, then inspect actual extension processes and their logs. A successful bootstrap command
does not prove that CalendarWidgetExtension or another provider ran. If using a custom service selection, include both jobs;
the host adds chronod's ExtensionKit opt-in when generating that exact selected job.

For a bounded diagnostic run, `ACTIVITY_LOG_STDERR=1` and `OS_ACTIVITY_MODE=debug` in an isolated job plist exposed the
PlugInKit/ExtensionKit failures without changing the ordinary guest logging policy. Remove these options after diagnosis.
The optional `LSB_TRANSPORT_TRACE=1` shim build records rejected spawn attributes in the simulator log directory.
Stop all affected guests before replacing that dylib and restore a normal build afterward.

Do not replace failed entitlement queries with success. For example, `pluginkit`'s privileged `PKDiscoverAll` mode can be
denied after the connection itself works. Keep widget-provider startup and rendering results distinct from remaining location,
account, data-service and network dependencies.
