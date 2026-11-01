# Native process names and argument storage

Every extension still executes the signed `LiveProcess` image. After authenticated broker check-in, a simulator guest writes `LSB:<logical PID> <basename>`
into the original native `argv[0]` allocation, for example `LSB:1234 SpringBoard`. The portable 8 MiB-stack build can subsequently lose that
argument window when the guest cache replaces it; the optional preservation build below keeps it on tested devices. Native `ps`'s PID column remains the genuine native PID,
which can differ from the logical PID in the name. An exec replacement retains the logical PID and uses its replacement executable's basename. Guest argument
and environment vectors, signed executable path, audit tokens and broker identity are separate and remain unchanged.

Native helpers use `LSBHelper: <helper name>` without a guest PID: `Metal.backboardd`, `Metal.SpringBoard`, `Metal.public`, `Metal.probe`,
and `Signer`. Replacement SpringBoard helpers retain the same label. The native host itself is unchanged. Both the native argv capacity
and the independent 32-byte kernel long-name limit preserve the complete `LSB:<PID> ` or `LSBHelper: ` prefix; only the basename is
truncated when necessary. An argv buffer too short for the complete prefix, one name byte and its terminator returns `ENOSPC` without
changing either name. `kern.procname` takes bytes excluding the terminator and adds its own NUL.

`Tests/ProcessDisplayNameTests.c` checks all native helper labels, unchanged guest labels, independent argv/kernel limits, overlapping input,
untouched surrounding arguments, zero argv capacity, and preserved errno when the kernel update fails. The helper-prefix change passed
focused tests normally and under ASan/UBSan. The iPhone 15 verification below separately establishes the active helper names.

`kern.procname` is best effort: failure is logged without preventing launch. Native `p_comm` remains `LiveProcess`, so `ps -o ucomm` still shows
the native executable. With argument preservation enabled and validated, use `ps -ww -axo pid,pcpu,rss,command` for the name and both PID namespaces. This is diagnostic metadata, not a complete
copy of guest argv. Never select a process for a privileged operation by parsing this label instead of validating its actual identity.

## Preserve native arguments with LC_MAIN

`LSB_PRESERVE_NATIVE_ARGUMENTS=NO` is the portable default in both Xcode configurations and the build-script fallback. It keeps the linked
8 MiB initial stack and the existing guarded simulator-stack relocation. Native `ps` argument names may again show cache bytes when the
original argument window overlaps the guest cache; guest argv and the broker's logical/native PID records remain separate and correct.

The older low-cache development iPhone 15 experiment enabled preservation with `LSB_PRESERVE_NATIVE_ARGUMENTS=YES`.
Do not carry that override into the automatic-cache setup: the 2026-11-01 retest selected cache base `0x300000000` and the
guarded handoff rejected the preserved stack overlap with `EADDRINUSE`. The default `NO` layout avoids that reservation.
The pre-signing `Tools/prepare-native-stack.py` phase changes
only `LC_MAIN.stacksize` from the linked 8 MiB to `0x90000000` (2.25 GiB). Segment addresses, extents, chained fixups and entry point stay unchanged.
This initial stack cannot fit at arm64 XNU's ordinary stack address without overlapping PAGEZERO/the executable. `create_unix_stack` therefore
uses its allocation fallback and records the new address in `p->user_stack`. Native `KERN_PROCARGS2` now reads the preserved argument window there.
The linker limits its stack-size option to 512 MiB; the checked pre-signing tool supplies the larger value accepted by the inspected kernel.

Before guest handoff, the loader validates that the current frame is in the upper 8 MiB and that this usable stack plus its guard lies outside
the entire simulator cache reservation. It protects one page below that usable range and publishes 8 MiB through the guest `main_stack` apple
entry. It keeps the original kernel stack top and native pthread metadata intact, including for failure returns before handoff. If the kernel
placed this argument window inside the cache, startup fails rather than overwriting it.

