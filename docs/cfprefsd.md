# Simulator preferences and touch visualization

The `--launchd-backboardd` configuration now starts the stock iOS 26 **23A343** preferences daemon alongside backboardd.
It uses job label `com.apple.cfprefsd.xpc.daemon`, arguments `/usr/sbin/cfprefsd daemon`, and Mach service
`com.apple.cfprefsd.daemon`. These three fields are validated against the matching runtime's stock plist before generating
the isolated job. The generated job has `RunAtLoad = true`, `KeepAlive = false`, and `EnableTransactions = false`; native
scheduler, resource-limit and credential policy are not emulated. Its output goes to `data/Library/Logs/cfprefsd.log` under
the selected App Group's `LSB/iOS/23A343` directory. Older device evidence below predates the `SimulatorData` → `data` rename.

The configuration also enables `BKDigitizerVisualizeTouches` in the simulator's `com.apple.backboardd` preferences domain.
It reads and merges `data/Library/Preferences/com.apple.backboardd.plist`, retaining unrelated keys, then publishes
the complete plist through an atomic rename. An already-enabled file is left byte-for-byte intact. No host `NSUserDefaults`
domain or native phone preferences are changed. Empty, df and lifecycle launch modes do not alter this file.

## Pinned runtime evidence

The simulator `cfprefsd` executable is a small branch into CoreFoundation's `__CFXPreferencesDaemon_main`; its two linked
dependencies, CoreFoundation and libSystem, already exist in the simulator cache. In this build, the entry calls the direct
`-[CFPrefsDaemon initWithRole:testMode:]` implementation with role 0 at CoreFoundation offset `0x19a75c`. The simulator's
role-selection path sets role 1 and chooses `com.apple.cfprefsd.daemon` at `0x199fc4`–`0x199fd4`.

The runtime also ships `com.apple.cfprefsd.xpc.daemon.system.plist`, with a `.daemon.system` service and a System-session
restriction. It is deliberately not loaded: the pinned simulator executable follows the normal daemon path, and this
bootstrap does not implement a distinct System login session or native root credentials. No separate macOS-style
per-user cfprefsd agent is needed for this simulator binary.

BackBoardServices `-[BKSLocalDefaults init]` passes the string `com.apple.backboardd` to `_initWithDomain:` at offset
`0x355c`. Its property-binding code at `0x380c` maps `digitizerVisualizeTouches` to `BKDigitizerVisualizeTouches`, with false
as the stock default. Backboardd observes that property and feeds it to the per-display digitizer visualizer. This is the
stock touch-visualization preference, not a host-drawn overlay.

The simulator CoreFoundation preference paths consult `SIMULATOR_SHARED_RESOURCES_DIRECTORY`; launchd and children
receive the isolated `data` directory there, and through `HOME` and `CFFIXED_USER_HOME`. The file is prepared before
the new launchd session starts, so its new cfprefsd instance can read the enabled value without an existing daemon cache.
Reconfiguring a still-running simulator preferences session is not a live preference-update mechanism.

## Staging and validation

Stage these matching runtime files before choosing backboardd mode:

| Source below stock RuntimeRoot | Destination below selected group/LSB/iOS/23A343 |
| --- | --- |
| `usr/sbin/cfprefsd` | `RuntimeRoot/usr/sbin/cfprefsd` |
| `System/Library/LaunchDaemons/com.apple.cfprefsd.xpc.daemon.plist` | `RuntimeRoot/System/Library/LaunchDaemons/com.apple.cfprefsd.xpc.daemon.plist` |

Sign the executable using the existing runtime identity/identifier procedure; preserve its simulator platform and original
executable file type. The generated jobs live in `data/Library/LiveSimBooter-BackboardDaemons/`; the stock runtime
plists remain input files. The host creates the Preferences directory with mode 0700 and newly written preference files
with mode 0600. Existing preferences must be an owned, singly-linked regular file of at most 1 MiB containing a dictionary.
Malformed data, oversized files, symlinks, hard links and other file types fail preparation without overwriting the input.
Unexpected extra jobs in the generated directory are also rejected, including a manually added system cfprefsd job.

