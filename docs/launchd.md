# Simulator launchd

The opt-in `--launchd` host argument starts the matching iOS 26 (23A343) `sbin/launchd_sim` through a C LiveProcess.
The ordinary icon launch waits for Start, then boots the graphical session with launchd-owned Fish. The minimal `--launchd`
bootstrap described first below intentionally has no service search paths; it is a separate checkpoint from the graphical boot.

The native host writes `data/var/run/LiveSimBooter-launchd-bootstrap.plist` beside RuntimeRoot under the selected group's
`LSB/iOS/23A343` directory. Historical device records below may use the old `SimulatorData` root name.
It supplies the runtime/data environment, a regular-file stdout/stderr at
`data/Library/Logs/LiveSimBooter-launchd.log`, and `/dev/null` stdin. Stock launchd refuses terminal-backed stdout,
including the console inherited from a CoreDevice launch. The configuration is generated independently of any Mac simulator device.

## Replacing the trampoline and holding tank

The boot-chain reference is [Sim26Booter](https://github.com/khanhduytran0/Sim26Booter), especially its `SimBootProcess.md`
and trampoline hook. Here the native broker starts launchd directly, with no `execve` and no trampoline process.

For the pinned launchd binary, the tank transaction is Mach MIG message 708, not an XPC dictionary. The broker allocates
both a tank endpoint and launchd's future service port. An optional Mach send right in the existing synchronous XPC bootstrap
reply supplies the tank endpoint to LiveProcess. After its final native check-in, LiveProcess installs that endpoint as its
bootstrap port and enters simulator dyld.

Launchd sends a plain 24-byte request. `LaunchdHandshake.c` checks the complete kernel audit token against the extension's
check-in identity and replies with message 808, a 40-byte complex message carrying one `MOVE_RECEIVE` descriptor. The broker
retains a send right to the service port. A dispatch cancellation handler closes the tank only after its Mach receive source
is retired; job disposal releases the service right.

Stock launchd next calls `waitpid` using `XPC_SIMULATOR_HOLDING_TANK_HACK`. This implementation supplies `-1` while the initial
broker child ledger is empty. The shim returns `-1/ECHILD`, and this version of launchd compares that return value directly
with the parsed selector without checking success. It proceeds because they match. This is a pinned compatibility sentinel,
not a real tank PID or evidence that a tank process exited. A launchd update requires rechecking this sequence.

## Workqueue handoff

Production or unknown devices keep the native shared cache mapped and enable hardware redirection of its registered workqueue entry.
The host caches an exact Boolean `EffectiveProductionStatusAp` result and supplies it through authenticated check-in metadata.
Only false permits the development detach/software path; production/unknown rejects `LSB_RETAIN_HOST_CACHE=0`.
Explicit retention is available on development devices, and launchd propagates the resolved settings to descendants.
Rebuild the native app, extension, and guest shim together after this bootstrap-metadata change. See [handoff policy](runtime-handoff.md).
The retained-cache evidence
below establishes the workqueue handoff without a debugger. The later metadata-name patch reached a home screen with app
icons; input validation remains separate.

An earlier libpthread registration survives in the kernel. The existing suspended-pthread adapter can replace each ordinary
thread's entry PC, but kernel workqueue threads start asynchronously at the registered native `start_wqthread` address.
Simulator libpthread also receives `EINVAL` from its second registration and consequently exposes an empty feature mask.

Before detachment, LiveProcess captures the native workqueue entry address and actual `_pthread_workqueue_supported()` mask.
The guest adapter uses these only after validating the known duplicate-registration ABI. On the first `__workq_open`, it
starts one simulator pthread and installs a Mach exception handler. It redirects only an instruction fault at the exact old
workqueue entry, with the expected workqueue argument shape, to simulator `start_wqthread`. Other register state is retained;
only the PC and its signing flag change. Other faults are forwarded to the previously installed exception handlers, including
LLDB. The handler uses Mach messages and no Objective-C or dispatch queue. Programs that never open workqueues do not start
this server thread.

Attach LLDB before the handoff for this experiment. Attaching a debugger later can replace the task exception handler. An
exception inside the exception-server thread cannot be handled by that same thread; a previously installed debugger handler
is retained for it when available.

### iOS 27 hardware-breakpoint trial

The first hardware-breakpoint trial explicitly enabled `LSB_WORKQUEUE_HARDWARE_BREAKPOINT=1`. Before opening guest workqueues,
the bridge reserves one unused task-default `ARM_DEBUG_STATE64` slot for the captured native entry, verifies its readback,
and handles `EXC_BREAKPOINT` as well as the existing access faults. A redirect requires the exact entry PC, expected workqueue
arguments, and the thread's inherited owned slot. Unrelated exceptions retain their previous handlers. Native pthread
registration can precede arming: the breakpoint observes the registered address without changing registration.

Public XNU copies task-default debug state into newly created threads, including workqueue threads. It does not retroactively
update existing threads; this path relies on the single-threaded native handoff. Reused workers return to the registered entry
again, so the slot must remain enabled for the process lifetime if this mode is used. Clearing it after the first bootstrap
would leave subsequent worker entries unhandled.

On 2026-10-27, a separate signed control passed on the iOS 27 phone without LLDB. A new ordinary pthread inherited the slot
and the Mach handler redirected a mapped function to return 84. Removing only that thread's owned slot restored its original
return value 42; exact task-state restoration made a subsequent thread return 42 too. Code-signing flags remained `0x22003305`
with `CS_DEBUGGED` clear before, during, and after the test. This verifies breakpoint inheritance and removal for that control.

The actual detached-cache launchd trial still failed. Its file trace confirms environment value 1, successful task-state
installation and exact readback, slot 0 at native `start_wqthread` (`0x190116908`), and bridge readiness. No handled breakpoint
was recorded before launchd exited with wait status 9; its crash report confirms `CODESIGNING / Invalid Page` at that
same entry. This does not yet distinguish instruction-fetch priority from a device-specific workqueue inheritance difference.
That trial did not verify hardware mode as a startup fix with the native cache removed; the retained-cache trial below is separate.
Evidence: `tmp/hardware-workqueue/results/control-01-console.log`,
`launchd-hardware-03-trace.txt`, and the launchd console/crash reports in that directory.

Launchd redirects all three standard descriptors to `/dev/null` before this setup. Its lack of stderr diagnostics does not
show that arming was skipped; the trial used a temporary dedicated-file logger. The original signed guest shim was restored
after that experiment. The subsequent retained-cache trial below led to the current defaults.

### Retained native cache with hardware redirection

The next 2026-10-27 experiment retained native mappings and enabled both `LSB_RETAIN_HOST_CACHE=1` and
`LSB_WORKQUEUE_HARDWARE_BREAKPOINT=1`; this combination is now the default. If supplied, the first setting value wins and only
exact `1` enables it; omission also enables it. Launchd receives the same settings in its own environment and `Bootstrap.Child`,
including an explicit `0` diagnostic opt-out. Retention alone does not redirect workers.
Before changing debugger state or TLS, the handoff requires a present, nonzero native cache base and a complete guest cache
reservation ending at or below that base. An absent cache returns `ENOTSUP`; overlap returns `EADDRINUSE`. The single-thread
guard and switch to app-owned simulator TSD remain active. Native functions are still unsafe to call with that replacement TLS.

On iPhone18,2 running iOS 27 build 24A435, the guest was arm64 iOS 26.0 build 23A343 with 1,353 cache images, UUID
`73ED978A-FD25-3166-A57A-34E21BB7E5B8`, spanning `[0x110000000, 0x18e194000)`. The native cache base was `0x19f708000`,
with reported size `0x185800000`; the gap above the guest reservation was `0x11574000`, about 277.45 MiB. These are recorded
addresses from this boot, not constants. Native `start_wqthread` at `0x19fc42908` had leaf current/maximum protections `5/5`
(`r-x`); a query stopping at the enclosing submap misleadingly reported `r--`. The guest entry was `0x16aae0a74`.

Probe v1 ran without LLDB and kept native code-signing flags `0x22003305`, with `CS_DEBUGGED` clear. It completed eight
dispatch tasks and a dispatch timer, then exited 0. The bridge recorded four new-worker traps with flags `0x244005` and four
reused-worker traps with flags `0x64005`. The owned hardware slot stays enabled for worker reuse and future threads until
process exit. This run did not observe the `x5 == -1` workqueue cleanup sentinel. Retention keeps the existing shared mapping;
it does not copy the native cache. The probe's physical footprint changed from 14.03 to 14.22 MiB, which is not a full graphical
runtime memory measurement.

The first launchd job test also completed `launchd_sim → xpcproxy_sim → df`: the replacement df exited 0 and launchd's
authenticated health lookup returned `BOOTSTRAP_UNKNOWN_SERVICE` (1102), as expected for the deliberately missing service.
That run accidentally inherited the pre-handoff debug-stop option; only our stopped guests were resumed with `SIGCONT`,
without attaching a debugger. The clean repeat explicitly disabled both debug-stop variables: launchd 5063 spawned
xpcproxy 5064, its replacement df 5065 exited 0, and the health lookup again returned 1102. The frozen v2 probe (5061), with
an exact RX-protection check and an explicitly unblocked watchdog, also completed eight tasks plus its timer and exited 0.
Both code-signing queries still had `CS_DEBUGGED` clear, and all four second-wave workers reused first-wave threads.

The broader no-debugger run reached native host 5074, launchd 5078, and backboardd 5105. The framebuffer registered
`CAWindowServer`, created its 1290 × 2796 swapchain, and presented through native Metal at approximately 60 fps. Guest logs
also recorded event-bearing workqueue entries and a reused-worker cleanup callback (`x5 == -1`). This establishes those
redirect paths beyond the small probe. It does not establish a completed SpringBoard UI: lsd repeatedly aborted with
`Error getting container url (WRITE_METADATA_DICTIONARY)! Cannot continue.` Its captured stack reaches `_LSServerMain`
through an Objective-C exception and `abort`, rather than the old native workqueue instruction-fetch failure. The full test
was stopped to end that crash loop; a final launchd-to-df run again exited 0. A guest launchctl inspection was not submitted
because its separate SSH invocation rejected the host key; no launchctl result is claimed.

No AMFI rejection or TXM fault was found in the bounded native capture. No LLDB/debugserver was attached for these tests.
Native and guest builds, focused loader/handoff/workqueue/launchd-configuration tests, and the diff check passed. These runs used
an experimental build with both flags supplied explicitly; the source now enables the pair by default. All test hosts and
guests were stopped afterward, with no debugger left attached. The previous signed
shim is preserved outside RuntimeRoot for rollback. No runtime cache was rebuilt or copied for this experiment.

Evidence: `tmp/retain-native-cache-audit/cache-layouts.json`, `tmp/retained-host-cache/results/probe-console.log`,
`launchd-df-console.log`, `df-first.log`, `probe-v2-console.log`, `launchd-df-clean-console.log`, `services-retained-console.log`,
`lsd-control-state.log`, `graphical-exit.ips`, `final-df-console.log`, and `security.log` in that results directory. The v2 probe
uses a frozen source snapshot matched to its unsigned build; its provenance record is
`tmp/retained-host-cache/provenance-audit/report.json`. Local signing receipts identify the supplied development identity;
guest code uses the configured host identifier without signature entitlements.

### Container-metadata filename compatibility

A subsequent clean file-API probe isolated the lsd failure to the exact basename
`.com.apple.mobile_container_manager.metadata.plist`. Ordinary `open` and protected open with class 4 returned `EPERM` for that
name, while an ordinary basename worked. Creating the `.atomic.*` temporary file, writing, `fchmod`, and `fsync` succeeded;
renaming it to the stock basename returned `EPERM` with ordinary, exclusive, and secluded rename modes. This establishes the
filename-specific restriction. A subsequent kernel capture explicitly records `System Policy ... deny(1) file-write-create`
for the stock basename; the specific sandbox policy definition remains unidentified. This is separate from workqueue startup
and data-protection-class selection. The probe exited 0; evidence is
`tmp/lsd-metadata-default/results/metadata-api-clean-rows.json` and `sandbox-policy-kernel.log`.

[patch-ios26-container-metadata.py](../Tools/patch-ios26-container-metadata.py) creates a new cache with the equal-length
`.com.kdt.LiveSimBooter.container-metadata.v1.plist` literal at five pinned sites: ContainerManagerCommon, DMCUtilities,
MobileInstallation, SafariCore, and libsystem_containermanager. All five are in `__TEXT,__cstring`. Validation requires the
matching 23A343 image paths/UUIDs, original whole-section hashes, exact literal boundaries and offsets within those sections,
and read-only mapping bounds. Cache file offsets and its UUID may differ on a matching rebuild. It rejects extra occurrences
or an already patched input. No pointers, CFString lengths, or container-authorization checks change.

Its `--mach-o` mode applies the same replacement to the one additional loose consumer, the matching
`MobileInstallation.framework/XPCServices/com.apple.MobileInstallationHelperService.xpc/com.apple.MobileInstallationHelperService`.
The staged-code inventory found this one executable among 3,840 loose code files; there is no shared filename getter that
covers all six copies. Patch the cache before cache signing and supply the patched helper through the runtime stager's
`--add-binary` override before executable signing, as shown in [agent setup](agent-setup.md#build-and-inspect-the-cache).

The patched cache and loose helper were signed and deployed together. On iPhone18,2 running iOS 27.0 / 24A435, without LLDB,
lsd started, its launchctl status reported last exit 0 and two runs, and 647 guest metadata files were created under the new
basename. The user confirmed a home screen with app icons. Cache UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`, base `0x110000000`,
and the 1,353-image layout were preserved; the new cache CodeDirectory hash is `308875ea5c35f30ed7143cf92fd8da54dd9b1179`.
Evidence is in `tmp/lsd-metadata-default/results/patched-lsd-status.log`, `patched-services-first-console.log`, and `guest-logs-first/`.

This confirms the metadata and graphical-startup path, not every app or input operation. The first full migration omitted
CoreSimulator's `Library/Frameworks/SimulatorHID.framework`; after restoring its signed executable and `Info.plist`, backboardd
registered its Indigo endpoint, and the user confirmed touch markers, Home Screen swipes, and opening Contacts. The required external payload now appears in [agent setup](agent-setup.md#stage-resources-apps-and-signed-loose-code).
Preserve matching cache/helper/data rollbacks, because an existing guest data root using the stock metadata name needs a
separate migration plan.

## Initial findings

On the iOS 26.1 test phone, the first file-backed launch reached configuration parsing and reported `no bootargs string`.
The launchd-only `kern.bootargs` read adapter supplies an empty string; unrelated sysctls and writes remain unchanged.
During that fatal log path, LLDB showed the exception server inside simulator `mach_msg` and a workqueue thread inside
simulator `__workq_kernreturn`, after the host cache had been removed.

After the empty bootargs response, the next hardware breakpoint caught `could not create temporary state directory`.
The stock path is `/private/var/tmp/com.kdt.LiveSimBooter.launchd`; setting `TMPDIR` does not change it.
The first shim draft also exposed an early-initialization constraint: libSystem calls unrelated `sysctlbyname` queries before
`getenv` is usable. The adapter now checks the query name and write parameters before touching environment or errno state.

The scoped state adapter maps that exact directory and its descendants to `data/tmp/launchd`. It covers mkdir,
open, fopen, stat, unlink and renamex_np; unrelated paths are unchanged. It rejects traversal components and overlong translated
paths. Legacy syslog still attempts its original Unix socket path and can fail nonfatally; no process-wide chdir or socket
namespace emulation is introduced.

On 2026-10-25, PID 79886 completed the tank handoff and reached the hardware breakpoint at launchd's `dispatch_main` call.
After continuing, the original main thread exited. LLDB showed the simulator exception server, a simulator workqueue thread,
and libdispatch's simulator signal thread. A direct legacy MIG 404 lookup sent to the retained simulator service port returned
`BOOTSTRAP_UNKNOWN_SERVICE` (1102), with the exact launchd audit token verified. The host reported `error=0 bootstrap=1102`.
This verifies an answering empty launchd instance, rather than just successful mapping or a breakpoint before the event loop.

The native-cache address inspected during the same stop lay in `[0x180000000, 0x300000000)` with `---` protection; the relocated
simulator cache at `0x120000000` was `r-x`, with zero dirty pages reported. LLDB discovered 48 modules. Evidence is in
`tmp/launchd-boot/console-state.log`, `lldb-live-service.txt` and `guest-state.log`.

## Main-thread exit and final validation

Native libpthread also consumes the apple-vector `main_stack` entry. The loader now captures `pthread_get_stackaddr_np` and
`pthread_get_stacksize_np` before detachment, verifies that the native main thread's current frame is inside those bounds,
and supplies a fresh writable `main_stack=0xTOP,0xSIZE,0x0,0x0`. Without this, simulator pthread can assume the macOS default
stack size when `dispatch_main` calls `pthread_exit`. No native pthread storage or allocation ownership is copied.

The final LLDB run (PID 79895) captured a real stack size of `0xfc000`, with bounds `[0x16ae5c000, 0x16af58000)`. Simulator
pthread's fields matched those exact bounds before `dispatch_main`, with zero allocation metadata. After the main thread
exited, `[0x16ae5c000, 0x16af50000)` was inaccessible and the two upper pages remained mapped. The authenticated lookup again
returned 1102. Evidence is in `tmp/launchd-boot/lldb-final.txt` and `console-final-lldb.log`.

The final full host test suite and signed device build pass. The same updated shim also passed the established Fish command
path: Fish launched `./bin/df`, waited for status 0, printed `LAUNCHD_SHIM_FISH_OK`, and exited 0. This uses RuntimeRoot-relative
paths because the runtime still has no general chroot. Evidence is in `tmp/launchd-boot/console-fish-final.log`.
A fresh run without LLDB (PID 79911) again returned `error=0 bootstrap=1102`; it was left running with the host foregrounded.
Its evidence is `tmp/launchd-boot/console-final-normal.log` and `guest-final.log`. The SSH capture in `security.log` contained
no amfid rejection or kernel TXM event during these runs. The capture used the configured test phone; these results do not
establish ordinary stock provisioning or full simulator-service support.

## Reproducing the empty bootstrap

Deploy the matching signed `sbin/launchd_sim`, the rebuilt guest shim, and the existing working iOS 26 runtime. Build the app
normally. To enable the bounded native health lookup, launch with:

```sh
xcrun devicectl device process launch --device 00008130-00021D122680011C --terminate-existing --console \
  --environment-variables '{"LSB_LAUNCHD_HEALTHCHECK":"1"}' -- com.kdt.LiveSimBooter --launchd
```

For LLDB, also set `LSB_DEBUG_STOP_BEFORE_HANDOFF=1`, attach to LiveProcess, and use hardware breakpoints. The probe starts five
seconds after the tank transfer and waits at most one second; a debugger pause can make it time out without implying a runtime
failure. It sends a deliberately missing lookup directly to the simulator service right. Native `bootstrap_look_up()` uses
libxpc's cached bootstrap pipe and would not establish that this simulator instance answered.

This mode has an empty `Bootstrap.Paths`. The empty-bootstrap result alone does not establish daemon loading or a full system
boot. The following job probes separately exercise endpoint propagation, `xpcproxy_sim`, and supported exec replacement.

## A df job and exec replacement

The opt-in `--launchd-df` bootstrap writes one RunAtLoad job, `com.kdt.LiveSimBooter.df`, into the dedicated
`data/Library/LiveSimBooter-LaunchDaemons` directory. It refuses unexpected entries in that directory. The job uses
guest path `/bin/df` and writes stdout/stderr to `data/Library/Logs/df.log`. The plist uses the guest path
`/private/var/mobile/Library/Logs/df.log`; launchd translates it through the configured simulator data directory.
WorkingDirectory uses `/private/var/mobile/` with a trailing slash to select that same prefix rule.
`LSB_DEBUG_STOP_CHILDREN=1` propagates the pre-handoff debugger stop to the job processes as well as launchd.

Stock launchd first spawns `usr/libexec/xpcproxy_sim`. A plain job has no optional service-controller endpoint, so its spawn
attributes do not override the registered Mach ports. Native spawn would inherit launchd's registered service send right;
the shim snapshots those ports explicitly and carries owned copies through the broker. The ordered array preserves null
slots and repeated rights. TASK_BOOTSTRAP_PORT is separate, including the distinction between inheritance and explicitly
clearing the port. LiveProcess installs this state before entering simulator libSystem.

xpcproxy asks launchd for its job configuration, then calls posix_spawn with POSIX_SPAWN_SETEXEC. The broker prepares a new
LiveProcess for the final image. The parent keeps the original **guest PID**, while the broker records a separate current
native PID and audit generation. The new instance maps the same child-status ledger and wake endpoint as the old instance.
Only after preparation succeeds does the broker commit the replacement, reparent its existing children, and retire the old
native process. A preparation failure returns an error to the original caller. The old native exit is not a guest exit;
waitpid returns the replacement's final status under the original guest PID, and kill targets its current native identity.

The ledger carries a nonconsuming observation of the current native PID, exec generation and exit status. A staged exec
publication keeps readers from observing an accepted replacement until the native READY reply succeeds. Guest getpid/getppid
expose the logical identity. Kernel audit tokens retain their actual native identity.

Launchd also watches process dispatch sources and queries proc_pidinfo. The compatibility source for a managed child uses
a real DATA_OR dispatch source and polls the ledger for exec/exit transitions; it does not watch the retired proxy's native
PID. Process-info queries translate to the current native PID. These adapters are scoped to broker-managed children.
They do not create a kernel PID namespace, and raw syscalls or arbitrary audit-based service authentication are not virtualized.

The process source polls every 10 ms and coalesces events. It supports C dispatch retain/release ownership and does not emit
fork events. The child ledger pins each subscribed ticket until polling completes: waitpid and SA_NOCLDWAIT may
consume the status without losing an existing source's EXIT event. A pinned slot cannot be reused; abandoning a live child
does not manufacture EXIT. The proc_pidinfo flavor-18 reply rewrites PID/PPID only; unique IDs and audit
identity remain native. wait4 supports a null rusage pointer; nonnull usage fails without consuming a status.

Stock xpcproxy unconditionally calls setsid for a valid program. Native extensions reject that call with EPERM on this test
device. The launchd bootstrap therefore explicitly enables `LSB_SIMULATE_SESSIONS=1`. This gives the current guest a logical
SID/PGID, preserves them across spawn/exec even when proxy supplies a new environment, and permits setsid once unless it is
already a guest group leader. It does not change kernel groups, detach a controlling terminal, or implement group signals.
Cross-process session queries, setpgid and POSIX_SPAWN_SETPGROUP reject with ENOTSUP in this mode.

On 2026-10-25, the standalone SETEXEC probe preserved guest PID 79999 while replacing native PID 79999 with 80000; the parent
reaped exit 23 under 79999. Its second case preserved guest PID 80001 across native PID 80002, delivered SIGTERM using 80001,
and reaped signal status 15. The real launchd job subsequently ran proxy PID 80117, replaced it with df PID 80118, and reported
df status 0 while retaining guest PID 80117. Launchd remained responsive: the native health probe returned 1102.
Evidence: `tmp/launchd-df/console-exec-probe.log`, `console-final-normal.log`, and `df-normal.log`.

## Final LLDB validation and startup storage

The final hardware-breakpoint run used launchd native PID 80141. Its stock xpcproxy PID 80145 called posix_spawn with flags
`0x4040`, including SETEXEC, for RuntimeRoot/bin/df. The replacement ran as native PID 80150 while retaining guest PID 80145.
LLDB caught launchd's EXEC event handler, df's LC_MAIN entry, and df's exit 0. Launchd's wait4 returned 80145 (`0x13911`),
and the caller consumed status 0. The inspected native-cache region `[0x180000000, 0x300000000)` remained inaccessible;
the simulator TEXT region at `0x120000000` was executable with zero dirty pages. LLDB discovered all 50 df images.
Evidence: `tmp/launchd-df/lldb-final.txt`, `console-final-lldb.log`, and `df-final-lldb.log`.

This testing exposed a separate startup lifetime bug: native malloc placed long apple-vector strings inside addresses later
replaced by the simulator cache's fixed __LINKEDIT mapping. A missing ptr_munge token was actually an overwritten string.
The loader now serializes the kernel argument words and every argv/env/apple byte into a bounded 512 KiB writable allocation
in the app image. Temporary C++ staging allocations are destroyed before detachment. Oversized startup data fails with E2BIG;
main_stack and ptr_munge remain writable for simulator pthread consumption. Tests cover complete pointer containment,
exact-fit and overflow boundaries, destroyed staging allocations, and consumption of the writable startup entries.

Debugger detachment has a separate limitation. Immediately after detaching from PID 80141, a new workqueue thread faulted at
`0x1e4a538b8` inside the unloaded native cache, with the workqueue cleanup entry register shape. Debugserver's detach path
[restores its saved exception ports](https://github.com/llvm/llvm-project/blob/main/lldb/tools/debugserver/source/MacOSX/MachException.cpp#L437-L458),
overwriting the runtime adapter installed after attachment. The df exec/wait sequence had completed before
detach. Do not rely on a detached instance remaining healthy; start a fresh instance without LLDB after these tests.
Evidence: `tmp/launchd-df/post-detach.ips` and the final lines of `console-final-lldb.log`.

The final normal run used launchd PID 80165, proxy/guest PID 80166, and replacement native PID 80167. df exited 0 and launchd
again answered the authenticated lookup with `error=0 bootstrap=1102`. This instance was left running without a debugger.
The SSH amfid/TXM capture contained no rejection or TXM event during these runs. Evidence: `console-final-verified.log`,
`df-final-verified.log`, and `security-final.log` under `tmp/launchd-df`.

`sh Tests/run.sh` and the signed iPhone build pass. Focused loader and process-event lifecycle tests also pass ASan/UBSan.
Process-event metadata is retained through running event callbacks and queued cancellation callbacks, including after the
caller's final release; cancellation wrappers preserve the user's block/function handler and context.

## Process lifecycle follow-up

When a simulated parent exits, the broker transfers its running children and unconsumed exit statuses to a live root simulator
launchd. Each child's own shared ledger carries its current parent PID, so getppid updates without an exec or constructor rerun.
Replacing a parent through SETEXEC preserves its existing children and does not trigger adoption. With no live simulator
launchd, or after launchd exits, the explicit fallback is guest parent PID 0 with no simulated wait owner.

If launchd's 64 child slots are full, copied adoption records wait for space and new spawns receive EAGAIN. Parent metadata
updates immediately, but a specific waitpid for a deferred orphan can return ECHILD until its record is imported. The retry
queue does not retain whole old ledgers. Unknown exit-monitor failures do not invent an exit status.

`--launchd-lifecycle` runs the signed `/bin/lifecycle-probe` as an isolated job. On 2026-10-25, all three device cases reaped
exit 23 before delivering their process-source EXIT callbacks; metadata remained valid. Child 80212 observed parent PID
80211 change to simulator launchd 80205, then exited 37. The broker published its status to launchd; the probe exited 0 and
launchd answered the health lookup. Evidence is in `tmp/backboardd/console-lifecycle.log` and `lifecycle.log`.
Native tests additionally cover multiple watchers, discard/reuse races, bounded observer counts, abandonment, deferred
adoption, zombie transfer and exec during adoption. Focused ledger, event and broker tests pass ASan/UBSan.

## Native identity and service ownership

The backboardd experiment revealed that the logical PID adapter must leave some native runtime identity checks intact.
After xpcproxy replacement, libdispatch's firehose setup queried the old guest PID and received ESRCH. A self
`proc_pidinfo(guestPID, ...)` query now resolves to the current native PID and returns the native payload unchanged. Direct
`getpid()` calls from libxpc's executable segments also retain the native PID, because libxpc compares it against kernel
audit tokens. With `LSB_SIMULATE_NOTIFICATION_SHM=1`, direct calls from the exact `DYLD_ROOT_PATH/usr/sbin/notifyd` image also
retain the native PID: notifyd publishes it for clients' native process-exit watches. `libsystem_notify` and unrelated guest
callers still receive the stable logical PID. Image add/remove notifications maintain these executable address ranges;
an indirect call through another image or a tail call cannot be classified by this direct-caller adapter.

These changes passed the phone run with guest PID 80376 and native replacement PID 80379. They deliberately leave kernel
audit tokens, process unique IDs and PID generations unchanged. Managed-child `proc_pidinfo` translation and logical
wait/kill behavior do not provide a general kernel PID namespace.

The next observed failure was ownership of `com.apple.iohideventsystem`: simulator launchd resolves the native caller PID from the
kernel audit token against jobs still indexed by the proxy's stable guest PID. Backboardd's check-in returned 1100
(`BOOTSTRAP_NOT_PRIVILEGED`), then `-[BKHIDSystem startHIDSystem]` failed. Details, the backtrace and the expanded cache's
verified df run are in [backboardd and the simulator UI cache](backboardd.md).

The implemented adapter changes eight inline PID loads in the pinned 23A343 launchd_sim, UUID
`C3EC5C45-8C85-3D11-B59C-62C4248F5EF9`. The current bootstrap check-in (routine 206) and XPC listener activation (routine 805)
both call the endpoint handler at image offset `0x23670`. Its PID load at `0x236b0` now resolves the caller through the broker
before the existing job lookup and endpoint-owner comparison. The two legacy check-in loads at `0x2e658` and `0x2e6b8`, plus
XPC self lookup at `0x1bc08`, use the same adapter. The fifth load at `0x1db98` covers the owner comparison before returning
an XPC service's bundle dictionary (routine 718). The sixth, at `0x2d130`, selects the managed requestor's domain before
endpoint activation. The seventh and eighth, at `0x30048` and `0x2ffd8`, resolve an event publisher's owner for fetching
subscriptions (routine 403 / `0x193`) and arming its poll (routine 406 / `0x196`). Interposing `audit_token_to_pid` alone
cannot cover these inline reads.

The dictionary comparison became observable after explicitly registering the stock MTLCompilerService bundle. Launchd
spawned xpcproxy with guest PID **81562**, then the broker committed its compiler replacement with native PID **81564**.
The existing self-lookup adapter found that job, but routine 718 still compared `job->pid` from `[x0 + 0x3a0]` against the
native audit PID from `[x1 + 0x14]` and returned `EPERM`. Guest libxpc then trapped in
`_xpc_copy_xpcservice_dictionary.cold.2`, reporting that it could not retrieve the XPCService dictionary. The fifth veneer
translates only that audited owner operand into `w9`; it preserves the job PID in `w8`, the original comparison, error path
and complete audit token. This is separate from libxpc's native-PID domain creation and lookup, which remain unchanged.
The compiler crash is recorded in `tmp/metal-iosurface/compiler-crash.ips`. The signed five-site adapter let compiler
**81647** finish `xpc_main` startup, but its listener canceled with `failed at listener activation with error 1 - Operation
not permitted`; the client's source compilation then waited until its 60-second alarm. This was a separate domain lookup
failure, rather than another dictionary failure or evidence that compilation had begun.

The next hardware-breakpoint run stopped launchd **81737** at the endpoint handler's failure path. It received the original
audit token for native compiler **81752**, which the broker resolved to logical PID **81749**, but its domain-local lookup
selected `com.apple.xpc.launchd.unmanaged.confusing.1.81749` in the user domain. A read-only walk of launchd's global managed
job hash found the actual compiler job at the same logical PID in a different application PID domain. The upstream requestor
domain resolver at `0x2d0e8` was still loading the native audit PID at `0x2d130` before global managed lookup `0x17f84`; on a miss
it fell back to the user's domain. Translating only this load lets the original lookup select `job->domain`, while retaining
the original full token and user-domain fallback. Endpoint checks, native PID-domain handles and kernel identities are unchanged.
The capture is `tmp/metal-iosurface/lldb-compiler-listener-failure.txt`. The signed six-site output subsequently passed on
the phone: launchd **81795** started native compiler **81799** with stable logical PID **81798**, and native guest **81797**
completed source shader compilation, pipeline creation and GPU triangle rendering before exiting 0. Evidence is
`tmp/metal-native/console-compiler-sixth.log` and the **81797** section of `tmp/metal-iosurface/guest-compiler-sixth.log`.

The next two loads became observable while starting [selected core daemons](springboard-services.md). After the exact-name
notification shared-memory adapter let notifyd finish its initial setup, it exited with native termination reason
`namespace=19, code=1, signal=9`. Attaching to notifyd caught `brk #0xb001` at its image offset `0x5ec4`, with error-handler
argument `1` (`EPERM`) and the message `BUG IN LIBNOTIFY: Event publisher error`. Its main thread was already in the normal
`dispatch_main` / `pthread_exit` cleanup path; that stack was not the fault. The Foundation namespace did not establish a
Foundation caller: XNU's [user breakpoint handler](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm64/sleh.c)
marks this trap range potentially unrecoverable, and its
[exception policy](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/exception.c) uses the Foundation reason
namespace when terminating an undebugged process through that path.

The generated job already preserved stock `PublishesEvents = com.apple.notifyd.matching`. The simulator libxpc publisher
first sends routine `0x193` with that stream name, then routine `0x196` to arm polling. The pinned launchd jump table routes
these requests to `0x30048` and `0x2ffd8`; both load the native audit PID from `[x22 + 0x14]` and call the global managed-job
lookup at `0x17f84`. A lookup miss returns `EPERM` through `0x30228`, before consulting the job's declared streams. The two
new veneers translate only that lookup operand into `w0`, preserving the full token in `x22`, stream ownership checks,
poll lifecycle and existing error paths. Other event routines' inline PID reads remain outside this patch's scope.

With the signed eight-site output on the phone, notifyd remained alive and CoreServicesProbe native **83502**, logical
**83491**, completed a real notification round trip: two check registrations, a 64-bit state set/get, post, observed change
with matching state, and both cancellations all returned status 0; the probe exited 0. Its six bootstrap lookups also
succeeded, which establishes endpoint availability rather than full functionality of the other daemons. Failure evidence
is in `tmp/springboard-services/lldb-notifyd-first-trap.txt`; the successful **83502** run is in
`tmp/springboard-services/probe-events-fixed.log`, following an earlier timed-out run in the same file.

[The offline patcher](../Tools/patch-ios26-launchd-identity.py) replaces each load with a `B` into verified empty __TEXT
padding. Each veneer preserves all general and SIMD registers except the original load's destination, restores NZCV,
FPCR/FPSR, SP and LR, then branches to the next original instruction. It obtains the token pointer from the original register
or stack location without changing any token byte. A new libSystem `_dlsym` chained import resolves the injected shim's
`LSBLaunchdResolveAuditPID` export. The symbol lookup uses RTLD_DEFAULT; a missing symbol returns PID -1. Existing chained
imports and pointer fixups remain intact, and the signature allocation is moved after the new fixup metadata. The resulting
file must be signed again before deployment.

The helper sends the complete audit token through the existing synchronous XPC broker channel and preserves errno. Only an
authenticated, running simulator launchd may make this request. An exact full-token match to a running, committed managed
process returns its stable guest PID. Pending replacements, retired processes, stale credentials or PID generations, and
native PIDs aliasing an active or retired managed PID are rejected. Unmanaged identities otherwise retain their native PID;
the broker trusts the authorized launchd to supply the token from its kernel-audited request. The original token remains
available to launchd for sandbox and entitlement checks. The child ledger's `execGeneration` is not the kernel PID version.

Replacement commit and these resolver requests run on the same broker queue, so a lookup cannot run between the READY reply
and completion of the exec commit. Exited and replaced identities are retained in a bounded history of 256 records. Once
that capacity is exceeded, unmanaged lookups fail closed until the host restarts; exact active managed identities still
resolve. Old records are not evicted into native-PID fallback. Launchd's job PID and hash tables remain stable, as do guest
wait/kill semantics. This translation does not provide missing entitlements or a general kernel PID namespace.

The owner RPC uses `xpc_pipe_routine` without an explicit timeout. A returned transport or authentication error yields PID -1,
but a host that stops answering can stall launchd inside its service handler. This is separate from the one-second native
Indigo registration deadline below. Avoid pausing the host broker while expecting a guest owner lookup to complete.

## Reproducing the signed launchd adapter

Use a fresh output directory: the patcher and executable signer refuse to overwrite their source or an existing output.
The patcher accepts the original stock payload or that same payload with a different signature allocation, validates its
UUID and all nonsigning bytes, rejects partial or unrelated patches, and recognizes an already patched input idempotently.
Older four-site, five-site and six-site outputs are not inputs to this eight-site patch: regenerate from the original stock
or unpatched signed payload. The fifth and sixth veneers occupy verified zero padding at `0x52400` and `0x52600`; the event
publisher veneers occupy `0x52800` and `0x52a00`. The resolver name is at `0x52c00`, with the reserved range ending at
`0x52d00` inside the existing `__TEXT` segment. The last named section ends at
`0x51b8c` and the segment ends at `0x54000`; the complete reserved range is verified empty before patching.
It never signs or deploys a file. Rebuild and deploy the matching host app and shim with the patched launchd; an older shim
without the exported resolver cannot complete the patched owner lookups.

```sh
runtime='/Library/Developer/CoreSimulator/Volumes/iOS_23A343/Library/Developer/CoreSimulator/Profiles/Runtimes'
runtime="$runtime/iOS 26.0.simruntime/Contents/Resources/RuntimeRoot"
work=tmp/launchd-identity-repro
identity=D7610EDC80D34A9B1A3D3C8563D66A5CD4716293

python3 Tools/patch-ios26-launchd-identity.py \
  --source "$runtime/sbin/launchd_sim" --output "$work/launchd_sim-patched"
python3 Tools/sign-sim-executable.py \
  --source "$work/launchd_sim-patched" --output "$work/launchd_sim-signed" \
  --identity "$identity" --identifier com.kdt.LiveSimBooter

sh Tools/build-spawn-shim.sh "$work/libLiveSimSpawn.dylib" "$work/spawn-probe"
codesign --force --sign "$identity" --identifier com.kdt.LiveSimBooter \
  --timestamp=none "$work/libLiveSimSpawn.dylib"
codesign --verify --strict "$work/libLiveSimSpawn.dylib"

python3 Tests/LaunchdIdentityPatchTests.py --source "$runtime/sbin/launchd_sim" -v
python3 Tests/LaunchdIdentityVeneerTests.py
xcrun dyld_info -validate_only "$work/launchd_sim-signed"
xcrun dyld_info -imports "$work/launchd_sim-signed"
```

Install the signed executable as `RuntimeRoot/sbin/launchd_sim` and the signed shim as
`RuntimeRoot/usr/lib/libLiveSimSpawn.dylib`, retaining the previous files for reversal. The executable signer preserves
MH_EXECUTE while clearing CS_EXECSEG_MAIN_BINARY, as required by this runtime's earlier signing tests.

The earlier twelve offline patch tests passed against both stock and the device's unpatched signed binary; the current
fourteen-test suite passes against stock 23A343, including the two publisher routes and rejection of an older patch revision.
An independent chain walk verified all 1,708 existing fixups and exactly one new `_dlsym` bind. The native arm64 harness assembles the same veneer source,
compares its instructions with the emitted patch bytes, and exercises all eight sites with positive, zero, negative and
missing-helper results. It verifies original token bytes, destination W-register zero extension, and GPR/SIMD/flags/SP/LR
preservation. Broker, transport and resolver tests cover authorization, stale generations, aliases, replacement ordering,
and history saturation. The requestor-domain test also preserves the original managed lookup, fallback and native PID-domain
creation instructions. The publisher tests pin both original jump-table entries, managed-job calls, missing-owner error
branches and unchanged stream validation. The separate phone trials above establish the sixth site's compiler-listener and
rendering result and the eight-site revision's notification round trip.

`dyld_info -validate_only` and `-imports` accept the new metadata. The installed `dyld_info -fixups` printer traps when it
tries to name the added GOT slot's section: the slot is valid segment/page padding outside a named section. Use the independent
chain-walk test to inspect this layout. The veneers likewise live outside the named __text section, so inspect them by address
in LLDB rather than relying on section-based disassembly. Local artifacts and test/signing logs are under `tmp/launchd-identity/`.

## Native Indigo endpoint

The `--launchd-backboardd` mode creates a native Indigo input receiver before the tank handoff. After the handoff, the host
registers `IndigoHIDRegistrationPort` with the isolated launchd using MIG request 701 and reply 801. The helper accepts only
the expected full native audit token, validates the reply shape, and applies a one-second deadline. It runs off the main
queue so the UIKit host and broker can continue servicing startup. A timeout is an unknown registration outcome and blocks
the dependent replacement; it is not retried blindly.

The initial xpcproxy is allowed to become ready while registration is pending, so launchd can finish its synchronous spawn
and receive the registration request. Its queued SETEXEC replacement waits for endpoint registration to complete. A
registration failure is returned to the replacement request. Once the selected backboardd instance has authenticated to
the broker, its exact native audit token authorizes the Indigo event-port registration; job exit revokes that authorization.

A process already owning Indigo authorization cannot currently perform another exec: the broker returns ENOTSUP before
creating a replacement or changing the live event port, audit authorization or gesture state. This applies even before
that owner connects. The initial xpcproxy-to-backboardd replacement remains supported. A future ownership transfer must
preserve those capabilities transactionally; restoring only the old token after failure cannot restore a dropped event port.

Host tests cover the MIG endpoint protocol, audit mismatch, malformed replies, send/receive timeout cleanup and retained
rights. They also verify Indigo touch/cancel/backpressure behavior. The health-lookup helper now destroys any copied send
right returned by a timed-out send, while retaining the caller's original right. The phone's native host 80851 successfully
registered the endpoint with simulator launchd 80852 (`error=0 bootstrap=0`) before the backboardd replacement was prepared.
That initial trial was still blocked by IOHID job ownership. Subsequent native-to-guest identity fixes established the
Indigo handshake; the current host also registers [framebuffer](framebuffer.md) and [native IOSurface](iosurface.md) endpoints.
The current broker also starts an isolated [native Metal helper](metal.md) before launchd and registers its generic and
backboardd-specific endpoints. Phone probes passed authenticated Metal peer connections, completed a GPU clear and then
stock guest shader compilation, pipeline creation and triangle rendering with verified IOSurface pixels. Stock backboardd
then presented its first spinner frame to the host image view, and the next run completed 300 frames with partial updates
before aborting on an Indigo HID target mismatch. End-to-end touch delivery and complete simulator boot remain unverified;
see [framebuffer evidence](framebuffer.md#first-stock-phone-frame).

## Signaling children after SETEXEC

The stock 23A343 launchd first obtains a target task-name right and reads its genuine audit token before handling
`launchctl kill` or stopping a service. Its PID-to-audit helper is at `launchd_sim + 0x353d0`; the call to
`task_name_for_pid` is at `+0x35404`. The signal-service handler propagates that Mach error directly, explaining an observed
`launchctl kill SIGSTOP` result of 5. The stop path at `+0x18460` converts a failed audit lookup to ESRCH. Consequently,
`kickstart -k` could retire the old job in launchd and start another instance while the original native process still held
its listening socket. The observed sshdog pair was logical 5199/native 5201 and logical 6652/native 6653.

`PeerProcessCompatibility` now recognizes only the exact `/sbin/launchd_sim` executable with `LSB_SIMULATOR_LAUNCHD=1`.
Direct calls from that executable resolve child task-name requests through the host before trying a native PID lookup;
falling back after a failed syscall would be unsafe when an old proxy PID has already been reused. The broker authenticates
the full caller audit token and launchd role, accepts only its live direct children's logical PIDs, and validates the returned
native task-name right against the current full audit token. The returned task/audit identities are never rewritten.
RunningBoard's separate peer inspection continues to require native PIDs and retains its original framework caller scope.

The stock signal adapter at `+0x35370` extracts the native PID from that audit token. For this verified launchd only, broker
signaling accepts either a direct child's logical PID or its current native PID. It excludes retired, finishing, unrelated
and native-service jobs, rejects ambiguous matches, and validates the current native generation before delivery.
The normal `proc_terminate` path is also adapted, but only for direct calls from the opted-in launchd executable. It requests
SIGTERM and reports that signal using libproc's errno-as-return-value convention. This is the conservative untracked-process
case in [XNU's proc_terminate implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/proc_info.c#L3226-L3292).
Guest sudden-termination/dirty tracking and native task termination policy are not emulated; launchd's existing timeout and
SIGKILL escalation remain responsible for an unresponsive service. Other callers retain the native `proc_terminate` API.

Successful signal delivery never manufactures an exit or releases a job slot: the existing native exit monitor must publish
the real wait status. Group signaling, stopped/continued wait notifications, and `terminate_with_reason` metadata remain
outside this adapter. Local tests cover logical/native PID divergence, reused proxy IDs, ownership, generation and full-audit
checks, malformed task replies, send-right ownership, graceful termination, and native-lookup ordering. On the phone,
`launchctl kickstart -k` terminated sshdog's old native process 7761 with actual wait status `0xf`; its replacement 8623
accepted a fresh Fish SSH command. The old PID no longer had a process path. This verifies replacement and socket ownership,
in addition to launchctl's successful return. See `tmp/sshdog/restart-verified.log` and
`tmp/app-launch-fix/restart-peer-result.txt`. Opt-in diagnostics record `[launchd-child]` lookups and termination requests
in the simulator spawn diagnostic log.

## App-facing job PIDs after SETEXEC

The app launch path needs a different PID at its public job-info boundary. Hardware-only LLDB stopped RunningBoard's
launch-result path at `RunningBoard + 0x43f7c`: it requested the retired proxy PID 8754, while the broker had committed
replacement 8755. Both the task-name object and its audit token were nil at `+0x43f94`. RunningBoard then explicitly
removed the job through launchd routine 1003. Its incoming-connection path, using a current native PID, succeeded.
This was an identity mismatch before process monitoring, rather than a launchd execution timeout.

`LaunchdJobIdentity` translates the successful OSLaunchdJob reply's signed `pid` field to the current native child PID.
The 23A343 serializer calls the setter at `launchd_sim + 0x33fe8`; the adapter additionally requires unsigned
`job-state == 1`. Explicit native PID queries in routines 1008 and 712 translate back at getter sites `+0x3308c` and
`+0x1bcb0`. The new broker request authenticates the simulator launchd and resolves only a live direct child's exact
native PID, validating its task-name right against its complete current audit token. Genuine audit tokens, task-name
rights, launchd's stored logical PID, parent wait status, and job-handle UUIDs retain their existing meanings.

These adapters require the exact arm64 MH_EXECUTE image, UUID `C3EC5C45-8C85-3D11-B59C-62C4248F5EF9`, executable path,
TEXT geometry, and three original call instructions. They match the caller return address and dictionary field types;
unmatched calls retain native behavior. A failed mapping at a valid matched boundary returns PID zero, so a reused native
number cannot accidentally select another job's logical PID. Send-right ownership and errno are preserved.
Image callbacks select the pinned executable because an inserted dylib can precede it in dyld_sim's image array.

The focused tests cover image ordering, modified instructions, malformed dictionaries, exec/reuse changes, failure paths,
and send-right counts. Broker and transport tests cover sender authentication, exact native lookup, stale identities,
and malformed wire replies. The on-device adapter now initializes automatically and Contacts' launch request returns
`accepted=1, error=none`. The subsequent SpringBoard reconnect, UIKit asset and framework weak-link failures are addressed below. The final device run
opened Contacts successfully. Evidence is under `tmp/app-launch-fix/`.

## FrontBoard scene reconnection storage

After job identity translation, Contacts reached its application process but SpringBoard stopped in
`-[FBSceneWorkspace scene:didPrepareUpdate:]` at FrontBoard `+0x6b4cc`: the `FBSceneManager` workspace did not support
reconnection. LLDB found a valid `RBSOSServiceProcessIdentity` for SpringBoard, while its workspace domain had no
`reconnectShmemIdentifier` and an empty reconnectable-workspace set. The default domain initializer disables that support
when its POSIX shared-memory sandbox preflight fails. A manually supplied read-data probe for
`/FBW1:com.apple.frontboard.syste` returned denial (`1`), but that identifier is 26 characters, one longer than the stock
25-character limit; it does not by itself establish the result of the initializer’s exact check.

The 23A343 implementation derives two names, `/FBW1:` and `/FBW2:` followed by the first 25 characters of the domain's
Mach service name. These are workspace connection-state buffers, unrelated to the framebuffer's IOSurfaces.
Evidence and the stopped stack are in `tmp/app-launch-fix/lldb-springboard-contacts-reconnect.txt`.

With `LSB_SIMULATE_NOTIFICATION_SHM=1`, the existing shared-memory adapter now maps those bounded names to separate
`frontboard-workspace-1-*.shm` and `frontboard-workspace-2-*.shm` files in canonical, owned `data/tmp`.
It retains normal creation, exclusive creation, shared mapping and unlink lifetime. Symlinks, hardlinks, special files,
unsafe roots and unsupported names remain rejected. The sandbox wrapper handles only native-self queries with filter 17
and the three matching POSIX SHM operations, reporting access to those backing files. Unmatched calls retain their
original arguments and native sandbox behavior; the adapter is inactive until guest constructors have initialized TLS.

The next device run confirmed all three native preflights deny the correctly truncated
`/FBW1:com.apple.frontboard.syst`, while the managed backing-file checks succeed. FrontBoard created its workspace buffer,
and SpringBoard stayed alive through the Contacts launch. Contacts then aborted independently; that next application
failure is separate from the resolved SpringBoard reconnect assertion. Diagnostic evidence is in
`tmp/app-launch-fix/workspace-shm/guest-evidence.txt` and `console.log`.

These are ordinary file-backed shared mappings. Dirty pages may be written to storage; this is not a RAM-only POSIX SHM
implementation.

A separate device probe verified native POSIX SHM with the entitled name `group.com.kdt.LiveSimBooter/rqk`. Apple's
[app-group naming rules](https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.application-groups)
allow `<group identifier>/<unique name>`. This 27-byte group identifier leaves three suffix bytes under Darwin's 31-byte limit.
Exclusive creation, a 16 KiB shared mapping, a spawned child's read/write exchange, real exit status zero, and mappings surviving
unlink and FD closure all passed. Fileport and `xpc_fd` imports also mapped the same bytes after their wrappers were released.
Those FD conversions were tested locally within the parent; the child opened the native name separately. This establishes that
a broker can distribute genuine native SHM descriptors without replacing `mmap`. It is not yet the production backing store.
The probe cleaned up its objects and job; results are in `tmp/app-launch-fix/appgroup-shm/result-v3.json`.

## Application framework resources

After the reconnect fix, Contacts aborted in UIKitCore's `-[_UIScreenComplexBoundingPathUtilities _loadBitmapForScreen:type:]`.
The requested `BoundingPathBitmap-1290x2796-430x932-3.00x` exists in the staged simulator resources, but LLDB showed its bundle
path was the native `/System/Library/PrivateFrameworks/UIKitCore.framework/BoundingPathData.bundle`. The LiveProcess guest
handoff now enables the existing framework-bundle adapter for every guest, including applications with replacement environments.
It removes duplicate input settings before adding `LSB_SIMULATE_FRAMEWORK_BUNDLES=1`; native Metal helper startup bypasses this path.
The next device run still stopped Contacts at the same assertion. LLDB confirmed its bundle path now points into RuntimeRoot,
and the phone contains the matching asset catalog. The remaining asset lookup failure was traced to missing device capabilities below; a widget renderer
initially mistaken for Contacts was a separate process. Evidence is in `tmp/app-launch-fix/workspace-shm/lldb-contacts-framework.txt`.

CoreUI's default data-asset lookup obtains its artwork idiom, subtype, gamut and memory class from MobileGestalt. The simulator
MobileGestalt implementation reads those values from the device plist named by `SIMULATOR_CAPABILITIES`; the model identifier
and screen dimensions alone do not supply them. Stage the matching device type's `Contents/Resources/capabilities.plist` at
`RuntimeRoot/Library/Developer/CoreSimulator/DeviceType/capabilities.plist`. For the current iPhone 15 Pro Max configuration,
the source is `/Library/Developer/CoreSimulator/Profiles/DeviceTypes/iPhone 15 Pro Max.simdevicetype/Contents/Resources/capabilities.plist`.
Keep it consistent with `SIMULATOR_MODEL_IDENTIFIER=iPhone16,2`, product class `D84` and the 1290×2796 display.

The stock plist supplies the capabilities; it is a separate runtime resource and is not embedded in this repository. The native
handoff passes its physical path to simulator guests so application launch environments cannot drop it. Native Metal helpers
retain the phone's own environment. A fresh-process device comparison confirmed that the ordinary `NSDataAsset` and CoreUI
lookups return the expected 4,410 bytes with this profile. Without it, default lookup returned no data while an explicit
CoreUI phone-idiom lookup succeeded against the same catalog. The subsequent Contacts launch reached IOSurface and then
exited with SIGSEGV; the cache-only framework resolution fix below addresses that next failure.

## Cache-only framework weak linking

Contacts next stopped at a null indirect call in `IMDAuditTokenTaskHasEntitlement`, before it could evaluate an entitlement.
Its once initializer calls CommonUtilities' `CUTWeakLinkSymbol` to resolve `SecTaskCreateWithAuditToken`. That resolver searches
framework directories under `IPHONE_SIMULATOR_ROOT` and calls `dlopen` with the resulting physical path. The staged Security
framework has its resources, while its executable lives only in the rebuilt shared cache.

Hardware-only LLDB confirmed Contacts had the correct runtime environment. Opening
`/System/Library/Frameworks/Security.framework/Security` succeeded and resolved both `SecTaskCreateWithAuditToken` and
`SecTaskCopyValueForEntitlement`; opening the RuntimeRoot-prefixed path failed. dyld first tried a doubly prefixed path, then
the nonexistent loose executable. This failure is independent of the existing entitlement implementation and CFBundle resource
translation. The captured comparison is `tmp/app-launch-fix/workspace-shm/lldb-contacts-weaklink.txt`.

`SharedCachePathCompatibility` translates a physical RuntimeRoot `dlopen` path only when its logical suffix is present in
the current shared cache and there is no loose entry or symlinked parent. It accepts the canonical root and its strict `/var`
alias, rejects ambiguous paths, and preserves native behavior for other inputs. The final call is a mandatory tail call so
`@loader_path`, `@rpath`, and caller-sensitive modes retain their original calling image. Early startup bypasses translation
until guest constructors have initialized the runtime.

On the iPhone, the fresh-process weak-link probe resolved both real Security exports through `CUTWeakLinkSymbol`, before
its explicit logical/physical load checks. Both paths then returned the same handle without a dyld error. The production
shim, with no temporary fault-stop hooks, launched Contacts native PID 9996 (logical 9995) and displayed its “No Contacts”
screen. SpringBoard remained running. Evidence: `tmp/app-launch-fix/workspace-shm/security-weaklink-device.log`,
`console-cache-path.log`, `jobs-cache-path.txt`, and `contacts-cache-path.png`.
