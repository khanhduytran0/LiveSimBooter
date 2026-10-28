# Guest process inspection

The process-inspection bridge exposes current managed simulator jobs to authorized guests. Process enumeration and task-port access
have separate entitlement checks. Neither includes the native host, native Metal helpers, or unrelated device processes.

## API and authorization

`ProcessSupport/ProcessInspection.h` defines a bounded version-1 snapshot with at most 256 records. The broker checks the full
kernel audit token against a running simulator job, rejects retired/finishing jobs, and requires an actual Boolean `proc_info-allow=true`
in that job's immutable original entitlement snapshot. Missing metadata, numeric/string lookalikes, malformed DER, and native helper
roles are denied. The original `__TEXT,__ents_der` is captured before guest execution; native code-signing entitlements are not substituted.

| Guest operation | Current behavior |
| --- | --- |
| `sysctl(KERN_MAXPROC)` | Authenticated capacity, 256; not a live process count. |
| `sysctl(KERN_PROC_ALL)` | Fresh membership snapshot of committed simulator jobs, with logical PIDs/parents and guest executable paths. |
| `proc_pidpath` after a successful snapshot | Physical executable path under the canonical `RuntimeRoot`; unknown PIDs return `ESRCH`. |
| `KERN_PROC_PID` / `KERN_PROCARGS2` for self | Translate logical self to the real native PID and preserve the kernel's decision. |
| Self `task_for_pid(mach_task_self(), ...)` | Return `mach_task_self()` with one additional owned send reference, for logical self or an unambiguous native-self alias. |
| Peer `task_for_pid(mach_task_self(), logicalPID, ...)` | Independently requires original Boolean `task_for_pid-allow=true`; returns an owned control right for the current committed simulator target. No prior list query is required. |
| Peer `KERN_PROC_PID` for stock druid | Authenticated cooperative query returns fresh native debugger flags; see the dedicated path below. |
| Other peer `KERN_PROC_PID`, `KERN_PROCARGS2`, `proc_pidinfo` and `proc_pid_rusage` after a snapshot | Unsupported known peers are denied; unknown PIDs are rejected instead of reaching a possibly reused native PID. |

List copyout returns complete `kinfo_proc` records. A short buffer receives `ENOMEM` and the number of bytes actually copied;
size-only queries return the required size. Before initialization, unmatched operations retain their original behavior.
The implementation is in `Guest/ProcessInspection.c`, the existing process wrappers, `ProcessSupport/SpawnTransport.c`, and `Host/Broker.m`.

The broker retains virtual paths for identity and role checks. Only `proc_pidpath` converts them to physical paths, using the runtime root
captured during guest initialization. CocoaTop reads the adjacent `Info.plist` and icon files directly; a virtual `/Applications/...` path
can be missing on the native filesystem or refer to a different native app. This matches the existing physical-path behavior used by
RunningBoard. An unavailable root or a path/buffer overflow remains an error, without falling back to a native process with the same PID.

Membership and logical parentage are current at each broker request, but the BSD status/flags/credentials come from the immutable native
CHECKIN snapshot. They are not live CPU, memory, FD, thread, or debugger-state measurements. Zero-filled unsupported `kinfo_proc` fields
must not be interpreted as measured zero usage. Detail tabs remain incomplete. Real self-debugger queries still
use the kernel; the list's initial `P_TRACED` value cannot establish whether a debugger attached later.

Peer FD enumeration, rusage and unsupported `proc_pidinfo` operations remain unimplemented. A returned task right permits the caller
to use native Mach task/thread APIs; it does not synthesize those other interfaces. The device task-port results below are separate
from the earlier process-list evidence, which predates this implementation and correctly reports `KERN_NOT_SUPPORTED` for peer requests.

## Managed task ports

Outer spawn transport version **5**, bootstrap version **3** adds `COPY_PROCESS_TASK` and a simulator-only native capture offer.
The broker authorizes a request using the caller's immutable original entitlement dictionary: `task_for_pid-allow` must be an actual
Boolean true. `proc_info-allow` neither grants nor is required for this operation. Missing original metadata, malformed values,
numeric/string lookalikes and native helper roles remain denied. A guest cannot supply a task capture offer or select a native role.

The request names a positive logical guest PID. The broker resolves exactly one current committed simulator job, excluding pending,
finishing and retired exec instances. After an exec replacement, the logical PID selects the replacement's genuine native task rather
than the retired process or an unrelated process that reused its PID. Both requester authentication and target validation compare
all eight audit-token words, including the native PID and generation. The captured task's current `TASK_AUDIT_TOKEN` must still match
the target's authenticated identity. The reply includes that complete token and one copied send right; the guest verifies the token
against the received task before returning an owned right to its caller. Errors carry no capability and clear the output port.

