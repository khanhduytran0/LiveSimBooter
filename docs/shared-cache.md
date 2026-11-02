# Signed simulator shared cache

## Low image allocation and the 0x110000000 rebuild

The 2026-10-26 full-cache Fish trial exposed a remaining address-space limit. One native LiveProcess started at
`0x106c18000`; its low image allocator could not find a contiguous `0xa8c000`-byte range before the cache at `0x108000000`.
The native image, other surviving allocations, dyld_sim and Fish must share that small remaining window. Retrying an
unconstrained allocation could place executable code inside the future cache reservation rather than solve the limit.

[`DyldSimLoader.cpp`](../LiveSimBooter/DyldSimLoader.cpp) now provides root-aware mapping APIs used by LiveProcess for both
dyld_sim and the guest executable. They read the cache reservation before mapping either image and release the temporary
whole-file parse view before reserving image memory. The layout keeps owned copies of its segment commands; the file
descriptor remains open for signature registration and segment mapping.

With a cache present, the allocator scans occupied VM regions between the native main-image/preferred-image lower bound
and `min(cache base, 0x180000000)`. It requests fixed reservations **without `VM_FLAGS_OVERWRITE`**, skips insufficient gaps,
and bounds retries if another mapping wins a race. Segment `MAP_FIXED` operations replace only pages inside the reservation
it owns. No fitting gap is an explicit failure; executable images are not moved into the high shared-region interval.
The original mapping APIs and roots without a cache retain the loose-library behavior. Handoff checks remain and now name
native-loader, dyld_sim, guest-executable or static-storage collisions, including addresses.

The apparent gap below a PIE native executable is not available memory. Apple's
[`mach_loader.c`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/mach_loader.c) extends PAGEZERO by ASLR
and requires at least 4 GiB of hard PAGEZERO for a 64-bit ARM64 process. A 64 KiB PAGEZERO would therefore fail validation.
[`vm_map_raise_min_offset`](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/vm/vm_map.c) refuses to lower that
minimum, and [`task_set_info`](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task.c) exposes no
TASK_VM_INFO setter. The native-header lower bound also avoids treating this unreported prefix as a free VM gap.

The focused loader suite passed 1,164 checks, plus its short/large terminal handoff fixtures. Tests cover bounds and
overflow, occupied and undersized gaps, raced allocations, retry exhaustion, source-map release and source-FD cleanup.
A real kernel allocation test verifies that even a deliberately stale free-gap query cannot overwrite existing bytes.
Both changed production sources passed iPhoneOS syntax checks. Evidence:
[`loader-low-reservation-tests.log`](../tmp/app-launch-fix/workspace-shm/loader-low-reservation-tests.log).

### Offline cache and signing validation

An isolated rebuild moves the same **1,353-image 23A343 closure** up by `0x08000000`, to **`0x110000000`**. This leaves
256 MiB above the 4 GiB floor instead of 128 MiB. The build reused immutable staged originals through hardlinks and took
9.68 seconds. The original builder and prior signed `0x108000000` cache remain preserved.

| Mapping | Start | End | Initial / maximum protection |
| --- | --- | --- | --- |
| TEXT | `0x110000000` | `0x16c594000` | `r-x` / `r-x` |
| DATA_CONST | `0x16c594000` | `0x176018000` | `r--` / `rw-` |
| TPRO_CONST | `0x178018000` | `0x17801c000` | `r--` / `rw-` |
| DATA | `0x17801c000` | `0x17c6c8000` | `rw-` / `rw-` |
| Read-only metadata | `0x17e6c8000` | `0x1824fc000` | `r--` / `r--` |
| LINKEDIT | `0x1824fc000` | `0x18e190000` | `r--` / `r--` |

The reservation ends at `0x18e194000`. Every executable and potentially writable mapping stays below `0x180000000`;
only read-only metadata and LINKEDIT cross it. All expected images, export tries and function-start tables remain present.
Comparison with the old cache verified 5,366 segments, 41,237 sections, 803,819 cache pointer slots, 672 unchanged
non-cache pointer slots and 821,761 section symbols, in addition to mapping and image records.

The builder uses the same four changes documented below, except file offset `0x5ea60` now contains `0xb2640108`
(`orr x8, x8, #0x10000000`). The locally-built and optimize-for-size settings are unchanged. **The deployed dyld_sim stays
unchanged:** its existing three cache instructions read the mapping base and cleanup span from the header. All eleven
platform compatibility instructions remain intact; a new hardcoded dyld_sim base is unnecessary.

The existing cache signer completed CMS verification, `codesign` verification and all **125,028** contiguous 16 KiB page
hashes. The signed file is **2,064,482,304 bytes**, SHA-256
`ad3b1eb360f5f1883d8eb49efec351116084e26b1009b73b521e91c2acbebd09`, CDHash
`2bb599f972ee3c49db0417a94a6aea83877a216d`. It retains CodeDirectory v`0x20400`, zero scatter/exec-segment flags, no
entitlements, and the existing app identity/identifier/team. Only signature capacity differs from the unsigned payload.

Reproduction and evidence are in [`full-cache-110-strip/NOTES.md`](../tmp/springboard-stage/full-cache-110-strip/NOTES.md),
[`validation.json`](../tmp/springboard-stage/full-cache-110-strip/validation.json),
[`rebase-comparison.json`](../tmp/springboard-stage/full-cache-110-strip/rebase-comparison.json) and
[`sign-result.json`](../tmp/springboard-stage/full-cache-110-strip/sign-result.json).
The redundant unsigned cache was removed after those checks
([cleanup receipt](../tmp/springboard-stage/full-cache-110-strip/unsigned-cache-cleanup.json)); the signed replacement,
original rollback cache, input tree, scripts and manifests are retained.

### Device boot with the rebuilt cache

The signed cache was then activated on the iPhone. Native host PID 10621 started simulator launchd 10625, SpringBoard
10645 and backboardd 10646. The launchd-owned Fish job reached its prompt promptly as native PID 10656, without the
previous image preparation failure. The loader reported cache reservation `0x110000000–0x18e194000`.

Evidence is [`console-110.log`](../tmp/terminal-launchd-tests/console-110.log). The bounded
[amfid/TXM capture](../tmp/terminal-launchd-tests/cache-110-amfid-txm.log) contained no identified rejection for this trial.
This confirms boot and Fish initialization with the new cache. The mapping table above remains an offline inspection of
the signed cache layout; this trial did not independently measure every live VM region or its protection under LLDB.

## Verified result

The completed 2026-10-24 test used a matching iOS 26 build **23A343** `dyld_sim`, `df`, and library set on the test iPhone.
The builder produced a single arm64 cache containing 219 dylibs. The rebuilt, signed file is 292,749,312 bytes.

| Property | Verified value |
| --- | --- |
| Cache base / slide | `0x120000000` / `0` |
| Full declared span | `[0x120000000, 0x135508000)` |
| Executable mapping | `[0x120000000, 0x12b2f4000)`, 187,645,952 bytes, actual `r-x` |
| Cache UUID | `CCB560A4-7593-3E93-BE1F-2AFDB35D6BA5` |
| CodeDirectory | v`0x20400`, SHA-256, 16 KiB pages, no scatter table |
| Signing identifier / team | `com.kdt.LiveSimBooter` / `UU7W52U79B` |
| Entitlements / executable-segment flags | None / `0` |
| Cached initializer reached | `libSystem_initializer` at `0x129d4ff78` |
| Cached images in cache-only run | All 46 dependencies; no loose dylibs remained in RuntimeRoot during the test |
| Debugger / exit | 49 automatically recognized modules; `df` exited `0` with and without LLDB |

Native dyld code at `0x18ff9a000` remained inaccessible after detachment. No extra `mprotect` call was needed to make the
cache executable. The initial run used two cached dylibs and loose overrides for the other dependencies. In the subsequent
cache-only test, all 219 loose dylibs were temporarily moved outside RuntimeRoot and zero remaining loose dylibs were verified.
All 46 loaded dependencies then had `MH_DYLIB_IN_CACHE` and addresses inside the relocated cache mappings. The image array
contained 48 entries (host app, `df`, and 46 dependencies); dyld_sim is published separately, giving 49 LLDB modules.

LLDB reached cached `libSystem_initializer` at `0x129d4ff78`, `__pthread_init` at `0x12b2688d8`, and `getmntinfo` at
`0x12b09e984`. `df` printed the filesystem table and exited `0` under LLDB (PID 3934) and in a fresh normal launch.
AMFI accepted the cache profile, with no signature rejections in either capture.

One `TXM selector: 38 | 23` event appeared during the LLDB run at `12:20:51.144`. A later three-run comparison captured
LLDB breakpoint packets: its internal thread-creation software breakpoints dirtied exactly the two observed pthread pages.
Requiring hardware breakpoints globally kept cache TEXT clean. All three traced runs exited `0` without reproducing the TXM
event, including a default-setting replay through the original later stops. The dirty-page cause is established; the TXM
event's cause remains unproven. See [the debugger investigation](debugging.md#internal-breakpoint-investigation).
The normal-launch capture also contained no TXM error. All 219 loose dylibs were restored and verified after testing; the
relocated cache remains active, so later launches can again use loose overrides.

## Signing and mapping are separate checks

Ordinary embedded `codesign` signing passes the AMFI profile check but creates a scatter table rejected by this device's TXM.
The logged signature error is `selector: 24 | 0x70 | 0x28 | 1`. In the supplied TXM 182.0 binary,
`codeDirectoryValidate` returns that error when `CodeDirectory.scatterOffset` is nonzero.

Removing scatter and generating a fresh signature with the same legitimate identity allowed `F_ADDFILESIGS_RETURN`
to succeed. Plain detached signing avoids scatter but emits v`0x20200`, which the device rejected as an unsupported
signature version. The working format retains v`0x20400` and the original executable-segment metadata.

The DSC writer also leaves `codeSignatureSize` stale when its signature grows. This header field must describe the reserved
signature region before the final page hashes are made. [The signer](../Tools/sign-dyld-cache.py) obtains ordinary codesign
metadata, reserves space, fixes the header, hashes contiguous 16 KiB pages, and generates fresh CMS through
[Security.framework](../Tools/sign-code-directory.m). It uses the keychain identity without exporting its private key.
It verifies the CMS, code-page and requirements hashes, final codesign result, and unchanged payload apart from signature size.
It does not add entitlements, alter device policy, relocate the cache, or set executable-segment privileges.

Signature acceptance did not make the stock-base cache executable. At `0x180000000`, its TEXT mapping returned successfully
but had actual `r--` permissions, followed by an execution fault and `TXM selector: 38 | 23`.
The supplied TXM binary identifies `23` as a code-region overlap. Its nested-address-space handler registers the full nested
interval in the parent code-region tree; ordinary VM unmapping does not remove that registration. The binary predates the
device OS, but both selector numbers match the observed operations. A fresh executable mapping outside that interval worked.

## Original-base hardware-only retest

A later controlled test on 2026-10-24 (PID 5350) restored the signed original-base cache temporarily, keeping the same patched
`dyld_sim` that reads the base from its header. The cache UUID was `B901DB5B-C4DF-3546-B213-B9385C297907`, with CDHash
`df9f80def31f0d67f830cdcfb22b8c931048dd3c`, v`0x20400`, no scatter table, and 16 KiB pages. All 219 loose dylibs were moved
outside RuntimeRoot, and a separate inventory confirmed zero remained during the test.