The lower allocation remains demand-zero virtual address space; it is not a 2.25 GiB resident allocation. Every LiveProcess role, including
native Metal helpers, has the larger initial virtual reservation. Native helpers retain their native stack bounds. The simple Mac probe used
approximately 1.6–1.7 MiB RSS; this is not a measurement of a simulator app's total memory cost. Different devices may have different VA limits.
Do not enable the larger reservation for an untested destination merely because it works on the Mac or another phone.

On 2026-10-31, iPhone18,2 / native iOS 27.0 build 24A435 rejected the 2.25 GiB-stack extension before `main`, with RunningBoard
reporting `posix_spawn` `ENOMEM`. Restoring the same package's 8 MiB stack allowed startup. This isolates the larger initial
reservation as a launch blocker on that destination; it does not establish the precise kernel allocation limit. The 8 MiB package
then booted the graphical simulator with native-cache retention. Artifacts are under ignored `tmp/lazy-sign-20261031/`.

## Why guest names became instructions

The kernel's `KERN_PROCARGS2` implementation reads the saved native argument window beneath `p->user_stack`. Moving execution to a new stack,
copying guest arguments, or updating libSystem's argv pointers does not change that kernel address. The full simulator cache overwrote the old
native stack after the handoff. Read-only samples from native launchd PID 12197 and Contacts PID 12247 returned ARM64 instructions as argv strings.
Those bytes match the signed cache at VM addresses `0x16b6f3b08` and `0x16bd13b08`, respectively.

