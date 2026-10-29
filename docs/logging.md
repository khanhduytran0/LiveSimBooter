# Guest logging and retry loops

The guest shim disables libtrace's Mach logging ports while the simulator logging services are unavailable. It follows
[Sim26Booter's 2025-09-11 workaround](https://github.com/khanhduytran0/Sim26Booter/blob/main/PATCHING.md#2025-09-11),
implemented here at the entitlement query instead of patching libtrace instructions.

[LoggingCompatibility.c](../Guest/LoggingCompatibility.c) interposes the typed
`xpc_copy_entitlement_for_token(const char *, const audit_token_t *)` API. In guest builds, exactly
`com.apple.private.disable-log-mach-ports` with a **NULL token** returns an owned XPC true object. NULL is libtrace's
self-query convention. Every explicit token, including a token for the current process, and unrelated key or NULL-key
dictionary queries tail-forward unchanged. The notify restriction self-query described below uses its actual guest snapshot.
The logging override changes no signed entitlement, broker entitlement snapshot,
native host policy, or service authorization. It requires no environment variable.

The matching iOS 26 libtrace self-check calls this API at image offset `0x7330` and caches the result. A true result makes
`_os_trace_diagnosticd_pipe` return before trying `xpc_pipe_create("com.apple.diagnosticd", 0)`. Without it, an unavailable
service produces a NULL pipe that is retried by subsequent streaming log calls under an unfair lock. This workaround
suppresses that transport path; it does not provide a replacement diagnosticd, forward to the host, or establish log persistence.

The check can run before guest TLS and constructors. Its matching path reads no environment, errno, audit token, or path,
and uses no libc string function. The pinned libxpc Boolean factory is a leaf that returns an immutable singleton without
allocation or initialization; recheck this assumption when changing runtimes. See the
[early-init disassembly review](../tmp/cpu-investigation/logging-tests/early-init-review.md).

## WebKit notification loopback

On 2026-10-28, foreground Safari on iPhone16,2 / iOS 26.1 showed about 29 MiB of physical-footprint growth in 30 seconds.
Its native PID 6318 sample spent 173 of 178 main-thread samples in WebKit's notification-forwarding callback, including
172 in entitlement lookup. Those lookups repeatedly reached the broker; the networking process was waiting, not spinning.
This is consistent with notification work accumulating; the sample alone does not prove the allocation source or a permanent leak.

The pinned WebKit [UI observer](https://github.com/apple-oss-distributions/WebKit/blob/2331c43da3e7123e51ba8682d7ec023b8591460e/Source/WebKit/UIProcess/Cocoa/WebProcessPoolCocoa.mm#L768)
forwards allowlisted Darwin notifications to WebContent processes having
`com.apple.developer.web-browser-engine.restrict.notifyd`. The
[WebContent receiver](https://github.com/apple-oss-distributions/WebKit/blob/2331c43da3e7123e51ba8682d7ec023b8591460e/Source/WebKit/WebProcess/cocoa/WebProcessCocoa.mm#L1609)
updates local state and calls `notify_post` again. Stock
[libnotify](https://github.com/apple-oss-distributions/Libnotify/blob/main/notify_client.c)
uses that entitlement to make all notification registration and posting process-local. If the UI process sees the guest
entitlement but WebContent's libnotify does not, the repost returns to notifyd and the UI observer can forward it again.

The 23A343 libnotify UUID is `94562B39-D2AB-32AF-8C4E-F7718C6AB63A`. Its query at `+0x34fc` uses
`xpc_copy_entitlement_for_token(key, NULL)`, caches the result at standalone image offset `0x14180`, and checks it in
`notify_post` at `+0x3694` before choosing the same local path as `self.` names. A rebuilt cache relocates its data:
derive the loaded `__DATA,__bss` address and add `0x18` for this byte (`0x10` for its once token), rather than adding the
standalone data offset to the loaded image. Pinned libxpc's NULL-token route uses `getpid` and `csops`; the existing
libxpc identity rule gives it the native PID, but this still bypassed the guest `csops_audittoken` entitlement snapshot.

For this exact key and NULL token, `LoggingCompatibility.c` now obtains the genuine kernel `TASK_AUDIT_TOKEN` and passes
it to the original XPC API. Stock libxpc then uses `csops_audittoken`, which authenticates the full native identity against
the broker's immutable guest entitlement snapshot. The original Boolean, absent value, or error is preserved; the adapter
does not grant a restriction unconditionally. It runs on libnotify's first query, before libnotify caches the result, rather
than depending on constructor order. The separate early libtrace override above still performs no audit lookup or IPC.

Focused tests cover exact self scope, full audit-token preservation, true/false/non-Boolean/missing metadata, failed token
reads, unchanged explicit targets and native builds, plus real dynamic interposition.

A read-only device check after deployment verified initialized once tokens (`-1`) and the cached restriction byte `1`
in all three current WebContent processes (native PIDs 6948, 6950, 6956); Safari PID 6944 retained byte `0`. The reader
checked the task identity, libnotify UUID, section shape and relocated instructions before reading the byte, without
suspending or attaching a debugger to any process. This confirms the first cached query reached the guest entitlement
snapshot in this run, rather than merely proving that a later query would return true.

With Safari visibly rendering `example.com`, seven foreground samples over 60.53 seconds measured its footprint at
39.55 → 39.31 MiB and CPU at 0.01% of one core. WebContent footprints were stable or slightly lower (12.52 → 12.50,
17.96 → 17.80, and 12.38 → 12.28 MiB); each used at most 0.01% CPU. Notifyd used 0.04%, versus the original feedback-loop
activity. All sampled tasks were unsuspended. This fixes the observed idle notification storm; it is not a long-duration
leak guarantee or an interaction stress test. Device thermal pressure stayed at 0. See [networking](networking.md) for
the independent route/TLS corrections needed before the page loaded.

A second default cold boot with stock trustd selected automatically reproduced the stable result: Safari PID 7100 stayed
at 38.971 MiB across 60.53 seconds (0.001% CPU), with all three WebContent footprints unchanged and CPU rounded to zero.
Networking was also flat; notifyd used 0.037%. See `safari-default-measurement.jsonl` and `default-resource-summary.json`.

Local evidence is under `tmp/safari-network-20261028/analysis/` and the Safari sample beside it, with
`notify-cached-state.json` and `safari-alias-measurement.jsonl`. Rusage CPU counters are Mach absolute ticks: convert with the device's `mach_timebase_info`, not an assumed nanosecond unit.

## Device results, 2026-10-26

The iOS 26 guest runtime was cold-started with the new shim on the iOS 26.1 phone: native host **11278**,
launchd **11282**, SpringBoard **11305**, and systemstatusd **11314**. Commands ran through launchd-owned Fish.
The [console transcript](../tmp/cpu-investigation/final-console.log) records these separate outcomes:

| Test | Result |
| --- | --- |
| Stock `/usr/bin/log show` and `log stream` | Exit 64: `Cannot run while sandboxed`. |
| Temporary `log-preflight-probe`, with only the CLI sandbox-check branch disabled | Help text renders; help invocation exits 64. |
| Diagnostic copy, `show --last 1m` with the marker predicate | Exit 65: `Could not open local log store: Connection to logd failed`. |
| Diagnostic copy, `stream --timeout 5` with the probe subsystem predicate | Prints the predicate/header and exits 0; no records delivered. |

The concurrent test started marker process **11391** with a two-second delay and stream process **11392**. The stream
exited before the marker emitted, despite the requested five-second timeout. The marker reported default/error/info/debug
enabled, emitted `LSB-LOG-LIVE-20261026`, and exited 0. That proves the probe reached the logging calls, not delivery or
retention. Guest log streaming and persistent retrieval remain unverified. The stock tool was retained; the branch patch
exists only in the temporary diagnostic copy under [log-stage](../tmp/cpu-investigation/log-stage/).

## CPU observations

The default graphical service list now includes stock `com.apple.systemstatusd`. Before it was loaded, SpringBoard's
SystemStatus publisher repeatedly reconnected, and the missing diagnosticd lookup amplified its logging work. Two
approximately 20.09-second windows around loading systemstatusd gave the following native CPU deltas, with one core = 100%:

| Process | Before systemstatusd | After systemstatusd |
| --- | ---: | ---: |
| launchd, native 11009 | 106.38% | 22.48% |
| SpringBoard, native 11156 | 102.16% | 2.72% |
| All stable app/extension processes | 242.24% (22 processes) | 27.35% (23 processes) |

Identities matched PID, executable UUID, and process start time across all five snapshots in each window. SpringBoard
and launchd retained their identities between windows. These are sequential observations with a ten-minute gap and
different display workloads, not a controlled benchmark. See the [comparison](../tmp/cpu-investigation/systemstatus-comparison.md).

After the subsequent cold boot with the logging override, another 20.09-second window measured launchd at 12.52%,
SpringBoard at 13.05%, and the 23 stable app/extension processes together at 43.99%. This changed workload cannot isolate
the override's CPU effect. The captured [launchd tail](../tmp/cpu-investigation/launchd-after-logging-tail.log) contains no
diagnosticd or SystemStatus publisher lookup failures; other missing services and Spotlight spawn retries remain.
Raw counters are in [after-logging-window.json](../tmp/cpu-investigation/after-logging-window.json).

Guest/non-guest unit tests and a real dynamic-interposition test cover exact matching, explicit tokens, passthrough
results, and errno. The full [test suite](../Tests/run.sh) passed with native framework access; its output is saved in
[final-suite.log](../tmp/cpu-investigation/final-suite.log). Evidence under `tmp/` is local and intentionally untracked.