The control right is acquired before guest handoff, while LiveProcess still has native TLS and libraries. The host supplies a send right
to a private receive endpoint and a fresh 128-bit nonce in the CHECKIN reply. The native child temporarily installs an `EXC_BREAKPOINT` handler
only on its current thread and executes a nonce-bearing breakpoint. The kernel sends an `EXCEPTION_STATE_IDENTITY` message to the
host containing the genuine thread and task rights. The receiver checks the kernel audit trailer, exact message layout, full target
audit identity, nonce, expected instruction and control-port capability before resuming the child past that instruction. It does not
accept a task-self right supplied by the caller over XPC. The existing name-only CHECKIN right continues to establish identity; it
does not become a control right merely because `task_info` succeeds.

The child restores its previous thread exception handlers before simulator initialization, preserving a newer debugger registration
if one replaced the temporary handler. It then drops its capture send reference. The host keeps the capture endpoint alive until READY
confirms this handoff and releases the endpoint's receive/send rights on cancellation. The captured task right belongs to its native
job instance and is released with identity cleanup on failure, retirement or termination. Every successful task query gives the guest
one independent send reference, which the caller must release with `mach_port_deallocate`; failed validation or copyout releases any
imported reference. Native Metal roles skip this capture path.

Before the guest constructor, `task_for_pid` preserves the native call path without accessing guest TLS. After initialization,
logical self and an unambiguous native-self alias return `mach_task_self()` with one additional local send reference. This right
never leaves its own process, so its immovability is irrelevant. Native `task_for_pid` can deny even a self query on the tested phone.
A known logical peer wins if its PID collides with that native-self alias in the cached list. Other supported peer calls go through the broker without native-PID fallback
after denial. The existing druid debugger-state operation remains a separate fresh self-query protocol.

Local tests cover real cross-process exception capture, task reply ownership, complete audit matching, exact entitlement types,
current/retired job selection, malformed messages, bootstrap offer restrictions, pre-constructor forwarding and output-pointer faults.
Task transport fixtures use task-name rights to test message shape and reference ownership; the separate exception-capture tests
establish control-right provenance. Device evidence is recorded separately below. Avoid stepping or placing a breakpoint inside the
short temporary-handler window: a different breakpoint fails validation instead of being advanced as the expected rendezvous.

## CocoaTop input

The initial staged app used the user's CocoaTop fork at `b28d80cae46e604b49af86c1c3ae132b01f576e7`. Its working tree was clean when
snapshotted; the later scene-compatible alert fix is described below. The complete 13-entry fork `task.xml` is embedded as XML and iOS 15+ DER metadata, including Boolean
`proc_info-allow=true`, `task_for_pid-allow=true` and `com.apple.private.security.no-container`. The production Mach-O reader and
CoreEntitlements decoder verify full dictionary equality. The processed source Info.plist retains guest bundle ID `ru.domo.cocoatop64`; 28 fork resource files, including
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
The initial fork build UUID is `53EEA2A2-23BD-3162-AD07-66F2CCED51F9`.

Sources, metadata, resources, binaries and recipes remain ignored under `tmp/cocoatop`; they are not setup prerequisites or committed
third-party binaries. `tmp/cocoatop/fork-b28d80c/README.md` and its provenance files record the immutable source snapshot, processed
Info.plist, build choices and staged unsigned app. These files may be absent in a fresh checkout.

## Historical process-list device evidence: 2026-10-28

This run predates the task-port implementation above.

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

Introduced in outer spawn transport ABI 4 and retained in ABI 5, this path registers a private Mach endpoint after guest TLS is initialized.
Its receive right joins the existing workqueue exception server's port set; no additional worker is created. A shared initializer also handles a guest
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

## Physical executable paths: 2026-10-28

The inspection path correction was deployed to the same iPhone16,2 / native 23B50 with guest 23A343. Host PID 1906 and guest
launchd PID 1910 booted without LLDB. The native host was unchanged (`A02E398B-B7E2-33F6-B6C7-984A78B2E59C`); the new shim UUID
was `0F093912-974A-3FF9-B05D-75CC5DCF4D40`. The cache remained at the baseline base/UUID above; mappings were not remeasured.
Only the shim was staged and replaced after stopping the session, with a device rollback copy and no post-copy checksums.

The authorized probe (PID 1975) returned 31 physical paths, including `RuntimeRoot/Applications/Contacts.app/Contacts` and
`RuntimeRoot/Applications/CocoaTop.app/CocoaTop`. Its peer task/rusage requests still returned `KERN_NOT_SUPPORTED`/`ENOTSUP`.
The unentitled probe (PID 1976) still received `EPERM`; both probes exited 0. CocoaTop logical/native PID 1968/1969 then visibly
displayed the Contacts, CocoaTop and PosterBoard icons. The screenshot also still shows unsupported CPU/state/role fields;
this change does not provide task inspection or fix apps whose icon lookup requires an asset-catalog-only path.

The focused path/interposition checks, full `sh Tests/run.sh`, guest shim build and `git diff --check` passed. Ignored evidence is
`tmp/process-path-icons/results/console.log`, `before.png`, `after.png` and `tests.log`; these are not setup prerequisites.

## CocoaTop scene-compatible alerts

