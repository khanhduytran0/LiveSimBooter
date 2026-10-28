# Services toward SpringBoard

This note records the service-selection evidence after the working backboardd display/input path. The latest verified
display trial shows [SpringBoard with application icon artwork](#public-graphics-services-and-icon-rendering-verified-2026-10-26).
The subsequent [app-launch investigation](#app-launch-investigation-2026-10-26) has identified further spawn boundaries;
it has not yet verified a stock application launch. Each service still requires actual startup, replies and failure traces;
a generated job plist does not establish that its daemon works or that the selected set is sufficient for a complete system.

## Pinned simslim evidence

The reference is [MobAI-App/simslim at `2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc`](https://github.com/MobAI-App/simslim/tree/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc),
committed on 2026-10-25. The read-only local clone is `tmp/simslim-reference`. Its
[profile implementation](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles.go#L378-L448)
constructs a set of services to **disable** and removes explicitly retained categories or labels from that set.
Everything outside its managed catalog is left alone. It starts with a stock CoreSimulator installation and boot process;
it does not construct a minimal replacement launchd environment.

The pinned catalog contains 168 unique disable labels. Comparing them with the matching iOS 26 **23A343** runtime's
`System/Library/LaunchDaemons` and `System/Library/LaunchAgents` gives:

| Static inventory | Count |
| --- | ---: |
| Stock job definitions inspected | 323 |
| Definitions matching a disable label | 158 |
| Definitions outside the disable list | 165 |
| Unique labels among those remaining definitions | 164 |

The remaining definitions include stock-disabled and feature-conditional jobs, so these are neither live process counts
nor a list of jobs that must be started. The full comparison, stock executable paths and service names are recorded in
`tmp/simslim-reference-inventory.json`. simslim likewise
[distinguishes managed-label counts from processes](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/README.md#L22-L27).
Its current category measurements use
[iOS 26.5](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/docs/category-memory.md#L8-L18),
while this project uses 26.0 / 23A343 on a jailed iPhone. Its retained services are useful evidence, not a directly portable
boot recipe or memory budget.

## Services simslim deliberately preserves

Three services have explicit `AlwaysEnabled` entries:

| Label | Reason recorded by simslim |
| --- | --- |
| `com.apple.assistantd` | Dictation service availability prevents a system Spotlight memory problem. |
| `com.apple.gamed` | Required for timely XCUITest app launches through the development tooling/GameKit path. |
| `com.apple.sharingd` | Required for system share sheets. |

These are workflow requirements, not proof that all three are necessary to reach SpringBoard's initial screen. Their
definitions are in the pinned [Siri](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles.go#L85-L90),
[apps](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles.go#L275-L280), and
[connectivity](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles.go#L317-L322) categories.

The separate [forbidden-label regression test](https://github.com/MobAI-App/simslim/blob/2c68da93700aa1a1c45ce31a7ea9f7cac2174ddc/profiles_test.go#L9-L38)
protects these fifteen exact names from disabling:

```text
com.apple.nanoregistryd
com.apple.nanoregistrylaunchd
com.apple.nanoprefsyncd.2
com.apple.nanotimekitcompaniond
com.apple.nanobackupd
com.apple.sleepd
com.apple.appprotectiond
com.apple.ManagedSettingsAgent
com.apple.managedconfiguration.profiled
com.apple.mobiletimerd
com.apple.routined
com.apple.biomed
com.apple.biomesyncd
com.apple.dmd
com.apple.donotdisturbd
```

The test's comments specifically associate disabling `sleepd` with SpringBoard retry storms and disabling
`nanoregistryd` with stalled CoreSimulator operations. The other entries were observed to break boot in that environment.
This is evidence against indiscriminately removing them from a working stock simulator. It is not proof that this
project must launch the entire set before its first SpringBoard experiment, especially where an observation concerns
CoreSimulator's Mac-side management rather than a guest UI dependency.

## Current selected-job mode

A normal no-argument app launch and bare `--launchd-services` boot the graphical session. Both retain the fixed backboardd,
simulator cfprefsd and `com.kdt.LiveSimBooter.terminal-fish` jobs, and select these additional defaults in `Host/Broker.m`:

| Job label | Matching 23A343 program | Main service declarations |
| --- | --- | --- |
| `com.apple.notifyd` | `/usr/sbin/notifyd` | `com.apple.system.notification_center` |
| `com.apple.lsd` | `/usr/libexec/lsd` | Stock `com.apple.lsd.*` endpoints for application, icon, plugin and database operations |
| `com.apple.runningboardd` | `/usr/libexec/runningboardd` | `com.apple.runningboard`, `.watchdog`, `.resource_notify` |
| `com.apple.containermanagerd` | `/usr/libexec/containermanagerd` with stock agent-mode arguments | `com.apple.containermanagerd` |
| `com.apple.fontservicesd` | `/System/Library/PrivateFrameworks/FontServices.framework/Support/fontservicesd` | `com.apple.fontservicesd` |
| `com.apple.containermanagerd.system` | `/usr/libexec/containermanagerd_system` with stock privileged-runmode arguments | `com.apple.containermanagerd.system` |
| `com.apple.distnoted.xpc.daemon` | `/usr/sbin/distnoted daemon` | `com.apple.distributed_notifications@0v3`, `com.apple.distributed_notifications@1v3` |
| `com.apple.configd_sim` | `/usr/libexec/configd_sim -d` | `com.apple.SystemConfiguration.{NetworkInformation_sim,DNSConfiguration_sim,configd_sim}` |
| `com.apple.sleepd` | `/System/Library/PrivateFrameworks/SleepDaemon.framework/sleepd` | `com.apple.sleepd.sleepserver` and the stock sleep wake endpoint |
| `com.apple.SpringBoard` | `/System/Library/CoreServices/SpringBoard.app/SpringBoard` | Stock SpringBoard, keyboard, status bar and Control Center service declarations |
| `com.apple.iconservices.iconservicesagent` | `/System/Library/CoreServices/iconservicesagent` | `com.apple.iconservices` |
| `com.apple.TextInput.kbd` | `/System/Library/TextInput/kbd` | Stock 17 TextInput services, including input management, preferences and lexicon |

Explicit native-host command-line labels replace those twelve defaults; the three fixed jobs remain included. For example,
`--launchd-services com.apple.notifyd` selects only notifyd as the additional job. Labels do not come from guest environment
variables. The mode writes its generated jobs under the owned `data/Library/LiveSimBooter-CoreDaemons-<selection-hash>` directory, with
per-label output in `data/Library/Logs/<label>.log`, under the selected group's `LSB/iOS/23A343` directory. Sorted selections have stable directory names; narrowing or
expanding a selection leaves earlier job files intact and only bootstraps the selected directory.

`LSBPrepareLaunchdServicesPlan` validates the selected matching-runtime plist, label, program and argument fields before
creating a bounded one-shot job. It preserves `Program`, `ProgramArguments`, `MachServices`, accepted `PublishesEvents`
and permitted stock environment entries. It sets `RunAtLoad` true, `KeepAlive` false and `EnableTransactions` false for
the experiment. Stock credentials, scheduling/resource policy, session restrictions and `LaunchEvents` are not reproduced.
Thus this mode tests selected daemon startup and ordinary service handling; it does not claim full stock launch policy.

Disabling `KeepAlive` avoids unconditional restarts, but requests to a declared Mach service can still relaunch a failed
daemon. The stock `containermanagerd` agent uses proxy modes for bundle and system containers; its separate system partner
is now included, but their presence alone does not establish successful container operations. Each parent retains 64
logical-child slots. This mode still accepts at most 14 selected jobs in addition to backboardd and cfprefsd, leaving room
for applications, compiler services and adopted children. The broker permits 132 physical jobs: the 64 children and their
queued exec replacements, plus simulator launchd and the backboardd, SpringBoard and public Metal helpers. Demand-launched
children consume the same parent ledger; unreaped exits and pinned observations also occupy slots.

Host graphics endpoint registration now shares one **30-second monotonic deadline** across the Indigo, IOSurface,
framebuffer and two Metal registrations. The larger initial xpcproxy batch can occupy launchd_sim long enough that the
previous one-second-per-RPC limit expired before it serviced a registration. Each original request and reply remains
alive within the shared budget; requests are not retried, because a timeout leaves their outcome unknown. This bounds
startup registration without duplicating endpoints or treating a delayed reply as permission to register again.

## Staged plan and concrete boundaries

The following order is an implementation plan inferred from the stock jobs and current project boundaries, not a minimal
set established by simslim:

1. Test the current eight selected jobs with the working backboardd/cfprefsd path. Confirm actual service replies and inspect
   each first failure before adding consumers. Notification delivery, LaunchServices registration, RunningBoard identity
   and container operations are distinct checkpoints.
2. Add further supporting jobs when a trace identifies their use, such as `com.apple.mobile.installd`
   (`/usr/libexec/installd`). The newly included configd_sim and system container manager still require service-level
   verification. distnoted has passed the serialized notification round trip below. The two container-manager jobs have
   different service names and argv; they are not substitutes.
3. Stage the matching SpringBoard executable, framework closure and resources, and provide `com.apple.sleepd` before
   judging SpringBoard's retry behavior. Its stock program is `/System/Library/PrivateFrameworks/SleepDaemon.framework/sleepd`,
   exporting `com.apple.sleepd.sleepserver` and `com.apple.private.alloy.sleep.classd-idswake`.
4. Follow observed startup requests for `mobiletimerd`, `donotdisturbd`, application protection and managed-settings jobs.
   Preserve simslim's negative evidence while avoiding a claim that every untouched job is needed immediately.

The stock SpringBoard job is **`com.apple.SpringBoard`**, while the app's bundle identifier is `com.apple.springboard`.
Its executable is `/System/Library/CoreServices/SpringBoard.app/SpringBoard` (134,544 bytes in the inspected runtime).
That executable directly links `/System/Library/PrivateFrameworks/SpringBoard.framework/SpringBoard`, whose standalone
binary is 35,179,968 bytes and is absent from the current [545-image cache](backboardd.md). `RunningBoard.framework` and
`SleepDaemon.framework` are absent too. Adding executables alone cannot satisfy those dependencies. Resources and dynamic
loads also need verification; dependency closure is separate from the daemon-selection list.

The host graphics services currently authorize the selected backboardd instance's full native audit token. The IOSurface
server has a single authorized owner, and the isolated native Metal helper retires when its owner's authorization
generation changes. SpringBoard must receive deliberate additional graphics routing if it requests those services; a
new job must not be accepted merely because its PID belongs to a simulator. Replacing backboardd's token with SpringBoard's
would revoke the working owner. Indigo and framebuffer presentation remain backboardd-owned. See
[IOSurface](iosurface.md) and [native Metal](metal.md) for the existing ownership boundaries.

One stock-plist collision also needs care: SpringBoard declares `com.apple.systemstatus`,
`com.apple.systemstatus.publisher` and `com.apple.systemstatus.activityattribution` when `SystemStatus/EmbeddedDaemon`
is disabled. `com.apple.systemstatusd` declares the same names when that flag is enabled. Preserve the mutually exclusive
conditions; flattening both dictionaries to unconditional services would assign the same endpoints to two jobs.

Device results for this selected-service stage should be recorded separately from these source findings. Existing
[backboardd display/input results](backboardd.md) do not establish that the additional daemons or SpringBoard have booted.

## Notification service on the phone

The 2026-10-26 trial uses the matching 23A343 runtime on iPhone16,2 / iOS 26.1. The first notifyd startup aborted because
`shm_open("apple.shm.notification_center", ...)` returned `EPERM`. `Guest/NotificationSharedMemory.c` adapts only that exact
object (with or without its leading slash), and only with `LSB_SIMULATE_NOTIFICATION_SHM=1`, to an owned regular file at
`SimulatorData/tmp/notification-center.shm` (the historical name of the current `data/tmp` location). It preserves creation/exclusion/truncation, normal file permissions, close-on-exec,
and descriptor/mapping lifetime across unlink. It does not change unrelated POSIX shared-memory objects.

notifyd publishes its PID to libnotify clients, which create native process-exit watches. The identity adapter therefore
returns the native PID for direct calls from the exact configured runtime notifyd executable. Other guest code retains
logical identity; `libsystem_notify` is not broadly switched to native identity.

The next failure appeared as Foundation namespace 19 / code 1 / SIGKILL in native process logs. LLDB instead caught the
actual instruction: notifyd offset `0x5ec4`, `brk #0xb001`, with `BUG IN LIBNOTIFY: Event publisher error` and error 1.
The initial publisher fetch and poll used two additional native-audit-PID job lookups in launchd_sim. The
[eight-site identity adapter](launchd.md) now resolves those through the existing authenticated broker; audit tokens and
event-stream authorization checks remain intact.

With these fixes, native host **83483**, simulator launchd **83485**, notifyd **83505**, and probe **83502** / logical PID
**83491** completed a real notification round trip. The probe registered two tokens, set/read the 64-bit state
`0x4c534200a6fa2007`, posted, observed the change on its first poll, read the same state, cancelled both tokens, and exited 0.
The six bootstrap lookups also succeeded. A lookup alone does not establish the functionality of the other five services.
Evidence: `tmp/springboard-services/probe-events-fixed.log`, `console-events-fixed.log`, and `lldb-notifyd-first-trap.txt`.
The older timeout at the beginning of the probe log belongs to the deliberately stopped debugger run.

That run also kept runningboardd, fontservicesd, distnoted and configd_sim alive and restored backboardd framebuffer updates.
configd_sim logged a denied `setiopolicy_np` request. LaunchServices then aborted while obtaining its system-container URL;
the container-manager agent had crashed first. LLDB located its null dereference inside CoreFoundation during
`NSISO8601DateFormatter` initialization from `MCMMigrationStatus`. The matching runtime's `/usr/share/icu` data was missing.
The next trial installed the original `usr/share/icu/icudt76l.dat` and `usr/share/icu/icutzformat.txt`, together with the
separate stock `containermanagerd_system` executable/job. Its libraries and 29 configuration plists were already staged.
Both container-manager processes then stayed alive: native agent **83660** and system partner **83657** in the observed
run. This removes the observed missing-ICU formatter crash; it does not yet establish successful container creation.
Evidence: `lldb-container-first-crash.txt`, `containermanagerd-system-inventory.json`, `icu-small-state.log` and
`lsd-after-icu.log` in the same temporary directory. The ICU hashes recorded on the phone match the staged runtime data.

In the intermediate trial, LaunchServices native **83659** got farther and aborted with
`Error getting container url (NOT_CODESIGNED)! Cannot continue.` while obtaining `systemContainerURL`. A later temporary trace identified the failed operation: system container manager **83810** called
`csops_audittoken(83808, CS_OPS_IDENTITY, ..., 1032, &token)` for native lsd **83808**, with the matching native PID and
pidversion in the audit token. The original syscall returned **-1 / EPERM (1)** at
`libsystem_containermanager.dylib+0x3668`. The same query for backboardd **83809** was also denied. The container manager
then translates the missing identifier into `NOT_CODESIGNED`; this result does not mean the mapped executable lacked a
signature. Its own libxpc entitlement query (`CS_OPS_ENTITLEMENTS_BLOB`, operation 7) succeeded.

This established a denied native cross-process identity query, not a logical-PID lookup failure. The temporary shim only
logged original results, preserved return/errno/output, and was removed after capture. The guest compatibility adapter and
successful request/reply below subsequently resolved this observed `NOT_CODESIGNED` boundary. Native signing policy is
unchanged. Historical evidence: `tmp/springboard-services/csops-phone-trace.log`
and `console-csops-trace.log`. Both native Mac interposition comparison and focused preservation tests passed for the
temporary tracer before the phone run.

The subsequent full-batch run used native host **83683** and simulator launchd **83685**, with the shared registration
deadline build. All five host endpoint registrations reported transport error 0 and bootstrap status 0: Indigo,
IOSurface, framebuffer, generic Metal and backboardd Metal. Both guest probes completed actual service operations:

| Probe | Native / logical PID | Verified result |
| --- | --- | --- |
| Distributed notifications | **83699 / 83689** | One main-thread callback; `delivered=1`, `invalid=0`; exact serialized payload returned in **0.381 seconds**; exit 0. |
| libnotify | **83704 / 83693** | Set/read state `0x4c5342000a6895ce`, posted, observed change on the first poll, read the same state, cancelled both tokens; exit 0. |

The distributed probe also resolved both distnoted service names, and the libnotify probe's six bootstrap lookups all
succeeded. Those lookup results do not establish the other services' request handling: LaunchServices **83703** still
aborted with `NOT_CODESIGNED` in the same run. Evidence is `tmp/springboard-services/probes-registration-budget.log`
and `console-registration-budget.log`; the latter confirms wait status `0x0` for both probes and `0x6` for that lsd
instance. The initial libnotify lines in the probe log are retained output from the earlier run, before the new
`pid=83704` entry.

The earlier checkpoint, committed as `614737c` (`Start simulator core services and verify notification delivery`),
enforced the 14-selected-job boundary and allowed the 34 physical jobs needed during a
full set of exec replacements. Current local boundary tests permit 16 selected jobs and cover all 64
replacement overlaps plus the three native Metal helpers. At that checkpoint the phone was left using the production shim
and the seven surviving additional daemons, with
LaunchServices omitted to avoid demand-driven abort/restart cycles:

```text
--launchd-services com.apple.notifyd com.apple.runningboardd com.apple.containermanagerd
  com.apple.containermanagerd.system com.apple.fontservicesd com.apple.distnoted.xpc.daemon com.apple.configd_sim
```

That was an explicit experimental selection, not a claim that SpringBoard had booted. The default selection retained
lsd; its historical failure is resolved by the later adapter below. That checkpoint's final verification added the
distributed-notification probe as a transient job: native **83841** / logical **83831**
again delivered one valid main-thread callback and exited 0, in **0.210 seconds**. Host **83825**, launchd **83827** and
backboardd **83842** remained running; all seven selected daemon PIDs were present in the final process snapshot. The
production shim SHA-256 matched `3206b627ef9a4023c81f919c50c39fa3d459c66fed945ba8abe71a0ade4576d2`, and the installed
extension still carried the increased-memory-limit entitlement. Evidence: `final-phone-state.json`,
`phone-liveprocess-final-entitlements.plist`, and `console-final-working-services.log` under `tmp/springboard-services`.
The final console also records one auxiliary replacement failing with `ENOENT`; its target has not yet been identified.
This did not terminate the seven selected daemons or framebuffer updates.

## LaunchServices identity compatibility and verified reply

`Guest/CodeSigningCompatibility.c` adapts guest `csops_audittoken` reads when `LSB_SIMULATE_CODE_SIGNING=1`; the host enables
this only in the selected-service simulator environment. XML and DER entitlement reads use the authenticated guest snapshot
first, as described below. STATUS, CDHASH, IDENTITY, TEAMID and VALIDATION_CATEGORY first issue the exact original syscall
and adapt only `EPERM`; their successful native calls and other errors pass through. Mutation and unknown operations remain
native. Neither the requested PID nor its audit token is rewritten, and `csops` itself is not interposed.

The initial fallback for denied identity/CDHash/team/category reads queried `csops(0, ...)`, using the current extension's real signature. The later per-executable CDHash change is described at the end of this note. This models the shared signed LiveProcess executable identity; it does not recover each mapped guest binary's
distinct signing identity or independently authenticate the requested peer. STATUS first requires a successful kernel
self query and then reports the guest policy `CS_SIGNED | CS_VALID`. The initial denied-entitlement fallback returned a
uniform `com.apple.private.MobileContainerManager.allowed=true` profile. This was sufficient for the LaunchServices trial
below, but omitted SpringBoard's `com.apple.backboard.client` and caused backboardd to reject its shell check-in. The
per-executable replacement is described below. These guest replies grant no native entitlement and change no kernel,
AMFI or sandbox policy. The bounded copy path preserves short-buffer/error behavior rather than directly dereferencing
unvalidated output addresses. Focused tests cover the read allowlist, original-result forwarding and invalid destinations.
STATUS normalization also uses `mach_vm_write`, so a destination becoming read-only after the self query returns `EFAULT`
instead of crashing the caller. The complete `Tests/run.sh` passed after this hardening; evidence is
`tmp/lsd-services/tests-final.log`.

With this adapter, native lsd **85195** reached its server run loop under LLDB. The captured CoreServices frames resolve to
`runServerMainRunLoop + 12` and `_LSServerMain + 2100`, after the prior container-identity abort point. This was a debugging
run with a temporary pre-main stop; evidence is `tmp/lsd-services/lldb-lsd-mcm-profile.txt` and `lsd-runloop-symbols.txt`.

The subsequent production-shim run also completed a real LaunchServices request. `Guest/LaunchServicesProbe.m` connects
to the guest `com.apple.lsd.mapdb` endpoint and invokes the declared
`getServerStatusWithCompletionHandler:` protocol method. Native probe **85314**, logical PID **85302**, received status
**`0x00000080`** from peer **85317** and exited **0**. The status is an opaque 32-bit reply; zero would also be a valid reply,
so success depends on receiving the callback, not on a nonzero status value. This proves request/reply handling for that
method, not application installation, registration or every LaunchServices endpoint.

The probe requires active platform 7 and matching `DYLD_ROOT_PATH`/`SIMULATOR_ROOT` before contacting the service. Its
earlier guard failure remains at the beginning of the results log; the later PID 85314 entry is the successful run.
It has an 18-second reply deadline and a 20-second hard timeout. Evidence is
`tmp/lsd-services/ipc-sleepd-v2-results.log`; `console-ipc-sleepd-v2.log` records the native probe's wait status `0x0`.

The same stage added matching original Sleepd inputs: 308 files comprising its executable, 72 missing strong dependency
images, stock job, and initial framework resources/metadata. The original inputs were staged separately, the 73 binaries
were signed with the established runtime identity, and deployed file hashes were checked. Sleepd reached its guest entry
and remained running in the observed service run; no Sleepd request/reply probe has established its functionality yet.
Evidence is `tmp/lsd-services/sleepd-staging.log`, `sign-sleepd.log`, `deploy-sleepd-hashes.log` and `ipc-sleepd-results.log`.

## First full-cache SpringBoard service trial

The matching SpringBoard executable and resources were published from the verified staging tree: **1,826 files**, with
the prior paths preserved in the trial's `before-activation` directory. Evidence is
`tmp/lsd-services/springboard-resource-publication.log`. This makes the original bundle available to the selected job;
file publication alone establishes no runtime result.

The 1,353-image cache and guarded main-stack handoff also supported a real LaunchServices reply in this service set.
Native probe **97188**, logical **97175**, received `getServerStatusWithCompletionHandler:` status **`0x00000080`** from
peer **97192**, and the broker recorded wait status **`0x0`**. The appended log preserves the earlier smaller-cache runs;
the PID 97188 entry is the full-cache result. The subsequent capacity-debug trial's probe **97285** / logical **97271**
timed out after 18 seconds, so the successful request must not be generalized to every later run. Evidence is
`tmp/lsd-services/full-cache-launchservices-results.log` and `console-springboard-full-debug.log`.

SpringBoard initially failed before its debug constructor because its image set exceeded the debugger archive's
1,024-image limit. After increasing the shared bound to 2,048, native **97287** / logical **97273** reached the intentional
constructor stop before main. The [debugger capacity notes](debugging.md#larger-springboard-image-snapshots) describe the
bounded serialization change and tests. This is a startup checkpoint, not a completed SpringBoard boot.

The full service trial exposed two Objective-C exceptions:

| Process | Observed exception | Call path |
| --- | --- | --- |
| backboardd | `-[MTLSimDevice featureProfile]: unrecognized selector` | Metal device setup, reached from QuartzCore window-server startup |
| sleepd, native 97194 | `-[CHSTimelineController initWithExtensionBundleIdentifier:kind:]: unrecognized selector` | Sleep widget timeline construction during SleepDaemon initialization |

Both methods are present in the matching files inspected on disk. LLDB subsequently found loose CoreRoutine and Sleep
images allocated inside unmapped holes in the cache's broad address range. The
[whole-cache reservation fix](shared-cache.md#reserving-the-gaps-between-cache-mappings) prevents these ordinary images
from being classified as cache contents by address. The original exception stacks remain in
`tmp/lsd-services/full-services-first-failures.log`; they did not establish missing implementations.

The first reserved-cache run under host **97445** made further progress: backboardd **97466** registered `CAWindowServer`
with a 1290×2796 swapchain and established three native Metal connections without reproducing the `featureProfile`
exception. Sleepd **97469** remained alive without the preceding timeline-controller initializer exception. Probe **97463**
/ logical **97450** again received LaunchServices status `0x80`, this time from peer **97467**, and exited 0. SpringBoard
**97465** reached the constructor stop, and its loose CoreRoutine class moved outside the cache range to `0x1079bb050`.
The duplicate `RTHangsMetrics` class warning remains. Evidence is `tmp/lsd-services/console-springboard-reservation.log`
and `reservation-initial-guests.log`; those appended guest logs also contain older failures, distinguishable by PID.
These observations do not establish a functional Sleepd request, complete service startup or SpringBoard boot.

### SpringBoardUI initialization guard

After the earlier native SpringBoard **97287** constructor stop was resumed, it raised
`NSInternalInconsistencyException` during SpringBoardUI initialization: `This process should not be linking or loading
SpringBoardUI.framework`. Evidence is `tmp/lsd-services/springboard-first-exception.log`. This was a distinct framework
guard after passing the compact-image limit, not a failure to locate the framework.

`Guest/SpringBoardEntitlements.c` now provides a guest-only report of the stock simulator SpringBoard entitlement
`com.apple.springboard-ui.client`, verified in the original executable's `__TEXT,__entitlements` plist. It calls
`BSSelfTaskHasEntitlement` first, preserving an existing positive answer and forwarding every other key. The fallback
requires `LSB_SIMULATE_SPRINGBOARD_ENTITLEMENTS=1` plus the canonical regular SpringBoard executable at the expected path
under `DYLD_ROOT_PATH`. A missing weak original symbol fails closed. The host adds this opt-in only for the exact selected
`com.apple.SpringBoard` label and `/System/Library/CoreServices/SpringBoard.app/SpringBoard` program; stock environment
strings cannot select it for another job.

This reports one guest framework policy value. It grants no native signing entitlement or host-service privilege.
On resuming native **97465**, the prior SpringBoardUI exception did not recur and the hardware exception breakpoint did
not fire; the process instead exited with status **255**. The reserved-cache mapping checks also confirmed no-access
regions at the tested gap starts and loose CoreRoutine/Sleep images outside the cache range. Evidence is
`tmp/lsd-services/lldb-reserved-cache-mappings.txt`, `springboard-reservation-after-resume.log` and
`console-springboard-reservation.log`. The follow-up below identifies the same exit path in a fresh run; this is not a completed SpringBoard boot.

### Backboardd versioned identity and SpringBoard's display-service exit

Under host **97530**, SpringBoard native **97552** reached a hardware breakpoint at `_exit` with status `-1` (255).
The actual caller was `BKSDisplayServicesStart`: its `exit(-1)` instruction ends exactly at the next function's entry,
so a nearest-symbol lookup incorrectly labels the return address `0x1246ed0c0` as `_BKSDisplayServicesGetMainScreenInfo`.
The matching iOS 26 instructions show that this exit requires two consecutive `MIG_SERVER_DIED` (`-308`) replies from
the `com.apple.backboard.display.services` main-screen query. The call chain is `SBSystemAppMain` →
`FBSystemShellInitialize` → `FBSystemShell._initWithOptions:` → `BKSSystemShellService.initWithConfigurator:` →
`BKSDisplayServicesStart`. The failure occurs before `UIApplicationMain`.

In that run, backboardd native **97553** / logical **97538** aborted while handling a BoardServices XPC request with
`Failed to validate current vpid.` The exact iOS 26 BaseBoard implementation obtains an authentic current audit token,
combines its native PID and PID version, then compares the low PID word against `getpid()`. Our stable guest PID differs
after the xpcproxy `SETEXEC` replacement, causing the assertion. This explains SpringBoard's subsequent dead-server reply;
the display-service failure does not call for returning fabricated screen data or bypassing its error check. The
[user-supplied iOS 26.1 decompilation](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/blob/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoard/BaseBoard_01.mm#L2604-L2614)
provided the lead. Disassembly of the actual simulator iOS 26.0 23A343 framework independently confirms it: the once block
at original offset `0xe3ac` calls `getpid` at `0xe3dc`, then compares `w0` with the audit-derived PID in `w19` at `0xe3e0`.

Evidence is `tmp/lsd-services/lldb-springboard-exit-minus-one.txt`, `backboard-vpid-guest.log` and
`console-springboard-exit-debug.log`. Guest log prefixes show logical PID **97538**; the broker log records its replacement
by native PID **97553** and subsequent signal 6 exit. The same console also records two rejected native Metal connections
from SpringBoard **97552**, because the current graphics authorization covers only backboardd. That separate multi-client
graphics requirement remains pending. The stopped SpringBoard was later terminated during cleanup, so the broker's final
signal 9 status is distinct from the captured impending `exit(-1)`.

`Guest/ProcessIdentity.c` now preserves native `getpid()` identity for direct calls from the exact BaseBoard framework
install path or its configured RuntimeRoot path, as it already does for libxpc. The change leaves ordinary guest callers
on the stable logical PID and does not modify audit tokens or their PID versions. Focused unit and real dylib-interposition
tests cover the exact path, unrelated same-name images, image removal and ordinary guest callers.

LLDB then attached to backboardd native **97619** after startup. A real SpringBoard connection reached the comparison at
`0x1248e43e0`, with **`w0 == w19 == 0x17d53` (97619)**. This directly verifies the corrected input to the original guard;
the assertion was neither skipped nor patched. The trace is `tmp/lsd-services/lldb-baseboard-vpid-fixed.txt`.

That late-attach debugging session also intercepted the known workqueue entry fault at `0x1e4a538b8`. Validated upcall
register states were manually redirected to simulator entry `0x162ae0a74` to reach the identity breakpoint, and another
workqueue fault appeared after continuing. This was a debugger correction, not a production code change or proof of an
uninterrupted boot. The diagnostic run was terminated, the production shim restored, and a fresh normal run started in
`tmp/lsd-services/console-services-production-vpid.log`; its result remains separate from the comparison proof above.

The fresh normal run used host **97647**, backboardd **97668**, lsd **97670** and Sleepd **97672**. All four remained alive
after 84 seconds and repeated SpringBoard connection attempts. LaunchServices probe **97665** received status `0x80`
from lsd **97670** and exited 0. No new vpid assertion occurred. SpringBoard **97667**, **97684** and later replacements
exited 0; the native Metal helper rejected their connections because its current authorized owner is backboardd. These
observations identify a remaining separate graphics route, not a completed SpringBoard boot or proof that Metal rejection
alone caused those exits. Evidence: `production-survivors.log`, `production-backboardd.log`,
`production-com.kdt.LiveSimBooter.launchservices-probe.log` and `console-services-production-vpid.log` under `tmp/lsd-services/`.

The retrying SpringBoard test was stopped. Host **97723** was then launched with the same core-service set excluding
SpringBoard, using the production shim without a diagnostic SIGSTOP constructor and the full reserved cache. Its probe
**97740** exited 0 (`console-core-services-final.log`). The integrated suite passed after the reservation and entitlement
changes (`tests-reservation-integrated.log`); the later BaseBoard identity change additionally passed focused unit,
ASan/UBSan and actual dylib-interposition checks.

## Separate SpringBoard graphics route

The broker now provisions a second native Metal LiveProcess only for the host-selected, normalized stock SpringBoard job.
Its endpoint is registered as `com.apple.metal.simulator.SpringBoard`; the generic endpoint and `.backboardd` still belong
to the first helper. Each helper has its own authorization page, full native audit identity and stock `processRef` namespace.
The stock host `IOSurfaceRemoteServer` accepts two independently revocable owner records. Indigo and framebuffer remain
backboardd-only. The [Metal notes](metal.md#isolated-native-service-route) describe registration and lifetime handling.

On the iPhone, host **97774** started native helpers **97775** and **97776**, and all six endpoint registrations returned 0.
SpringBoard **97795** opened three accepted connections through helper **97776**; backboardd **97796** independently opened
three through **97775** and registered the 1290×2796 framebuffer and IOSurface connection. SpringBoard then exited with
status 0, retiring only its own helper. Backboardd and its graphics helper remained alive. This verifies the process-specific
Metal route and isolated cleanup; it does not establish that SpringBoard drew a frame or completed startup. Evidence is
`tmp/lsd-services/console-springboard-metal.log`. The next exit path is under investigation.

The two-owner IOSurface tests use real subprocess identities and the stock IPC implementation, including shared-pixel
surface preservation when the other owner is revoked. Normal and ASan/UBSan runs passed. The complete `Tests/run.sh`
also passed after the production changes (`tmp/lsd-services/tests-springboard-metal-integrated.log`), and the signed iOS
build succeeded. Those local IOSurface tests are distinct from a SpringBoard IOSurface connection observed on the phone.

## Reproducing the service probes

Build the three standalone arm64 simulator probes from the repository root:

```sh
Tools/build-core-services-probes.sh tmp/core-services-probes
```

This produces `core-services-probe`, `distributed-notification-probe` and `launchservices-probe`; signing and installation use the existing
RuntimeRoot executable workflow. Run them as guest jobs under the simulator launchd bootstrap, so their lookups and IPC
reach the selected simulator services. Merely running them under the native host bootstrap would test different daemons.

`Guest/CoreServicesProbe.c` performs bootstrap lookups and the libnotify state/post round trip described above.
`Guest/DistributedNotificationProbe.c` is pure C and separately registers with
`CFNotificationCenterGetDistributedCenter`, posts a unique name/object with a property-list payload, and validates the
returned nonce, 64-bit number and binary bytes. It pumps the main CFRunLoop for at most five seconds, with a 20-second
hard process timeout. The matching simulator CoreFoundation sends this registration and post over its distnoted XPC
connection and delivers the callback from an inbound `post_token`; the distributed post does not dispatch directly to
its local observer table.

The distributed probe has compiled for the simulator and passed against the Mac's actual distnoted, with one valid
main-thread callback and exit 0. Its sandbox-denied Mac run produced no callback and failed as expected. Those are local
probe checks, separate from the successful iPhone service run above. Local evidence is
`tmp/distributed-notification-probe/macos-native.log` and `macos.log`.

## Cache capacity and provisioning

The established 545-image cache reserves **1067.27 MiB**, from `0x120000000` through `0x162b44000`, leaving **468.73 MiB** below
`0x180000000`. Adding SpringBoard's full strong dependency closure while retaining the current set gives 1347 images:
their original TEXT segments alone total **1482.61 MiB**, with roughly 249 MiB of data before LINKEDIT. These are input
segment totals. A subsequent offline build included that union plus the supporting daemon/Sleepd images, producing
**1353 images** at **`0x108000000`** with the builder's local-symbol stripping option. Its RX mapping ends at
`0x164594000`, and all writable mappings also remain below `0x180000000`; only read-only LINKEDIT crosses that boundary.
The complete reservation ends at `0x186194000`. The first phone trial replaced the live native stack with read-only
DATA_CONST. A conditional guarded-stack handoff then allowed complete cache loading: TEXT remained `r-x`, and LINKEDIT
crossed `0x180000000` as `r--`. That second run reached Foundation initialization and stopped in featureflags because its
direct df environment omitted `SIMULATOR_ROOT`, already supplied by the launchd configuration.
After supplying both simulator-root and shared-resources-directory variables, native **85403** under host **85402**
printed the complete df table and exited **0**, verified by the broker wait status. No matching amfid/TXM events appeared
in the concurrent capture. Complete service startup and SpringBoard boot with this cache remain pending. The
[full-cache notes](shared-cache.md#springboard-closure-offline-build) separate the offline checks, initial failures and
successful df execution; the successful run is `tmp/lsd-services/console-full-cache-df-final.log`.

The refreshed LiveProcess provisioning profile permits `com.apple.developer.kernel.increased-memory-limit`; adding the key
to `LiveProcess/LiveProcess.entitlements`, rebuilding, and installing produced an on-device extension signature containing
`true`. This verifies provisioning/signing and successful launch, not a measured memory ceiling. It does not enlarge the
executable virtual-address window. See [installation notes](debugging.md#install-and-transfer).

## Per-executable guest entitlement snapshots

The matching 23A343 `libsystem_sim_kernel.dylib` intercepts `CS_OPS_ENTITLEMENTS_BLOB` (operation 7), resolves the target
with `proc_pidpath`, and reads its Mach-O `__TEXT,__entitlements` XML section. Other operations delegate to the host syscall.
That stock path resolves to the native LiveProcess executable in this loader. The guest executable has different mock
entitlements, even after its native code signature has been replaced. Stock SpringBoard retains 498 keys in
`__TEXT,__ents_der` (33,208 bytes); backboardd retains 111 keys (6,757 bytes).

The host snapshots each simulator job's `__TEXT,__ents_der` before replying to its initial check-in. The reader accepts
the version-1 DER dictionary used on iOS 15 and later, validates bounded Mach-O sections and DER values, and never parses
XML input. The native host's `xpc_create_from_ce_der` converts this dictionary once; a generated XML blob supports existing
operation-7 callers. Operation 16 returns the original DER. Native conversion tests compare all entries against the
original stock XML as a test oracle. The simulator's export of that decoder is a NULL-returning stub, so conversion runs
only in the host, which retains native libraries.

The synchronous XPC request includes the complete target audit token. The broker requires both caller and target to be
live simulator jobs with matching full audit identities; retired exec instances, stale PID generations and native Metal
helpers are excluded. Replies use immutable snapshots, so a later file replacement does not change a running job's profile.
Missing or malformed DER is cached as an error. Relative executable paths currently return `ENOTSUP` for entitlement
queries because their resolution can depend on ordered child CHDIR/FCHDIR actions; the launch itself is unaffected.

With the opt-in enabled, XML/DER operations 7 and 16 now use this lookup before issuing any native `csops_audittoken` query.
Native self reads describe LiveProcess, and the matching Security framework caches `EINVAL` or empty success as an empty
entitlement dictionary. Waiting for native `EPERM` can therefore hide an existing guest entitlement. Missing snapshots,
unknown or stale identities, and transport errors fail with their original errors; they never fall back to the extension's
native profile. Exact full-audit validation and bounded kernel copies still govern every reply. With the opt-in disabled,
both operations forward unchanged. All other signing operations retain their earlier behavior.

This change follows Spotlight's SpringBoardUI `+load` assertion: the stock guard first asks for
`com.apple.springboard-ui.client`, which is true in Spotlight's original DER (184 keys, 13,874 bytes). BaseBoard obtains
the genuine task audit token and calls Security directly. The new contract fixes the entitlement source rather than
authorizing an executable name or bypassing the framework assertion. Focused tests cover Security's eight-byte size probe,
full XML parsing, native success/`EINVAL`/`ERANGE` cases, invalid destinations, missing DER, stale identities and opt-out;
they pass, as does simulator syntax validation. Spotlight's on-device result is still pending. The existing narrow
SpringBoard-only self-query adapter remains in place; this is not complete emulation of every SecTask path.

Host caching removes repeated file reads and conversion but adds synchronous IPC and reply copying. A warm local cache
would avoid those costs; a warm filesystem read may also beat IPC. Performance must distinguish this stock simulator file
path from native kernel `csops`, and local microbenchmarks from end-to-end iPhone timings.

A macOS microbenchmark using the production transport APIs and the stock SpringBoard payload measured these medians
over 25,000 samples per case, with warmed file pages:

| Operation | Median |
| --- | ---: |
| Cached host XML reply over synchronous XPC | 25.8 µs |
| Local path query plus open, Mach-O section scan and XML read | 10.5 µs |
| Two host XML requests, corresponding to size probe and full retrieval | 51.7 µs |
| Stock-like size probe and full file retrieval | 19.5 µs |

These are retrieval measurements, not complete SecTask timings or measurements of Apple's exact simulator function.
They omit consumer plist parsing, the guest's final kernel copy, and production broker scheduling load. The benchmark's
`proc_pidpath(self)` approximates the path-query cost; it reads the verified stock executable separately. Its native test
broker-port lookup also differs from the guest's handed-off port. Apple’s
[SecTask implementation](https://github.com/apple-oss-distributions/Security/blob/main/sectask/SecTask.c) caches the parsed
dictionary within each task object, so repeated key lookups on the same object avoid either retrieval path. The result
supports using the host cache for correct guest identity and immutable profiles, without claiming a speedup over warm
stock file access. Harness, raw samples and results are in `tmp/entitlements-benchmark`.

On the phone, host **97994** served SpringBoard **98012** and backboardd **98013**. Hardware breakpoints at backboardd
`+0xa78c` and `+0xa958` observed the peer `com.apple.backboard.client` result **w0=1** and the admitted-shell lookup
**w22=1**, respectively. No register overwrite, forced return or migration skip was used. This verifies the formerly
denied peer entitlement and subsequent shell membership, not complete SpringBoard boot. After resuming and detaching,
backboardd exited with signal 10 and SpringBoard exited 0; the subsequent fault was not captured by those breakpoints.
Evidence: `tmp/lsd-services/lldb-backboard-entitlements-cache.txt` and `console-entitlements-cache-debug.log`.

The full integrated test suite and signed device build passed. The reader, converter, broker, transport and guest adapter
also passed their focused ASan/UBSan checks. The temporary constructor-stop shim was replaced with the signed production
shim after the LLDB check.

A fresh run without LLDB, host **98046**, kept SpringBoard **98067**, backboardd **98068**, and both native Metal helpers
alive for the observed 84 seconds. Both guest processes established their Metal connections and backboardd registered
the 1290×2796 framebuffer. LaunchServices probe **98065** received status `0x80` from lsd **98069** and exited 0. The
concurrent amfid/kernel-TXM capture contained no matching events. This removes the immediate peer-entitlement rejection;
it does not establish that SpringBoard completed startup or drew its interface. The production run was left active.
Evidence: `console-entitlements-cache-production.log`, `production-entitlements-check.txt` and
`security-entitlements-cache-usb.log` under `tmp/lsd-services`.

## DataMigration services and RunningBoard identity

After USB disconnection, subsequent SSH and rsync operations use Wi-Fi at
`root@iphone-0xf-26-cua-duy.local:2222`. A live backboardd stack showed a synchronous DataMigration message-0 request
waiting for its service. The matching stock bundles `com.apple.datamigrator.xpc`, `com.apple.migrationpluginwrapper.xpc`
and `datamigratorhelper.xpc` were then deployed beneath
`RuntimeRoot/System/Library/PrivateFrameworks/DataMigration.framework/XPCServices`. Their executables and Info.plists
passed the publication hash checks. In the next run, host **98155** launched datamigrator as native **98186**, logical
**98185**, and recorded exit status **0**. This run used no debugger return or migration skip. It verifies service
startup and completion of this invocation, not execution of every possible migration plugin.
See the [blocked stack](../tmp/lsd-services/lldb-wifi-backboard-migration.txt),
[publication checks](../tmp/lsd-services/datamigrator-stage/publication-wifi.log) and
[launch/exit log](../tmp/lsd-services/console-wifi-datamigrator.log).

SpringBoard **98176** subsequently failed during FrontBoard initialization with an inert RunningBoard process handle
whose PID was `-1`: the crash report records `invalid pid for <inert:[anon<SpringBoard>:-1]*>`.
The [crash report](../tmp/lsd-services/SpringBoard-98176.ips) is separate from the successful datamigrator exit.

In a later diagnostic run, SpringBoard native **98243**, logical **98242**, reached RunningBoardServices' call to
`sandbox_check(98242, "mach-lookup", 0x40000002, "com.apple.runningboard")`. The real call returned **1**, denying the
lookup. Repeating that same read-only check with native PID **98243** returned **0**. No success result was forced and
no audit token was changed. The [hardware-breakpoint capture](../tmp/lsd-services/lldb-wifi-rbs-sandbox.txt) records both
calls and the service-name argument.

The guest identity adapter now returns native identity for direct `getpid` callers in the exact RunningBoardServices
framework, following the existing narrow BaseBoard/libxpc treatment. Ordinary guest callers retain their logical PID;
unrelated paths with the same leaf name do not receive the exception. Focused unit, real interposition, ASan/UBSan and
simulator syntax checks passed; see the [unit result](../tmp/lsd-services/rbs-identity-tests/unit.log),
[interposition result](../tmp/lsd-services/rbs-identity-tests/interpose-RunningBoardServices.log) and
[sanitizer result](../tmp/lsd-services/rbs-identity-tests/asan.log). A fresh deployed run subsequently verified the specific
sandbox-check fix: SpringBoard **1891** received **0** without argument or result overrides.

After the diagnostic sandbox-check replay, the RunningBoard handshake still produced `NSCocoaErrorDomain` code **4101**
(`NSXPCConnectionReplyInvalid`). RunningBoardServices itself wraps an `XPC_TYPE_ERROR` synchronous response with this
code before decoding an ordinary reply. The observation therefore establishes an unresolved raw XPC connection error;
it does not establish malformed serialized data. See the [reply capture](../tmp/lsd-services/lldb-wifi-rbs-reply.txt).

In that fresh run, SpringBoard's raw reply was `XPC_ERROR_CONNECTION_INVALID` at `0x16df73980`. The server-side trace in
runningboardd **1887** found a false process-existence preflight. A debugger-only branch skip then reached the real
`proc_pidinfo(1891, 13, 0, buffer, 64)` call, which returned **0** with errno **1** (`EPERM`). This demonstrates a denied
peer process-information query beyond the preflight; merely forcing the preflight true cannot supply the required data.
The daemon trace also required the known workqueue-entry redirection while LLDB was attached. That part is diagnostic
evidence, not an uninterrupted production success. Both the clean client sandbox result and the separately modified
server investigation are recorded in the [native-identity trace](../tmp/lsd-services/lldb-wifi-rbs-native-identity.txt).

The matching UIKitCore, SpringBoardHome and SpringBoardUIServices data resources were uploaded to the inactive
`Library/ServiceTrials/ui-resources-20261026/incoming` directory, then published into RuntimeRoot:
**657 files, 15,377,390 bytes**, with final hashes verified. Differing existing files were backed up in the trial's
`before-activation` directory. The stage excludes Mach-O executables and `_CodeSignature` contents. Publication verifies
the files on disk; guest resource loading and successful drawing remain unconfirmed. See the
[resource manifest](../tmp/lsd-services/springboard-ui-resources/manifest.json) and
[publication result](../tmp/lsd-services/ui-resources-publish-wifi.log).

## Managed peer identity snapshots

To support the observed denial of peer `proc_pidinfo` calls, native LiveProcess now publishes its own 64-byte
`PROC_PIDT_SHORTBSDINFO` record and a task-name send right during check-in, before simulator code runs. The broker checks
that the record's PID and the port's `TASK_AUDIT_TOKEN` match the kernel-authenticated sender. It preserves the record's
actual bytes and associates them with the guest executable path from the launch plan; it does not fabricate process flags.
The port is created with `task_name_for_pid(self)` by the trusted native capture path. A send-right check and
`TASK_AUDIT_TOKEN` validate the received identity, but do not independently distinguish a task-name right from a stronger
task port. The flavor guarantee therefore depends on that native capture path.

Peer lookup is initially restricted to the exact managed `/usr/libexec/runningboardd` executable. Both sender and target
must be committed, live simulator jobs; the complete caller audit token must match, and the retained target port is
revalidated against its complete recorded audit token. Native helper roles, retired exec instances and finishing jobs
are excluded. Relative executable paths cannot supply a snapshot because their resolution can depend on child-only file
actions. The reply copies the name-port reference; the broker releases its own reference on job finish, including failed
startup, while already-copied reply rights retain their independent ownership.

Focused broker tests cover real kernel task-name validation, full-audit authentication, immutable snapshot/path capture,
PID reuse and exec retirement, foreign/receive/send-once/dead ports, and rejection/finish reference cleanup. Normal and
ASan/UBSan runs passed, as did strict arm64 iPhoneOS syntax checking. See the
[normal result](../tmp/lsd-services/peer-broker-tests/normal.log) and
[sanitizer result](../tmp/lsd-services/peer-broker-tests/asan.log). The initial device trials below exposed a path-snapshot
failure before the subsequent successful peer lookups.

The full integrated [test run](../tmp/lsd-services/tests-wifi-peer-identity.log) and
[signed app build](../tmp/lsd-services/build-wifi-peer-identity-final.log) passed. A subsequent guest-helper hardening change
passed its focused sanitizer tests; the full-suite result predates that last focused change. The app was then installed
over Wi-Fi into bundle container `6ABA5C90-A482-4BDB-9DBD-3827B8AC29C9`, with the diagnostic guest shim SHA-256
`5f08eb6ed2a3058ba21ad27739087950f22efb3f71a3e5e00968e53188b6265e` deployed for the next capture. See the
[installation result](../tmp/lsd-services/install-wifi-peer-identity-reconnected.log). The initial device capture is
[console-wifi-peer-identity.log](../tmp/lsd-services/console-wifi-peer-identity.log); the retry and its result
are described below.

The [next launch](../tmp/lsd-services/console-wifi-peer-identity-retry.log), host **1941**, completed native child check-ins,
verifying self-snapshot and task-name export on the phone. Peer queries from runningboardd **1959** were nevertheless
rejected with `EACCES`. Host LLDB showed all eight caller audit words matched, the job was running in simulator role,
and neither retirement nor finish was active. The actual failure was `peerIdentityError=EINVAL` with an empty executable
path. See the [host rejection capture](../tmp/lsd-services/lldb-wifi-peer-host-denial.txt).

The snapshot path resolver now uses canonical filesystem containment instead of comparing a physical path with
Foundation's standardized string. It accepts physical aliases only when their resolved file remains inside canonical
RuntimeRoot, resolves guest absolute paths beneath that root, and checks the opened regular file's `F_GETPATH` result.
Lexical dot/empty components and escaping symlinks are rejected. The exact runningboardd path gate and full audit checks
remain unchanged. Alias, containment and right-cleanup regressions passed in
[normal](../tmp/lsd-services/peer-broker-tests/normal-path-fix.log) and
[ASan/UBSan](../tmp/lsd-services/peer-broker-tests/asan-path-fix.log) runs; strict arm64 iOS compilation also passed.

The corrected app was installed and launched with production shim `libLiveSimSpawn-peer-path.dylib`, SHA-256
`7e40a9abc6390afd6ea27de4ddf1f346e61929cead532197907b40337f06e475`. In host **2001**'s fresh run, runningboardd **2032**
successfully queried native SpringBoard **2022**, PID version **243529**, and other managed peers: the broker returned
**error 0**. This verifies on-device use of the authenticated identity bridge after the path correction. See the
[installation result](../tmp/lsd-services/install-wifi-peer-path-retry.log) and
[successful lookup log](../tmp/lsd-services/console-wifi-peer-path-retry.log). The final integrated
[Tests/run.sh run](../tmp/lsd-services/tests-wifi-peer-path-final.log) also passed.

SpringBoard still did not complete startup. It exited with `SIGTRAP` at FrontBoard **`+0x4048`**
(`0x131b22048`), with the diagnostic `already have a source registered for 2022: <FBWorkspaceEventDispatcherSource: ...>`.
This is the next observed failure after successful peer lookups. The
[SpringBoard 2022 crash report](../tmp/lsd-services/SpringBoard-2022.ips) and broker wait status `0x5` establish the exit.
Neither a running SpringBoard interface nor successful UI drawing is established by this trial.

The matching FrontBoard disassembly explains the duplicate-source assertion. `registerSourceWithProcessHandle:` obtains
the native handle PID at framework offset `+0x3d5c` and finds an existing source. It deliberately permits that duplicate
for its own process, but the `getpid` call at `+0x3da4` still returns the logical guest PID. Comparing it with native PID
**2022** at `+0x3da8` fails, and the branch at `+0x3dac` takes the assertion path ending at `+0x4048`.
See the [matching 23A343 disassembly](../tmp/lsd-services/frontboard-23A343-disassembly.txt). The compatibility change
extends the exact-image native `getpid` treatment to FrontBoard, preserving ordinary guest PID behavior and the
duplicate-source check.

The production FrontBoard fix, shim SHA-256 `f17a3d8970b053849d377af852f2c2784cc7e58ea09ea59d9f19be2ab3ba32c2`, passed the
full [integrated test run](../tmp/lsd-services/tests-wifi-frontboard-native.log). The first device attempt, launchd
**2106**, stopped before guest entry because the cache reservation overlapped resident loader storage or images. The
[preflight failure log](../tmp/lsd-services/frontboard-launchd-initial-failure.log) records that address-layout rejection;
it was separate from FrontBoard execution. A fresh launch succeeded.

On that retry, host **2113** launched SpringBoard **2134**, which passed the former duplicate-source failure, established
its native Metal connections, and reached `UIApplicationMain`. It then terminated with an uncaught
`NSInternalInconsistencyException`: required `dlopen` of
`/System/Library/PrivateFrameworks/RTTUtilities.framework/RTTUtilities` failed because the image was absent from RuntimeRoot
and the cache. The [guest exception and stack](../tmp/lsd-services/springboard-frontboard-native-tail.log),
[crash report](../tmp/lsd-services/SpringBoard-2134.ips), and
[host console](../tmp/lsd-services/console-wifi-frontboard-native-retry.log) agree on the failure and `SIGABRT` exit.
This verifies progress through the FrontBoard identity fix and into UIKit startup, not a running SpringBoard interface.
The matching RTTUtilities framework was subsequently published and loaded as recorded below.


## RTTUtilities and ExtensionKit discovery

The matching 23A343 RTTUtilities framework was signed with the existing app identity and identifier, without entitlements,
and published with **117 files / 763,546 bytes**. All required dylib dependencies were already in the 1353-image cache;
the absent LiveTranscription dependency is weak and was not added. The executable SHA-256 is
`1b5e7a890a593418335f952c1529658ae21f523a21dba325e9acce08e6e1ad08`. Publication verified every file hash, checked existing
Info.plist consistency and retained the prior framework resources in its trial backup. See the
[manifest](../tmp/lsd-services/rtt-stage/manifest.json) and [publication result](../tmp/lsd-services/rtt-stage/publication-wifi.log).

In SpringBoard **2218**, LLDB identified RTTUtilities at **`0x30db7c000`**. A hardware breakpoint at FrontBoard's self-PID
comparison observed **w25 = w0 = 2218**. This verifies the intended self-duplicate exception without changing its branch,
return value or workqueue entry. The next failure was a Swift abort while casting `_NSXPCDistantObject` to
`ExtensionFoundation._EXDiscoveryServiceProtocol`. See the
[LLDB transcript](../tmp/lsd-services/lldb-wifi-frontboard-rtt.txt) and
[guest diagnostic](../tmp/lsd-services/springboard-rtt-lldb-tail.log).

The stock ExtensionFoundation getter at `+0x5ece0` requests the BoardServices endpoint for
`com.apple.extensionkitservice`, service `Discovery`. If that lookup returns nil, its fallback at `+0x5f15c` creates a plain
NSXPCConnection without setting a remote object interface. The caller then force-casts its proxy at `+0x60ae0`.
In the stopped process, the existential at `0x3022f25d8` contained proxy `0x30776ef90`; the proxy's interface field at
`+0x10` was **null**. Foundation's `conformsToProtocol:` consults that interface's protocol, so this observation explains
the failed cast without requiring damaged Swift metadata or a cast override.

The stock bundle is
`/System/Library/Frameworks/ExtensionFoundation.framework/XPCServices/extensionkitservice.xpc`, not a LaunchDaemon plist.
Its Info.plist identifies an Application XPC service and declares the BoardServices `Discovery`, `Launch`, `Observer` and
`TCCProxy` services. Its two required files were signed/published and hash verified: **2 files / 69,658 bytes**, executable
SHA-256 `6b0f3b6c6ee7931d80a948b92f8b4caf9f965bc506d2bba1a348a66150331f91`. The executable retains `MH_EXECUTE`, with
`CS_EXECSEG_MAIN_BINARY` cleared and no added entitlements. All dependencies are already cached. See the
[manifest](../tmp/lsd-services/extensionkit-stage/manifest.json) and
[publication result](../tmp/lsd-services/extensionkit-stage/publication-wifi.log).

Publishing that bundle alone did **not** fix discovery: production SpringBoard **2319** reproduced the same cast abort,
and no extensionkitservice launch was observed. See the
[retry failure](../tmp/lsd-services/extensionkit-production-failure.txt) and
[host console](../tmp/lsd-services/console-wifi-extensionkit.log). BoardServices calls libxpc's
`xpc_endpoint_create_bs_service`, which resolves the calling process's PID domain directly. This path does not use the
CFBundleCreate resource adapter. The earlier [Metal compiler registration investigation](metal.md#guest-compiler-bundle-registration)
identified the same issue with importing physical RuntimeRoot bundles from cache image paths.

`Guest/ExtensionKitRegistration.c` now registers this one physical bundle using the normally declared
`xpc_add_bundle(path, 2)` API. The host sets `LSB_REGISTER_EXTENSIONKIT=1` only on selected jobs matching both their exact label
and effective Program: `com.apple.SpringBoard` at `/System/Library/CoreServices/SpringBoard.app/SpringBoard`, or
`com.apple.PosterBoard` at `/Applications/PosterBoard.app/PosterBoard`. The constructor
also checks the actual canonical executable, canonical RuntimeRoot containment, and nonempty regular readable Info.plist
and service executable. Inherited variables cannot activate it in xpcproxy, launchd or the service itself. It runs after
libSystem initialization, not from a dyld image callback. Because the API returns void, its log reports a registration
request and return, not successful service activation. Focused role/path/configuration tests, ASan/UBSan and simulator
compilation checks passed. The subsequent device trial below passed the former nil-interface cast failure.

## Signing snapshot aliases and distinct CDHashes

The broker's signing snapshot path now shares canonical filesystem containment and descriptor-path validation with peer
identity snapshots, allowing physical `/var`/`/private/var` aliases without treating them as unrelated guest paths.
With that change, host **2298** kept lsd **2321** alive for the observed **80 seconds**; LaunchServices probe **2317** received
status **`0x80`** from that exact daemon. This is a real service reply, not merely a bootstrap lookup. See the
[process snapshot and reply](../tmp/lsd-services/wifi-alias-lsd-survival.txt).

A separate structural issue remained in the prior CDHash fallback: every denied peer query could receive the querying
LiveProcess executable's hash, despite the guest entitlement profiles differing. A consumer using CDHash as a profile
cache key could therefore conflate guests. This risk was identified from the implementation; it has **not** been proven
to cause a particular on-device MCM failure. The new reader and broker snapshot preserve each guest executable's actual
embedded CodeDirectory hash alongside its immutable entitlement snapshot. The existing authenticated full-audit RPC
returns its fixed 20 bytes for operation 5, and the guest uses it only after the original audited syscall returns EPERM.
Neither the native code signature nor kernel authorization changes.

The CDHash transport tests cover distinct targets, exact output size, zeroed failure output, malformed replies and FD/Mach
right cleanup in native, guest and wire-adapter modes, plus ASan/UBSan. An earlier integrated alias run exposed an unrelated
asynchronous framebuffer test-release race; its focused correction passed four normal runs and ASan/UBSan. The final
[integrated suite](../tmp/lsd-services/tests-wifi-extensionkit-cdhash.log) then passed, and the
[signed app build](../tmp/lsd-services/build-wifi-extensionkit-cdhash.log) succeeded. The combined ExtensionKit/CDHash device
trial is recorded below; the test/build results alone do not establish SpringBoard startup or drawing.


## ExtensionKit progression and the next missing resources

The combined production build ran as host **2403**, with native SpringBoard **2425** (logical guest PID **2411**).
Its constructor requested the physical ExtensionKit bundle registration and returned; SpringBoard passed the former
Discovery protocol cast and reached lock-screen configuration work, including `#configurationUpdateManager`.
It was observed alive after 38 seconds. This is a progression past the previous abort, not proof that every ExtensionKit
service is functional. See the [host console](../tmp/lsd-services/console-wifi-extensionkit-cdhash-retry.log) and
[SpringBoard output](../tmp/lsd-services/springboard-extensionkit-cdhash-tail.log); that output also contains older failed
runs, so the new segment begins at `pid=2425`.

SpringBoard then aborted roughly one minute into this run. The crash captured at **11:04:42 +0700** identifies required
`dlopen(/System/Library/PrivateFrameworks/TextInputUI.framework/TextInputUI)` as the next failure: the image was absent
from RuntimeRoot and the cache. See [SpringBoard-2425.ips](../tmp/lsd-services/SpringBoard-2425.ips). A device screenshot
captured afterward shows the native host with a blank guest viewport; it does not establish a rendered SpringBoard.
See the [post-crash screenshot](../tmp/lsd-services/springboard-wifi-extensionkit.png). TextInputUI staging is handled
separately from this font preparation.

Before the abort, CoreText reported fallback to LastResort for two missing stock fonts:
`/System/Library/Fonts/Core/SFUI.ttf` and `/System/Library/Fonts/Watch/ADTTime.ttc`. The matching **23A343** originals are
regular files, not symlinks. Exactly those two files are staged, without signing or substitution, totaling **8,572,520
bytes**. The [font manifest](../tmp/fonts-stage/manifest.json) records source paths, sizes and SHA-256 hashes. The
[publisher](../tmp/fonts-stage/publish-wifi.py) is local-plan-only unless passed `--publish`: it verifies the inactive upload,
rejects any existing destination or symlink, creates each absent file exclusively, and verifies the installed hashes.
Font publication completed with both active-file hashes verified; see the [publication log](../tmp/fonts-stage/publication-wifi.log).
The subsequent drawing result is recorded below.


The later `Cannot get endpoint for mach service com.apple.sessionservices` messages have a separate identified owner:
stock job `com.apple.liveactivitiesd`, executing `SessionCore.framework/Support/liveactivitiesd`. SessionFoundation's
observed warning branch returns a nil connection; it does not itself abort. The stock job declares this Mach service and
is normally demand launched. A read-only strong-dependency inventory found nine images outside the current cache,
4,750,400 original code bytes, with no unresolved required source files. These files have not been staged or launched
by this investigation, and this warning has not been established as fatal to SpringBoard startup. See the
[precise call path and inventory](../tmp/sessionservices-research/README.md).


## First visible SpringBoard interface

The matching TextInputUI framework was published successfully with **182 files / 12,001,296 bytes** and verified hashes;
the previous framework resources were retained in the trial backup. See the
[publication log](../tmp/lsd-services/textinputui-stage/publication-wifi.log). With TextInputUI and the two stock fonts
installed, host **2536** ran native SpringBoard **2555** (logical **2543**) past the earlier missing-image exception.

The [device screenshot](../tmp/lsd-services/springboard-wifi-textinputui.png) shows actual guest SpringBoard UI inside the
host viewport: a status bar with time, Wi-Fi and battery indicators, a white search pill and a white dock, with an otherwise
empty black home screen. This is the first captured SpringBoard interface in this experiment. It does **not** establish
app-icon population, working applications, complete visual effects, or a fully functional simulator system. The native
host status bar outside the viewport is distinct from the guest status bar visible within it.

SpringBoard **2555** was still present at the **20-minute elapsed** process snapshot, which includes time stopped by the
debugger and therefore is not 20 minutes of uninterrupted execution. ContainerManager peers **2554** and **2560**, lsd
**2558**, and runningboardd **2567** were also alive. LaunchServices probe **2553** received status **`0x80`** from lsd **2558**
and exited successfully. See the [live process snapshot](../tmp/lsd-services/textinputui-live-render-state.txt),
[peer logs](../tmp/lsd-services/textinputui-peer-log-tails.txt),
[lifecycle audit](../tmp/lsd-services/textinputui-lifecycle-audit.json) and [host console](../tmp/lsd-services/console-wifi-textinputui.log).

The first diagnostic LLDB attach crashed **LLDB on the Mac**, with recursive Objective-C language-runtime initialization,
`CanJIT` and unwind-plan lookup in its stack. This is a debugger failure, separate from a guest SpringBoard crash; the
[LLDB crash report](../tmp/lsd-services/lldb-textinputui-attach.ips) and
[post-attach guest state](../tmp/lsd-services/textinputui-after-lldb-state.txt) preserve that distinction. The retry with
`target.process.disable-language-runtime-unwindplans=true` succeeded: the main thread was waiting in
`mach_msg` through CoreFoundation's run loop, GraphicsServices, `UIApplicationMain` and `SBSystemAppMain`.
TextInputUI was loaded from physical RuntimeRoot at `0x312200000`, and LLDB detached cleanly. See the
[successful main-loop capture](../tmp/lsd-services/lldb-wifi-textinputui-mainloop.txt) and
[debugger setup](debugging.md). This verifies a live main loop, not application launch or complete UI behavior.

## Material resources and current rendering limits

The focused matching-runtime material payload was published with **98 files / 540,475 bytes**. The publisher verified
the files and preserved existing content; see the [publication receipt](../tmp/material-resources-stage/publication-wifi.log).
The [subsequent screenshot](../tmp/lsd-services/springboard-wifi-materials.png) still shows the white dock/search treatment.
Installing these resources did not by itself change that appearance.

Guest material probe **3045** then used CoreMaterial's stock bundle lookup and received the physical RuntimeRoot
`CoreMaterial.framework` bundle with identifier `com.apple.CoreMaterial`. It read `dockLight.materialrecipe` (**347 bytes**),
`dockDark.materialrecipe` (**298 bytes**) and `platformContentGlass.materialrecipe` (**1,849 bytes**). An explicit physical
bundle lookup returned the same dockLight resource. See the [guest probe output](../tmp/material-resources-probe/guest-wifi.log).
These particular recipes and their bundle routing are therefore present and readable. The remaining appearance has not
been attributed to the bundle adapter, missing versions of those recipes, or Metal; determining its cause requires
additional evidence.

## Stock application population and registration

The [database query](../tmp/lsd-services/springboard-registered-applications.txt) returned no rows from
`application_identifier_tab`; neither RuntimeRoot/Applications nor SimulatorData/Applications existed at that checkpoint.
The visible empty home screen therefore preceded population of the stock application bundles.

The matching 23A343 inventory selects **254 top-level app bundles** across `/Applications`, `/System/Applications` and
`/System/Library/CoreServices`. Local staging signed **624 Mach-O files**, comprising the app code and missing required
dependencies, with the existing identity/identifier and no added native entitlements. See the
[inventory](../tmp/lsd-services/all-apps-inventory/inventory.json),
[staging manifest](../tmp/lsd-services/all-apps-stage/manifest.json) and
[signing log](../tmp/lsd-services/all-apps-stage/sign.log). The inactive phone transfer completed with exit 0:
**88,574 files / 2,862,553,159 bytes**; see the [transfer log](../tmp/lsd-services/all-apps-stage/transfer-wifi.log).
Missing-only publication also completed with exit 0: **88,098 new files / 2,853,694,685 bytes** were installed and verified,
and **476 existing files** were preserved. The sole differing existing file was the active SpringBoard executable, which
was intentionally retained instead of the incoming signed variant. See the
[completed receipt](../tmp/lsd-services/all-apps-stage/publication-receipt-wifi.json) for both hashes and the
[publication log](../tmp/lsd-services/all-apps-stage/publication-wifi.log). This confirms app payload publication;
registration completion and successful app launches remain separate checks.

The simulator-only [registration tool](../Guest/ApplicationRegistration.m) uses normally declared
`LSApplicationWorkspace` and `LSApplicationProxy` methods. Matching iOS 26 CoreServices implements `registerApplication:`
by calling `LSRegisterURL`; the tool does not construct installation dictionaries or modify the database directly.
Build it independently of the native app:

```sh
sh Tools/build-app-registration.sh tmp/app-registration
```

The explicit JSON manifest has this form:

```json
{"version": 1, "applications": ["/Applications/Example.app", "/System/Applications/Another.app"]}
```

The generated [254-app manifest](../tmp/app-registration/applications.json) is intended for
`RuntimeRoot/usr/share/LiveSimBooter/applications.json`. The generated
[job plist](../tmp/app-registration/com.kdt.LiveSimBooter.app-registration.plist) runs:

```text
/usr/libexec/app-registration --manifest /usr/share/LiveSimBooter/applications.json
```

The host can select label `com.kdt.LiveSimBooter.app-registration` through the existing services configuration. The tool
requires simulator platform 7 and matching absolute `DYLD_ROOT_PATH`/`SIMULATOR_ROOT`. It validates the complete manifest
before registration: at most 512 unique paths, only direct app children of the three stock directories, canonical
RuntimeRoot containment, and no internal symlink traversal. Each candidate must have valid metadata and a present regular
thin arm64 Mach-O executable. Original resource-only bundles are reported as `SKIP` without a LaunchServices call.

Before changing a record, the tool checks its identifier, installed state and canonical physical bundle URL. A matching
record produces `ALREADY`, allowing a rerun to resume after a failure or timeout. New registrations receive the same
verification afterward. Every potentially blocking registration/verification operation has a 30-second hard timeout;
logs identify the current bundle, and any ordinary per-bundle failure contributes to a nonzero final exit status.

Normal and ASan/UBSan boundary tests passed without contacting native LaunchServices. A read-only preflight against all
matching stock bundles found **163 ready, 91 expected resource-only skips, and 0 failures**. These are filesystem results,
not successful registrations. See [tests](../tmp/app-registration/tests.log) and
[sanitizers](../tmp/app-registration/asan.log). The tool itself has been signed and deployed.

A later preflight review identified that opening a FIFO could block before the regular-file check. Metadata and executable
inspection now add `O_NONBLOCK`, preserving ordinary file behavior while rejecting special files promptly. Regression
fixtures cover a FIFO in each of the manifest, Info.plist and executable positions, with deadlines to detect a future
blocking regression. Focused [normal](../tmp/app-registration-v2/tests.log) and
[ASan/UBSan](../tmp/app-registration-v2/asan.log) tests passed. The simulator binary was rebuilt separately as
`tmp/app-registration-v2/app-registration`; the earlier staged artifact was not modified during the rebuild. This version
has since been signed and deployed.

The first device registration run, native PID **3473** under host **3450**, completed all **254 selected bundles**:
**162 `ALREADY`, 91 expected `SKIP`, 0 new registrations and 1 failure**. The existing records passed the tool's installed-state
and physical-URL checks, consistent with lsd seeding records after publication. The failure correctly made the registrar
exit 1; the broker recorded wait status **`0x100`**. See the
[final registration output](../tmp/app-registration-v2/registration-final-wifi.log) and
[host console](../tmp/lsd-services/console-wifi-app-registration.log). This verifies those 162 records, not successful app launches.

The sole failure was `/System/Applications/Image Playground.app` (`com.apple.GenerativePlaygroundApp`):
`registerApplication:` returned `NO`. Its original metadata has `LSInstallByDefault=false`, `LSDefaultSystemHidden=true`
and the `DeviceSupportsGenerativeModelSystems` requirement. The inspected stock InstalledContentLibrary scanner uses
RuntimeRoot's `/Applications` and `/System/Library/CoreServices`, without a corresponding `/System/Applications` scan.
This explains why automatic discovery differs for this bundle, but neither the scan scope nor those metadata values
establishes the cause of the explicit registration failure. This was the only actual registration mutation; the other ready
apps were already present. The BOOL API supplied no underlying LaunchServices status, and the tool reported generic `EIO`.
No entitlement, eligibility or path restriction has been established as the failing gate. See the
[read-only analysis](../tmp/app-registration-v2/image-playground-review.md) and
[lsd log](../tmp/app-registration-v2/lsd-register-system-app-wifi.log); historical entitlement failures in that log belong
to older processes, not this request.

An integrated test attempt stopped at native `IOSurfaceCreate`/lock setup inside the tool sandbox. The identical focused
FramebufferServer test binary reproduced that failure when sandboxed and passed with native IOSurface access; no test or
renderer change was needed. See [sandboxed](../tmp/app-registration/framebuffer-focused-sandboxed.log) and
[native-access](../tmp/app-registration/framebuffer-focused-native.log) runs. The subsequent full
[native-access integrated suite](../tmp/app-registration/integrated-tests-native.log) passed with exit 0; the sandboxed
assertion was not an app-registration regression.

A separate data-resource transfer and publication completed from the matching runtime:
**429,666 files / 9,652,892,167 bytes (9.65 GB; 8.99 GiB)**, without another full local payload copy.
Its [plan](../tmp/runtime-resources-stream/plan.json) preserves existing regular files and rejects symlinks. Publication
installed **395,680 new files** and preserved **33,986 existing files**. The source/staging hash pass had completed, but,
at the user's request to skip checksums after copying, the original publisher was interrupted and resumed with
`--skip-checksums`. The final [receipt](../tmp/runtime-resources-stream/publication-receipt-no-checksums-wifi.json) reports
`complete-checksums-skipped` and `payload_checksums_after_publication=false`: this is completed publication without
post-copy hash verification. This payload contains data resources only; missing framework executables still require
separate code staging and signing.

### Feature-flag gate isolation

The glass-gates probe under host **3112** returned `SwiftUI/Solarium=0`, `_UISolariumEnabled=0` and
`_UIFlexiGlassEnabled=0`, while both Reduce Transparency and Reduce Motion were false. The runtime lacked
`System/Library/FeatureFlags/Domain/SwiftUI.plist`. Its defaults contained no explicit Solarium or FlexiGlass override.
See the [baseline output](../tmp/material-resources-probe/guest-gates-baseline-wifi.log).

A separate missing-only publication installed all **391 original FeatureFlags plists / 413,422 bytes**, including
`GlobalDisclosures.plist`; no existing flag files were present or overwritten. See the
[verified publication](../tmp/featureflags-stage/publication-wifi.log). With a fresh process under host **3173**, the
same probe returned **1 for all three gates**, still with Reduce Transparency and Reduce Motion false. These are
standalone guest probe values; SpringBoard's process-local cached state was not directly queried.

The newly enabled feature set also exposed a startup failure: SpringBoard **3206** aborted with Metal's
`vertexFunction must not be nil` validation. The [crash report](../tmp/material-resources-probe/LiveProcess-featureflags-3206.ips)
places the caller in **SiriUICore** (`+0x144a4`, `+0x13840`, `+0x13628`) through SiriUIActivation, rather than establishing
that a glass shader itself failed. No renderer capability or feature values were patched to obtain the gate change.

The original shader-resource stage then completed publication with exit 0: **65 files / 3,168,418 bytes**, comprising
**58 newly installed files with verified hashes** and **7 existing files preserved**. See the
[publication receipt](../tmp/siri-shaders-stage/publication-wifi.log). Fresh SpringBoard **3290** still reached the same
`vertexFunction must not be nil` assertion; the [post-publication output](../tmp/siri-shaders-stage/springboard-after-shaders.log)
contains older output too, so this result is the segment beginning at `pid=3290`. Installing this shader subset did not
resolve the failure.

Two subsequent CoreDevice LLDB device-selection attempts timed out. Direct attachment to SpringBoard **3374** through an
SSH-forwarded research debugserver connected, but automatic initial image preloading continued for more than ten minutes;
no shader breakpoint observations were obtained. The host **3351** trial was ended, its remote debugserver **3401** stopped,
and LLDB exited. The production shim was restored and its hash verified before the next launch; see
[debugger details](debugging.md#springboard-attach-over-wi-fi) and the
[restoration log](../tmp/siri-shaders-stage/restore-production-wifi.log). Siri-bundle probe **3382** had timed out during
UIKit gates while SpringBoard was stopped, before its Siri query, so that run supplied no Siri result.

The reordered probe then ran as native PID **3483** under host **3450**, querying Siri before the UIKit gates. The stock
`SUICOrbView` class bundle resolved to physical RuntimeRoot's `SiriUICore.framework`; `default.metallib` was readable,
**217,164 bytes**, with an `MTLB` header. The explicit physical bundle lookup returned byte-identical contents and the same
canonical resource path. See the [probe output](../tmp/lsd-services/app-registration-com.kdt.LiveSimBooter.material-resources-probe.log).
This verifies bundle routing and resource bytes; the probe did not request a Metal device or establish shader-function
creation. Production SpringBoard **3475** still aborted with `vertexFunction must not be nil`; see its
[output](../tmp/lsd-services/app-registration-com.apple.SpringBoard.log).

A scoped diagnostic in SpringBoard **3722** then observed the actual calls: the stock default-library lookup succeeded,
but specialization of **`siriChipVert`** returned `NULL`. Its otherwise discarded NSError was `MTLLibraryErrorDomain`,
code 3: MTLCompilerService was unavailable, with lookup error **3, "No such process"** (`ESRCH`). The temporary wrappers
preserved the original return values and did not force shader success. See the
[diagnostic output](../tmp/siri-metal-diagnostic/guest-wifi.log) and
[diagnostic scope](../tmp/siri-metal-diagnostic/README.md).

Production compiler registration now admits the exact canonical SpringBoard executable as well as backboardd, with the
existing opt-in and contained compiler-bundle checks. The generated trusted SpringBoard job enables this registration;
inherited options still do not admit unrelated executables. In the fresh production run, host **3884** / SpringBoard
**3908** launched compiler processes **3925** and **3928**, passed the previous nil-vertex failure and reached a later
required `dlopen` of **AutoFillUI.framework**, whose executable was absent. See the
[production guest output](../tmp/siri-metal-diagnostic/guest-production-fix-wifi.log) and
[compiler launches](../tmp/siri-metal-diagnostic/console-production-fix-wifi.log). The completed data-only resource publication
could not supply that executable.

The signed **577,008-byte AutoFillUI executable** was then copied separately, with post-copy checksums skipped as requested;
see the [deployment receipt](../tmp/autofillui-stage/deploy-wifi.log). A fresh production run under host **3976** / SpringBoard
**3998** displayed the populated home screen. Its [screenshot](../tmp/autofillui-stage/springboard-wifi.png) shows application
names, Calendar's rendered icon, dark placeholders for other icons and white widget tiles. The dock and search pill now have
dark, bordered material treatment instead of the earlier white appearance. See also the
[guest log](../tmp/autofillui-stage/guest-wifi.log) and [host console](../tmp/autofillui-stage/console-wifi.log).
This establishes visible progress after the compiler-registration and AutoFillUI fixes. A black background does not establish
correct glass refraction, and app launches, missing icon artwork and widget behavior remain unverified.

### Final app/resource session and icon-service boundary

The final working selection is running under native host **4104**, simulator launchd **4107**, and SpringBoard **4126**.
The [final screenshot](../tmp/lsd-services/springboard-wifi-apps-final.png) shows the populated home screen, Calendar artwork,
dark bordered dock/Search materials, placeholder artwork for other icons and blank widget tiles. The production shim
contains the compiler-registration fix without diagnostic hooks. The temporary diagnostic helper was removed.

The separate stock `com.apple.iconservices.iconservicesagent` trial started its global `com.apple.iconservices` job,
but native PIDs **4063, 4083, 4087 and 4089** were rejected by the generic Metal endpoint, which was then reserved
for backboardd. Its log also reports missing Core Image filter plug-in executables. Adding the service alone did not
repair icon artwork. The final working selection therefore omits this experimental job; its signed executable and stock
job plist remain installed for follow-up. See the [console](../tmp/iconservices-stage/console-wifi.log),
[service log](../tmp/iconservices-stage/guest-wifi.log), and [Metal boundary](metal.md).


### Public graphics service implementation

The generic `com.apple.metal.simulator` route now points to a separate public native LiveProcess helper. It accepts any
client holding the endpoint, while `com.apple.metal.simulator.backboardd` and `.SpringBoard` retain separate helpers and
exact full-audit-token admission. A SpringBoard graphics session therefore has three native Metal helpers. The public
`com.apple.IOSurface.Remote` service remains in the native UIKit host and no longer requires per-owner registration.
Indigo and framebuffer submission remain backboardd-owned.

Public Metal uses revocable public-session mode in control ABI **2**. The broker publishes it before helper startup and
revokes it on session teardown. Client exit or exec replacement does not revoke other public clients. The stock server
creates independent application contexts through type-0 handshakes; the client's type-1/type-2 connections reuse that
context's `processRef`. The public helper shares one server and failure domain across those clients, separate from both
dedicated graphics processes. See [Metal routes and lifetime](metal.md#isolated-native-service-route).

Local tests cover public helper readiness/failure cleanup, endpoint ownership and registration races, dedicated/public
policy separation, client exit/exec and session teardown. The native IOSurface integration test passes with three real
unregistered processes, allocation/export/shared-pixel checks, 64 live peers, capacity recovery after disconnect and
shutdown. That reconnect check found that `NSHashTable.count` can retain zeroed weak entries; both wrappers now count
`allObjects.count` instead. Public Metal permits 256 live peers and dedicated helpers permit 64 each. Evidence for the
IOSurface run is `tmp/IOSurfaceServer-public-test.log`.

These implementation and local-test results remove the admission boundary from the earlier IconServices experiment.
They do not yet establish public Metal rendering, repaired icon artwork or a successful stock-app launch on the phone.

### Public graphics services and icon rendering verified (2026-10-26)

The final Wi-Fi trial used host **4357**, native Metal helpers **4358** (backboardd), **4359** (SpringBoard),
**4360** (public), simulator launchd **4361**, SpringBoard **4382**, backboardd **4384**, and IconServices **4380**.
The public helper accepted IconServices' three Metal connections, and the host public IOSurface service accepted its connection.
Dedicated backboardd and SpringBoard connections remained on their separate helpers.

RenderBox initially aborted while creating a binary archive because the simulator device's target architecture is nil.
The global guest `RB_USE_BINARY_ARCHIVE=0` setting avoids that unsupported path. IconServices now also registers the simulator
Metal compiler and uses matching framework resources. Its compiler subprocess reached READY, and IconServices remained running.
The log still prints `IOSurfaceClientSetSurfaceNotify failed e00002c7`; that message did not prevent the verified icon rendering.

The screenshot `tmp/public-graphics-global.png` shows actual artwork for Photos, Reminders, News, Maps, Health, Wallet,
Settings, Safari and Messages, alongside the live Calendar icon. The Maps/Calendar widgets remain white and the wallpaper
remains black; this result verifies application icons, not complete widget or wallpaper functionality.
Evidence: `tmp/public-graphics-global-console.log`, `tmp/public-graphics-global-iconservices.log`, and the screenshot.

Validation: full `sh Tests/run.sh` passed before the final IconServices opt-in; the affected compiler/configuration tests
passed again afterward, and the final signed native build succeeded. Public-control tests also passed with ASan/UBSan.
Native IOSurface tests exercised three processes, shared pixels, connection capacity recovery, and shutdown.
Both app and extension retain the increased-memory-limit entitlement. Five signed CoreImage bundles and the updated guest shim
were copied over Wi-Fi without post-copy checksums.

## App-launch investigation (2026-10-26)

After the public-graphics/icon changes were committed as `b876905`, the next trial requested Contacts
(`com.apple.MobileAddressBook`) through `LSApplicationWorkspace`. Its outer error was
`LSApplicationWorkspaceErrorDomain` **115**. An lsd-scoped diagnostic preserved the original FrontBoard completion result
and exposed the underlying chain: `FBSOpenApplicationServiceErrorDomain` 1 → `FBProcessExit` 64 →
`RBSRequestErrorDomain` 5 → `NSPOSIXErrorDomain` **45**, **"Launchd job spawn failed"**. This establishes a spawn failure;
the outer request-denied wording alone did not establish an entitlement or process-identity rejection.
See [the original error chain](../tmp/app-launch-fix/original-fbs-error-1.txt).

### Dynamic child capacity

LLDB observed launchd's new control jobs returning **`EAGAIN` (`0x23`)** after compiler jobs and adopted children had filled
the old 16-slot parent ledger. A new child needs a free, unpinned slot; an exec replacement retains its existing logical
slot. Reducing the selected startup jobs cannot guarantee space for later applications and their demand-launched services.
The attribute snapshots are in [the LLDB request log](../tmp/app-launch-fix/lldb-spawn-requests.jsonl).

The shared ledger is now **ABI 4**, with **64 slots** and a **2,080-byte** structure inside the unchanged **4 KiB** mapping.
Old ABI/capacity combinations are rejected before slot access. Host, LiveProcess and guest shim must be deployed together;
changing a header does not expand a running process's already initialized ledger. The initial service-selection limit
is **16 selected jobs**, plus the fixed backboardd, cfprefsd and terminal helper jobs. The broker's physical-job limit is **132**: 64 children with room
for simultaneous exec replacements, simulator launchd, and three native Metal helpers. Unreaped statuses and observations
that pin a slot still consume capacity. This increase supplies headroom; it does not remove resource limits.

The boundary regressions cover the highest slot, exec replacement, exit/wait ownership, stale generations, reuse and
cross-process mapping, plus a full broker with all three helpers. See
[the integrated validation record](../tmp/app-launch-fix/integrated-tests.txt) and
[the broker boundary run](../tmp/app-launch-fix/child-capacity-tests/Broker-all-helpers.log).

### Stock application spawn attributes

Two stages of the stock launch path use different attributes. Launchd creates `xpcproxy_sim`; the proxy then requests
`POSIX_SPAWN_SETEXEC` for the application itself. The compatibility checks now handle these observed forms:

| Request | Observed fields | Handling |
| --- | --- | --- |
| Launchd app trampoline | Flags `0x400c`, process type `APP_DEFAULT` (`0x100`), role `UI_FOCAL` (`1`) | Accept the app scheduler hints while retaining LiveProcess's native scheduler policy. |
| xpcproxy app replacement | Flags `0x4240`, process type `0`, role `1` | Accept the inherited focal role only with SETEXEC; ordinary type-0/role-1 spawn still rejects. |
| Nano allocator preference | Flag `0x0200` in `0x4240` | Consume the optional hint before transport; allocator defaults or explicit environment settings remain in effect. |
| Dataless-file policy | Options at `0x80` equal `4`, policy at `0x98` equal `5` or `6` | Accept only the stock OFF/ON pairs for the fully materialized local RuntimeRoot; do not change native FileProvider policy. |

`0x0200` is **`_POSIX_SPAWN_NANO_ALLOCATOR`**, not SETSID; real `POSIX_SPAWN_SETSID` is `0x0400` and remains unsupported
as a spawn attribute. XNU uses the nano flag to append `nanozone=1` to the executable's apple vector. The current
compatibility path does not synthesize that allocator preference. See the pinned
[spawn flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/sys/spawn.h#L53-L59) and
[nano handling](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_exec.c#L5987-L5999).

The iOS 26 libkernel dataless setter adds the `ORIG` marker (`4`) to its input, producing `5` for OFF and `6` for ON.
`xpcproxy_sim` uses precisely those two inputs. The implementation checks both fields together and continues rejecting
other option bits, malformed pairs, adjacent resource-limit fields and `START_SUSPENDED`. It does not implement cloud
placeholder materialization. The pinned code is recorded in
[xpcproxy disassembly](../tmp/app-launch-fix/xpcproxy-disassembly-current.txt) and
[host-support libkernel disassembly](../tmp/lsd-services/host-kernel-csops-disassembly.txt); the
[source field layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/sys/spawn_internal.h#L221-L227)
names the option and policy fields.

These changes were driven by real Contacts and WidgetRenderer requests. Their earlier nano rejection is preserved in
[the collected spawn log](../tmp/app-launch-fix/spawn-files-current.txt), and the subsequent options-field rejection is in
[the device dataless-policy log](../tmp/app-launch-fix/dataless-rejection-device.log). Regression tests exercise the exact
`0x4240`/type-0/role-1 request with options `4` and dataless policy `6`: it reaches exec transport carrying only
`CLOEXEC_DEFAULT` (`0x4000`), preserves errno on transport failure, and does not alter native or logical session/group IDs.
Unknown policies still return `ENOTSUP`.

### Physical executable aliases at the native handoff

After the attribute fixes, WidgetRenderer's xpcproxy reached the broker's exec RPC. Its replacement extension checked in,
then failed native preparation with **`ENOENT` (2)** before READY. The requested executable began with `/var/mobile/...`,
while the broker's canonical RuntimeRoot began with `/private/var/mobile/...`. `LiveProcess/main.c` previously recognized
physical executables by a literal prefix comparison and prepended RuntimeRoot to the alias, producing a nonexistent
`RuntimeRoot/var/mobile/.../RuntimeRoot/Applications/...` path. See the replacement requests in
[the host console](../tmp/app-launch-fix/console-capacity-ui.log).

Read-only checks on the phone confirm that each pair identifies the same file: WidgetRenderer is **87,568 bytes**,
inode **5,992,372**; Contacts is **212,064 bytes**, inode **5,949,300**. Both incorrectly prefixed candidates return `ENOENT`.
The full path/stat evidence is saved in [the device check](../tmp/app-launch-fix/executable-path-device.json).

The extracted native resolver now canonicalizes a physical path first and accepts it only when it is below RuntimeRoot.
Otherwise it translates guest absolute paths, or resolves relative paths after the ordered working-directory actions.
Final containment remains mandatory, and an explicit canonical RuntimeRoot path that escapes through a symlink is rejected
instead of being reinterpreted. Tests reproduce the prefix mismatch using macOS's `/tmp` → `/private/tmp` alias and cover
contained aliases, ordinary guest paths, relative paths, missing files, length limits and symlink escapes. The focused
[resolver test](../tmp/app-launch-fix/executable-path-tests/result.log) and iPhoneOS syntax check passed. This fix requires
a rebuilt native LiveProcess extension; replacing only the guest shim does not change the resolver.

### Diagnostics, control and validation boundary

Stock launchd replaces its standard descriptors with `/dev/null`, so absence of stderr traces did not prove rejection
before the spawn shim. `LSB_TRANSPORT_TRACE` now writes bounded error-only records to stderr and to
`SIMULATOR_LOG_ROOT/spawn-rejections.<pid>.log`, preserving errno. Each source emits at most 32 errors per process;
the file is created mode `0600` with `O_NOFOLLOW`. The logging tests also run with stderr redirected to `/dev/null`.

The [guest launchctl worker](guest-control.md) permits further job inspection and control without restarting the whole
runtime. Its queue claims complete requests atomically, preserves uncertain operations for inspection, and retrieves by
the original request ID without resubmission. A completed historical result remains retrievable while another request
is active, without releasing that request's lock. The worker's 60-second timeout and five-second kill/reap grace period
use full monotonic timestamps. A successful `kickstart` command or zero launchctl exit status establishes acceptance of
that control operation; it does not establish that an old daemon was killed, a new daemon started, or an application window
appeared. Those outcomes need separate process and display evidence.

The [full integrated suite](../tmp/app-launch-fix/integrated-tests-full.log) passed after the ABI/capacity and initial
app-hint changes. Subsequent nano/dataless changes passed both normal and trace-enabled SpawnPlan/SpawnAttributes runs,
including logical-session mode; see `tmp/app-launch-fix/nano-exec-tests`, `nano-exec-trace-tests`,
`dataless-policy-tests` and `dataless-policy-trace-tests`. The control worker's native tests and five local Python queue
tests passed, and its simulator build succeeded; logs are under `tmp/app-launch-fix/control-review`.
These local results verify the implemented compatibility boundaries. Subsequent fixes to FrontBoard reconnection, UIKit
resources/device capabilities and cache-only framework loading now open a visible Contacts window on the iPhone; see
[the app launch results](launchd.md#cache-only-framework-weak-linking).

## Native tabs and input progress (2026-10-26)

The native foreground window now retains an IOSurface tab and a Fish tab. Icon launch selects the graphical defaults above;
the generated terminal helper belongs to simulator launchd and replaces itself with Fish through SETEXEC. The broker attaches
the fake terminal only to that authenticated helper lineage, preserving logical PID and wait ownership through replacement.
Tab selection changes presentation and keyboard focus, without creating another guest or stopping the existing one. See
[the terminal architecture](fake-pty.md#launchd-owned-fish-and-tabs) for FD attachment, session lifetime, build/staging and tests.
The explicit `--terminal` option remains a direct-shell diagnostic; it is no longer the icon-launch default.

Ordinary Contacts input exposed a separate process-identity mismatch. Pinned BackBoardServices uses `getpid` when registering
its HID client, constructing dispatch targets, and comparing delivery-policy observations. The IOHID connection is validated
against a genuine native task audit token; a retired logical xpcproxy PID therefore cannot identify that client. The exact
BackBoardServices install path and canonical RuntimeRoot image now receive native self PID through the existing caller-scoped
adapter. Ordinary guest callers retain their stable logical PID. Tests retain malformed-path, unrelated-image and unload/reload
boundaries; the pinned call chain is recorded in
[the routing analysis](../tmp/app-launch-fix/gesture-research/ordinary-touch-routing.md).

After the parent-coordinate and BKS identity fixes, the user confirmed ordinary touches work. A device screenshot also shows
Control Center opening: [Control Center result](../tmp/app-launch-fix/gesture-research/control-after-bks.png).
Home and lock-screen behavior remain under investigation; the current reproduction reaches a simulator launchd `SIGTRAP`.
Those observations do not establish complete gesture or lock-screen support.

The [full local suite](../tmp/terminal-launchd-tests/full-suite.log) passed and the native build succeeded. The subsequent
[icon-launch defaults regression](../tmp/terminal-launchd-tests/broker-icon-default.log) passed as well. The new Fish tab's
launchd-owned path still needs device verification; earlier direct-terminal Fish results are recorded separately in
[the fake-terminal notes](fake-pty.md#device-checks-and-diagnostics).

## Guest keyboard service staging

The normal service selection now includes the matching stock `com.apple.TextInput.kbd` job. SpringBoard already declares
`com.apple.UIKit.KeyboardManagement.hosted`, but the separate `kbd` job was absent. The daemon supplies the stock TextInput
input-manager, preferences, lexicon and related services. The generator retains its MachServices and `MallocNanoZone=0`
environment, using the same bounded `RunAtLoad`/no-restart policy as other selected daemons.

The current 1,353-image cache omits six strong dependencies: TextInputCore, InputTranscoder, Transliteration,
KeyboardServices, InputContext and ProactiveInputPredictions. The isolated matching-23A343 stage contains those frameworks,
`kbd`, the English input plugin, and their resources plus the cached TextInput framework's resource files. Existing verified
KeyboardServices code from the all-apps stage is reused byte-for-byte. Every other staged Mach-O uses the established
signing identity/identifier, no native signature entitlements, and preserved original file type/platform/code. The daemon
retains `MH_EXECUTE` with `CS_EXECSEG_MAIN_BINARY` cleared by the executable signer.

The [stage manifest](../tmp/keyboard-stage/manifest.json) records 454 files / 10,317,488 bytes, eight Mach-Os and zero missing
strong dependencies against the current cache. The payload was published missing-only, preserving existing runtime files.
A sanitized generated job started `kbd` as native PID 11125; its startup log contains a duplicate-class warning but no observed
startup failure. The cached TextInput binary matches the matching runtime source exactly, so that warning alone does not
show a version mismatch. No guest software keyboard display or typing success is established yet. A software keyboard should
consume the existing simulator touch path.

### Hardware keyboard focus

The IOSurface view forwards UIKit `UIPress.key.keyCode` keyboard-page usages through the stock Indigo generic HID packet.
Only a focused, visible, active simulator view with a frame forwards presses. Selecting IOSurface or touching within its
displayed image requests focus; Fish retains its own text-input responder. No global event capture or hardware-attachment
state override is installed. The host releases accepted keys on focus loss, tab changes, inactivity, removal and server
replacement. The server retries an undelivered release before accepting another key, preventing a hidden view from leaving
guest modifiers held down.

The host orders modifier downs before ordinary keys and releases ordinary keys first. Known left/right modifiers retain
their side; aggregate UIKit flags synthesize a left modifier only when the original press occurred before the view gained
focus. The portable routing tests and real Mach packet tests cover ordering, cancellation and delivery failure. On-device
hardware typing through this new route still needs verification.

The native Fish responder did become first responder during the keyboard investigation. The host reported an attached
hardware keyboard, and the user confirmed Keyboard/Mirroring was connected. Hiding the native software keyboard in that
state is expected UIKit behavior; no text-field replacement or forced software-keyboard policy was needed.

## SystemStatus publisher retry loop

The default graphical service selection includes the matching `com.apple.systemstatusd` job. In 23A343,
`SystemStatus/EmbeddedDaemon` is `FeatureComplete`; the stock daemon owns `com.apple.systemstatus`,
`com.apple.systemstatus.publisher` and `com.apple.systemstatus.activityattribution` when that feature is enabled.
SpringBoard declares those same names only when the feature is disabled. The generator preserves both opposite
conditions exactly, preventing duplicate unconditional registrations.

Without the daemon, SpringBoard repeatedly recreated its publisher NSXPC connection and serialized its status domains.
The sampled SystemStatus frames resolved to `_resendDataIfNecessary`, `_internalQueue_setupXPCConnectionIfNecessary`
and `_internalQueue_reregisterForPublishingDomains`; launchd's log repeatedly reported the missing publisher endpoint.
Loading the stock daemon into the same running session reduced SpringBoard CPU from **102.16% to 2.72%** and launchd
from **106.38% to 22.48%** in two approximately 20.09-second windows. Each percentage uses one core as 100%, converts
native process CPU ticks with the device's 125/3 Mach timebase, and checks PID, executable UUID and process-start time.
These are sequential observations with different foreground activity, not a controlled benchmark; the remaining launchd
CPU needs separate investigation. [The comparison](../tmp/cpu-investigation/systemstatus-comparison.md) links the result
to the [complete per-process counters](../tmp/cpu-investigation/systemstatus-comparison.json).

The [matching stage](../tmp/cpu-investigation/systemstatus-stage/manifest.json) contains the stock signed daemon and its
resources/plists: 111 files totaling 103,060 bytes. Its seven direct strong dependencies are already in cache110. Signing
preserves MH_EXECUTE, simulator platform 7 and every original __TEXT section while clearing CS_EXECSEG_MAIN_BINARY,
using the existing identity/identifier without native signature entitlements. The focused Broker regression verifies the
new default job, exact executable, both mutually exclusive service conditions and the bounded RunAtLoad/no-KeepAlive policy.

## PosterBoard selected LaunchAngel

PosterBoard is a hidden application whose `LSLaunchDLabel` names the matching stock
`System/Library/LaunchAngels/com.apple.PosterBoard.plist`. The default graphical selection now includes this exact label.
For each explicitly selected label, the generator first checks LaunchDaemons, then checks LaunchAngels only when the
daemon plist is absent. It does not import every Angel or bypass path validation.

Angels retain on-demand startup, their exact MachServices, the non-launching endpoint marker, App process/spawn types and
the bounded RunningBoard Managed/Reported metadata. PosterBoard's stock `RunAtLoad=false` and `KeepAlive=false` remain
false. The selected-job limit is 16; with three fixed jobs this leaves 45 slots in the 64-child ledger for demand-launched
children. The host icon-launch regression includes PosterBoard and checks its non-launching service and RunningBoard flags.

The first normal device open failed with nested `RBSServiceErrorDomain` 4, **"Launchd could not find job."** Registering
the matching job in the running simulator resolved that boundary. `launchctl print` showed an on-demand App job with
zero runs before the next request. The next open reached spawning but failed with nested `NSPOSIXErrorDomain` 45,
**"Launchd job spawn failed."** This is not a verified PosterBoard start or wallpaper fix. The confirmed user symptom is
black wallpaper; the picker has not been tested. See [the captured error progression](../tmp/posterboard-investigation/after-angel-evidence.txt)
and [the job before activation](../tmp/posterboard-investigation/job-before-request.log).

Read-only comparisons found the existing stock wallpaper resources complete at 5,189 files / 446,917,590 bytes. The three
uncached strong code images and bundle resources are also present. Stock PosterBoard has an initial-configuration helper;
missing per-user PosterBoard directories alone do not justify fabricating those configurations.

Static inspection of the pinned 23A343 launchd (`C3EC5C45-8C85-3D11-B59C-62C4248F5EF9`) identified a narrower spawn
boundary: its App finalizer assigns role `UI` (2) at `+0x5a2c`, while the shim accepted only `UI_FOCAL` (1). The captured
PosterBoard job already reports `spawn role = ui (2)`; launchd passes that field directly to the spawn setter. A local
probe using the native private attribute setters reproduced error45 for `APP_DEFAULT + UI` with otherwise supported
flags. This establishes an unsupported stock attribute, while a complete live spawn trace is still needed to exclude
additional failures.

The shim now consumes `UI` alongside `UI_FOCAL` as metadata for `APP_DEFAULT`, and for the existing inherited app-role
case with type zero plus `SETEXEC`. It does not apply native scheduling policy. The role meanings are defined by
[XNU's resource_private.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/sys/resource_private.h#L134-L142),
and XNU applies them through its
[spawn policy handler](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_exec.c#L2402-L2476).
Tests retain rejection of other roles/types, role-only ordinary spawns, suspended starts and session changes. Both accepted
roles reach exec transport while preserving the input attributes, errno and native process/session grouping. The change
still requires device validation.


The production app-role change was validated on the next boots. PosterBoard logical **12397** / native **12398** started on
demand, completed SETEXEC, opened the public Metal connections, and remained in `job state = running` with one run and no
exit. Its `com.apple.posterboardservices.services` endpoint was active. The later app-compiler registration also includes
PosterBoard. This resolves the earlier missing-job/spawn45 boundaries; wallpaper presentation and picker behavior still
need separate verification. Evidence: `tmp/posterboard-investigation/job-after-role-fix.log` and `live-role-fixed.log`.

PosterBoard native **12531** then exposed a separate discovery failure: its PID domain rejected the cached virtual path for
`ExtensionFoundation.framework/XPCServices/extensionkitservice.xpc` with error **147**, followed by two failed lookups for
`com.apple.extensionkitservice`. The existing ExtensionKit registration adapter now accepts exact PosterBoard too, and the
sanitized job receives its own opt-in. Canonical-path, symlink-escape, inherited-role and label/program-spoof tests pass;
device activation and default-poster creation remain unverified. This imports the stock Discovery service and does not
create poster database rows or guess initial configuration. Evidence: `tmp/posterboard-investigation/discovery-launchd-tail.txt`.

## iPhone 17 metadata and input migration (2026-10-27)

The iOS 26.0/23A343 guest on iPhone18,2 / iOS 27.0 build 24A435 initially aborted lsd with
`WRITE_METADATA_DICTIONARY`. A clean LiveProcess probe isolated the exact basename
`.com.apple.mobile_container_manager.metadata.plist`: ordinary/protected creation and atomic rename to that name returned
`EPERM`, while the same operations using an ordinary name succeeded. The native kernel independently reported
`com.apple.sandbox.reporting:violation`, `System Policy: LiveProcess(5652) deny(1) file-write-create` for that exact path.
This was a filename policy restriction inside the otherwise writable App Group, not a missing directory or protection-class failure.

[patch-ios26-container-metadata.py](../Tools/patch-ios26-container-metadata.py) replaces only the five validated cached string
literals and the matching loose MobileInstallationHelperService literal with the equal-length guest-only basename
`.com.kdt.LiveSimBooter.container-metadata.v1.plist`. Validation pins image UUIDs, complete original `__TEXT,__cstring`
section hashes, literal boundaries and mapping protections, while permitting freshly rebuilt cache layouts. The source remains
immutable and outputs require separate signing. This changes the guest's metadata naming convention; it does not change device
sandbox policy, entitlement checks or native container metadata. The alternate-name atomic probe produced 464 records with no `EPERM`.

The signed outputs were staged over USB and published with all affected processes stopped, keeping rollback copies and skipping
post-copy checksums as requested. The cache remains at `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`, with 1,353 images;
its new CodeDirectory hash is `308875ea5c35f30ed7143cf92fd8da54dd9b1179`. Host **5669**, launchd **5673**, backboardd **5700** and
SpringBoard **5698** ran without LLDB using the now-default retained native cache and hardware workqueue redirection. ContainerManager
created 647 metadata files; lsd's later job inspection showed running, two runs and last exit code 0. The user confirmed a home
screen with app icons. This verifies the metadata failure is resolved, not that every included extension starts successfully.

Touch initially failed because the migration omitted the external CoreSimulator `SimulatorHID.framework` entirely. Backboardd logged
`Failed to initialize HID services: Failed to bundle`, while the host's Indigo endpoint registration succeeded. Restoring and signing
the previously tested CoreSimulator 1171.7 plugin with its `Info.plist` fixed the connection: the next host **8371** boot registered
backboardd **8400** and queued built-in digitizer target `1073741825`, alias `50`, display `PurpleMain`. The user confirmed touch markers,
home-page swipes and opening Contacts. The [setup recipe](agent-setup.md) now explicitly stages this external framework alongside
SimFramebuffer. No guest/host debugger was attached for these checks.

Evidence lives in ignored `tmp/lsd-metadata-default/`: `cache-patch.json`, `cache-sign.json`, `helper-sign.json`, `publish.json`,
`results/sandbox-policy-kernel.log`, `results/metadata-alternate-rows.json`, `results/patched-lsd-status.log` and
`results/patched-services-hid-console.log`. A separate wallpaper-extension Swift assertion loop remained and was investigated next;
it must not be confused with the resolved metadata abort or input-plugin omission.

### Wallpaper extension identity and subsequent failures

The migrated runtime produced up to 119 Swift `SIGTRAP` exits per minute in stock wallpaper extensions. In the pinned
CoreServices image (`73F411D9-84A9-33FE-84C9-8CD57CA633DC`), two self-audit-token checks compared the native token PID to
the logical guest PID. The mismatch selected an unadapted native process-path query and identified `LiveProcess.appex`
instead of the guest extension. The identity adapter now returns the native PID only at the two validated return PCs
`+0x6f294` and `+0xd0524`. It validates the image UUID, text bounds and surrounding instruction bytes, and removes the
callsite registration when the image unloads. Other CoreServices callers still receive the logical PID.

With the updated signed shim, host 10710 / launchd 10714 ran for more than six minutes without another Swift trap, versus
the previous 119/minute baseline. ReportCrash sampled at 0% CPU; later sampling found no ReportCrashService. This does
not establish that every wallpaper extension works: PosterBoard still aborted separately with a nil transient-info URL,
and tapping a Contacts text field exposed a CoreHaptics crash. Both remained distinct from the resolved identity loop.
Evidence: `tmp/lsd-metadata-default/results/identity-fixed-services-console.log`, `identity-fixed-exit-rate.json`,
`extension-crash-rate-baseline.json`, and `cpu-identity-later.txt`. No debugger was attached during this comparison.

The user then ran guest `launchctl reboot userspace`. Guest launchd sent SIGTERM to its services, eventually killed
SpringBoard after its shutdown timeout, and exited with status 0. Only native host 10710 remained. The current host
does not automatically start a replacement simulator launchd after that shutdown; this command is not yet a complete
simulator restart operation. The next diagnostic boot was explicitly started by relaunching the host.

A subsequent exact coordinator probe isolated PosterBoard's nil URL: native `kern.bootsessionuuid` returned `EPERM`,
leaving its zero-filled 37-byte buffer untouched. Stock `+[NSString(PBFUtilities) pbf_bootInstanceIdentifier]` nonetheless
constructed a 36-character string, all NULs. The resulting `TransientInfo-...<boot ID>.plist` filename could not form an NSURL.
Ordinary `NSTemporaryDirectory`, `PFTemporaryDirectory`, and URL construction had already passed independently.

The native host now generates one UUID per simulated boot and publishes `LSB_SIMULATOR_BOOT_UUID` to launchd and all its
children. The shim snapshots a validated UUID after libSystem initialization, then answers only read-only
`kern.bootsessionuuid` requests. Other queries, writes, pre-constructor calls and missing/invalid configuration retain
native forwarding. Focused tests cover length queries, short buffers, unchanged output/errno, forwarding and distinct
boot generations. The fixed coordinator probe returned a valid transient URL; the real PosterBoard job then reported
`state = running`, one run, and `last exit code = (never exited)` on host11508 / launchd11512, logical11558 / native11559.
This verifies the startup crash boundary, not the wallpaper picker or presentation. Evidence:
`results/poster-coordinator-probe.log`, `poster-coordinator-fixed.log`, `poster-bootuuid-status.log` under
`tmp/lsd-metadata-default/`. These tests ran without LLDB.

### PosterBoard scene exists while displayed output stays black

The subsequent V3 diagnostic ran without a debugger on native host **12095**, with PosterBoard logical **12235** /
native **12236**. The guest remained iOS 26.0/23A343, using the 1,353-image cache at `0x110000000`, UUID
`73ED978A-FD25-3166-A57A-34E21BB7E5B8`. A temporary load-command dependency in a separately signed PosterBoard executable
loaded the diagnostic; the original executable was retained. Restoration was still pending at this checkpoint.

Four main-thread snapshots at **2, 10, 30 and 60 seconds** consistently found the application and its
`com.apple.PosterBoard.switcher` scene foreground-active. `PBFPosterSwitcherSceneDelegate` owned a key, visible UIWindow
with alpha 1 and bounds 430×932. Its loaded `PBFPosterRackCollectionViewController` had a full-size attached collection,
a `LockScreenPosterCollectionViewCell`, a gallery-affordance cell, a section header, two buttons and a page control.
The inspected ordinary controls had visible ancestor chains. Nevertheless, the user still saw black output, and the
[host screenshot](../tmp/lsd-metadata-default/results/poster-scene-v3-screen.png) captured a black simulator display.
The [V3 log](../tmp/lsd-metadata-default/results/poster-scene-v3-live.log) includes earlier launches before the V3 constructor;
only the four labeled V3 snapshots belong to this result.

These observations rule out an absent scene/root controller or wholly empty rack as the immediate explanation. They do not
prove that its Core Animation context was submitted or visible in SpringBoard's scene host. `layerHasContents=0` on a
composite UIKit view also does not establish missing pixels. Public Metal and IOSurface connections were accepted from
this process, which proves connection authorization, not successful rendering or composition.

Pinned static inspection narrows the next measurement. `PBFDisplayTransformer` returns the supplied display configuration
in a set without applying a geometric transform. The switcher window has an explicit initial publication of its context ID
through FrontBoard client setting `0x640d`, plus attach/detach updates; it does not rely solely on later notifications.
The custom context getter is also used for touch transfer, so matching context IDs alone would not prove rendering.
SpringBoard presents this private-role scene through CoverSheet's `CSPosterSwitcherViewController`, a traits-aware host,
and `SBDashBoardHostedAppViewController`; their content-ready, attachment and visibility state still needs measurement.

The IOSurface notifier warning is not sufficient evidence of a missing implementation: in the original 23A343 simulator
IOSurface image, `IOSurfaceClientSetSurfaceNotify` unconditionally returns `kIOReturnUnsupported` (`0xe00002c7`).
See the [context/hosting audit](../tmp/lsd-metadata-default/poster-context-static.md) and
[rendering review](../tmp/lsd-metadata-default/poster-rendering-review.md) for image UUIDs and exact call sites.
A bounded V4 context and offscreen-capture probe was pending. Offscreen capture can omit remote-backed content, so its
ordinary local controls, success result and live output must be compared separately. No rendering fix or successful
wallpaper picker presentation is established yet.

V4 then ran without a debugger in native PosterBoard **12348**. The window context ID and published switcher context ID
both read **910221592**. The inspected window/root/rack model and presentation layers had full-size bounds, opacity 1,
identity transforms and no masking that explained the black display. At ten seconds,
`drawViewHierarchyInRect:afterScreenUpdates:` returned true and produced a 430×932 offscreen image with **49,930 nonblack
pixels**. The [guest-window PNG](../tmp/lsd-metadata-default/results/poster-scene-v4-window.png) visibly contains the
Collections header, clock/date, poster outline, Customize and plus buttons, and page indicators. The corresponding
[live host screenshot](../tmp/lsd-metadata-default/results/poster-scene-v4-device.png) still shows a black simulator display;
the measurements are in [the V4 log](../tmp/lsd-metadata-default/results/poster-scene-v4-live.log).

This establishes that ordinary PosterBoard UI can draw and that the custom context ID was published correctly. It does
not establish correct remote wallpaper rendering or successful live scene composition. The next boundary is SpringBoard's
private-role scene host and compositor visibility; the black-screen issue remains unresolved.

After native PosterBoard **12348** exited, the original executable was restored from its preserved device backup. The
temporary probe library and launch helper were moved out of RuntimeRoot, and the diagnostic launchd job was booted out.
The user-requested guest SpringBoard restart completed with new native SpringBoard **12389**, but the
[later live screenshot](../tmp/lsd-metadata-default/results/after-respring-later-device.png) remained black while touch
diagnostics still rendered. A respring therefore did not resolve the symptom. The next full simulator boot began under
native host **12420** at 17:03:54 after stopping only the owned host and children; its
[console](../tmp/lsd-metadata-default/results/poster-full-reboot-console.log) records that separate test. The full restart
restored the Home Screen, but the user confirmed that reopening PosterBoard still produced black output.

At the user's request, a supplied arm64 simulator FLEX dylib was then loaded only into guest SpringBoard. The original
SpringBoard executable was preserved on the device. Its temporary diagnostic copy adds a 32-byte `LC_LOAD_DYLIB /FLEX`
command inside verified unused header padding; no section offsets or existing dependency ordinals change. The small
bootstrap loads `/usr/local/lib/libFLEX.sim.dylib` and calls the declared `FLEXManager` API on the main queue after a
SpringBoard window is ready. Both libraries and the executable were signed with the existing app identity/identifier;
transfers used inactive staging and skipped post-copy checksums as requested. This is an inspection setup, not a production fix.

A fresh session under native host **12494**, SpringBoard **12526**, reported the explorer visible, and a
[device screenshot](../tmp/lsd-metadata-default/results/flex-springboard-device.png) independently confirms the FLEX toolbar
over the guest Home Screen. No debugger was attached. The simulator was left running for the user's examination.
The [preparation and rollback notes](../tmp/lsd-metadata-default/flex-springboard/README.md) and device activation receipt
record the temporary files. Restore the preserved original SpringBoard executable with affected guests stopped when
inspection ends; the normal setup guide does not install FLEX. PosterBoard's live presentation remains unresolved.

### FLEX disappearance and framebuffer boundary

The user observed FLEX disappearing when PosterBoard activated and returning afterward; it stayed visible over Contacts
and Passwords. A temporary V2 monitor in native SpringBoard **12618** recorded unchanged FLEX visibility, alpha, level
**1999**, context ID, scene activation and model/presentation geometry. No higher visible SpringBoard UIWindow appeared.
Its snapshots did not inspect the compositor's global scene ordering, so numeric window levels alone do not explain this.

The timestamped V3 monitor ran under native host **12678**, SpringBoard logical **12689** / native **12710** with the same
23A343 cache noted above. During the user-confirmed black screen, SpringBoard's secure-display provider remained false,
and FLEX remained visible according to UIKit. An explicit temporary marker changed only FLEX's existing context through
the verified typed `-[UIWindow _setSecure:]` API. Its secure getter changed false to true at elapsed **70.3 seconds**, but
the screen remained black. Removing the marker restored false at **115.2 seconds**, confirmed in the next snapshot.
No window level, global secure policy, PosterBoard executable or production shim was changed. This test did not support
secure-display filtering as the explanation; it does not characterize every compositor policy.

A separate native-host LLDB inspection invoked the existing asynchronous framebuffer readback. The retained 1290×2796
image contained **zero nonblack RGB pixels**, matching the black native screenshot. The host's renderer and presentation
were enabled, and its frame log reported successful copies without rejection or GPU failure. This excludes a nonblack
retained frame merely being obscured by the native UIView; it does not distinguish black incoming pixels from a copy error.
The next useful pixel comparison is the incoming IOSurface versus the retained frame for one presentation while its
producer fence is held. PosterBoard's earlier nonblack offscreen rendering remains a separate observation.

The host debugger detached and the process resumed. A subsequent guest-backboardd attachment recognized guest stack
symbols but could not evaluate the typed Objective-C getters: LLDB failed to resolve selector references. After detach,
native backboardd **12708** exited with `EXC_BREAKPOINT` / `SIGTRAP` at native address `0x19fc4c700`; the other recorded
frame was `0x19fc42910`, eight bytes past this run's native workqueue entry. The exact detach/exception interaction is
unresolved. This interruption occurred after the black-frame measurements and must not be treated as PosterBoard's
original failure. No valid backboardd display-state getter result was obtained.

Only the owned simulator session was restarted for recovery: native host **12792**, backboardd **12822**, SpringBoard
**12824**. No debugger/debugserver remained and the temporary secure marker was absent. The user then reported that all
apps failed to open. The original simple V1 FLEX bootstrap was restored, preserving V3 outside RuntimeRoot, and a fresh
session started under native host **12870**, backboardd **12907**, SpringBoard **12901**. The user confirmed Contacts
opened normally again. This rollback/restart establishes recovery, not which diagnostic action caused the regression.
FLEX itself consumes ordinary touches in Select/Move mode; that mode was not recorded, so interception is also unproven.
Only the simple FLEX injection remains active; no window-state monitor or secure-marker experiment remains installed.
Device evidence is under
`tmp/lsd-metadata-default/results/`: `flex-secure-enabled.log`, `flex-secure-restored.log`,
`poster-black-host-baseline.png` and `.json`, `backboard-debugger-detach-crash.json`, and
`poster-recovery-flex-screen.png`. The separate bootstrap rollback copies are recorded in
`tmp/lsd-metadata-default/flex-springboard/v3/device-activation.json`; the V1 restoration receipt is
`results/flex-v1-restored.json`.

The user subsequently confirmed that **Home Screen → Edit → Edit Wallpaper** opens the wallpaper editor successfully.
This is a different entry path from directly activating PosterBoard. The screenshot
`results/wallpaper-editor-done-visible.png` independently shows its Home Screen preview, Cancel/Done controls, and
Pair/Color/Gradient/Photo/Blur choices with FLEX visible. Done extends beyond the right edge of the guest display;
the user identifies a drawing-position problem, not an offset tap target. The central preview and bottom controls fit.
The user also confirmed that color and gradient wallpapers render through this editor. Do not treat the direct-launch
black screen as proof that this intended editor flow or the entire compositor is broken. Saving wallpapers and every
wallpaper extension are not established by these observations.

### Done-button measurement and PosterBoard FLEX

A bounded read-only startup probe then ran in native PosterBoard **12997**, without LLDB. It resolved the named rack-controller
ivars through the Objective-C runtime and sampled on the main queue; it did not change constraints or request layout.
The visible Home Screen editor's root and window were both **430×932**, with identity transforms. Its Done button had alpha 1,
frame **{366, 9, 92, 42}**, and an active trailing-to-root-trailing constraint of **+16**. Its `topButtonsEdgeInsets` remained
zero. A separate inactive switcher rack had already received the normal **−50** trailing constraint and hid its Done button.
This distinguishes the editor's uncorrected default from a host framebuffer width mismatch.

In the original 23A343 loose PosterBoard framework, UUID `8F1D23B9-955D-3DC6-A3D1-E863DFD6C49F`, the initializer at
file/VM offset `0x144bb4` uses `fmov d0, #16.0` for that trailing constraint. The later inset observer at `0x1376b4` negates
the left inset when it changes; that correction was absent in the measured editor. The user deferred the fix: **no framework
instruction or layout behavior was changed**. Exact original-byte/context pins and the live snapshots are retained under
`tmp/lsd-metadata-default/wallpaper-done-layout-review.md`, `wallpaper-done-initializer-pin.json`, and
`results/wallpaper-done-live.log` for a later controlled test.

At the user's request, the read-only probe was replaced with a small FLEX bootstrap for PosterBoard. Only its guest launchd
job was booted out and restarted; the native host **12870**, backboardd **12907**, and SpringBoard **12901** remained running.
New native PosterBoard **13039** loaded the supplied signed FLEX library and reported its explorer visible. The user confirmed
FLEX was available in **Home Screen → Edit → Edit Wallpaper**. This was a no-debugger test; relevant processes were running,
and no LLDB/debugserver remained at completion. The same cache/build/signing setup described above was retained.

The temporary bootstrap uses FLEX's declared public `showExplorerFromScene:` API on the main queue. Initial attempts stop
after showing it; scene-activation notifications move an already-visible explorer to the newly active PosterBoard scene.
It excludes FLEX's own window from readiness checks and respects the user's FLEX Hide action. Scene dismissal is left to
UIKit: the bootstrap does not hide the explorer on editor dismissal. No polling monitor, secure-window adjustment, production
shim change, or wallpaper layout patch remains active. SpringBoard's simple FLEX injection remains available too.

The diagnostic main executable still uses the previously validated dependency path `/usr/local/lib/PosterSceneProbeV3.dylib`;
that file now contains only the PosterBoard FLEX bootstrap. The preserved original executable and completed layout probe
remain outside RuntimeRoot. With PosterBoard stopped, restore its preserved original main and archive that bootstrap to
remove this injection; keep the shared `/usr/local/lib/libFLEX.sim.dylib` while SpringBoard still uses it. Local build,
signing, activation and verification receipts are under `tmp/lsd-metadata-default/flex-posterboard/`. Files were staged
inactive before replacement, and post-copy checksums were skipped as requested. These temporary inspection artifacts are
not part of normal runtime setup.

### Current wallpaper status

On 2026-10-27, after the FLEX inspection, the user confirmed that PosterBoard's lock-screen path still shows black output
and explicitly asked to stop investigating it for now. Treat this as a **known broken, deferred feature**, not a successful
wallpaper bring-up. Direct PosterBoard activation also remains black. No root cause or compositor fix has been established.

The separately verified path is **Home Screen → Edit → Edit Wallpaper**: the editor opens, and color/gradient wallpaper
rendering works. Its Done button remains clipped on the right, with the live constraint evidence recorded above; that fix
is independently deferred. Saving every wallpaper type, lock-screen wallpaper presentation and all wallpaper providers
are not established by the working Home Screen editor. FLEX remains a temporary inspection aid in SpringBoard and
PosterBoard; loading it did not establish a lock-screen rendering fix. No additional runtime changes or restarts were made
when recording this deferral.