LLDB required hardware breakpoints globally before target creation/attach. Its packet log contained zero `Z0`/`z0` requests;
all 30 hardware insertions and 30 removals succeeded. No memory-write packets targeted cache TEXT. The host-cache span was
unmapped before the test mapping, including the complete proposed range `[0x180000000, 0x195508000)`.

`F_ADDFILESIGS_RETURN` returned `0`. The subsequent TEXT `mmap` requested `r-x` and returned `0x180000000`, but both endpoints
of `[0x180000000, 0x18b2f4000)` had actual `r--` permissions and zero dirty pages. The SSH capture recorded
`TXM selector: 38 | 23` at `12:43:52.272 +0700`, without an AMFI signature rejection.

Continuing stopped with `EXC_BAD_ACCESS (code=2)` at `0x180047e78`, the first instruction of cached
`libdyld.dylib`'s `dyld4::LibSystemHelpers::version() const` (`mov w0, #7; ret`). This happened during `dyld4::prepare`, before
cached-image publication or `libSystem_initializer`; LLDB still listed only the host app, dyld_sim, and df. The cached pthread
hooks had never been inserted. The fault address matched FAR, with ESR `0x8200000f`.

This reproduces the original-base execute-permission failure without software breakpoints or cache code writes. The isolated
TXM event in the earlier relocated-cache test remains unexplained. After recording the fault, the debugger terminated the
process. Both cache files were renamed back, with the same recorded inodes. A fresh cache-only launch using the restored
`0x120000000` cache ran df to exit `0`, without TXM errors or AMFI rejections. All 219 loose dylibs were then restored and
independently verified. The working dyld_sim was unchanged throughout.

Evidence is summarized in `tmp/dsc-original-hardware/result.json`, with LLDB/packet/security logs, the pre-map region check,
cache swap/restoration records, signature readback, and the successful restored launch. These artifacts are ignored by Git.

## Global registration and trust-cache requirements

Source review on 2026-10-24 found no supported way for this ordinary jailed app to publish its Documents cache as the native
shared-region cache. This was a source/binary investigation; no system-wide registration was attempted.