The evidence is in `tmp/process-arguments/procargs-before.json`; it contains only a short prefix, not full process environments. Kernel source:
[sysctl_procargsx](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_sysctl.c) and
[create_unix_stack](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_exec.c#L6680).

## Current limits: 2026-11-01

The regression is confirmed with the portable 8 MiB build: bounded native reads show valid helper argv labels but cache
bytes for simulator guests. The capture is `tmp/process-name-fix-20261101/native-names15.json`; only argv[0] prefixes were
read, not process environments. The broker's logical/native PID records and guest executable identities remain intact.

The inspected XNU exposes no supported runtime setter for the saved argument window. `KERN_PROCARGS2` is read-only;
`proc_setcontrol` has no argv/stack flavor, and `task_set_info` provides no usable setter. Moving SP or changing userspace
argv pointers cannot retarget `p->user_stack`. `kern.procname` changes the separate kernel `p_name`, while Apple's
`ps command` and `ps comm` read KERN_PROCARGS2; `ps ucomm` reads `p_comm`. Thus even a successful kernel-name update does
not repair those argument columns. The current capture still reports `LiveProcess` in p_name, and native device logs show
`Sandbox: LiveProcess(...) deny(1) sysctl-read kern.procname`. The formatter already uses a numeric MIB with only a new value;
the log's `sysctl-read` classification does not establish that it issued a read. Numeric and named sysctls both pass through
the MAC policy check. `setprogname` uses the same setter; a host-side call would rename the host, not a selected child.
See [XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_sysctl.c),
[process control](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/proc_info.c),
[task_set_info](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/kern/task.c), and
[Apple ps](https://github.com/apple-oss-distributions/adv_cmds/blob/main/ps/print.c).

Replacing LC_MAIN with LC_UNIXTHREAD is not a validated alternative. On the arm64 Mac, an ad-hoc-signed disposable
LC_MAIN control reached main and exited 0; its LC_UNIXTHREAD variant aborted before main with “main executable has no
entry point.” Pinned dyld also restricts its older CRT startup support to macOS x86_64. This did not test iPhone acceptance
or a custom stack placement. Evidence: `tmp/process-name-fix-20261101/unixthread-research.md`;
[dyld build definitions](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/common/Defines.h)
and [startup preparation](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/dyldMain.cpp).

There is currently **no validated portable fix for unmodified native ps with this cache layout**. A future cache layout
that leaves the native argument pages intact, or a supported initial-stack placement accepted on every target, would
need separate mapping and startup validation. Neither is implemented. Broker-backed or guest process viewers can use
the existing identity records without depending on those pages; that does not change unmodified native ps.

### Native helper verification

The helper-prefix build was installed on iPhone16,2, native iOS 26.1 build 23B50, with guest iOS Simulator 26.0 build 23A343.
Both stack experiments remained disabled. Host UUID is `EB949FCB-FFD6-318C-BAE9-4EB0C3E8EE7D`; LiveProcess UUID is
`8E277FCD-74E6-31F0-80CA-4C9447257222`. The guest shim and cache were not replaced; the cache header still reports base
`0x110000000` and UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`. No checksum pass or new mapping-protection measurement was performed.

After foreground launch, native `ps -ww -o pid,state,command` and bounded KERN_PROCARGS2 reads showed `LSBHelper: Signer`,
`LSBHelper: Metal.backboardd`, `LSBHelper: Metal.SpringBoard`, and `LSBHelper: Metal.public`. The `Metal.probe` label has local
formatter coverage but that optional role was not launched in this device check. Kernel `p_name`/`p_comm` still read
`LiveProcess`, and guest argv names remain affected by cache overlap. No debugger was used; all 76 sampled extensions were untraced.
The signed native build, signature verification, focused formatter tests and `git diff --check` passed. Evidence is under
ignored `tmp/process-name-fix-20261101/`, including `helper-names15.json`, `helper-ps15.txt`, and `procname-log15.txt`.

## Disabled experiment: preserve the kernel's initial stack

`LiveProcess` retains ordinary `LC_MAIN` startup and an 8 MiB stack request. Its assembly source contributes a 16 KiB zero-fill placeholder
`__LSB_STACK` segment with no access permissions. Normal builds keep only that linked placeholder and treat it as an ordinary resident segment.
`LSB_ENABLE_NATIVE_STACK_RESERVATION` is `NO` in both Xcode configurations. Testing this old experiment also requires
`LSB_PRESERVE_NATIVE_ARGUMENTS=NO`; the modes are mutually exclusive. Only an explicit `YES` passes `--enable` to the pre-signing
`Tools/prepare-native-stack.py` phase, expanding the segment to `0x10b000000–0x170000000` and updating its private symbol. Disabling it again restores
the linked placeholder, including on incremental builds. Segment count, chained fixups, entry point, file size and low `__LINKEDIT` remain intact.
The normal signing step covers either layout.

On the supported arm64 layout, XNU slides the executable upward by less than 80 MiB and the default stack downward from `0x16fe00000`.
The inaccessible segment covers that entire stack window, causing `create_unix_stack` to use its normal allocation fallback and record the new
address in the kernel. The segment ends below `0x175000000`, leaving the native shared region at `0x180000000` available during startup.

The guard leaves 176 MiB between the native image's slid base and its own beginning. The 8 MiB stack and roughly 36 MiB native image leave about 132 MiB in the current debug build;
this is sufficient for the current `dyld_sim` and Fish images. At most 80 MiB of unused guard can remain below the fixed simulator cache
base during native preparation. These are inaccessible virtual reservations, not initialized file bytes or a physical-memory allocation of that size.
After the final native worker check, the loader explicitly deallocates only this validated empty segment before cache handoff. The kernel-selected
argument stack is separately checked against both the simulator cache and the entire segment, and remains mapped. A deallocation error aborts
before TLS/cache detachment and reports the Mach error and exact extent.

Only the native loader image's exact prepared, file-empty, inaccessible segment is exempted from cache collision checks. A simulator image
cannot obtain that exemption by reusing its name. If the kernel fallback stack still overlaps the selected simulator cache, handoff fails before
overwriting arguments. This deliberately fails rather than relying on undocumented updates to kernel process structures or modifying cache text.

The kernel's native argc remains the original extension argc. The readable native name identifies the guest; it is not a complete mirror of the
guest's argument list. Guest programs continue to receive their actual full argv and environment from the broker.

## Validation

`Tests/NativeStackReservationTests.py` builds and signs a disposable arm64 executable with modern chained fixups, validates malformed layouts and
all possible ASLR pages, then launches five fresh processes. Each checks the kernel and pthread stack addresses, the native `KERN_PROCARGS2`
name, and unchanged adjacent arguments/environment. Loader tests cover the narrow exemption, malformed reservations, ordinary code collisions,
and refusal of a fallback stack inside the cache. Broker tests check all three native helper labels and the replacement-helper label.

Local tests cover default-off layout, on/off incremental transitions, malformed layouts, and releasing/reusing the exact experimental guard
while preserving kernel argv in five signed native probe launches.

The iPhone 17 test preserved the native argument stack at top `0x105150000`, size 8 MiB, but the subsequent anonymous cache reservation failed.
Explicitly deallocating the exact guard succeeded; cache `mmap` still returned `EACCES` (13) at `0x110000000`, size `0x7e194000`.
The previous package without the wide reservation successfully ran `df` with the required runtime environment. These results establish a regression
associated with the wide native layout, but do not establish the kernel/TXM mechanism. The experiment remains off; no TXM state manipulation is used.
Evidence: `tmp/migration-iphone17/df-console.log`, `df-stack-release-console.log`, and the fallback test logs in that directory.

Reservation failures log numeric errno, address and size through app-local code even after cache detachment. No native libc formatting is used
at that point. That historical package retained helper names and legacy simulator stack relocation. The LC_MAIN approach above supersedes it
without enabling the wide segment. The exact cause of the old wide-segment cache mapping failure remains unproven.

## Initial device validation: 2026-10-30

These captures predate the requested `LSB:<PID> <basename>` format. They preserve the actual earlier suffix labels and validate the stack placement;
the prefix format's device verification is recorded below.

The default LC_MAIN placement and PID labels were tested on iPhone16,2, native iOS 26.1 build 23B50, with guest iOS Simulator 26.0 build 23A343.
The unchanged cache is based at `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`; this check read its header, without remeasuring
mapping protections or performing a checksum pass. Host UUID is `4BD4748F-38E1-3191-9E0E-19BB573B5A56` and final LiveProcess UUID is
`903CD17E-CD40-38E2-8A0F-FBEB844BAE42`, signed with the existing development identity and identifiers. The guest shim/cache were not replaced.

The first boot visibly reached SpringBoard and returned valid kernel argv for 28 simulator guests plus three native Metal services. The final
build repeated those 28 guest identities with host PID 9238. Native `ps` showed, for example:

```text
native PID   command
9250         launchd_sim [gpid=9250]
9279         SpringBoard [gpid=9260]
9293         fish [gpid=9269]
9310         WidgetRenderer_Default [gpid=9309]
9248         Metal.SpringBoard
```

Every sampled guest suffix matched the broker's committed identity, including xpcproxy/SETEXEC replacements. Fish's builtin `$fish_pid`
reported its logical identity. Invoking the physical `$DYLD_ROOT_PATH/bin/df -h` spawned native/logical PID 9331 and returned status 0;
the earlier virtual `/bin/df` spelling was rejected by Fish with status 127 before spawning. Native `p_comm`/`proc_name` readers can still report `LiveProcess`;
the demonstrated readable names are the native argument/command field. No LLDB was attached. Thermal pressure was 0 at the final snapshot.
The full local suite, focused loader guard tests, signed native-stack probes, native build and separate guest-shim build passed.

Artifacts and bounded captures are under ignored `tmp/process-names-20261030/`, with the final package/evidence under `final/`.
At the time of that capture, the new LC_MAIN approach had not been tested on iPhone 17. Its subsequent pre-main launch failure is
recorded above and makes preservation opt-in. The earlier iPhone 17 failure was from the different wide-segment experiment,
which remains disabled. Widget rendering was a separate unresolved issue in that capture.

The subsequent prefix-format build, LiveProcess UUID `6CA5C6B9-0789-3AEA-A24B-B94B56C764BC`, was installed on the same device with unchanged
host, guest shim and cache. Native `ps` reported `9384 LSB:9365 SpringBoard`, `9398 LSB:9374 fish`, and `9356 LSB:9356 launchd_sim`.
All 26 sampled guest prefixes matched the broker's logical/native identity records; the three native Metal labels remained distinct.
Formatter/alias tests, ten signed native stack probes, loader tests and the native build passed. No debugger was attached.
Captures and the signed package are under ignored `tmp/process-names-20261030/prefix/`.
