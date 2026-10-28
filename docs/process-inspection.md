# Guest process inspection

The process-list bridge exposes current managed simulator jobs to an authorized guest. It does not enumerate the native host,
its Metal helpers, or unrelated device processes, and it transfers no task-control or task-inspect rights.

## API and authorization

`ProcessSupport/ProcessInspection.h` defines a bounded version-1 snapshot with at most 256 records. The broker checks the full
kernel audit token against a running simulator job, rejects retired/finishing jobs, and requires an actual Boolean `proc_info-allow=true`
in that job's immutable original entitlement snapshot. Missing metadata, numeric/string lookalikes, malformed DER, and native helper
roles are denied. The original `__TEXT,__ents_der` is captured before guest execution; native code-signing entitlements are not substituted.

| Guest operation | Current behavior |
| --- | --- |
| `sysctl(KERN_MAXPROC)` | Authenticated capacity, 256; not a live process count. |
| `sysctl(KERN_PROC_ALL)` | Fresh membership snapshot of committed simulator jobs, with logical PIDs/parents and guest executable paths. |
| `proc_pidpath` after a successful snapshot | Path from the last authorized snapshot; unknown PIDs return `ESRCH`. |
| `KERN_PROC_PID` / `KERN_PROCARGS2` for self | Translate logical self to the real native PID and preserve the kernel's decision. |
| Peer `KERN_PROC_PID`, `KERN_PROCARGS2`, `proc_pidinfo`, `proc_pid_rusage`, `task_for_pid` after a snapshot | Unsupported known peers are denied; unknown PIDs are rejected instead of reaching a possibly reused native PID. |

List copyout returns complete `kinfo_proc` records. A short buffer receives `ENOMEM` and the number of bytes actually copied;
size-only queries return the required size. Before initialization, unmatched operations retain their original behavior.
The implementation is in `Guest/ProcessInspection.c`, the existing process wrappers, `ProcessSupport/SpawnTransport.c`, and `Host/Broker.m`.

Membership and logical parentage are current at each broker request, but the BSD status/flags/credentials come from the immutable native
CHECKIN snapshot. They are not live CPU, memory, FD, thread, or debugger-state measurements. Zero-filled unsupported `kinfo_proc` fields
must not be interpreted as measured zero usage. Peer task inspection and detail tabs remain incomplete. Real self-debugger queries still
use the kernel; the list's initial `P_TRACED` value cannot establish whether a debugger attached later.

## CocoaTop input

The current staged app uses the user's CocoaTop fork at `b28d80cae46e604b49af86c1c3ae132b01f576e7`. Its working tree was clean when
snapshotted and was not modified. The complete 13-entry fork `task.xml` is embedded as XML and iOS 15+ DER metadata, including Boolean
`proc_info-allow=true` and `com.apple.private.security.no-container`. The production Mach-O reader and CoreEntitlements decoder verify
full dictionary equality. The processed source Info.plist retains guest bundle ID `ru.domo.cocoatop64`; 28 fork resource files, including
its launch storyboard, are preserved. Native signing uses the host identity/identifier without these guest privilege entitlements.