Selecting an app-information row in the fork's detail screen caused `SIGABRT`. The native report for PID 1987 identified
`NSObjectNotAvailableException`: `UIAlertView` is unavailable in a scene-based app. The exception backtrace led to
`SockViewController`'s `tableView:didSelectRowAtIndexPath:`. This was an alert presentation failure, independent of the unsupported
task-inspection APIs and the corrected executable paths.

The source fork now uses `UIAlertController` for this row, the signal-delivery error dialog and Settings' Reset confirmation.
Titles, messages and the reset action are preserved; the obsolete alert delegate is removed. Its deployment targets already support
`UIAlertController`, so no legacy fallback is needed. These edits live in the separate CocoaTop repository, not a guest UIKit hook.

The rebuilt app UUID is `B6E89D33-EAF6-315B-8581-9B0EDB8EC317`. All 13 original entitlement entries passed the production metadata
decoder after signing. It replaced only the guest app executable after stopping the session, with rollback retained and no copy checksum.
On the same iPhone/guest/cache baseline, the user confirmed that tapping the information row now shows a working dialog and dismisses
without crashing. The signal-error and Reset paths compile but were not separately exercised. No debugger was attached.
Ignored source snapshot, build/signing provenance, deployment receipt and console evidence are under `tmp/cocoatop/scene-alerts`.

## Task-port and self-inspection device evidence: 2026-10-28

These tests ran on iPhone16,2 with native iOS 26.1 build 23B50 and guest iOS Simulator 26.0 build 23A343, without LLDB attached.
The cache remained at base `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`; mappings and protections were not remeasured.
The final session used host PID 2286, guest launchd PID 2290 and native Metal helper PID 2287. The authenticated native policy reported
development status and selected cache detachment for simulator guests; the native host and Metal helpers retained their cache.

The first task-port build passed the authorized peer probe (requester 2238, target 2239) and rejected the unentitled requester 2242.
It exposed a separate self-query failure: after exec replacement, logical/native self 2262/2263 both returned `KERN_FAILURE` (5) with
null ports through native `task_for_pid`. CocoaTop's `updateMachInfo` returned early on that error. Returning an additional local
reference to `mach_task_self()` fixed this path without changing peer authorization or transferring an immovable right between processes.

After replacing only the guest shim and starting a fresh session, the self probe executed an actual `POSIX_SPAWN_SETEXEC` replacement
from logical/native PID 2350 to logical 2350 / native 2351. Queries using either PID returned `KERN_SUCCESS` and the genuine self-port
name 515, preserved `errno=EDOM`, and allowed `MACH_TASK_BASIC_INFO`. Both returned references were released; the original self port
still answered `TASK_AUDIT_TOKEN`. The probe printed `SELF_PROBE PASS` and exited 0. It carried no original entitlement metadata.

The repeated authorized peer probe (2352 → 2353, target generation 5872) carried only original Boolean `task_for_pid-allow=true`;
it did not require `proc_info-allow` or a process-list query. It obtained port 8203, verified the real target audit identity, read
basic task information, enumerated two threads, and used `mach_vm_read_overwrite` to read the child's eight-byte test value
`0x153426ab9876cdef`. The host, native Metal helper and an unknown PID were rejected with `KERN_INVALID_ARGUMENT` (4) and null ports.
After SIGTERM, `waitpid` returned target 2353 with status `0xf`; a new query for that exited target was also rejected. The probe printed
`TASK_PORT_PROBE PASS` and exited 0. The unentitled requester 2354 was denied access to child 2355 with `KERN_PROTECTION_FAILURE` (2)
and a null port, both while the child ran and after termination; its denial test also exited 0 with `PASS`.

CocoaTop then launched as logical PID 2357 / native PID 2358, and the user confirmed that its self inspection works. This confirms
the task/self paths exercised above, not complete FD enumeration, rusage, every detail tab, or the accuracy of every displayed CPU field.

| Artifact | UUID |
| --- | --- |
| Native host, unchanged by the self fix | `B08FF7D1-3E37-37C7-8CDC-07F6107B0787` |
| Native LiveProcess extension | `3DD025DD-9F24-36BA-9234-D843E2059F4E` |
| Initial task-port shim | `7639FC3A-EB1A-3309-8949-A5B8E1849F6A` |
| Final shim with self fix | `F9AA9316-EA57-35BB-97A6-3EC5B6F1E810` |
| Authorized peer probe | `923517AF-A53F-3A58-B822-833BC5248277` |
| Denied peer probe | `067B5B2C-8DD0-3449-B19D-9FC3D691E8FA` |

The full `sh Tests/run.sh` suite passed before the self fix. After it, focused self/peer unit and actual interposition checks passed,
including local send-reference counts, copyout failures, early initialization and PID alias collisions; the simulator compile and
device checks above also passed. The final shim was staged after stopping the session, with a rollback copy and no post-copy checksums.
Ignored evidence is `tmp/task-ports/device/results/{console.log,tests.log,capture-tests.log,uuids.txt}` and
`tmp/task-ports/self-fix/results/{console.log,uuid.txt,deploy.log}`. These files may be absent in a fresh checkout.
