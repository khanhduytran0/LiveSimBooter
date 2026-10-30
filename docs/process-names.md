# Native process names and argument storage

Every extension still executes the signed `LiveProcess` image. After its authenticated broker check-in, it writes `LSB:<logical PID> <basename>`
into the original native `argv[0]` allocation, for example `LSB:1234 SpringBoard`. Native `ps`'s PID column remains the genuine native PID,
which can differ from the logical PID in the name. An exec replacement retains the logical PID and uses its replacement executable's basename. Guest argument
and environment vectors, signed executable path, audit tokens and broker identity are separate and remain unchanged.

Native Metal helpers use `Metal.backboardd`, `Metal.SpringBoard`, and `Metal.public` without a guest-PID prefix. Replacement SpringBoard helpers
retain the same label. Both the native argv capacity and the independent 32-byte kernel long-name limit preserve the complete `LSB:<PID> ` prefix;
only the basename is truncated when necessary. `kern.procname` takes bytes excluding the terminator and adds its own NUL.

`kern.procname` is best effort: failure is logged without preventing launch. Native `p_comm` remains `LiveProcess`, so `ps -o ucomm` still shows
the native executable. Use `ps -ww -axo pid,pcpu,rss,command` for the name and both PID namespaces. This is diagnostic metadata, not a complete
copy of guest argv. Never select a process for a privileged operation by parsing this label instead of validating its actual identity.

## Preserve native arguments with LC_MAIN

`LSB_PRESERVE_NATIVE_ARGUMENTS=YES` is the default in both Xcode configurations. The pre-signing `Tools/prepare-native-stack.py` phase changes
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
Set `LSB_PRESERVE_NATIVE_ARGUMENTS=NO` only for a diagnostic comparison with the old argument-overwrite behavior.

## Why guest names became instructions

The kernel's `KERN_PROCARGS2` implementation reads the saved native argument window beneath `p->user_stack`. Moving execution to a new stack,
copying guest arguments, or updating libSystem's argv pointers does not change that kernel address. The full simulator cache overwrote the old
native stack after the handoff. Read-only samples from native launchd PID 12197 and Contacts PID 12247 returned ARM64 instructions as argv strings.
Those bytes match the signed cache at VM addresses `0x16b6f3b08` and `0x16bd13b08`, respectively.

The evidence is in `tmp/process-arguments/procargs-before.json`; it contains only a short prefix, not full process environments. Kernel source:
[sysctl_procargsx](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_sysctl.c) and
[create_unix_stack](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_exec.c#L6680).

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
The new LC_MAIN approach has **not yet been tested on iPhone 17 or another production device**. The earlier iPhone 17 failure was from the
different wide-segment experiment, which remains disabled. Widget rendering itself remains a separate unresolved issue.

The subsequent prefix-format build, LiveProcess UUID `6CA5C6B9-0789-3AEA-A24B-B94B56C764BC`, was installed on the same device with unchanged
host, guest shim and cache. Native `ps` reported `9384 LSB:9365 SpringBoard`, `9398 LSB:9374 fish`, and `9356 LSB:9356 launchd_sim`.
All 26 sampled guest prefixes matched the broker's logical/native identity records; the three native Metal labels remained distinct.
Formatter/alias tests, ten signed native stack probes, loader tests and the native build passed. No debugger was attached.
Captures and the signed package are under ignored `tmp/process-names-20261030/prefix/`.
