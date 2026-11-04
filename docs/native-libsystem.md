# Reusing retained native libSystem support

## Current status, 2026-11-02

The native-support bridge runs the pinned iOS Simulator 26.0/23A343 pthread/dispatch probe and boots SpringBoard on
iPhone18,2 with native iOS 27.0/24A435 and iPhone16,2 with native iOS 26.1/23B50, without the workqueue hardware breakpoint.
Both have Home Screen/widget evidence and user-confirmed Contacts input and Fish (`df`). On 23B50, the final build also
started Fish on subsequent cold boots and after a userspace reboot. An initial `ENOTTY` from its terminal helper was not
reproduced; the failed guard was not captured, so no specific terminal fix is claimed. Native 26.7.2/23H128 has offline decoder checks only.

The host selects this mode from the installed, fully validated native-support cache profile. Installing the original cache
retains its existing support-library/workqueue behavior.
See [the implementation and device result](#native-support-bridge-and-device-probe) below; the earlier compatibility audit
and unmodified placeholder-build results remain recorded separately.

## Initial finding

Reusing the retained native `libsystem_kernel`, `libsystem_platform`, and `libsystem_pthread` is a plausible way to remove
the workqueue entry breakpoint. It is not equivalent to disabling the existing bridge: the current handoff still creates
guest pthread/TSD state and initializes the Mac-derived support pthread, even while the native cache remains mapped.
The kernel still enters the previously registered native pthread. One coherent pthread implementation must own that path.

The initial audit read source, original simulator binaries, and already extracted DeviceSupport libraries. It made no
device changes. The offline placeholder build and subsequent opt-in implementation are separate stages described below;
the symbol-availability comparison alone does not establish runtime compatibility.

## Scope and method

The guest baseline is the original arm64 iOS Simulator 26.0/23A343 runtime. Its three Mac support inputs were checked against
the recorded build inputs of the working 1,353-image cache, UUID `44EAAA94-7C15-563D-A394-13296CB1AD7A`.
Their hashes match. Retaining the existing simulator wrappers matters:

```text
libSystem.B → libsystem_sim_{kernel,platform,pthread}
            → libsystem_sim_{kernel,platform,pthread}_host
            → libsystem_{kernel,platform,pthread}
```

The audit scanned 5,186 plain-arm64 original runtime images, covering all 1,350 original cache inputs after resolving
symlink aliases. It read 5,183 chained import tables and three legacy undefined-symbol tables, preserving library ordinal
and weak-import status. All 152,076 system-library import records resolved against the current original simulator/Mac
inputs. Symbol availability was then compared through the corresponding reexport graph.
All 31 independent `dyld_info -imports` checks matched symbol names and weakness, including the three legacy images and
the six iOS 15.4.1 gap examples.

DeviceSupport's extracted libraries had stripped export tries. Native availability therefore uses externally defined nlist
symbols, excluding debug, local, private-external and undefined symbols. All 27 native support-library inventories were
cross-checked against `nm -gUj`; this remains extracted-symbol evidence rather than an audit of each live cache's export trie.
DriverKit and ExclaveKit images were excluded. The extracted iOS 10.3.3 image is armv7s and is not an arm64 candidate.

The table counts distinct missing symbols used by the scanned imports, not every extra export in the newer Mac libraries.
The narrow case keeps all guest libraries except the three support implementations. The broad case replaces the whole
`/usr/lib/system` family and libSystem, so it counts only consumers outside that replaced family; simulator forwarding
internals that would disappear with it are excluded.
Broad-family counts use the union of native family exports, an optimistic availability test: symbols found in a different
native provider still need correct routing. This does not validate exact native library ordinals for wholesale replacement.

| Extracted native OS/build | Three support libs: missing strong | Missing weak, with stock adapter fallback | Whole system family: missing strong for external consumers |
| --- | ---: | ---: | ---: |
| 15.4.1 / 19E258 | 6 | 14 | 464 |
| 16.5 / 20F66 | 0 | 14 | 315 |
| 17.0 / 21A327 | 0 | 14 | 191 |
| 18.5 / 22F76 | 0 | 5 | 89 |
| 23A239d beta, directory labeled 19.0 | 0 | 5 | 9 |
| 26.1 / 23B50 | 0 | 0 | 2 |
| 26.7.2 / 23H128 | 0 | 0 | 2 |
| 27.0 / 24A435 | 0 | 0 | 6 |

These are binding requirements across the inspected images, not proof every importing image runs during boot or every
function is called. Runtime `dlsym` requests, kernel ABI/operation support, third-party apps and project-built helpers need
their own checks. The native app currently targets iOS 26+, independently of this older-library comparison.

## Which missing functions matter?

The six strong gaps in iOS 15.4.1 are below, using Mach-O symbol spelling. All six are also absent from the aggregate native
system-family symbols; they have not simply moved to another provider.

| Missing symbol | Example original guest consumer |
| --- | --- |
| `_mach_msg2_internal` | libdispatch, IOKit |
| `__platform_memcmp_zero_aligned8` | libsystem_malloc |
| `_mkfifoat`, `_mknodat` | libarchive |
| `_openat_authenticated_np` | trustd, libmecabra, LanguageModeling |
| `_posix_spawnattr_setdataless_iopolicy_np` | loose xpcproxy_sim |

Five are required by images in the selected cache; the xpcproxy_sim import is outside it. These need actual compatibility
implementations or a supported alternate caller path, not dummy success returns.

In contrast, the 14 weak gaps on iOS 15–17 are explicitly handled by the 23A343 simulator adapters:

- Five `os_sync_*` entry points have fallback implementations using `__ulock_wait2` and `__ulock_wake`.
- `os_unfair_lock_lock_with_flags` falls back to the options entry point.
- The three `os_security_config_get*` resolvers select stock unavailable/default results.
- `posix_spawn_file_actions_addchdir/addfchdir` fall back to their `_np` forms.
- `posix_spawnattr_set_use_sec_transition_shims_np` validates the attribute pointer and otherwise returns success without setting the option.
- `_pthread_workqueue_allow_send_signals` and `pthread_attr_setworkinterval_np` have `ENOTSUP` fallbacks.

The five remaining gaps on iOS 18.5/23A239d are the three security-config APIs and the two non-`_np` chdir APIs. The adapter
entry points are export resolvers selecting native or fallback addresses; preserve their resolution behavior when bridging.
Fallback availability does not prove every older kernel operation or caller tolerates the fallback result.

There is also an independent startup gate: stock `libsystem_sim_kernel`'s `_assert_simulator_supported_host` requires
`kern.osrelease` major greater than 22. Its original arm64 instructions at `0x830..0x834` compare against 22 and branch
to a `brk` failure for older values. Thus the zero strong-symbol gaps on iOS 16.5 do not make an unchanged runtime boot there.

Replacing all of libSystem would remove required simulator behavior. Even native 26.1/26.7.2 lack
`__sim_load_entitlements_blob`, used by InstallCoordination/InstalledContentLibrary, and
`_container_codesign_copy_current_identifier`, used by ContainerManagerCommon. Native 27 additionally lacks four
`_ccapsic_client_*` imports required by this guest's CryptoKitPrivate. Keep guest libc, malloc, dispatch, XPC, libdyld and
the simulator adapters for an initial narrow experiment.

## Static compatibility of the current handoff

The 2026-11-03 audit compiled the five decoders from `NativeSupportState.cpp` and ran them on instruction bytes from
each extracted arm64 DeviceSupport image, using the entry's original virtual address. Every passing decoder returned
the addresses named by the image's local symbol table. All nine state cells had local symbols even on the builds
where instruction matching failed. For example, 15.4.1 branches from `__libkernel_init` to a separate `mach_init`
function containing the once guard. The current decoder expects that guard inside the initializer. In all eight arm64
samples the static-key code stores complemented destructors and the native pthread size is consistent with a 224-byte
prefix and 512 TSD slots.

| Native build | Pthread init | Static keys, workqueue, workgroup, kernel guard | Missing signed-cache patch targets (strong / weak-only) |
| --- | --- | --- | --- |
| 15.4.1 / 19E258 | Pass | All four reject | 5 / 14 |
| 16.5 / 20F66 and 17.0 / 21A327 | Pass | All four reject | 0 / 14 |
| 18.5 / 22F76 and beta 23A239d | Pass | All four pass | 0 / 5 |
| 26.1 / 23B50, 26.7.2 / 23H128 and 27.0 / 24A435 | Pass | All four pass | 0 / 0 |

The counts come from the signed guest cache `9a2fb463-a774-53ed-a661-ff3f3fcac717`. Its patch table contains 16,074
pointer locations. [NativeSupportBindings](../LiveSimBooter/NativeSupportBindings.cpp) rejects an unresolved target
at any such location. On 18.5 and 23A239d this includes three weak security-config functions and the two weak file-action
functions `posix_spawn_file_actions_addchdir` and `posix_spawn_file_actions_addfchdir`. Their stock simulator wrappers
have fallbacks, but binding fails before those wrappers can select one. The earlier six-strong count for 15.4.1 includes
one additional import in loose `xpcproxy_sim`; the five here count only cache targets.

Guest spawning has separate constraints. The native 18.5 and 23A239d setters match the 23B50 probe offsets, but store
the dataless I/O policy as 1 or 2 where the current shim accepts 5 or 6. Requests from `xpcproxy_sim` that set this
policy return `ENOTSUP`. On 18.5 the shim also compares four uninitialized bytes at offsets `0xbc..0xbf`, which can
reject otherwise identical attributes. On 16.5 and 17.0 the port-action pointer is at `0xb8`, outside the recognized
layouts. On 15.4.1 the shim's strongly imported `posix_spawnattr_set_crash_count_np` is absent.

The 10.3.3 extraction is armv7s. Both signed native executables specify minimum iOS 26.0. These older-host results
are static only. Device runs cover 23B50 and 24A435. Local receipts are under ignored
`tmp/older-native-static-20261103/`, `tmp/older-binding-audit/` and `tmp/older-spawn-static/`.

## Ownership requirements identified during the audit

1. **Preserve one pthread owner.** The default `MainThreadBootstrap.cpp` path creates fresh storage with a 224-byte prefix and
   768 TSD slots. Extracted native 26.1 pthread uses a 4,320-byte layout with 512 slots. Worker allocations round to
   16 KiB, but this does not enlarge the declared main-thread layout or supported key range. Do not apply the guest initializer to that layout.
2. **Handle initialization and callbacks deliberately.** Guest libSystem calls `__libkernel_init`, `__libplatform_init`
   and `__pthread_init`. They are not interchangeable idempotent initialization calls. Native pthread already owns its
   thread list, token, stack metadata and malloc/free/exit callbacks; libkernel also retains allocator, loader and
   cancellation callbacks. Skipping or repeating initialization has different effects. The relocated guest main stack
   must agree with whichever implementation answers stack-bound queries.
3. **Reconcile fixed TLS cleanup.** Native libdispatch initializes reserved TSD destructors before LiveProcess enters C,
   even before it starts workers. Native 26.1 `pthread_key_init_np` keeps an existing destructor while returning success;
   `pthread_key_delete` accepts only dynamic keys 256–511. Guest dispatch uses the same fixed slots, so simply forwarding
   those calls can leave native cleanup functions handling guest objects. Independently built caches also assign optimized
   TLVs in the reserved 125–209 range; an actual conflicting live assignment was not measured here.
4. **Check workqueue callback ownership.** Native `pthread_workqueue_setup` returns `EBUSY` if its worker callback is already
   installed. The current one-thread guard does not establish that it is unset. Guest dispatch callbacks must be routed to
   the native worker/kevent/workloop entry coherently; treating `EBUSY` as success would leave the wrong callback table.
5. **Preserve guest bindings and behavior.** dyld_sim needs explicit native symbol/data bindings or compatible proxy
   libraries. A matching install name in another loaded cache does not transfer ownership to its loader graph. Existing
   spawn, PID, bootstrap, filesystem and other guest interpositions must still apply. Native-internal cache calls do not
   automatically inherit guest interpositions. Retain the separate process-inspection Mach service when removing the
   workqueue exception path.

ARM64e is not by itself a reason to reject this approach: the arm64 native app already calls the native system libraries.
Native 26.1 pthread's authenticated callback instructions require checking the actual process's calling behavior; header
CPU subtype alone does not establish a failure. The concrete state/initialization conflicts above remain either way.

The resulting experiment is an isolated retained-cache launch that checks native callback/TSD ownership after check-in,
then bridges the three support libraries with explicit initialization and TLS handling. Verify pthread create/join,
errno/cancellation, destructor behavior, ordinary/cooperative dispatch, kevent/workloops, worker reuse and guest hooks
before removing the redirect in that experimental mode. Graphical validation comes after those checks; the current
working runtime keeps its bridge.

## Cache without the Mac implementations

The working cache embeds the actual Mac `libsystem_kernel`, `libsystem_platform`, and `libsystem_pthread`. The simulator
builder opens these three absolute Mac paths independently of its `-root` directory. Its `-skip` argument is parsed but
does not exclude them. Removing their install names entirely would break strong dependencies and transitively evict guest
libraries, including libSystem.

Apple's [B&I simulator cache path](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/mrm_shared_cache_builder.cpp)
instead synthesizes three placeholder dylibs. Every exported name, including data names, has a distinct zero-word function
atom. These satisfy dependency closure and preserve cache override metadata; they are not working functions or data objects.
The ordinary simulator overrides them with standalone Mac libraries from disk. It does not automatically adopt matching
images from a retained native cache. A native-image bridge is still needed here.

The experimental [placeholder-cache builder](../Tools/setup-ios26-placeholder-cache.py) reproduces that structure with the
existing 1,350 pinned guest inputs. It preserves `libsystem_sim_*` and their `_host` reexport adapters. It reads export names
and compatibility versions from the Mac inputs, records their hashes, then generates zero-word placeholders with the local
Xcode compiler. No Mac implementation sections are copied into this cache. The ordinary build tools and production cache
layout remain unchanged.

```sh
python3 Tools/setup-ios26-placeholder-cache.py \
  --runtime-root '/absolute/path/iOS 26.0.simruntime/Contents/Resources/RuntimeRoot' \
  --output-dir tmp/native-support-cache
```

The output directory must be new. Original inputs are hash-checked and independently copied. Besides the existing slidable
cache patches, the private 23A343 builder receives two exact, original-byte-checked changes:

| File offset | Original word | Replacement | Purpose |
| --- | --- | --- | --- |
| `0x18D4` | `0xF000113B` | `0x14000030` | Branch past the three automatic Mac inputs to `loadMRMFiles`' root scanning setup at `0x1994` |
| `0x89B74` | `0x6DB923E9` | `0xD65F03C0` | Return at entry to `optimizeTLVs`, before its prologue |

The latter follows Apple's [B&I TLV policy](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/NewSharedCacheBuilder.cpp):
the eventual host pthread's static key range is unknown at build time. This older builder binary lacks the newer conditional
skip, so changing `filesRemovedFromDisk` alone is insufficient and would also change unrelated build/output policy.
The original builder SHA-256 remains `79e721958b8d733bd943cc550dee7336b9fcafa2e90619f16c36dd2931f396a3`;
the four patched instructions produce `8cc052c9414c9690952d25c2c8b78ba6498cf163cb113f692e180ec6d47d2d3c` before ad-hoc signing the Mac tool.

The full 2026-11-02 build succeeded with **1,353 images**: 1,350 original guest images and three placeholders. Offline validation
confirmed 2,008 distinct zero-word exports (kernel 1,614; platform 185; pthread 209), no placeholder dependencies, undefined
symbols or initializers, and all 1,353 export tries/function-start tables. The result is 2,052,079,616 bytes, UUID
`0B65861E-3746-3C90-B3B4-6F2881D6C80F`, preferred base `0x180000000`, span `0x7E134000`, with 15,154,973 v2 slide fixups
including 40,252 tagged values. The smaller 217-image dependency-cache build also succeeded first.

An independent import/reexport audit found exactly 15,302 expected `(client, support library, symbol)` pairs in the cache
patch tables, and checked all 16,074 recorded support-pointer locations against their target placeholder and addend. This
is complete client/symbol coverage, not an independent count of original pointer locations. All 301 TLV descriptors in
47 images retain zero keys and point to libdyld's `__tlv_bootstrap`; they do not use Mac-assigned fixed keys. Private-copy
negative controls rejected a changed pointer and a nonzero key without changing the cache file. These checks are evidence
for later rebinding, not proof that the native initialization/TSD bridge works.

This raw output is an **offline, non-deployable build artifact**. It has no device identity signature, export reserves, or
later runtime compatibility/alias patches. Its raw mapping layout is not accepted by the production loader profile.
The complete experimental preparation pipeline is below. Keep the working runtime until the replacement's device tests succeed. Local manifests,
generated placeholder assembly, builder logs and validation are under `tmp/native-support-cache-20261102/`.

### Initial inactive iPhone 17 deployment

On 2026-11-02, a separately signed copy was transferred over USB to iPhone18,2 running native iOS 27 / 24A435, under
`LSB/iOS/23A343/NativeSupportCache.experimental-20261102`, beside `RuntimeRoot`. It was **not activated at that stage**. The signed cache is
2,064,072,704 bytes; local CodeDirectory/CMS and all 125,004 code-page checks passed. Device verification checked size, UUID,
header layout and signature extent only, without a post-copy checksum. Device signature acceptance was not tested.

The active cache remained UUID `44EAAA94-7C15-563D-A394-13296CB1AD7A`, and the same native host remained running. No debugger
was attached by this deployment. At that point activation was blocked by the production loader's exact span/layout checks and
the missing native-library bridge; signing and copying alone did not remove either requirement. Receipts are under ignored
`tmp/native-support-deploy17-20261102/`.

## Native-support bridge and device probe

The host discovers the installed cache's exact reviewed profile and prepares the session's shared DATA sidecar. It tries the
original profile first, and retries the native-support profile only for an unsupported-profile result; malformed or unreadable
cache errors remain failures. A matching span alone cannot select a mode. Installing the completed native-support cache
selects native support on ordinary cold launch without extra environment variables. Exact `LSB_NATIVE_SUPPORT=1` in the
**native host's launch environment** remains a diagnostic force-native check: an absent or different cache is rejected.
It is not required for normal use. Bootstrap ABI version 7 carries
the selected base and a host-owned `nativeSupport` boolean through authenticated check-in. Guest requests cannot supply
these fields; LiveProcess removes guest-provided `LSB_NATIVE_SUPPORT` values and writes its own value from that metadata.
Native helper roles receive neither the mode nor a simulator cache base. Rebuild the host, extension and guest shim together.

Cache preparation and mode selection happen before generating the initial launch plan, both for cold starts and userspace
reboots. The host passes the selected mode explicitly to the launchd configuration writer. Both `Bootstrap.Self` and
`Bootstrap.Child` receive matching `LSB_NATIVE_SUPPORT`, cache-retention and workqueue settings, so launchd replacing
its environment cannot restore the original workqueue policy. These generated values describe the host's decision;
guest-supplied environment values cannot choose native support or override its retained-cache/no-breakpoint policy.

The child loader accepts the profile selected through that trusted metadata; it does not independently infer mode from its
environment. Native support retains the host cache and disables the workqueue hardware redirect. The original profile and
its support-library selection remain unchanged. The native-support profile has preferred
base `0x180000000`, span `0x7e14c000`, six reviewed mappings, 15,154,973 slide fixups and 40,252 tagged values. Its completed
alias/compatibility-patched artifact has UUID `9a2fb463-a774-53ed-a661-ff3f3fcac717`; later identity signing preserves that UUID.
Restore the original cache to return to the original support-library mode. Unknown slidable layouts remain rejected.

[NativeSupportBindings](../LiveSimBooter/NativeSupportBindings.cpp) prepares native addresses while native TLS and allocation
remain usable. It resolves the three native support libraries already loaded in this process, preserves the simulator
wrappers, and checks every recorded cache support-pointer location. After the final guest cache mapping exists, app-local
memory helpers replace only 96 KiB of reserved, non-executable export-trie pages with absolute-address exports and update
16,074 cache support pointers in private DATA mappings. Neither signed TEXT nor the retained native cache's export tables
are patched. The reserve operation moves the dynamic-data reservation and extends the three placeholder LINKEDIT segments.
Guest interpose tuples are replayed against these cache pointers before constructors because stock cache interposing does
not cover the newly absolute/native replacees.

[NativeSupportState](../LiveSimBooter/NativeSupportState.cpp) discovers the needed initializer/key/workqueue globals relative to
actual native code addresses. It validates the instructions that identify the state cells and their guard semantics, rather than
matching entire functions. The kernel decoder ignores unrelated prologue/PAC conversion instructions and follows the NULL-apple
branch to its return instead of requiring a build-specific parser length. The reviewed 23B50 variant also accepts the equivalent
reversed static-key comparison. These decoders passed against extracted 23B50, 23H128 and 24A435 code. Each simulator LiveProcess
still checks its own state and VM protections before handoff; unknown state layouts remain errors, and this does not establish
compatibility with every host OS. The bridge keeps the native main
pthread object, stack bounds, kernel-registered pthread/workqueue entries and TSD slots 0–9. Immediately before handoff,
it clears inherited user TSD values without running native destructors, refreshes the native bootstrap-port variable, and
transfers ownership of guest-claimed static-key destructors under the native key lock. Guest pthread initialization updates
only exit/malloc/free callbacks; native late initialization is not repeated. The guarded native kernel initializer receives
guest callbacks without repeating Mach/apple setup. Guest consumers receive a fresh alloc-once table; native platform's
internal ASL accesses retain their separate native table and VM allocator. The ordinary platform initializer remains the
simulator wrapper's implementation.

The workqueue callback must still be unclaimed before activation. Native libdispatch has already installed a workgroup
function table even in a single-threaded LiveProcess; the bridge validates and clears that table once so guest libdispatch
can install its own through the unchanged native API. Later duplicate installations retain the native failure behavior.
Validation uses recursive VM-region queries to reach cache leaf mappings: the outer shared-cache submap's R-- protection
does not describe its executable or writable leaves. The authenticated process-inspection port worker remains available,
but native-support mode installs no workqueue exception mask or hardware redirect.

The guest spawn decoder also needs the actual support library's private attribute layout. The reviewed native 23B50 and
23H128 `posix_spawnattr_init` implementations allocate `0xf8` bytes and put the port-action pointer at `0xc0`; native 24A435
and the original Mac support library allocate `0x100` bytes and put it at `0xc8`. The port-action header and 24-byte records
are unchanged. [SpawnAttributes](../Guest/libLiveSimSpawn/SpawnAttributes.c) identifies these structural profiles using the native crash-count,
CSM and special-port setters on a disposable attribute object. It checks the exact changed fields against
[SpawnAttributeLayout](../Guest/libLiveSimSpawn/SpawnAttributeLayout.h), then destroys the object; no process is spawned and no policy or Mach
rights are applied. The probe reads only the common `0xf8` extent. `malloc_size` bounds access but does not select a layout:
the allocator can round an older `0xf8` request to `0x100`. Only successful discovery is cached, without consulting environment
flags or matching initializer instructions. Architecture preferences use the native getter.
Unsupported-field comparisons retain each recognized layout's initialized ranges, excluding padding and the separately decoded
port pointer; they never read past the older extent. An unrecognized probe or unsupported requested attribute remains an error.
This supports the inspected structural profiles, not arbitrary future attribute layouts.

The 2026-11-03 simplification passed the integrated local suite, 78 focused native-state checks, both builds, and offline
native-code checks for all three inspected builds. On iPhone 15 Pro Max (host 23B50, guest 23A343), the signed build retained
the native cache and mapped guest cache UUID `9a2fb463-a774-53ed-a661-ff3f3fcac717` at `0x300000000` with R-X text.
The thread probe exited 0 with eight pthreads, eight destructors, 96 dispatch jobs, four timer callbacks and zero failures.
Guest launchd started `df` through `xpcproxy_sim`. It printed its filesystem table and exited 0. In the graphical session,
Fish also ran `df -h`, reported status 0 and returned to its prompt. A second graphical boot with terminal diagnostics off
displayed SpringBoard with Maps and Calendar widgets. The final snapshot showed 77 project processes and no debugger.
The Passbook widget extension still trapped in FinanceKit's Swift error path on both boots. The same stack was recorded on
the previous build. Receipts and screenshots are under ignored `tmp/native-support-simplify-20261102/`.

### Preparing the experimental cache

Use the setup guide's `LSB_WORK`, `LSB_RUNTIME_SOURCE`, signing and bundle variables. Keep the source runtime immutable and
use a new experiment directory; the commands below do not activate or deploy anything. Check free space before the full build.

```sh
LSB_NATIVE="$LSB_WORK/native-support"
mkdir -p "$LSB_NATIVE/aliases" "$LSB_NATIVE/patched"
python3 -B Tools/setup-ios26-placeholder-cache.py \
  --runtime-root "$LSB_RUNTIME_SOURCE" --output-dir "$LSB_NATIVE/build" > "$LSB_NATIVE/build-result.json"
python3 -B Tools/prepare-native-support-exports.py \
  "$LSB_NATIVE/build/unsigned/dyld_sim_shared_cache_arm64" \
  "$LSB_NATIVE/reserved/dyld_sim_shared_cache_arm64" > "$LSB_NATIVE/reserves.json"
python3 -B Tools/patch-ios26-cache-aliases.py --native-support \
  --source "$LSB_NATIVE/reserved/dyld_sim_shared_cache_arm64" \
  --output "$LSB_NATIVE/aliases/dyld_sim_shared_cache_arm64" > "$LSB_NATIVE/aliases.json"
python3 -B Tools/patch-ios26-slidable-cache.py --native-support \
  --source "$LSB_NATIVE/aliases/dyld_sim_shared_cache_arm64" \
  --output "$LSB_NATIVE/patched/dyld_sim_shared_cache_arm64" > "$LSB_NATIVE/compatibility.json"
python3 Tools/sign-dyld-cache.py "$LSB_NATIVE/patched/dyld_sim_shared_cache_arm64" \
  "$LSB_NATIVE/signed/dyld_sim_shared_cache_arm64" \
  --identity "$LSB_SIGNING_IDENTITY" --identifier "$LSB_BUNDLE_ID" --team "$LSB_TEAM" > "$LSB_NATIVE/signing.json"
```

Each patcher validates its original inputs and resulting output; the signer separately verifies its final CodeDirectory,
CMS signature and code pages. The patchers' `--check-only` option checks a proposed patch against an unpatched input;
it is not a post-sign validation command for an already patched cache.

Use the matching slidable `dyld_sim` from [the setup guide](agent-setup.md#optional-slidable-cache), the rebuilt native app,
and the matching guest shim. Stop all affected guests before manual cache/shim activation, stage files in an inactive
location and keep the working runtime available for rollback. The host generates or reuses a sidecar for the cache UUID
and selected session base; a sidecar for another profile, UUID or address is rejected.

Direct executable tests also require `SIMULATOR_ROOT` and `SIMULATOR_SHARED_RESOURCES_DIRECTORY`; the normal launchd plan
supplies them, while direct probe mode does not. After discovering the current device App Group path as described in
[the setup guide](agent-setup.md), a staged probe can be launched with:

```sh
DEVICECTL_CHILD_SIMULATOR_ROOT="$LSB_DEVICE_STORAGE/RuntimeRoot" \
DEVICECTL_CHILD_SIMULATOR_SHARED_RESOURCES_DIRECTORY="$LSB_DEVICE_STORAGE/data" \
xcrun devicectl device process launch --device "$LSB_DEVICE" --terminate-existing --console \
  -- "$LSB_BUNDLE_ID" /usr/local/bin/native-support-probe
```

The probe is a development artifact, not an installed-runtime requirement. The environment's paths must be real device
paths, not the Mac source runtime or a historical App Group UUID. Diagnostic `LSB_TRACE_HANDOFF=1` is optional;
`DEVICECTL_CHILD_LSB_NATIVE_SUPPORT=1` can also be added when a probe must reject any cache other than the native-support profile.

### iPhone 17 result

The 2026-11-02 v7 probe ran on iPhone18,2, native iOS 27.0/24A435, using guest 26.0/23A343 and the completed UUID above at
actual base `0x7000000000`. No LLDB was attached and no workqueue hardware breakpoint was installed. The guest printed:

```text
NATIVE_SUPPORT_PROBE PASS pthreads=8 destructors=8 dispatch=96 timer=4 failures=0
```

The pthread phase checked create/join, errno, TLS, allocation and destructor delivery. Three dispatch rounds completed
32 jobs each and observed worker reuse; timer/cancellation checks also passed. The broker independently recorded guest
wait status `0x0` and completion error `0`. The console's later signal 15 belongs to the native host being stopped after
the probe, not the guest exit. Cache TEXT was observed R-X at the selected address. The allocator reported its nano zone
could not reserve space and fell back; the allocation checks still passed.

The subsequent launchd-managed `df` test produced the real filesystem table in `df.log` and exited successfully: logical
PID 4179, native PID 4180, wait status `0x0`. These PIDs identify this run only. This checks the guest launchd/spawn route in
addition to the direct thread probe.

The first full-boot stall exposed missing guest interposition: live cache slots still pointed to native functions, including `getpid`.
The bridge initially recognized only `S_INTERPOSING`, while the guest shim's named `__interpose` section uses `S_REGULAR`.
Recognizing that section and replaying its tuples before constructors restored the guest behavior. The v8 full boot then
displayed SpringBoard's Home Screen with Maps and Calendar widgets, captured in
`tmp/native-support-integration-20261102/springboard-v8.png`. Guest `launchctl print` for SpringBoard returned status 0.
Two LLDB inspections during diagnosis used hardware-only breakpoints, disabled parallel module loading and waited for the
actual stopped notification. Both were detached, resume was verified, and no owned debugserver remained afterward.

The final build repeated the direct probe after the interpose correction and automatic profile selection, without setting
`LSB_NATIVE_SUPPORT` in the host environment. Native PID 4713 again completed all checks and exited 0. The user verified
Contacts search and Fish running `df` in the graphical session. A subsequent normal auto-start used the installed cache
profile without an environment override; `session-final.png` shows the populated Home Screen. The final process snapshot
contained 77 project processes, none traced, and no debugserver. The bounded final AMFI/TXM capture contained no signature
rejection or invalid-page event for the setup. `sh Tests/run.sh`, the signed app build and `git diff --check` passed.

A separate `launchctl submit` attempt returned 126, while `bootstrap` accepted a probe job without producing the expected
output; neither attempt is counted as a completed probe run. At this stage only the inspected 24A435 native ABI had been
accepted and tested; the subsequent iPhone 15 work is recorded separately below. All possible native internal call paths
remain unverified.

Evidence is under ignored `tmp/native-support-integration-20261102/`, including `probe-final-auto-console.log`, `tests-final.log` and the v8 screenshot,
with cache preparation and independent binding/TLV validation under `tmp/native-support-profile-20261102/`. These are
historical receipts, not setup dependencies.

### iPhone 15 result and remaining validation

The 2026-11-02 test used iPhone16,2 with native iOS 26.1/23B50 and the same pinned 26.0/23A343 guest. The installed cache
was UUID `9a2fb463-a774-53ed-a661-ff3f3fcac717`, selected at `0x300000000`. The native cache remained mapped and no
workqueue hardware redirect was installed. Without LLDB, the thread probe again reported eight pthreads, eight destructors,
96 dispatch jobs, four timer callbacks and zero failures; its wait status was 0 and cache TEXT was observed R-X.

The first launchd boot failed in `xpcproxy_sim`. An LLDB hardware breakpoint caught `posix_spawn(/bin/df)` returning
`ENOTSUP` (45). A bounded spawn trace located the rejected byte at attribute offset `0xc0`: the older library's port-action
pointer was being compared with a modern layout's default fields. The layout-specific decoder described above fixes both
that pointer offset and the old allocation/padding bounds. After correction, the complete launchd → xpcproxy_sim → `df`
route produced its filesystem table and exited 0. The diagnostic LLDB session was detached after inspection.

The full session then reached SpringBoard with Home Screen widgets, captured in
`tmp/native-support-iphone15-20261102/springboard-final.png`. The user confirmed Contacts and its keyboard work.
Fish's first cold-start terminal helper returned `ENOTTY`; restarting the ordinary launchd-owned helper restored the
terminal and `df` exited 0. A temporary diagnostic confirmed simulator platform 7, all three terminal descriptors, and the
shared state layout. It was removed and the original helper restored. The final shim then started Fish on two subsequent
cold boots and after `launchctl reboot userspace`; the user confirmed the prompt, typing/deleting and `df` work normally.
The original failed guard remains unknown, so these later passes do not establish a specific fix for the transient `ENOTTY`.

The first reboot command was submitted over SSH as root and left a root-owned mode-0700 control lock, which the host could
not inspect during cleanup. Its interrupted request was preserved and ownership repaired. Retrying through SSH as `mobile`
produced a fresh session with the same cache base, native-support policy, SpringBoard and Fish. Use the app's filesystem user
as described in [guest control](guest-control.md); this was a test-client ownership error, not a native-library failure.

The final integrated local suite, signed host/guest builds and `git diff --check` passed. The final process snapshot contained
78 project processes, none traced, and no debugserver. The original cache, shim and native app remain available on this
device for rollback. Native 23H128 initializer and spawn-layout checks used extracted libraries only;
no 23H128 device boot is claimed. Other native builds require their own decoder review and device validation.

Device evidence is under ignored `tmp/native-support-iphone15-20261102/`, including `progress-receipt.json`,
`probe-console.log`, `dfboot-passed-console.log`, `tests-final-v2.log`, `device-after-reboot.json` and the Home Screen screenshot.
These receipts are not setup dependencies.

## Evidence

Local audit scripts, inventories, caller lists, tool cross-checks and disassembly are under ignored
`tmp/native-libsystem-20261102/{exports,imports,pthread,wrappers}`. They read supplied original/extracted files and are
investigation artifacts, not a fresh-setup dependency.

The native 26.1 disassembly corroborates the key ownership checks: `pthread_workqueue_setup` at original cache VA
`0x1dfca21d4` returns `EBUSY` on an existing callback; `pthread_key_init_np` at `0x1dfca2424` skips an existing destructor
at `0x1dfca2464`; `pthread_key_delete` at `0x1dfca90fc` restricts deletion to dynamic keys. These are historical unslid
addresses for the inspected build, not patch offsets for other versions.

Apple's [kernel pthread registration](https://github.com/apple-oss-distributions/libpthread/blob/main/kern/kern_support.c)
rejects repeat registration before replacing the entry PCs.
[pthread initialization and workqueue setup](https://github.com/apple-oss-distributions/libpthread/blob/main/src/pthread.c),
[TSD ownership](https://github.com/apple-oss-distributions/libpthread/blob/main/src/pthread_tsd.c),
[dispatch initialization](https://github.com/apple-oss-distributions/libdispatch/blob/main/src/queue.c), and
[libSystem initialization](https://github.com/apple-oss-distributions/Libsystem/blob/main/init.c) explain the surrounding
design. These upstream sources support the architecture; the inspected binaries provide the build-specific evidence.