The simulator explicitly sets `opts.forcePrivate = true` in
[DyldProcessConfig.cpp](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/DyldProcessConfig.cpp#L1360).
The same revision excludes both `mapSplitCacheSystemWide` and the non-private branch of `loadDyldCache` from simulator builds
with `!TARGET_OS_SIMULATOR` guards. Clearing that option alone therefore does not enable global loading; it would require
an added loader path and a way for dyld_sim to adopt the resulting mapping.

Native dyld's global path calls `__shared_region_map_and_slide_2_np`. In XNU 12377.1.9, it populates the task's existing
exec-selected shared region rather than allocating a new arbitrary region for the caller. An already initialized shared
region is rejected by `vm_shared_region_map_file`. Our detach call, `__shared_region_check_np(NULL)`, also clears the task's
shared-region reference; a later global mapping request would find no shared region and return `EINVAL` through that path.
See [the syscall setup](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/vm/vm_unix.c) and
[the region mapper](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_shared_region.c).

The syscall setup additionally checks MAC/AMFI policy, root ownership of each cache file, and code-signature coverage. On
non-SIP builds it requires the root volume or preboot-cryptex volume. The tested file is owned by `mobile` in the app's Data
volume container. The trust-cache exception that skips root ownership in this source is specifically for macOS arm64;
it is not an iOS rule. Exact-directory enforcement defaults off for iOS in this source, so directory naming alone is not
claimed as a blocker. These source checks describe the examined release, rather than a live probe of every iOS 27 policy.

The supplied TXM 182.0 binary has an additional trust-class check for nested/shared address-space associations:
`associateCodeSignature` at `0xfffffff017022d08` checks address-space type `1`, then rejects signature trust levels `<= 5`.
Its numeric trust classes are not named by the supplied public headers, and the binary predates the test phone's OS. This
supports an additional system-trust requirement; it does not establish that trust-cache membership is the only acceptable
trust source. The ordinary private mapping already executed the developer-signed cache at `0x120000000` without us loading
any trust cache.

`F_ADDFILESIGS_RETURN` verifies and attaches a file signature to its vnode; it does not insert a hash into a trust cache.
Trust-cache loading is separate. XNU's `load_trust_cache_with_type` requires an IMG4 payload/manifest, applies type-specific
private entitlement checks, and authenticates the trust-cache object. The app's development signing identity does not grant
that authority. See [file signature registration](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_descrip.c)
and [trust-cache loading](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_trustcache.c).

## Unregistering the retained host shared region

Source and binary review on 2026-10-24 found no app-callable operation that removes only the host shared-region association
from TXM while preserving the current address space. This was a read-only investigation, not a new device experiment.
The distinction is between removing VM translations and retiring the monitor's address-space object.

In XNU 12377.1.9, our `__shared_region_check_np(NULL)` path calls `vm_shared_region_remove()` and then clears
`task->shared_region`. The removal replaces the region with a fixed anonymous mapping whose current and maximum protections
are `VM_PROT_NONE`. Removing the original submap reaches `pmap_unnest_options_internal()` and `sptm_unnest_region()`, but
does not clear `grand->nested_pmap` or its recorded interval. See
[the detach syscall](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/vm/vm_unix.c#L1031-L1034),
[VM removal](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_shared_region.c#L2624-L2660), and
[pmap unnesting](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/arm64/sptm/pmap/pmap.c#L9248-L9400).

The association was established earlier, during exec: `vm_shared_region_insert_placeholder()` calls
`pmap_set_shared_region()` before userspace dyld maps the cache. The latter retains the nested pmap, installs it with a
one-time compare-exchange from NULL, records the full region, and calls `sptm_set_shared_region()`. Calling it again is not
a reset: an existing nested pmap triggers a panic. See
[placeholder setup](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_shared_region.c#L2524-L2539) and
[association setup](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/arm64/sptm/pmap/pmap.c#L8995-L9087).

The only call to `txm_unregister_address_space()` found in this XNU source is in `pmap_destroy_internal()`, after the pmap's
reference count reaches zero. It operates on the entire pmap's TXM object, then clears that object's pointer from the pmap.
The nested pmap reference is released during the same final destruction. This requires kernel objects and the kernel's
monitor-call machinery; the TXM selector is not a BSD syscall number or a user Mach operation. See
[the destruction caller](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/arm64/sptm/pmap/pmap.c#L2683-L2693) and
[TXM unregister](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/code_signing/txm.c#L1188-L1213).

The supplied `txm.iphoneos.release`, source version **182.0**, independently supports this lifetime:

| Kernel selector | Handler | Effect |
| --- | --- | --- |
| 35 | `registerAddressSpaceWrapper` | Creates an address-space object. |
| 36 | `unregisterAddressSpace` | Deactivates the object, drains its entire code-region tree, and frees it. |
| 37 | `setupNestedAddressSpace` | Establishes the nested address-space configuration. |
| 38 | `associateCodeSignatureWrapper` | Associates executable code; overlap returns `23` in the examined path. |

`unregisterAddressSpace` at `0xfffffff017022344` also releases the parent's nested placeholder reference. It rejects
destruction of an object still used as a nested address space. A scan of all direct `BL` instructions in `__TEXT_EXEC`
found only one caller of `_TXMCodeRegionRBTree_RB_REMOVE`: `unregisterAddressSpace + 224`. Neither the kernel dispatcher
nor the SPTM dispatcher exposes a selective unnest or code-region removal operation in this binary. The SPTM
`resetAddressSpaceID` handler only clears the ID-table sentinel of an already destroyed object; it does not reset a live
address space. These are findings about this supplied binary, which predates the phone's iOS 27 monitor.

The alternative app paths examined do not provide a reset:

| Candidate | Result in the examined source |
| --- | --- |
| `munmap`, `mach_vm_deallocate`, or replacement mappings | Change VM mappings; do not destroy the active pmap or its TXM object. |
| `mprotect` | Changes permissions within VM limits; does not remove the monitor association. The live high mapping had no execute maximum. |
| Fork after cache detachment | `pmap_fork_nest()` inherits the retained nested pmap and interval into the child. |
| Exec or spawn | `vm_map_exec()` installs the shared-region placeholder for the new address space again. |
| Historical `shared_region_make_private_np` and related calls | Removed; old syscall slots are reused by pthread calls or reserved as `nosys`, depending on configuration. |
| `task_create` | Obsolete: its MIG kernel-server entry is skipped in this source. |

References: [fork inheritance](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/arm64/sptm/pmap/pmap.c#L9438-L9449),
[exec setup](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_map.c#L14138-L14140),
[syscall table](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/syscalls.master), and
[obsolete task creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/mach/task.defs#L94-L105).

Consequently, cache detachment alone does not make `[0x180000000, 0xfc0000000)` available for our private executable
associations. The working rebuilt cache below that interval remains the verified route. This review did not find an exposed
reset path; it does not prove the absence of every unpublished interface in the live OS.

Binary evidence is retained under ignored `tmp/dsc-test/txm/`, particularly `selector-lifetime.log`, `nesting.log`, and
`tree-selector.log`. The examined binary's SHA-256 is `1ef50f5ac034f9c3dd3d7dc96e273de58b8eb526230e488ac86f171d35509030`.

## Rebuild at the working base

Use original runtime files, not shared-cache extracts. This workflow reproduces the existing staged-library experiment;
`tmp/dsc-test/root` must contain the same iOS 26 source libraries. It is an ignored local artifact, not included in Git.
The builder automatically supplies the three macOS support libraries; do not add the separately patched device copies to its staging root.
Changing the input library set requires another layout and runtime validation.

Run from the repository root on the Mac. Use a fresh work directory because the patch tool refuses to overwrite files.

```sh
RUNTIME_RES='/Library/Developer/CoreSimulator/Volumes/iOS_23A343/Library/Developer/CoreSimulator/Profiles/Runtimes'
RUNTIME_RES="$RUNTIME_RES/iOS 26.0.simruntime/Contents/Resources"
STAGED_ROOT="$PWD/tmp/dsc-test/root"
WORK="$PWD/tmp/cache-base-tools"
IDENTITY=D7610EDC80D34A9B1A3D3C8563D66A5CD4716293

python3 Tools/patch-ios26-cache-base.py \
  --builder "$RUNTIME_RES/update_dyld_sim_shared_cache" \
  --dyld-sim "$RUNTIME_RES/RuntimeRoot/usr/lib/dyld_sim" --output-dir "$WORK"

codesign --force --sign - --timestamp=none "$WORK/update_dyld_sim_shared_cache"
mkdir -p "$WORK/unsigned" "$WORK/signed"
"$WORK/update_dyld_sim_shared_cache" -root "$STAGED_ROOT" -cache_dir "$WORK/unsigned" -iOS -arch arm64 -force -verbose

codesign --force --sign "$IDENTITY" --identifier com.kdt.LiveSimBooter --timestamp=none "$WORK/dyld_sim"
codesign --verify --verbose=4 "$WORK/dyld_sim"

python3 Tools/sign-dyld-cache.py "$WORK/unsigned/dyld_sim_shared_cache_arm64" "$WORK/signed/dyld_sim_shared_cache_arm64" \
  --identity "$IDENTITY" --identifier com.kdt.LiveSimBooter --team UU7W52U79B > "$WORK/sign-result.json"
```

The patched builder is only for arm64 simulator input. Its three instruction changes select `0x120000000` while preserving
the simulator's 4 GiB layout-size setting. The patched `dyld_sim` uses the first mapping's base and the cache header's
base/size for failure cleanup. The tool checks exact original and patched SHA-256 hashes and writes a patch manifest.
Both outputs initially have invalidated signatures; the commands above sign them separately.

The old executable-overlap heuristic remains in dyld_sim. The tested `df` intersects neither range and needs no override.
**Do not set `DYLD_SHARED_REGION=private` in the native app launch environment.** Native dyld sees it before `main()`;
that experiment failed before the simulator handoff. If a future simulator-only override is needed, add it after native startup.

This is a real rebuild, not a cache-header patch. The old/new comparison verified the exact `-0x60000000` address change
across all 219 image entries, 864 Mach-O segments, 4,564 sections, 765,301 section symbols, and 63,525 actual pointer slots.
Relative metadata and file layout remained consistent. The recorded check is `tmp/dsc-relocated/rebuild-verification.json`.
For every new build, verify the complete declared VM span is free before permitting fixed mappings; the working base is not
a general allocator or a guarantee for a different runtime.

## SpringBoard closure offline build

On 2026-10-26, an isolated matching **23A343** build combined the established 545-image cache set, SpringBoard's strong
dependency closure, Sleepd's strong closure and the currently loose supporting-daemon libraries. The result contains
**1,353 images**: 1,350 original runtime inputs and the three automatically supplied host support libraries. Every expected
image is present. The builder completed in 8.73 seconds. The layout and signature checks below are offline evidence;
the subsequent device trials are recorded separately. SpringBoard boot has not been established.

The new base is **`0x108000000`**. The measured mappings are:

| Region | Start | End | MiB | Initial / maximum protection |
| --- | --- | --- | ---: | --- |
| TEXT | `0x108000000` | `0x164594000` | 1477.578125 | `r-x` / `r-x` |
| DATA_CONST | `0x164594000` | `0x16e018000` | 154.515625 | `r--` / `rw-` |
| TPRO_CONST | `0x170018000` | `0x17001c000` | 0.015625 | `r--` / `rw-` |
| DATA | `0x17001c000` | `0x1746c8000` | 70.671875 | `rw-` / `rw-` |
| Read-only metadata | `0x1766c8000` | `0x17a4fc000` | 62.203125 | `r--` / `r--` |
| LINKEDIT | `0x17a4fc000` | `0x186190000` | 188.578125 | `r--` / `r--` |

The declared reservation ends at `0x186194000`, spanning **2017.578125 MiB**. All executable and writable mappings remain
below `0x180000000`; the highest instruction-marked section ends at `0x1645900c0`. Only read-only LINKEDIT crosses the
boundary, by approximately 97.58 MiB including the final reservation page. The earlier high-address execute-protection
failure therefore does not by itself reject this layout: the crossing region requires no execute permission. The device
trials below separately check mapping permissions and execution progress. The lower base leaves only
128 MiB above the arm64 executable's 4 GiB minimum for native images and early allocations; ASLR collisions remain a
separate question. Inspect the entire reservation before fixed mappings.

### Reproducing the measured builder configuration

The input builder is the original 23A343 runtime's `Contents/Resources/update_dyld_sim_shared_cache`, SHA-256
`79e721958b8d733bd943cc550dee7336b9fcafa2e90619f16c36dd2931f396a3`. A new temporary copy received these exact arm64 changes;
the source builder, runtime volume and existing cache were preserved:

| File offset | Original word | Replacement word | Effect |
| --- | --- | --- | --- |
| `0x5ea5c` | `0xb26107e8` | `0xd2c00028` | Start the base at `0x100000000`. |
| `0x5ea60` | `0xd2c00029` | `0xb2650108` | OR in `0x08000000`, selecting `0x108000000`. |
| `0x5ea64` | `0x9a881129` | `0xd2c00029` | Retain the simulator builder's 4 GiB layout limit. |
| `0x12b4` | `0x52800028` | `0x52802028` | Set the adjacent local-build and optimize-for-size bytes to `1,1`. |

The final change replaces `mov w8,#1` with `mov w8,#0x101`; the following halfword store writes
`BuildOptions_v3.isLocallyBuiltCache` and `optimizeForSize`. Apple's
[option routing](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/mrm_shared_cache_builder.cpp)
selects `LocalSymbolsMode::strip` when optimize-for-size is enabled. The simulator CLI normally disables it and keeps
locals. The local 23A343 instructions were independently disassembled to verify the field store and base selection;
the public source alone was not assumed to have identical instruction offsets.

The supported builder pass removes local nlist symbols while retaining runtime exports and metadata. The resulting cache
has zero local symbols, 821,761 globals and 501,761 undefined entries. All 1,353 images retain export tries and function
starts; 1,121 images retain ObjC classlist sections. Retained symbol strings occupy 41 MiB after deduplication. Matching
original images remain available on the Mac for symbolication; local names are no longer carried in the mapped cache.
There was no post-build deletion of runtime tables or manual relocation of the resulting cache.

The exact selected input paths and read-only staging methods are retained in
`tmp/springboard-stage/full-cache-108-strip/stage-manifest.json`. Preparation used
`tmp/springboard-stage/prepare-full-cache.py`; `run-full-cache.py` recorded this command against the prepared input tree:

```sh
WORK="$PWD/tmp/springboard-stage/full-cache-108-strip"
"$WORK/update_dyld_sim_shared_cache" -root "$WORK/root" -cache_dir "$WORK/unsigned" \
  -iOS -arch arm64 -force -verbose -time-passes -stats
```

Use a fresh output directory when recreating the experiment. The general `Tools/patch-ios26-cache-base.py` still targets
`0x120000000`; it was not changed by this local experiment. The temporary builder is ad-hoc signed for execution on the
Mac. Its 4 GiB build limit is not a guarantee of available device VA. The existing patched 23A343 dyld_sim already reads
the cache base and cleanup bounds from the header, so it requires no additional base-specific instruction change.

### Offline signing checks

`Tools/sign-dyld-cache.py` signed the output with the established identity, identifier and team. Strict `codesign`
verification, CMS verification and all **125,028 contiguous 16 KiB page hashes** passed. The signature is CodeDirectory
v`0x20400`, without scatter entries, entitlements or executable-segment flags. Its executable extent matches the TEXT
mapping; only the header's signature-capacity field differs from the unsigned payload.

The signed file is **2,064,482,304 bytes**, SHA-256
`2cee215fdafbb5493f8b03ecf7585ad0799e86dfe4382487b551e7876167da87`, CDHash
`bda9c46dd282b45baf49a819a03b4c5c0c186cfc`. The matching copied/resigned dyld_sim retains MH_DYLINKER and the previous iOS
platform-compatibility changes; its executable payload is unchanged, with SHA-256
`44cb8380b2db3801b88a7327abb27bec6252ada8a9ccc813f8f29afbbd4d919f` after signing.

Evidence under `tmp/springboard-stage/full-cache-108-strip/` includes `build.log`, `validation.json`, `sign-result.json`,
`cache-strict-verify.log`, `dyld-sign-result.json` and `delivery-manifest.json`. The signed outputs and manifests are retained;
redundant unsigned and compressed cache files can be removed after validation. These offline results must remain separate
from later device activation and runtime evidence.

### First phone mapping failure

The first hardware-breakpoint-only LLDB trial used host **85367** and df/LiveProcess **85368**, with the signed cache hash
above. It reached an executable TEXT mapping at `[0x108000000, 0x164594000)`, then failed while mapping DATA_CONST.
The cache's read-only DATA_CONST mapping, `[0x164594000, 0x16e018000)`, replaced the still-active native main stack.
With `sp=0x16b9187b0`, the app callback at `pc=0x1044fae04` executed `str x0, [sp, #0x50]`, faulting on the now-read-only
address **`0x16b918800`**. LLDB reported `EXC_BAD_ACCESS (code=2)` and the overwritten stack could no longer unwind normally.

This failure identifies a live-stack collision, not an execute-protection rejection. The stopped process was killed after
the capture. Mapping had not reached the high read-only LINKEDIT region, so this run establishes neither complete cache
mapping nor acceptance of the portion above `0x180000000`. It motivated the guarded stack handoff described below.
Evidence: `tmp/lsd-services/lldb-full-cache-first-failure.txt`.

### Guarded main stack and completed cache mapping

`LiveSimBooter/DyldSimLoader.cpp` now reads the cache's declared reservation before destructive handoff. It validates the
arm64 header, mapping-table extent, reservation alignment and arithmetic, and checks the reservation against the resident
loader's segments, guest executable segments, mapped dyld_sim, static startup vectors and static launch descriptor.
The captured native main-thread bounds must contain the current frame. These are checks of the named surviving regions,
not a general proof that every native heap allocation is outside the cache.

When the native main stack overlaps the reservation, the loader reserves a replacement stack of at least **8 MiB**, with
one inaccessible page at each end. The `0x400000000` address is a non-fixed mmap hint: it cannot overwrite an existing
mapping, and the returned reservation must lie at or above `0x300000000`, outside the cache including both guards.
Failure before handoff releases the reservation. A non-overlapping native stack retains the existing handoff path.

The startup vectors and launch descriptor live in app-local static storage. The rebuilt `main_stack` apple entry names
the selected usable stack bounds and sets both pthread allocation fields to zero. After the host-cache/TSD/debugger
handoff, a one-way arm64 bridge installs the new SP, clears FP/LR, and branches to a fresh C frame. It cannot return through
the overwritten native frames. It calls dyld_sim, invokes the returned `start_sim`, and uses a raw exit if that entry
returns; no host cleanup follows a successful detach.

The normal loader suite passed **1,030 checks**, and the handoff suite passed **882 checks**, including a real assembly
stack-switch child that makes its entire original stack `PROT_NONE` before invoking the returned entry and exiting 42.
The loader also passed ASan/UBSan, and the handoff suite passed UBSan. Evidence is under `tmp/main-stack/`: `loader-tests.log`,
`handoff-tests.log`, `loader-asan.log` and `handoff-ubsan.log`. These local checks are separate from the device observation.

The next hardware-only LLDB trial, native **85385**, completed cache loading and reached a Foundation initializer.
The observed memory regions were:

| Region | Start | End | Protection |
| --- | --- | --- | --- |
| Cache TEXT | `0x108000000` | `0x164594000` | `r-x`, zero dirty pages |
| Cache LINKEDIT | `0x17a4fc000` | `0x186190000` | `r--`, zero dirty pages |
| Low stack guard | `0x400000000` | `0x400004000` | `---` |
| Usable stack | `0x400004000` | `0x400804000` | `rw-` |
| High stack guard | `0x400804000` | `0x400808000` | `---` |

This establishes that the read-only LINKEDIT mapping can cross `0x180000000` for this signed cache while its code remains
executable below the boundary. At the stop, SP was `0x4007fecd0` on the new stack. The immediate failure was an explicit
BRK at `0x1629c8064` in `libsystem_featureflags.dylib`, called during Foundation initialization. Disassembling its preceding
instructions identifies the exact message as **`No SIMULATOR_ROOT`**. The adjacent cold path at `0x1629c8084` checks
`SIMULATOR_SHARED_RESOURCES_DIRECTORY`; that was not the observed stop. The direct df test lacked `SIMULATOR_ROOT`, while
the selected launchd configuration already supplies both variables. This run did not establish a successful df exit or
complete service startup. Evidence: `tmp/lsd-services/lldb-full-cache-mapped.txt`, `lldb-featureflags-failure.txt` and
`console-full-cache-guarded-df.log`.

### Successful df execution with the full cache

The next normal launch supplied both `SIMULATOR_ROOT` and `SIMULATOR_SHARED_RESOURCES_DIRECTORY`. Under host **85402**,
df/LiveProcess **85403** used the same full cache and guarded stack, printed the complete filesystem table, and exited
successfully: the broker recorded **`wait-status=0x0`**. Its stack diagnostic confirms the usable top `0x400804000`, size
`0x800000`, and cache reservation `0x108000000-0x186194000` before host-cache detachment.

The `malloc: nano zone abandoned due to inability to reserve vm space` warning remained, but did not prevent allocation,
table output or exit 0. The concurrent filtered amfid/TXM capture contained no matching events. Evidence is
`tmp/lsd-services/console-full-cache-df-final.log` and `security.log`. The console's later signal 15 applies to the host
app's shutdown, after the child's recorded successful exit.

This verifies a complete df launch and exit with the signed 1,353-image cache, including its read-only region above
`0x180000000` and relocated main stack. Complete service startup and SpringBoard boot remain separate, pending tests.

### Reserving the gaps between cache mappings

The larger service run exposed another address-layout requirement. Mapping only the cache's file-backed regions left
two 32 MiB holes free: `[0x16e018000, 0x170018000)` and `[0x1746c8000, 0x1766c8000)`. LLDB found ordinary loose images in
those holes, including CoreRoutine at `0x174a90000` and Sleep at `0x175e04000`; both gap starts reported executable
`__TEXT` mappings. These images were inside the cache's declared broad range even though their bytes came from loose
files. Evidence is `tmp/lsd-services/lldb-springboard-range-calls.txt`.

This violates an Objective-C runtime assumption: Apple's
[shared-cache address classifier](https://github.com/apple-oss-distributions/objc4/blob/main/runtime/objc-private.h)
uses one start/end interval, and `classInSharedCache` checks both the class and its read-only metadata against that
interval. It does not ask whether each address belongs to an individual cache mapping. Ordinary image metadata in a
cache hole can therefore receive cached-image treatment. The complete cache address span must remain exclusive, including
regions without file-backed cache contents.

`LSBDyldSimLaunch` now carries the validated cache base and size. After the host-cache/debugger handoff and any required
stack switch, `LSBRunDyldSimOnPreparedStack` reserves the complete span with the app-local
`lsb::mmap(PROT_NONE, MAP_FIXED | MAP_ANON | MAP_PRIVATE)`. This occurs before entering dyld_sim. The
[private mapper](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/SharedCacheRuntime.cpp)
then replaces individual cache segments using fixed mappings and leaves the gaps owned. The matching 23A343 disassembly
confirms this behavior: mapping flags are `0x12`, and whole-range cleanup on failure replaces the span with inaccessible
memory rather than releasing it. The last dynamic-data page is mapped separately.

Preflight additionally verifies that prepared TLS, helper-table and simulator process-info storage belongs to the
already-validated resident image, and rejects overlap with the page-rounded native `TASK_DYLD_INFO` metadata range that
the debugger fallback may preserve. Reservation failure or an unexpected returned address emits one bounded local
stderr diagnostic, then exits through the raw syscall with status 126. No host cleanup or return through old frames is
permitted after this overwrite.

Focused tests passed **1,044 loader checks** and **1,074 handoff checks**, with ASan/UBSan for the loader and UBSan for the
handoff. The real assembly-switch child reserves its original stack as the cache span, replaces two endpoint segments,
checks that the intervening gap remains `PROT_NONE`, and confirms a normal hinted allocation cannot occupy it. Failure
tests cover rejected/incorrect mmap results, malformed ranges and stderr failure. Evidence is in `tmp/main-stack/`
`loader-reservation-tests.log`, `loader-reservation-asan.log`, `handoff-reservation-tests.log`,
`handoff-reservation-ubsan.log` and `handoff-reservation-arm64e.s`.

In the first phone run with reservation enabled, host **97445** saw backboardd **97466** register `CAWindowServer` at
1290×2796 and establish three native Metal connections without the preceding `featureProfile` exception. Sleepd **97469**
remained alive without its preceding initializer exception. LaunchServices probe **97463** received status `0x80` from
peer **97467** and exited 0. SpringBoard **97465** reached its constructor stop; the loose CoreRoutine class reported at
`0x1079bb050` was now outside the cache, compared with `0x174b2f050` before reservation. These initial results support the
range-collision diagnosis; they do not establish complete service or SpringBoard boot. Direct LLDB verification of the
reserved gaps followed in the same SpringBoard process. Evidence is
`tmp/lsd-services/console-springboard-reservation.log` and `reservation-initial-guests.log`.

LLDB then confirmed inaccessible regions at `[0x16e018000, 0x170000000)` and `[0x1746c8000, 0x1766c8000)`. Cache TEXT still
reported `r-x`, and LINKEDIT through `0x186190000` remained `r--`, both with zero dirty pages. The loose CoreRoutine header
was at `0x10791c000` and Sleep at `0x300bfc000`, outside the cache reservation. These reads directly verify that the tested
gap starts no longer contain loose-image TEXT. The recorded first inaccessible region ends at `0x170000000`; it is not a
measurement of the final `0x18000` bytes before TPRO_CONST. Evidence is `tmp/lsd-services/lldb-reserved-cache-mappings.txt`.

After resuming SpringBoard, its hardware exception breakpoint did not fire and the process exited with status **255**.
The earlier SpringBoardUI guard exception did not recur, but SpringBoard did not boot. The reason for this later exit
remained under investigation; the appended output is `tmp/lsd-services/springboard-reservation-after-resume.log`.

## iOS 27 relocation limits

The later [minimum iOS 26 comparison](ios26-base-comparison.md) builds the same 217-image stock dependency closure at
`0x120000000` and `0x180000000`. All 546 instruction-marked sections are identical; the detailed report separates pointer,
metadata, UUID, and signature changes and demonstrates false positives from address-range scanning.

Read-only inspection of the installed iOS 27 build **24A434** runtime on 2026-10-24 found a main
`dyld_sim_shared_cache_arm64` file plus `.01`, containing 1,826 unique arm64 images across 11 mappings.
Their declared span is `[0x1c0000000, 0x2abccc000)`, with size `0xebccc000`. Both `maxSlide` fields and every
mapping's slide-info offset/size are zero. No cached image retains chained-fixup commands, dyld rebase streams,
split-segment information, or section/local/external relocation entries. The mapping-only device trials below retain these
original absolute pointers; they are not complete cache relocations and did not reach iOS 27 `df` execution.

Changing the mapping addresses alone leaves absolute pointers unchanged. For example, cached `libSystem.B.dylib` has a
`__DATA_CONST,__got` slot at VM address `0x264521fa0`, stored in `.01` at file offset `0x26c4dfa0`, containing
`0x2404fa690`, an address inside cached `libsystem_kernel.dylib`. Moving the cache without rebasing that slot leaves
an eventual indirect call or dereference targeting its old location. Changing only the cache header does not repair it.

Moving both files and all their mappings by the same 16 KiB-aligned delta preserves internal PC-relative instructions
and relative metadata. Absolute pointers and address-bearing cache/Mach-O metadata still require adjustment. The examined
builder [omits simulator slide information][simulator-slide-info] and [converts tracked fixups to VM addresses][simulator-fixups].
The surviving v4 symbol patch tables and typed pointer sections identify some sites, but are not a complete relocation inventory.
Rebuilding from original dylibs can apply the original fixups; relocating these finished files requires reconstructing the missing sites.
A blind scan for values inside the old address range cannot reliably distinguish pointers from ordinary data.

The working iOS 26 base `0x120000000` is unsuitable for this layout: its declared end would become `0x20bccc000`,
crossing the native shared-region interval. The smaller cache's successful mapping does not establish an available range
for iOS 27; a different base would also require validating the entire span and the device's supported address space.

A later [low-address feasibility audit](ios27-relocation.md) established that the complete stock cache cannot fit below
`0x180000000` in this arm64 app. Hard page zero leaves at most 2 GiB there, versus the cache's 3.684 GiB span; even the
two existing executable mappings total 2,145.71875 MiB. That audit also records the required pointer and instruction fixups.

[simulator-slide-info]: https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/BuilderConfig.cpp#L170-L174
[simulator-fixups]: https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/SubCache.cpp#L2271-L2285

### First forced-base trial: inside the native reservation

The completed 2026-10-24 trial under `tmp/ios27-forced-base/` used the actual 24A434 main cache and `.01` subcache.
It changed exactly 24 64-bit fields: both mapping tables' addresses and each header's `sharedRegionStart`, shifted by
`0x140000000` to declare `[0x300000000, 0x3ebccc000)`. All other metadata and payload pointers stayed unchanged,
including the original trie and prebuilt-loader addresses. Both files received fresh v`0x20400`, no-scatter, 16 KiB signatures
with the same signing identity, identifier, and team as the iOS 26 cache, without entitlements.

The matching 24A434 `dyld_sim` already honors the header base. Three instruction patches limit its mapping-failure cleanup
to the header's base/size. It was resigned separately. The matching `df` retained `MH_EXECUTE` with
`CS_EXECSEG_MAIN_BINARY` cleared in its fresh signature. The iOS 27 files were staged in a separate RuntimeRoot and activated
temporarily by renaming directories, preserving the working iOS 26 files.

Extended Virtual Addressing was unavailable to the Personal Team; the user selected Increased Memory Limit instead.
Xcode issued profile `cb5e60c5-0140-42a6-a6ab-46553b82f31a`, granting
`com.apple.developer.kernel.increased-memory-limit`, which was also present in the host app's signature. Initial installation
failed because custom provisioning-database cleanup conflicted with an `online_auth` delete trigger. A user-authorized,
guarded trigger replacement preserved cleanup and allowed ordinary app installation to succeed. See
[the provisioning repair](provisioning-cleanup.md) for the reproduced cause, backup, exact change, and scope.

Under hardware-only LLDB, process 5702 reached both cache signature registrations. `F_ADDFILESIGS_RETURN` returned `0`
for file descriptors 4 and 5. All 11 cache mappings completed. The pre-map VM check reported the requested span unmapped,
but this did not establish that it was outside TXM's native shared-region registration.

The first exception was `EXC_BAD_ACCESS (code=2, address=0x26b010000)` in
`mach_o::GenericTrie::find(CString, bool) const + 100`. The faulting `ldrb` used the unchanged header's `dylibsTrieAddr`
of `0x26b010000`; the trie bytes had instead been mapped at `0x3ab010000`. FAR was `0x26b010000` and ESR was `0x92000006`.
The call chain went through `PathToIntTrie::hasPath`, `DyldSharedCache::hasImagePath`, and dependency loading during
`dyld4::prepare`. This directly demonstrates one missing metadata relocation before cached code execution.

Independently, both requested executable mappings had actual `r--` protections:

| Cache member | TEXT mapping | Actual permissions |
| --- | --- | --- |
| Main | `[0x300000000, 0x362270000)` | `r--` |
| `.01` | `[0x37d8d4000, 0x3a181c000)` | `r--` |

The SSH capture contained two `TXM selector: 38 | 23` events, at `13:29:02.029` and `13:29:15.452 +0700`.
The relevant native arm64 shared-region reservation is **`[0x180000000, 0xfc0000000)`**. The installed MacOSX27.0 SDK's
`mach/shared_region.h` defines base `0x180000000` and size `0xe40000000`; the earlier device snapshot
`tmp/host-cache-lldb/df-entry.json` independently recorded the same endpoints. Thus `0x300000000` was still inside the
native reservation. Describing this trial as outside it was incorrect. VM unmapping and a successful file-signature
registration do not establish that a proposed range is free of the retained TXM association.

After capturing the first fault, the process was terminated and the original iOS 26 RuntimeRoot, cache, dyld_sim, and df
were restored with their recorded inodes. A fresh normal launch (PID 5713) ran `df` to exit `0`. The tested iOS 27 directory
was retained as `Documents/RuntimeRoot-ios27-forced-after-test`.

Evidence is in `tmp/ios27-forced-base/result.json`, `fault.json`, `lldb.log`, `range-before.json`, `security.log`,
`restore.log`, and `restored-launch.json`. The same directory retains patch manifests, signing results, installation logs,
and the pre-swap baseline. The trial established signature acceptance and completed mappings, plus a stale metadata fault;
it did not establish executable cache mappings or successful iOS 27 runtime startup.

### Repeat outside the native reservation

A second mapping-only trial completed under `tmp/ios27-outside-region/` at
`[0x8000000000, 0x80ebccc000)`, above both the corrected native reservation and the GPU carveout. The iPhoneOS27.0 SDK's
`mach/arm/vm_param.h` declares that carveout as `[0x1000000000, 0x7000000000)`, so a base at 64 GiB was excluded before
testing. This candidate begins at 512 GiB and retained the original absolute metadata and payload pointers.

Process 5741 ran under hardware-only LLDB. Both `F_ADDFILESIGS_RETURN` calls returned `0`. The VM-region query reported the
requested span unmapped, with no subsequent mapped region before `0xffffffffffffffff`; that query does not prove the task
is allowed to create a mapping at this address.

The first main-cache TEXT mapping failed:

| Argument or result | Observed value |
| --- | --- |
| Requested address / size | `0x8000000000` / `0x62270000` |
| Protection / flags at local wrapper | `5` (`r-x`) / `0x12` (`MAP_PRIVATE \| MAP_FIXED`) |
| File descriptor / file offset | `4` / `0` |
| Raw syscall result | `SyscallResult(value=12, error=12)`: `ENOMEM` |
| Return to dyld_sim | `MAP_FAILED`, `x0 = 0xffffffffffffffff` |

The local wrapper adds `MAP_UNIX03` before the syscall. LLDB caught the raw error in `LocalPOSIX.cpp`'s result conversion,
then confirmed the failed return at `dyld3::mapSplitCachePrivate + 860`. No high cache mapping was installed and no cached
code executed. The test stopped at this first failure; the debugger then terminated the process instead of continuing through
dyld's cleanup or fallback.

The packet log contained zero software-breakpoint `Z0`/`z0` requests; breakpoint requests used hardware throughout. The captured
security log contained no TXM error or AMFI signature rejection. This differs from the first trial's successful but
non-executable mappings inside the native reservation.

`ENOMEM` alone does not distinguish an unsupported task address range from other mapping-resource constraints. It does not
prove physical-memory exhaustion or establish that Extended Virtual Addressing would fix this mapping. The host already
had Increased Memory Limit; that entitlement did not make this particular mapping succeed.

A subsequent source review supports a task-map ceiling as the likely explanation. Public XNU routes both public
Extended Virtual Addressing and Increased Memory Limit through `vm_map_set_jumbo`; its ordinary jumbo maximum is
`0xfc0000000` (63 GiB), followed by the reserved commpage interval through 64 GiB. A separate supported-kernel path and
private large-file-addressing policy allow an extra-jumbo map. See Apple's [exec policy][xnu-va-policy],
[jumbo limit selection][xnu-jumbo-limit], and [address constants][xnu-va-constants]. This source predates the tested iOS 27
kernel, so the exact live task limit remains inferred; the observed `ENOMEM` is consistent with it.

This does not establish a universal 36-bit PAC limit. Both test attachments reported code and data address masks of
`0xfffff80000000000`, consistent with 43 significant address bits (`breakpoints.log`, lines 25–26 in each trial).
The iPhoneOS 27 SDK likewise declares an 8 TiB system maximum. Address representation and a particular task's allowed
allocation range are separate constraints; those larger masks do not authorize the app to map above its task limit.

[xnu-va-policy]: https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_exec.c
[xnu-jumbo-limit]: https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm64/sptm/pmap/pmap.c
[xnu-va-constants]: https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/arm/vm_param.h

The original iOS 26 RuntimeRoot, cache, dyld_sim, and df were restored with their recorded inodes. A fresh normal launch
(PID 5751) ran `df` to exit `0`. The tested iOS 27 directory was preserved as
`Documents/RuntimeRoot-ios27-outside-after-test`. No further mapping attempts were made in this experiment.

Evidence is summarized in `tmp/ios27-outside-region/result.json`, with `lldb.log`, `packets.log`, `security.log`,
`range-before.json`, `restore.log`, and `restored-launch.json`, alongside the exact mapping-field patch manifest and
signatures. Both iOS 27 trials accepted the file signatures; neither established executable cache loading or successful
runtime startup.

## Signed dyld_sim mapping above 0x180000000

A mapping-only comparison on 2026-10-24 (PID 5945) used the working iOS 26 build **23A343** `dyld_sim`, a thin arm64
`MH_DYLINKER` Mach-O. An isolated host build duplicated its complete `__TEXT` mapping at `0x300000000` after native-cache
detachment, while retaining the original loader at `0x105714000`. It did not relocate the full loader or execute the high
copy. The runtime files and working cache at `0x120000000` were unchanged.

The probe stopped before `MAP_FIXED`. LLDB verified that the entire destination span was unmapped; its enclosing reported
hole was `[0x1f69ec000, 0xfc0000000)`. This address was nevertheless inside the native shared-region interval established
earlier. Mapping after detachment avoided having the detach operation erase the experimental image.

| Check | Observed result |
| --- | --- |
| Signature registration | `F_ADDFILESIGS_RETURN = 0`, covered length `622592` (`0x98000`) |
| Mapping request | File offset `0`, length `0x50000`, `r-x`, `MAP_PRIVATE \| MAP_FIXED` |
| Mapping return | `0x300000000`, no syscall error |
| Original TEXT | `[0x105714000, 0x105764000)`, actual `r-x`, zero dirty pages |
| Duplicate TEXT | `[0x300000000, 0x300050000)`, actual `r--`, zero dirty pages |
| Full TEXT comparison | Identical SHA-256: `9bb78d8ee3c54c88a97a528f8f082991e01523c2573ba1c319f94e891df7ea40` |
| Security capture | `TXM selector: 38 \| 23` at `13:59:50.859 +0700` |

The app-local mmap helper added `MAP_UNIX03` as in the existing loader. LLDB required hardware breakpoints; the packet
capture contained zero `Z0`/`z0` requests and no memory-write packets. The same already-accepted file therefore reproduced
the high-address execute-permission failure without changed TEXT bytes or a shared-cache file format.

After evidence collection, `munmap` returned `0` and LLDB reported the duplicate span as `---`. The probe closed its file
descriptor and continued through the original low-address dyld; `df` completed with process exit status `0`.
This confirms permission failure for this duplicate mapping, not an execution failure of a relocated dyld or a result for
every address above `0x180000000`.

### Comparison with the native cache still loaded

A second isolated build (PID 5953) ran the same probe immediately after the shared-region query, before debugger handoff,
the TSD switch, or cache detachment. The query returned `0`, `hasCache` was `true`, and the reported cache header was
`0x18fb18000`. Native dyld's TEXT at `[0x18ff18000, 0x197c4c000)` was still mapped `r-x`, with zero dirty pages both before
and after the probe.

The initial destination `0x300000000` was occupied by a native read-only mapping `[0x2ecd78000, 0x315314000)`.
The pre-map check stopped the test, and no `MAP_FIXED` request was made there. LLDB changed only the app-global requested
address to `0x400000000` using `SBValue.SetValueFromCString`. A second check verified the complete `0x50000`-byte target
span was unmapped, inside the reported hole `[0x315318000, 0xfc0000000)`.

Signature registration again returned `0` with `622592` bytes covered. The same request for `r-x`, file offset `0`, and
length `0x50000` returned `0x400000000` without a syscall error, but `[0x400000000, 0x400050000)` was `r--` with zero dirty
pages. The original dyld TEXT at `[0x103da0000, 0x103df0000)` remained `r-x`; both complete TEXT mappings had the identical
SHA-256 recorded above. The security capture logged `TXM selector: 38 | 23` at `14:03:33.336 +0700`.

Hardware breakpoints were again required and the packet log contained zero `Z0`/`z0` requests. This capture does include
the intentional write to the app-global requested-address field; it is not a no-memory-writes test. The high duplicate
was never executed. Cleanup returned `munmap = 0`, its span became `---`, and the normal cache-detach and simulator
handoff then ran `df` to process exit status `0`.

This demonstrates that unloading the native cache is not required to reproduce the high-mapping permission failure.
The likely conflict is the destination within the already registered native shared-region interval, including its VM
holes. That cause remains an inference from the repeated permissions/error result and the source/binary analysis below.
The two tests used different high addresses because the original destination was occupied in the second process; this
was not a same-address before/after comparison.

Wrapping the cache in a signed Mach-O remains an **untested hypothesis**. These tests give no evidence that a wrapper alone
would solve the address-range conflict: an existing signed Mach-O also lost execute permission there. In the examined
[XNU signature-registration path](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_descrip.c),
file-signature acceptance is distinct from mapping. Its
[VM code-signature association](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_map.c)
passes the requested virtual address, size, and file offset to the monitor, and removes execute permission when association
fails. We infer that preserving the conflicting address would remain a problem for an ordinary wrapper; no wrapper was
constructed or tested. The interpretation of error `23` as a retained code-region overlap comes from the supplied older
TXM 182.0 binary, not a disassembly of the tested iOS 27 monitor.

After both probes, the original host app was reinstalled successfully. A fresh normal launch (PID 5956) printed df's
filesystem table using the active cache at `0x120000000` and exited `0`. This confirms the temporary probe build was
removed and the working runtime still ran without the debugger.

Evidence is in `tmp/dyld-high-mapping/result.json`, `range-before.json`, `lldb.log`, `packets.log`,
`breakpoint-audit.json`, and `probe-security.log`. The temporary source/build lives under the same ignored directory;
the tracked loader sources were unchanged.
The native-cache-loaded comparison is recorded separately in `tmp/dyld-high-mapping-before-detach/`, including
`result.json`, `range-original-target.json`, `range-before.json`, `lldb.log`, `packets.log`, `breakpoint-audit.json`, and
`security.log`. Final restoration evidence is `restore-install.json`, `restored-launch.json`, and `restored-console.log`
in that second directory.

## Manually mapped native dylib

The 2026-10-24 comparison in PID 7100 manually mapped both segments of the same signed native arm64 `MH_DYLIB` at
`0x140000000` and `0x400000000`, then called its exported leaf function. It ran through a separate native-main diagnostic,
before any simulator mapping or host-cache detachment. Native cache TEXT remained `r-x` with zero dirty pages.
LLDB verified both complete `0x10000`-byte destination spans were unmapped before the first `MAP_FIXED` request.

The reused control has iOS 27 platform metadata, identifier `com.kdt.LiveSimBooter`, team `UU7W52U79B`, no entitlements,
and CodeDirectory v`0x20400` with 16 KiB pages. Its file SHA-256 is
`80b292fab0be059bf9564546d0040db0eaa93dc75c660149f5cf783233ff81a7`; local codesign verification passed.
`F_ADDFILESIGS_RETURN` returned `0` with covered length `49152` (`0xc000`). All four segment mappings returned their
requested addresses without errors: `__TEXT` requested `r-x` and `__LINKEDIT` requested `r--`, using the same file descriptor
and original file offsets.

| Check | Low mapping | High mapping |
| --- | --- | --- |
| TEXT range | `[0x140000000, 0x140008000)` | `[0x400000000, 0x400008000)` |
| Actual TEXT permissions | `r-x` | `r--` |
| LINKEDIT range | `[0x140008000, 0x140010000)` | `[0x400008000, 0x400010000)` |
| Actual LINKEDIT permissions | `r--` | `r--` |
| Function address | `0x140004000` | `0x400004000` |
| Invocation result | Returned `42` | Faulted on the first instruction |

Both complete TEXT mappings were byte-identical, with SHA-256
`59bd39eab6a4dcea0b7aa7d1ad388b508da8f136c0e5a04eee36fbe572c0fda1`, and both had zero dirty pages before and after the
execution attempt. Hardware breakpoints were restricted to app markers; the packet log contained zero `Z0`/`z0` requests
and no memory-write packets. The low call completed before the high call. High execution stopped at its first
`mov w0, #42` with `EXC_BAD_ACCESS (code=2)`, PC and FAR both `0x400004000`, and ESR `0x8200000f`.
The security capture logged `TXM selector: 38 | 23` at `20:30:55.505 +0700`; it recorded successful AMFI profile validation
for the dylib without a reported signature rejection for it.

This extends the high-address result to an ordinary `MH_DYLIB` and an actual execution attempt. It remains a small leaf
control, not a general manual dylib loader: `_lsb_control_value` contains only `mov w0, #42; ret`, has no imported symbols,
and its chained-fixup table has zero imports and empty segment starts. No rebasing or binding was applied. Its declared
`LC_LOAD_DYLIB` dependency on libSystem was already present in the native host; the probe did not load dependencies or run
initializers. The high destination still lies inside the native shared-region interval discussed above.

The debugger killed PID 7100 after recording the fault, so the probe's normal unmap loop did not run. Only the temporary
device file `Documents/libLSBManualProbe.dylib` was removed, and the original host app was reinstalled successfully.
A fresh normal launch (PID 7108) printed df's filesystem table using the working cache at `0x120000000` and exited `0`.

Evidence is under `tmp/dylib-manual-mapping/`: `result.json`, `fault.json`, `ranges-before.json`, `lldb.log`, `packets.log`,
`breakpoint-audit.json`, `security.log`, `signature.txt`, `verify.log`, and the retained diagnostic source. Restoration is
recorded in `remove-device-probe.log`, `restore-install.json`, `restored-launch.json`, and `restored-console.log`.

### Requesting execute permission with mprotect

PID 7118 repeated the same native dylib comparison with the host cache loaded, then requested
`mprotect((void*)0x400000000, 0x8000, PROT_READ | PROT_EXEC)`. It returned `-1` with `errno=13` (`EACCES`), and high TEXT
remained `r--`. PID 7130 repeated it while querying actual VM permissions with `vm_region_64(VM_REGION_BASIC_INFO_64)`.
Each query succeeded with count `9` and returned the exact `0x8000`-byte TEXT region:

| Mapping and query point | Current protection | Maximum protection |
| --- | --- | --- |
| Low `0x140000000`, before mprotect | `r-x` (`5`) | `rwx` (`7`) |
| High `0x400000000`, before mprotect | `r--` (`1`) | `rw-` (`3`) |
| High `0x400000000`, after rejected mprotect | `r--` (`1`) | `rw-` (`3`) |

These are live VM region fields, distinct from the Mach-O segment's `maxprot`. High TEXT already lacked execute in its
maximum protection before mprotect, consistent with the `EACCES` result. The low maximum of `rwx` does not establish that
simultaneous writable and executable access would be allowed.

In both runs, signature registration and all four segment mappings succeeded, low execution returned `42`, and high
execution faulted at its first instruction: PC and FAR `0x400004000`, ESR `0x8200000f`, `EXC_BAD_ACCESS (code=2)`.
TEXT retained the identical hash recorded above and zero dirty pages; both packet logs contained zero `Z0`/`z0` requests.
Each security capture recorded one TXM `38 | 23` during initial mapping (`20:38:42.797 +0700` and `20:43:41.292 +0700`),
with no additional event through mprotect and the execution fault. The same leaf-function and native-cache scope applies.

Evidence is under `tmp/dylib-mprotect/` and `tmp/dylib-mprotect-maxprot/`, including `before-mprotect.json`,
`after-mprotect.json`, `result.json`, `fault.json`, and LLDB, packet, and security logs. Both faulted processes were killed
and the temporary device dylib removed. The original app was restored; final normal PID 7136 ran `df` with the cache at
`0x120000000` and exited `0`, recorded in the second directory's `restored-launch.json` and `restored-console.log`.

## Install, preserve, and inspect

The active device files are relative to the app data container:

```text
Documents/RuntimeRoot/usr/lib/dyld_sim
Documents/RuntimeRoot/bin/df
Documents/RuntimeRoot/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64
```

The earlier stock-base cache is preserved beside the active cache as `dyld_sim_shared_cache_arm64.test-disabled`.
The previous dyld_sim is preserved as `usr/lib/dyld_sim.before-relocated-cache`. Keep these and any loose-library backups
when changing experiments. Use [the device procedure](debugging.md) to transfer files, collect logs, and verify execution.
The cache-only test moved loose dylibs to `Documents/RuntimeLooseDylibs-cache-only`, outside the search root, and then restored
every moved path. Its move manifest and restoration log record those operations; do not assume the current device is still cache-only.

The tracked signer intentionally accepts only the tested single-file arm64 simulator layout and v`0x20400` metadata.
It rejects unsupported signatures and subcaches. The exact two-file iOS 27 trial used a separate experimental adapter under
`tmp/ios27-forced-base/`. Signature registration and mapping completed, but the cache still lacks a complete surviving
relocation inventory and the trial failed before runtime startup. It does not establish that the full cache can execute.

Local evidence from this experiment is in `tmp/dsc-relocated/result.json`, `sign-result.json`, `live2/inspection.json`,
`live2/lldb.log`, `live2/security.log`, and `normal-launch.json`. Cache-only evidence is in `tmp/dsc-cache-only/inspection.json`,
`lldb.log`, `security.log`, `normal-security.log`, `launch.json`, `normal-launch.json`, `move-manifest.json`, and `restore.log`.
The later breakpoint comparison is in `tmp/txm-breakpoints/result.json`, with `baseline/`, `hardware-only/`, and `baseline-full/`
packet/LLDB/security logs and a separate move manifest/restoration log. These logs are intentionally ignored and may not exist
in another checkout.

The requested local cleanup on 2026-10-24 removed large generated cache copies, upload staging trees, and build caches.
Historical binary paths above may therefore no longer exist locally; experiment logs, scripts, and manifests were retained.
The deletion inventory and retained-evidence details are under `tmp/cleanup-20261024/`.

## High executable-address limits, 2026-11-01

A native-only LiveProcess probe compared the current iPhone 17 Pro Max (`iPhone18,2`, iOS 27.0 / `24A435`) with the
development iPhone 15 Pro Max (`iPhone16,2`, iOS 26.1 / `23B50`, using its existing relaxed boot configuration). Both kept
the native cache loaded, used their installed extension's unchanged entitlements, including Increased Memory Limit, and
ran without LLDB. Neither profile added Extended Virtual Addressing. `csops` reported `CS_DEBUGGED=0` in both probes.
This comparison does not isolate the effects of hardware, OS version, and boot configuration.

The probe registered the same plain-arm64, identity-signed leaf dylib with `F_ADDFILESIGS_RETURN`, mapped its original
signed code, queried current/maximum VM protections, and called `mov w0, #42; ret`. Ordinary free destinations were reserved
without overwriting existing mappings. Within the native shared region, it descended through the `VM_MEMORY_SHARED_PMAP`
submap and replaced only a fully covering nested guard with both protections zero; loaded cache mappings were excluded.

| Check | iPhone 17 | iPhone 15 development device |
| --- | --- | --- |
| Signed code at `0x400000000`, inside the native shared region | `r--`, maximum `rw-`; not called | `r-x`, maximum `rwx`; returned 42 |
| Signed code at `0x7000000000` | `r-x`, maximum `rwx`; returned 42 | Address rejected |
| `TASK_VM_INFO.max_address` (exclusive) | `0x8000000000` (512 GiB) | `0xfc0000000` (63 GiB) |
| Highest 16 KiB page | `0x7fffffc000`, `r-x`; returned 42 | `0xfbfffc000`, `r-x`; returned 42 |
| Allocation at the reported limit or one page above | `KERN_INVALID_ADDRESS` | `KERN_INVALID_ADDRESS` |

The earlier October 24 `r--`/failed-`mprotect` control was on iPhone 17, not iPhone 15. The present iPhone 15 setup does
**not** reproduce that protection loss inside the shared region, but its smaller task address limit excludes the entire
`0x7000000000` range. The measured ceiling is per tested task/configuration, not a universal ARM64 or PAC limit.

The unchanged signed iOS Simulator 26.0 / `23A343` cache, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`, original base
`0x110000000`, also passed signature registration on both devices. Its 1,549,352,960-byte TEXT mapping received `r-x`
with maximum `rwx`. With no address hint, the kernel placed it at `0x7000008000` on iPhone 17 and `0x300000000` in the
final iPhone 15 run. These were protection-only mappings: no cache fixups were applied and no cache code was entered.
They do not establish that the complete runtime can boot at those addresses.

A separate anonymous **writable** 3 GiB allocation at exactly `0x7000000000` succeeded on iPhone 17. A full `bzero`
completed in 167.695 ms in the final run; sampled page starts and the final byte were zero. The measured footprint reached
3,223,998,224 bytes and returned to 1,197,584 bytes after successful deallocation. This test ran before other high mappings;
a prior ordering had encountered an occupied range and stopped before zeroing. On iPhone 15 the allocation was rejected
before any touch (`vm_allocate`: `KERN_INVALID_ADDRESS`; anonymous fixed `mmap`: `ENOMEM`). No memory at another address
was substituted for the requested test.

Evidence is under ignored `tmp/high-map-700-20261101/`: the C probe, signed package/rollback copies, JSON-lines VM results,
and bounded native AMFI/TXM captures. Final comparison PIDs were 20316 and 16430. Both original app packages were restored;
the runtime caches and simulator data were not replaced. The iPhone 17 simulator was restarted, while iPhone 15 returned
to its prior powered-off state. No debugger was attached during these experiments.

An explicit iPhone 15 follow-up tested `0x8000000000` with **direct** 16 KiB `mmap(PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED)`
calls, without the previous `vm_allocate` reservation gate. Both the registered signed file-backed page and an anonymous page
returned `MAP_FAILED` with `errno=12` (`ENOMEM`). Signature registration succeeded, and the kernel-placed signed control at
`0x103400000` was `r-x`, maximum `rwx`, and returned 42. PID 16542 retained the native cache and had `CS_DEBUGGED=0` without LLDB.
An earlier probe had skipped the calls because `mach_vm_region_recurse` queried at `0x8000000000` returned a low region address;
that query is not evidence that a mapping succeeded or failed. The corrected direct-call result is recorded in
`tmp/high-map-800-iphone15-20261101/results.jsonl`, with probe source, security capture, and app-restoration records alongside.

## Slidable iOS 26 cache on iPhone 17, 2026-11-01

An isolated **219-image arm64 simulator 26.0 / 23A343** cache executed successfully at both `0x7000000000` and
`0x7100000000` on iPhone18,2 / native iOS 27.0 build 24A435. The same signed cache file served both bases; only the
experimental loader and its native helper configuration changed. This establishes execution after rebasing, beyond the
earlier mapping-only tests. It does not establish full SpringBoard boot with a high-address cache.

### Builder and loader changes

The original 23A343 builder's instruction at file offset `0x5eb14`, `0x37000360`, skips slide-info generation for simulator
inputs. Replacing that validated instruction with `NOP` enabled its existing arm64 v2 slide format. The original 217-image
minimum closure plus stock `libxo` and `libutil` covers `df`; original runtime inputs were preserved.

The output retains preferred base `0x180000000`, six mappings, and a `0x1550c000` VM span. Its three data mappings contain
**2,131,232 exact fixups**, including **14,673 tagged pointers**, in standard v2 chains: 4 KiB pages, delta mask
`0x00ffff0000000000`, shift 38, zero value-add, and no extras. The no-rebase sentinel is `0x4000`.
Decoding these chains reproduces every data byte in an independently built stock-cache control. All 550 instruction-marked
sections, totaling 145,339,368 bytes, are identical between those two builds.

An experimental `dyld_sim` patch sets a nonzero slide and corrects a second buffer calculation that otherwise cancels it.
The native loader reserves the complete high range without overwrite before entering dyld. Its executable-local mmap helper
validates the cache UUID and layout, rejects an unexpected mapping address, then applies the exact v2 chains after all
regions have mapped. It temporarily makes data writable with `VM_PROT_COPY` and restores the protections dyld requested,
including writable TPRO data. Signed TEXT is not modified after mapping. Local tests independently checked every rebased
pointer against the builder inventory and verified that the complete TEXT mapping stayed unchanged.

The custom mapper forces the two selected slides, `0x6e80000000` and `0x6f80000000`. The builder still emits its ordinary
`maxSlide=0x20000000`; this prototype does not implement general address selection or a completed max-slide policy.
The experiment remains under ignored `tmp/slidable-cache-20261101/`, outside the normal build and setup tools.

### High-address Objective-C constant

The first `df` run passed at `0x7000000000`, but a Foundation smoke test looped in `CFStringGetCString`. A hardware-only
LLDB attachment found `__NSCFString` repeatedly dispatching `_fastCharacterContents`. The cache's exported
`objc_debug_isa_class_mask` was `0x0000000ffffffff8`, narrower than the simulator runtime's actual ISA decoding mask,
`0x007ffffffffffff8`. CoreFoundation consumes that exported mask and misclassified its own high-address class.

The exact scalar at cache file offset `0x39bb8` was widened to match the pinned runtime's decoder. It lies in `__TEXT,__const`,
so the correction was made **on disk before signing**, never by dirtying live executable pages. A new cache UUID distinguishes
this variant. All other payload bytes except the UUID remain identical; instructions and slide metadata are unchanged.
After this correction, the same Foundation probe passed at both bases without a debugger.

Final signed cache UUID: `4A00BEA1-55DE-530B-8B6D-242D4FBED3D4`; size 292,765,696 bytes;
SHA-256 `2c0984897d4092c9d07fee68976293784d681928b63e1418901bece9b0ab0694`.
It uses the configured development identity and app identifier, contiguous 16 KiB signature pages, no entitlements,
and no executable-segment privileges. USB rsync transfers omitted post-copy checksums.

### Device results and limits

| Actual base | Guest | Native PID | Result |
| --- | --- | --- | --- |
| `0x7000000000` | Foundation/CF smoke | 20882 | String conversion, array bridging, binary-plist roundtrip; exit 0 |
| `0x7000000000` | Stock `df` | 20884 | Filesystem table; exit 0 |
| `0x7100000000` | Stock `df` | 20898 | Filesystem table; exit 0 |
| `0x7100000000` | Foundation/CF smoke | 20900 | Same operations; exit 0 |

Kernel VM queries reported TEXT **current `r-x`, maximum `rwx`** at both bases. DATA_CONST returned to `r--` after its private
fixups. The native cache remained loaded, and native workqueue entries redirected to guest code at the selected high base.
The successful AMFI/TXM captures contained no signature rejection or TXM error. All four final runs used fresh guests without
LLDB attached. The earlier CF diagnostic debugger was detached and its debugserver verified gone before the retests.

Two earlier failures are kept separate in the evidence. The initial mapper accidentally canceled the slide, wrote mappings
at the preferred base, and faulted when reading the intended high cache header; that capture had a TXM overlap error.
After correcting the mapper, a direct-command run reached Foundation but trapped because its required `SIMULATOR_ROOT`
environment was absent. Supplying both simulator root variables resolved that setup error. A nano allocator reservation
warning remained during the successful runs; the allocator fell back and the probes completed.

The test used a separate `slide-probe/RuntimeRoot` with empty sibling `data`, temporarily disabled optional audio/haptic
helpers and lazy signing for this session, and preserved saved settings and the normal runtime. The native MobileGestalt
answer was development, but retention was explicitly forced for these successful tests; no security state was changed.
The normal app and original cache UUID were verified after restoration; its original-runtime `df` run (PID 20907) exited 0.
The simulator returned to its initial powered-off state. The unrelated pre-existing device debugger session was preserved.
General ASLR, arbitrary runtime builds, full-cache/UI boot, and production integration
remain untested. The iOS 27 stock simulator cache still lacks the exact relocation inventory used by this new iOS 26 build.

Evidence includes `builder/cache-manifest.json`, `builder/stock-twin-verification.json`, `loader/`, `vmquery/`,
`audit/cache-wide-isa/verification.json`, signed manifests, `device/console-{cf,df}-wide.log`,
`device/console-{cf,df}-710.log`, bounded security captures, and restoration records under the ignored experiment directory.

### Full-cache preparation at the high base

The subsequent full 23A343 build contains the tracked 1,350 runtime images plus the three recorded host-support images.
It retains preferred base `0x180000000`, has six mappings and a `0x7e1b0000` VM span, and targets actual base `0x7000000000`.
Its v2 chains contain **15,156,132 pointers**, including **40,252 tagged values**. Independently decoding every chain reproduces
the stock control's complete data mappings; all 3,698 instruction sections match that control before compatibility patches.
The final variant preserves all five container-metadata literal replacements and seven CoreHaptics instruction patches,
and widens the export-resolved ISA mask at file offset `0x61bb8`. Its UUID is `F79708E3-14B8-5968-A655-9E5E5791EFDD`.

Rebasing those data mappings independently in every guest would dirty 47,435 4 KiB pages, about 185 MiB per process.
For this experiment a separate 236,158,976-byte file holds the three already-relocated, non-executable data mappings.
The native helper opens it before switching TLS, validates its versioned header against the exact cache UUID/base/span and
mapping table, maps it `MAP_PRIVATE`, and closes the descriptor after mapping. Initial pages can be shared between guests;
only later guest writes require CoW. A local two-view test verified that a guest write changes neither the other view nor the file.
TEXT and read-only metadata still map from the signed cache. The sidecar's payload is **trusted, unsigned runtime input**:
its UUID/layout checks are consistency checks, not authentication by the cache signature. The cache itself stays slidable;
this prepared sidecar is specific to the selected base.

The experimental native package reserves the high span without overwriting existing mappings and requires native-cache
retention with hardware workqueue redirection. It rejects an explicit detach request for this cache. The matching loader
preserves all eleven existing iOS-platform compatibility patches and the ordinary low-cache fallback. Routine diagnostics
remain off unless `LSB_TRACE_HANDOFF=1`. These experimental artifacts and reproducible receipts are under ignored
`tmp/slidable-full-20261101/`; the tracked setup commands still build the established low-address cache.

The full cache was signed and deployed over USB to iPhone18,2 / iOS 27.0 build 24A435, preserving the working cache,
loader and shim outside RuntimeRoot. Transfers omitted post-copy checksums. The final signed cache is 2,064,596,992 bytes,
SHA-256 `100acf7af897d4c93d95c55d05471257a1d539960abdf49845c627b058da5598`.
Stock `df`, native PID 21165, exited 0. Kernel queries showed TEXT at `0x7000000000` with current `r-x`, maximum `rwx`;
the ordinary DATA_CONST mapping was `r--`, and the small TPRO mapping retained dyld's requested `rw-` protection.
All three data regions used the prepared backing without per-process rebase writes.

The normal graphical session then booted with native host PID 21169, launchd 21174, backboardd 21220 and SpringBoard 21221.
A device screenshot shows the populated Home Screen, wallpaper, icons, and Maps/Calendar widgets. The user confirmed Contacts
and Fish responded normally; `df` launched from Fish as PID 21343 and exited 0. Contacts was independently observed as PID 21341.
A later ten-second sample contained 80 project processes and about 2,200.7 MiB of combined physical footprint, including
the native host and Metal helpers; SpringBoard used about 93.8 MiB and launchd 17.5 MiB. Launchd still consumed about 15%
of one CPU in that sample, so this is not evidence of an idle-CPU improvement. The bounded AMFI/TXM capture contained no
signature rejection or TXM event. No debugger was attached for these tests, and the final inventory showed no traced project process.

This is a successful full-cache boot and basic app/terminal test, not a complete compatibility pass. An optional FinanceKitUI
widget process trapped through Swift's `swift_unexpectedError`; a SiriAnalytics service executable was absent. Neither report
establishes a relocation defect. The new deployment remains active with the previous files retained for rollback. General
ASLR, other load bases for the full cache, and integration into the tracked setup/native source remain separate work.

### ANYWHERE executable reservation, 2026-11-01

A native-only probe tested automatic placement on both phones, retaining the native cache without LLDB or new entitlements:

```c
mach_vm_address_t base = 0x180000000;
kern_return_t result = mach_vm_map(mach_task_self(), &base, 0xc0000000, 0,
    VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE,
    VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_EXECUTE, VM_INHERIT_DEFAULT);
```

The 3 GiB anonymous reservation was never read, written, or executed. VM queries sampled its first, middle and final pages.
After measuring the untouched reservation, the probe replaced only its owned first 16 KiB page with a registered, signed
leaf containing `mov w0, #42; ret`, called it, then deallocated the complete owned range. All eleven signed-leaf controls
passed on each device; every release succeeded.

| Device / native build | Probe PID | First returned base | Reservation current/max | Untouched resident / footprint increase | Signed leaf |
| --- | --- | --- | --- | --- | --- |
| iPhone16,2 / 23B50, existing development configuration | 16968 | `0x300000000` | `r-x` / `r-x` | 0 / 0 bytes | 42 |
| iPhone18,2 / 24A435 | 21567 | `0x7000000000` | `r-x` / `r-x` | 0 / 0 bytes | 42 |

The reservation increased reported virtual size by exactly 3,221,225,472 bytes. Mapping and executing the single signed page
subsequently faulted that page; this does not contradict the untouched reservation's zero physical-memory increase.
The signed replacement page had current `r-x`, maximum `rwx`. No AMFI rejection or actual TXM event appeared in the bounded
captures. Both probes reported `CS_DEBUGGED=0`. An unrelated pre-existing iPhone 15 debugger session was left untouched.

The full-cache-span control (`0x7e1b0000` bytes) also mapped and executed its signed leaf. Later iPhone 17 allocations
returned addresses incremented by 16 KiB, so the first base is an observation, not a stable per-model contract. In particular,
a consumer must use the returned address and a matching relocated DATA backing; independent per-process ANYWHERE choices
would not automatically share the same already-rebased data file.

Public [XNU allocation logic](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/osfmk/vm/vm_map.c)
selects VM ranges using size/tag and preserves the `mach_vm_map` hint, searching upward inside the selected range.
Its [arm64 threshold](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/osfmk/mach/arm/vm_param.h)
classifies allocations of at least 1 GiB as large-file allocations. This source explains why a large reservation can choose a
different range from a small page; it does not make `VM_FLAGS_ANYWHERE` a TXM-executable-address search API. Actual protection
and execution checks remain necessary. Exact private-kernel policy is bounded by the device measurements above.

Evidence is under `tmp/anywhere-cache-20261101/`, including the native C source, signature/packaging logs, complete JSON-lines
measurements, and restoration records. Runtime files and DATA sidecars were unchanged. Both original app packages were
restored afterward; iPhone 15 returned to its initially powered-off simulator state and iPhone 17's working high-cache session
was restarted. The deployed loader still uses its existing fixed base; this experiment validates the candidate-selection step.

### Automatic base selection and shared DATA preparation

The tracked implementation now extends the fixed-`0x7000000000` prototype above. Before queuing a new simulator session,
the native broker inspects the supported slidable cache, obtains a 3 GiB RX `mach_vm_map` ANYWHERE reservation using
`0x180000000` as the hint, and releases it without touching pages. The returned address is the session's common cache base;
it is not hardcoded by phone model. A previously selected base can be reused for the same cache UUID across a userspace reboot.
The authenticated bootstrap carries this value to guests (transport ABI 9, bootstrap ABI 7); native helper roles receive zero.
Rebuild the host, extension and guest transport consumers together.

The host decodes the exact v2 chains once into an immutable `lsb-slide-<cache UUID>-<base>.data` file beside the cache.
It publishes completed output atomically and validates its header before reuse. Each guest compares the selected base,
UUID and exact mapping profile, opens the file before changing TLS, and maps its three non-executable regions `MAP_PRIVATE`.
This shares their initial pages and leaves later guest writes private. It avoids dirtying about 185 MiB separately in every guest.
The file is **trusted unsigned DATA**, not authenticated by the cache's signature; keep it with the matching trusted runtime.
It is never used for executable mappings. Signed cache TEXT and read-only metadata continue to use the original cache file.

[The dynamic loader patch](../Tools/patch-ios26-cache-slide.py) takes the original, hash-pinned 23A343 `dyld_sim`, preserves
the eleven iOS-platform compatibility changes, and installs a signed instruction stub plus a marked 64-byte record in verified
unused `__DATA_DIRTY` padding. Native preparation validates that writable record and its section boundaries, then supplies the
selected base there. Changing the base does not modify signed TEXT or require another loader signature. Ordinary low caches
retain the zero-slide path.

The same tool provides a read-only `--find-only` patchfinder. It searches the entire original pure-instruction section for unique
contexts covering all five mapping/cleanup edits, then derives the two zero-filled tail locations from parsed segment/section
boundaries. It emits file/VM addresses and matched context bytes as JSON. The patch path repeats discovery and checks every
result against the reviewed locations; the complete 23A343 hash pin remains mandatory. This is reproducible discovery for the
supported loader, not an automatic patcher for unfamiliar dyld versions.

Each guest reserves the complete selected cache span without overwriting existing mappings before the destructive `MAP_FIXED`
stage. Slidable sessions require retaining the native cache and the native workqueue bridge, even on development devices.
After mapping cache TEXT, an executable-local VM query must confirm current read/execute permission across the entire mapping;
successful mmap alone is insufficient. Failure stops that guest before it returns to dyld. Detailed region logs remain opt-in
through `LSB_TRACE_HANDOFF=1`.

Support remains pinned to the reviewed 1,353-image 23A343 layout: six exact mapping profiles, span `0x7e1b0000`, standard
v2 chains with no extras or cross-page pointers, 15,156,132 fixups, and the corrected high-address ISA mask. Unknown layouts
fail closed. Cache UUIDs may differ between compatible builds; UUID equality still binds each prepared data file to its cache.
The host does not retry another address automatically if a child's reservation conflicts. The first preparation performs about
225 MiB of data I/O synchronously; it is not evidence of improved boot latency. Local tests cover both observed address ranges,
configuration boundaries, DATA descriptor routing, VM permission rejection, and ordinary-cache fallback.

On iPhone 15 Pro Max running native iOS 26.1, the automatic path selected `0x300000000` for cache UUID
`F79708E3-14B8-5968-A655-9E5E5791EFDD`. Initial DATA preparation completed in about 0.4 seconds; later launches reused the same
read-only file and inode. The first permission probe exposed an SDK compatibility issue: this OS returns 19 words of VM region
information rather than the current SDK's full structure. The loader now accepts the older response length that contains both
protection fields and still rejects a mapping without read/execute permission. The repeated `df` run exited zero with cache
TEXT at `0x300000000`, current protection `r-x`, and all three DATA mappings backed by the prepared file. Cold boot displayed
SpringBoard and widgets, Contacts rendered its main screen, and a Fish job spawned `df` with exit status zero. These tests used
no LLDB and retained the native cache.

Userspace reboot remains under investigation: two attempts started a new simulator launchd which died with SIGSEGV after
about two seconds; a third attempt with an otherwise equivalent exception-reporting shim completed and displayed SpringBoard.
No exception was reported in that successful attempt. A later normal-shim restart with the alias-corrected cache also completed,
starting a fresh launchd, SpringBoard and Fish. This does not explain the earlier SIGSEGV failures. The same cache base and DATA inode were reused in the first three attempts,
so a successful cold boot alone must not be reported as proof of reliable restart. The temporary reporting shim was restored
to the normal build after capture. Local evidence is under ignored `tmp/base-relocator-20261101/`.


### Cached aliases and Swift overlays

The 1,353-image cache is the pinned graphical dependency set, not every dylib in the supplied runtime. Other staged code remains
in loose files. The original source inventory contains additional private frameworks, plugins and application libraries;
adding cache aliases does not enlarge that image set.

An application importing `/usr/lib/swift/libswiftNetwork.dylib` exposed missing alias metadata. The original 23A343 file is a
symlink to `Network.framework/Network`, whose implementation and Swift exports are already cached. The simulator builder's
filesystem driver does not register symlinks with its cache builder, and runtime staging omits cached Mach-O files. Restoring
only that symlink on iPhone 15 therefore left a dangling target: native open returned `ENOENT` and guest `dlopen` still failed.
The temporary symlink was removed after the control test.

[The alias patcher](../Tools/patch-ios26-cache-aliases.py) restores the original 63 cached aliases in the existing signed path-trie
allocation. It shares identical terminal/subtree nodes and places small nodes first, retaining all 1,353 canonical image indices.
The 1,416 lookup paths occupy 57,124 of 57,200 bytes. Both pinned Apple traversal implementations accept shared nodes. Independent
whole-trie decoding and per-path lookup verify the result; every other payload byte is checked unchanged except the cache UUID.
Mapping extents, prebuilt-loader indices, instructions and v2 fixups are unchanged. The cache is re-signed afterward.

The complete original `libswift*` symlink audit found 12 paths, all targeting cached images: CloudKit, CompatibilitySpan, HomeKit,
IdentityLookup, Network, PencilKit, QuickLook, ShazamKit, SoundAnalysis, SoundAnalysis_Private, and WebKit at two paths.
The WebKit source lies under `/System/Cryptexes/OS`, but its install name is `/System/Library/Frameworks/WebKit.framework/WebKit`.
Alias validation therefore compares resolved source files with the pinned input's source path, independently of its install name.
The two WebKit aliases are `/usr/lib/swift/libswiftWebKit.dylib` and `/System/Cryptexes/OS/usr/lib/swift/libswiftWebKit.dylib`.

On stock macOS Simulator, resolving a filesystem symlink can still select the cached image. In the
[expected-on-disk path](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/Loader.cpp#L969-L1022),
dyld compares filesystem identity, inode and modification time with the cache image entry, rather than comparing Mach-O UUIDs.
A newly created empty file is not equivalent to the original target; it may be treated as a disk override and rejected as an
invalid image. The tests above establish the dangling-symlink failure and cache-alias success; no empty-placeholder strategy is
part of setup.

The first 61-alias cache (UUID `61503381-9FE5-5792-BEE5-717FC5C53033`) passed a pre-main `LC_LOAD_DYLIB` import of Swift Network,
all 61 cache-path queries, and dynamic loading on iPhone 15 at automatically selected base `0x300000000`. Network was supplied
by the cache at `0x30fcc1000`; both probes exited zero. The user also confirmed Feather opens. The additional WebKit audit
reproduced exactly two missing Swift paths before the final 63-alias build.

Final cache UUID `44EAAA94-7C15-563D-A394-13296CB1AD7A` passed all 12 Swift cache-path queries on the same phone. Both WebKit
aliases successfully loaded the same cached framework at `0x316c70000` and resolved the same `WKWebView` class; the probe
reported `missing=0 failed=0` and exited zero. The signer verified 125,035 contiguous 16 KiB pages. No AMFI/TXM rejection
appeared in the captured test window. The native cache remained loaded, no debugger was attached, and transfer checks used
size/header inspection rather than a post-copy checksum. The final normal cold boot displayed SpringBoard and left launchd and Fish running, with no debugger or diagnostic environment.
Prior cache files and their prepared DATA remain available for rollback.
Evidence and source-audit manifests are under ignored `tmp/slidable-full-swift-aliases-20261101/` and
`tmp/base-relocator-20261101/device15/`.