Focused configuration tests pass normally and under ASan/UBSan. They cover service identities, missing/mismatched stock
inputs, preserved binary-plist values, idempotent regeneration, isolated launch modes, log preservation, and unsafe or
malformed filesystem inputs. The first phone launch started cfprefsd with native PID **82275**, stable guest PID **82273**,
and the simulator preference file contained the enabled Boolean. Backboardd independently aborted while saving the newly
created digitizer's dynamic-service record; that trial does not prove a visible touch marker. Evidence is in
`tmp/metal-native/guest-cfprefsd-indigo.log`.

The later temporary observer trial ran backboardd with logical PID **82413**, native PID **82415**, while cfprefsd was alive with native PID
**82416**, stable guest PID **82414**. The observer was gated to the exact backboardd executable and reported:

```text
com.apple.backboardd/BKDigitizerVisualizeTouches boolean=1 value=1 replies=0 dictionaries=0 last-audit-pid=0
```

This proves the preference API returned true. It does **not** prove the separate cfprefsd served the value: the observer
counted no synchronous replies on its exact `com.apple.cfprefsd.daemon` service-name filter. The implementation and pinned
trial copy are `tmp/cfprefsd/CFPreferencesTrace.c` and `CFPreferencesTracePinned.c`; evidence is
`tmp/cfprefsd/guest-probe-fonts-ipc.log`. These are read-only diagnostics, outside production sources.

One possible explanation is CoreFoundation's direct-mode selection. In the pinned binary, `_CFPrefsDirectMode` checks
`__CFPREFERENCES_AVOID_DAEMON`, an explicit direct-mode flag, a process-path list, and
`sandbox_check(getpid(), "mach-lookup", ..., "com.apple.cfprefsd.daemon")`. Any nonzero sandbox-check result selects direct
mode. CoreFoundation receives the stable guest PID; after SETEXEC its original native process has retired, so that PID
could make the kernel check fail. Direct mode creates a local anonymous CFPrefsDaemon endpoint, which the observer's
named-service filter would not count. This is a **hypothesis**, not a measured return value or a confirmed diagnosis.
The examined `CFFIXED_USER_HOME` references only choose filesystem locations; they do not directly enable this mode.

Wire-level proof still requires a response for the backboardd domain, an original audit token matching the current
simulator cfprefsd, and the true returned preference. A successful preference API result, daemon liveness, or the on-disk
Boolean alone is insufficient.

## Touch-visualization runtime assets