The initial [D0m0/CocoaTop 2.2.3](https://github.com/D0m0/CocoaTop/releases/tag/2.2.3) rebuild reached UIKit but crashed when its cell builder
subtracted one from an empty column count, producing `SIZE_MAX`. Missing column preferences already have a fallback; the crash alone
does not establish whether the empty list came from an early zero-width layout or a disappearance callback. The user's fork already fixes
the subtraction, selects columns using the key window's width, and supplies scene lifecycle support. No additional UI fix is applied.

The fork rebuild uses arm64, Simulator SDK 27.0 and minimum OS 26.0, directly entering its unchanged `main_ui`. Its native setuid/setgid and
framework-loading wrapper is omitted. Only two C-dispatch casts and availability of the bundled `sys/proc_info.h` need SDK compatibility
adjustments; the UI and process-inspection implementations remain the fork's code. WebKit is linked for its `WKWebView`. The unused prebuilt
`res/CocoaTop_` and package postinst are not used.

The command-line probes below used the original 2.2.3 release's complete 12-entry entitlement dictionary; its XML matched source `task.xml`
at `c2b36cc38ffb2969bbd9487308f8a925552d742e`. Their authorization results are separate from the fork's app UI; both are verified below.
The fork UUID is `53EEA2A2-23BD-3162-AD07-66F2CCED51F9`.

Sources, metadata, resources, binaries and recipes remain ignored under `tmp/cocoatop`; they are not setup prerequisites or committed
third-party binaries. `tmp/cocoatop/fork-b28d80c/README.md` and its provenance files record the immutable source snapshot, processed
Info.plist, build choices and staged unsigned app. These files may be absent in a fresh checkout.

## Device evidence: 2026-10-28

The fork UI and repeated command-line probes ran on iPhone16,2 with native iOS 26.1 build 23B50 and guest iOS Simulator 26.0 build 23A343,
without LLDB attached. Native host PID was 1529 and guest launchd PID 1533. The unchanged cache baseline is base `0x110000000`,
UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`, size 2,064,482,304 bytes; this run did not remeasure mappings or VM protections.
The native production-status policy selects guest cache detachment on this device; the host and native Metal helpers retain their cache.

CocoaTop opened successfully as logical PID 1592, native PID 1593 after exec replacement. The screenshot shows 30 processes with names,
guest paths and logical PIDs, including launchd, SpringBoard and backboardd. Unsupported per-process CPU/state/role fields display `-`/`?`;
the displayed aggregate CPU `0.0%` is not a measured workload. This verifies the process-list UI, not live statistics or detail tabs.
The old startup-exception diagnostic was disabled for this test.

The authorized probe (PID 1636) reported capacity 256 and 31 records of 648 bytes, with 20,088 bytes for both list size and copy. The extra
record was the probe itself. CocoaTop appeared as logical PID 1592 at `/Applications/CocoaTop.app/CocoaTop`. A launchd peer task query
returned `KERN_NOT_SUPPORTED` (46) with a null task port, and `proc_pid_rusage` returned `ENOTSUP` (45). The probe exited 0 with `PASS`.

The otherwise identical probe without XML/DER metadata (PID 1637) received `EPERM` from both `KERN_MAXPROC` and `KERN_PROC_ALL`.
Its native signature was accepted, while the broker recorded missing original entitlement metadata. It also exited 0 with `PASS`.
The earlier 29-record probe run established the same allow/deny behavior before the fork UI test.

| Artifact | UUID |
| --- | --- |
| Native host | `98F1C210-D4BF-386B-8344-754986BCACB0` |
| Guest shim | `BD098C71-9C5D-32B6-914F-72547AC8CE83` |
| CocoaTop fork | `53EEA2A2-23BD-3162-AD07-66F2CCED51F9` |

Local evidence is `tmp/cocoatop/fork-b28d80c/device-first.png` and
`tmp/guest-interactions/results/console-cooperative-final.log` (exec identity at lines 738–753, probes at 818–877). The prior probe log is
`tmp/guest-interactions/results/console-peer-debug.log`. These ignored files may be absent in a fresh checkout. Local tests cover real
XPC/Mach ownership, malformed wire data, Boolean authorization, short-buffer sentinels, logical-PID guards and actual dyld interposition.

## Drag service debugger-state queries

The default jobs include stock `com.apple.DragUI.druid`. It first queries its own debugger state using a logical PID;
that exact self query is translated to the native PID. During a drag, it also queries the native PID reported by an
NSXPC connection. Both druid and the native host receive `EPERM` from direct cross-process queries on the tested phone.
Returning an invented `P_TRACED` value would conceal a real debugger, so this path uses a cooperative fresh query instead.

Outer spawn transport ABI 4 registers a private Mach endpoint after guest TLS is initialized. Its receive right joins the
existing workqueue exception server's port set; no additional worker is created. A shared initializer also handles a guest
constructor running before libdispatch's lazy `__workq_open`. Registration has prepare/activate phases so the endpoint is
published only after the guest authenticates and stores the native broker's full audit token.

Only the exact stock druid executable with actual Boolean `com.apple.druid=true` can request this operation. The broker
resolves a committed native peer identity, sends a bounded 500 ms request off its main queue, and validates the fresh reply's
nonce, full audit token, PID and shape. The target runs the original native self `sysctl`; the broker revalidates both jobs
and the target endpoint before returning the result. Missing endpoints, timeouts, stale identities and native failures
remain errors. Process-list snapshots are never substituted for live debugger flags.

On iPhone16,2 / native 23B50 with guest 23A343, host 1650, launchd 1654, druid logical/native 1658/1679 and SpringBoard
1662/1682, four drag queries returned genuine flags `0x4004004` (`P_TRACED` clear). No LLDB was attached. Druid remained
running with one launch and no exit; the user confirmed that an icon now moves, but reported that it disappears afterward.
That remaining visual/drop issue is unresolved and is not established to be an Apple bug. The native host UUID was
`98F1C210-D4BF-386B-8344-754986BCACB0`; shim UUID was `DF04683A-4D5D-3902-9DCF-A303FCD8BA42`. The cache was unchanged
from the baseline above; mappings were not remeasured. Evidence is in ignored
`tmp/guest-interactions/results/console-cooperative-startup.log` and the corresponding launchctl result.

The full local suite passes, including real authenticated Mach replies, the shared exception/control server, the
constructor-before-workqueue ordering, current traced flags, timeout/retirement cases and malformed request rejection.
`LSB_TRACE_PEER_DEBUGGER_STATE=1` in the native launch environment enables query diagnostics; normal startup leaves them off.
