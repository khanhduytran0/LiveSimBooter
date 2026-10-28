# Development notes

Start with [agent-assisted setup](agent-setup.md) for a fresh checkout: pinned iOS 26.0/23A343 inputs, tracked staging tools,
local signing configuration, native packaging, transfer, and device verification. Repository working rules are in [AGENTS.md](../AGENTS.md).
Use [the device migration runbook](migration.md) for USB transfer, App Group discovery, separate runtime/data roots, and rollback.
Current storage is `<selected store App Group>/LSB/iOS/23A343/{RuntimeRoot,data}`. Older dated experiments may call the mutable
root `SimulatorData` or place roots directly inside a group or Documents; those paths are historical, not current copy destinations.

These notes record experiments since 2026-10-25. The tested 23A343 guest reaches SpringBoard with app icons and opens
Contacts on iPhone18,2 / iOS 27.0 build 24A435. Touches, the simulator keyboard, the launchd-owned Fish terminal, native Metal
presentation, the performance HUD, and live touch-marker settings have device evidence. The host keeps persistent IOSurface,
Fish and Settings tabs. Production or unknown native MobileGestalt status retains the native cache with hardware workqueue
redirection. Only exact Boolean false for `EffectiveProductionStatusAp` selects development cache detachment; Developer Mode
alone does not select it. See [handoff policy](runtime-handoff.md).