Subsequent physical touches on the native display view reached the stock simulator visualizer and produced label/dirty
rectangle updates. This is independent evidence that the preference is effective, without establishing the daemon path.
The earlier dynamic-service save exception required the normal `SIMULATOR_BOOT_TIME` boot-generation environment, now
generated by the launch configuration; see [the Indigo prerequisites](backboardd.md#host-indigo-input-and-presentation-preparation).

The touch-visualization trials additionally exposed these missing, lazily used runtime assets. Stage each from matching
**23A343**, at the same relative path below `RuntimeRoot`:

| Path | Requirement |
| --- | --- |
| `usr/lib/libobjc-trampolines.dylib` | Sign with the existing runtime identity/identifier; retain simulator platform. |
| `System/Library/Fonts/Core/Helvetica.ttc` | Stock data file, no executable signing. |
| `System/Library/Fonts/Core/LastResort.otf` | Stock fallback font data, no executable signing. |

The deployed trampoline dylib's SHA-256 is `f9c02b9a27fa980d43fd04838639d9829d3fb46284598fd51035f5540c947001`;
`tmp/cfprefsd/deploy-trampolines.log` records the phone verification. CoreText explicitly requested Helvetica and then
reported the missing LastResort fallback in `tmp/cfprefsd/guest-probe.log`. Both font files were subsequently copied and
their phone hashes matched the local originals:

- Helvetica: `e5af39f85fa200eb0dfe5783c21de76139d76edeee6f3cebbe795b9d2f6cd5a1` (1,195,996 bytes).
- LastResort: `31eee5cd991e615fa7c8288555ace70b1170b4c18e35cbd18cf49428aa41838c` (2,532 bytes).

The source paths, sizes and hashes are in `tmp/framebuffer-server/fonts-manifest.json`, with transfer verification in
`tmp/cfprefsd/deploy-fonts.log`. The final phone screenshot confirms readable stock marker text after the font copy:
`t3 p1`, `in range`, `touch down`, and `ctx:C488CF8E`, together with the boot spinner. The native status bar remains clear
above the inset guest display. This validates the observed missing-font correction and stock touch visualization, without
claiming complete font coverage or a complete simulator boot.

That run used host **82455**, native Metal helper **82456**, simulator launchd **82457**, backboardd logical **82458** /
native **82460**, and cfprefsd logical **82459** / native **82461**. The normal production spawn shim had been restored;
its verified SHA-256 is `0d07af0348b0451dc318e623337e15e2b3e6f8a0b7a50d2fb655f2fae40c42ea`, recorded in
`tmp/cfprefsd/restore-shim.log`. No preference tracer was needed for the successful visual result.

The 1290 × 2796 phone screenshot is `tmp/framebuffer-server/phone-display-inset.png`, SHA-256
`11177bacf9c2d2914ca1b4f00b7e63574f4d7cabdbffb491fbf7c3b397e14476`. The process and frame evidence is
`tmp/cfprefsd/final-phone-state.log` and `tmp/metal-native/console-display-inset.log`. The run reached 3,300 completed frame
updates and remained alive at the final check; the scoped `tmp/metal-native/security-cfprefsd-boot.log` contained no amfid/TXM
events. Isolated-daemon delivery of the preference remains unproven, as explained above.

Local research disassembly is retained under ignored `tmp/cfprefsd/`. These offsets refer to the original 23A343 standalone
libraries, before shared-cache layout and ASLR relocation.

## Live preference updates on iPhone 17 (2026-10-27)

The Settings touch toggle exposed two separate failures in the 23A343 guest on iPhone18,2 / iOS 27.0 build 24A435.
The earlier direct-mode hypothesis is now measured: `_CFPrefsDirectMode` returned 1 because its native sandbox preflight
used a retired logical PID. The same query with the current native PID returned 0. Physical access to the isolated preference
file was allowed, yet the local direct-mode source could not synchronize changes.

`ProcessIdentity.c` now returns the native PID only at CoreFoundation's `getpid` return site `+0x1971a4`, immediately before
that native `sandbox_check`. Registration validates UUID `6E5CFB28-DD41-3756-9786-3EF0ADCD9D38`, text bounds and six unchanged
instructions. The sandbox decision itself is untouched. Before guest identity publication, calls retain native forwarding.
With this correction, direct mode became 0 and the probe received a genuine reply from the simulator cfprefsd, including
the original audit PID. However, the client still rejected the returned preference data.

The reply advertised the exact 30-byte shared-memory name `apple.sim.cfprefs.user.daemonv`. Stock `snprintf(..., 31, ...)`
truncates the intended version digit. The daemon had fallen back to private heap storage after failing to create the object;
clients' `shm_open(O_RDONLY)` returned `ENOENT`. CoreFoundation treats failure to map this counter page as an invalid source
before parsing its property list. XML was supported and was not the cause of the failed reads.

The existing owned-file shared-memory adapter now maps only that exact name to `data/tmp/cfpreferences-user-v1.shm`.
The real daemon creates a 16 KiB writable mapping; clients open separate read-only mappings and observe its change counters.
Other names, including slash-prefixed, untruncated `v1`, system-daemon and test names, retain native behavior. No sandbox
preflight exception was added. A fresh guest session is required when installing this fix because the daemon and clients
cache their earlier failures; subsequent preference updates do not require restarts.

Host12095 / cfprefsd12128 ran the signed shim with the existing keyboard-fixed cache at `0x110000000`, UUID
`73ED978A-FD25-3166-A57A-34E21BB7E5B8`, retained native cache and hardware workqueue redirection, without LLDB. Probe12191
read true, changed the value to false, and received successful synchronization plus a counter update through the audited
guest daemon. Disk readback later showed a typed Boolean in the daemon-written 210-byte plist and a 16 KiB owned counter
file. The user then confirmed both Settings toggle directions changed the live touch markers without I/O errors.

Focused tests cover exact caller selection, malformed image rejection, unrelated forwarding, cross-process read-only
counter updates, daemon reopen, unlink lifetime, per-runtime isolation and unsafe backing files. The integrated suite
passed. Evidence is in ignored `tmp/lsd-metadata-default/results/preferences-{probe,native-self-probe,probe-v2,probe-v3,shm-fixed-probe}.log`
and `tmp/cfprefsd/shm-adapter-tests/`. These experiments establish actual isolated-daemon preference delivery and live
backboardd observation, resolving the earlier validation limit above. They do not generalize the adapter to all POSIX SHM names.