The user verified **Home Screen → Edit → Edit Wallpaper** and color/gradient wallpaper rendering. PosterBoard's lock-screen
path and direct activation remain **broken (black output) and deferred**. The working editor's clipped Done button is also deferred;
see [current wallpaper status](springboard-services.md#current-wallpaper-status).
This remains an experimental partial simulator: generic fork/exec and complete job control are unsupported, and the observed
Spotlight last-character crash is deferred.
Historical sections below describe earlier cache layouts and failures; use the setup guide for current preparation commands.

- [Architecture](architecture.md): current process boundaries, launch roles, runtime compatibility, and pinned implementation references.
- [Simulator host services](simulator-host-services.md): observed Mac service inventory and the pinned SimRun audio reference.
- [Device types](device-types.md): original device profiles, phone/iPad choices, display dimensions, and applying a session restart.
- [Source licensing](licensing.md): MIT original code, Apple-derived APSL files, retained notices, and source availability.
- [Runtime handoff](runtime-handoff.md): startup, local callbacks, thread storage, cache detachment, and debugger metadata.
- [Multiprocess](multiprocess.md): App Group migration, native extension launches, synchronous XPC, FD replay, and current validation limits.
- [Process inspection](process-inspection.md): authenticated guest process lists, original entitlement metadata, CocoaTop staging, and device-tested limits.
- [Simulator launchd](launchd.md): empty bootstrap, tank handoff without execve, workqueue startup, and device findings.
- [Guest launchctl](guest-control.md): restart individual jobs and reboot the simulator session through the guest bootstrap namespace.
- [Guest SSH with Fish](sshdog.md): sshdog staging, keys, authenticated device connection, and remaining shell/PTY work.
- [Backboardd](backboardd.md): the expanded runtime cache, service identity, and boot progress.
- [Audio output](audio-output.md): optional fixed-format virtual HAL, shared PCM transport, native playback and device evidence.
- [Guest haptics](keyboard-haptics.md): optional host-owned relay, physically confirmed CoreHaptics playback, and the verified keyboard-pattern correction.
- [Display settings](display-settings.md): native shortcuts backed by a fixed launchd-owned helper, with verified HUD and live touch toggles.
- [Simulator preferences](cfprefsd.md): the stock cfprefsd job and isolated `BKDigitizerVisualizeTouches` preference.
- [Services toward SpringBoard](springboard-services.md): simslim evidence, selected daemon startup, notification compatibility, and remaining limits.
- [Guest logging](logging.md): the libtrace self-query override, guest `log` tool limits, and measured service retry CPU costs.
- [Shared-memory measurements](shared-memory.md): sampled page changes, process write counters, a controlled flush test, and attribution limits.
- [File locks](file-locks.md): native suspension crashes and descriptor-scoped RunningBoard lock metadata compatibility.
- [Framebuffer](framebuffer.md): verified first phone frame, frame scheduling, surface fences, and bounded dirty-rectangle composition.
- [IOSurface](iosurface.md): Apple's native remote server, authenticated guest connections, and allocation tests.
- [System power](system-power.md): the denied power user-client open and the tested virtual-awake subscription adapter.
- [Metal](metal.md): isolated native service, stock guest compiler registration, and verified shader-based triangle rendering.
- [Native Metal backend](../NativeMetal/README.md): host-selected roles, pinned metadata adaptation, signing and fixed-path packaging.
- [Bash](bash.md): noninteractive builtin baseline, terminal-access probes, and remaining fork/exec design requirements.
- [Fish](fish.md): unmodified-source shell results, executable-access compatibility, and interactive startup.
- [Fake terminal](fake-pty.md): launchd-owned Fish tab, socket transport, guest terminal controls, native keyboard/display, and validation limits.
- [Creation mask](creation-mask.md): tracked `umask` state for spawning after guest worker threads start on Darwin 25.
- [iOS platform compatibility](ios-platform.md): the pinned dyld_sim patch for unchanged device-platform binaries and dependencies.
- [Shared cache](shared-cache.md): signing format, TXM findings, the relocated iOS 26 cache, and reproduction commands.
- [iOS 27 relocation](ios27-relocation.md): low-address capacity, missing relocation records, and pointer/instruction fixups.
- [iOS 26 base comparison](ios26-base-comparison.md): minimum stock inputs, two rebuilt caches, and exact assembly/data differences.
- [Pointer classification](cache-pointer-classification.md): structural filtering of all false positives in both caches, preserving measured relocations.
- [Device debugging](debugging.md): installation, file transfer, SSH logs, LLDB attachment, and verification.
- [Provisioning cleanup](provisioning-cleanup.md): the custom trigger conflict, targeted repair, backup, and installation evidence.

The shader-compilation Metal trial on iPhone16,2 / iOS 26.1 used host **81793**, native helper **81794**, native guest **81797** and stock
guest compiler **81799**. Source compilation, pipeline creation and GPU triangle rendering succeeded. The guest verified
center/corner IOSurface pixels, wrote a visually inspected PNG and exited 0. A subsequent normal stock backboardd run used
host **82139**, helper **82140**, launchd **82141** and native backboardd **82143**. Operation 1 from surface **45** reached
the host image view; the recovered 1290 × 2796 PNG visibly contains a spinner on black. Later dirty-rectangle updates were
rejected by the then-full-frame-only server. After bounded partial composition was added, host **82181**, helper **82182**,
launchd **82183** and native backboardd **82185** reached 300 frames, including the observed `597,1350,96,96` updates.
Backboardd then aborted with `SIGABRT` after about 21 seconds because SimulatorHID rejected target 0. That historical input
failure is fixed by creating the built-in digitizer with the correct target and boot-generation environment. The latest run
uses native host **82532**, helper **82533**, launchd **82534**, backboardd **82537** and cfprefsd **82538**. Async Metal
presentation with three drawables passed 4,800 frames at 58.6 fps including startup, with later intervals approximately 60 fps.
See [framebuffer evidence](framebuffer.md) and [preferences/input results](cfprefsd.md). Full simulator boot remains unverified.

The matching iOS 26 build **23A343** runtime runs on the tested iOS 27 iPhone. With all 219 loose dylibs temporarily moved
outside RuntimeRoot, all 46 dependencies loaded from the relocated cache at `0x120000000`. `df` exited zero under LLDB
and in a normal launch; LLDB recognized 49 modules. One TXM overlap event occurred during the LLDB run, with no such event
in the normal capture; its cause is not established. The loose dylibs were restored afterward, and the relocated cache remains active.

The first iOS 27 build **24A434** mapping-only trial registered both signed cache files and mapped all 11 regions, then
faulted on an unchanged trie address. Its requested base `0x300000000` was still inside the native shared-region reservation
`[0x180000000, 0xfc0000000)`; both TEXT mappings were `r--`, with two TXM overlap errors. The iOS 26 files were restored
and `df` again exited zero. A repeat above both the reservation and GPU carveout at `0x8000000000` accepted both signatures
but failed its first TEXT mapping with `ENOMEM`; no high cache mapping was installed. The captured repeat had no TXM error
or AMFI rejection. The iOS 26 files were restored again and `df` exited zero. iOS 27 startup remains unestablished.

[Mapping-only dyld_sim comparisons](shared-cache.md#signed-dyld_sim-mapping-above-0x180000000) duplicated the signed iOS 26
loader's TEXT at `0x300000000` after cache detachment and `0x400000000` while the native cache remained loaded. In both,
signature registration and mmap succeeded, but the duplicate was `r--` while the identical low-address TEXT remained
`r-x`; TXM logged `38 | 23`, with zero dirty pages and no software breakpoints. Detachment was therefore unnecessary to
reproduce the failure. Both high copies were removed without execution, and the normal handoff ran `df` to exit zero.
The second test used a different verified-free address because the first was occupied by native cache data.
The original host app was restored afterward; a fresh normal launch also ran `df` to exit zero.
A signed Mach-O wrapper for the cache remains untested.

A [manual native dylib test](shared-cache.md#manually-mapped-native-dylib) mapped the same signed `MH_DYLIB` at
`0x140000000` and `0x400000000` with the host cache still loaded. Signature registration and all segment mappings succeeded.
The low TEXT was `r-x` and its leaf function returned `42`; the identical high TEXT was `r--` and faulted on its first
instruction, with TXM `38 | 23`. Neither copy had dirty TEXT pages or software breakpoints. This control needed no rebasing
or binding and does not implement a general dylib loader. The test process was terminated, the temporary device dylib
removed, and the restored original app ran `df` normally to exit zero.

[Requesting execute permission afterward](shared-cache.md#requesting-execute-permission-with-mprotect) failed with
`EACCES`. Actual VM queries showed low TEXT current/maximum protections `r-x`/`rwx`, but high TEXT `r--`/`rw-` both before
and after mprotect. High execution still faulted on its first instruction; the restored original app again ran `df` to exit zero.

[Shared-region lifetime research](shared-cache.md#unregistering-the-retained-host-shared-region) found no app-callable
selective TXM unregister path. Detachment removes VM mappings but retains the nested pmap association; the examined XNU
and TXM only remove it during whole-address-space destruction. Fork inherits the association and exec installs it again.
This conclusion is bounded to public XNU 12377.1.9 and the supplied TXM 182.0 binary, which predates the live iOS 27 monitor.

Keep runtime binaries, signed outputs, logs, and experiment scripts under ignored `tmp/`. They are not repository assets.
The repository's [license explanation](../README.md#license) covers the project sources, not separately supplied Apple runtimes or SDKs.

When recording a new result, include the runtime build, signing identity/identifier, cache base and UUID, loaded-image source,
actual VM protections, exit status, and whether a debugger was attached. Keep unsuccessful experiments separate from confirmed behavior.
