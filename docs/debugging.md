# Device debugging

The command-line procedure below grew from device tests beginning on 2026-10-24; historical evidence is labeled with its date.
Run from the repository root with Xcode's command-line tools selected. Unlock the iPhone and keep it unlocked for launch/attach.
Use the current device identifier from `xcrun devicectl list devices` and the signed artifacts prepared by [agent setup](agent-setup.md).

```sh
DEVICE='CURRENT_COREDEVICE_IDENTIFIER'
APP='/absolute/path/to/packaged/LiveSimBooter.app'
LSB_SELECTED_APP_GROUP='ACTUAL_EXPANDED_STORE_GROUP_IDENTIFIER'
LSB_SIGNED_DYLD='/absolute/path/to/signed/dyld_sim'
LSB_SIGNED_CACHE='/absolute/path/to/signed/dyld_sim_shared_cache_arm64'
mkdir -p tmp/device-log
```

## Install and transfer

The host and LiveProcess extension request `com.apple.developer.kernel.increased-memory-limit` through their respective
entitlements files. Each target needs a provisioning profile that grants it; inspect both final signed entitlements before installation.
The extension's profile was refreshed through a separate app target using the same extension bundle identifier, since Xcode's
extension capability UI did not offer it. The refreshed profile and the installed extension signature both contain the key
on the 2026-10-26 iOS 26.1 device test. Refreshing a profile alone does not add the key to this project's signed executable.
It is separate from Extended Virtual Addressing; verify the intended VM range under LLDB instead of assuming that range is available.

Install a normally signed development build of the host app. Compilation without signing is useful for checks but does
not produce an installable device build. Keep the local executable and dSYM matching the installed app for source debugging.
Both native targets must have the store-group entitlements/profiles described in [setup](agent-setup.md). Discover the selected
group before transferring; `LSB_SELECTED_APP_GROUP` is its actual signed identifier, not a container UUID. Stop the host and all
guests before replacing mapped code. The examples below copy into prepared inactive directories under `LSB/iOS/23A343`:

```sh
xcrun devicectl device install app --device "$DEVICE" "$APP" --json-output tmp/device-log/install.json

xcrun devicectl device copy to --device "$DEVICE" --source "$LSB_SIGNED_DYLD" \
  --destination LSB/iOS/23A343/RuntimeRoot.incoming/usr/lib/dyld_sim \
  --domain-type appGroupDataContainer --domain-identifier "$LSB_SELECTED_APP_GROUP" --json-output tmp/device-log/dyld-copy.json

xcrun devicectl device copy to --device "$DEVICE" --source "$LSB_SIGNED_CACHE" \
  --destination LSB/iOS/23A343/RuntimeRoot.incoming/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64 \
  --domain-type appGroupDataContainer --domain-identifier "$LSB_SELECTED_APP_GROUP" --timeout 3600 --json-output tmp/device-log/cache-copy.json
```

Transfer by the selected App Group domain instead of hardcoding a container UUID; reinstalling can change it. These two files
alone do not form a complete replacement runtime. Preserve the current runtime and publish the intended complete replacement
using [the inactive-transfer procedure](migration.md#transfer-to-inactive-directories). There is no Documents migration.
File transfer does not establish signature acceptance; capture the next launch.

## Capture AMFI and TXM

### USB SSH and incremental transfers

The historical iOS 26.1 trial used SSH on device port 2222 and this loopback-only USB forward. Discover the current phone and
SSH account/port using [the migration runbook](migration.md) before reusing its command shape:

```sh
iproxy -l -u 00008130-00021D122680011C 2223:2222
```

Use `ssh -p 2223 root@127.0.0.1` for commands and `rsync` over that same SSH connection for payloads. Both the Mac and
phone have protocol-29-compatible openrsync. The 2026-10-26 trial deployed the production shim through this route and
verified its SHA-256; a checksum synchronization of the exact 1,826-file signed SpringBoard payload had no remaining
file differences. The local source tree must contain the signed executable, not its original unsigned input.

For a new payload, select an inactive directory inside the selected group's `LSB/iOS/23A343` directory, then use the verified
SSH connection. This example uses placeholders for that connection and physical storage directory:

```sh
LSB_DEVICE_STORAGE="$LSB_DEVICE_GROUP/LSB/iOS/23A343"
rsync -rlpt --partial --itemize-changes -e "ssh -p $LSB_SSH_PORT" local-payload/ \
  "$LSB_SSH_DEST:$LSB_DEVICE_STORAGE/RuntimeRoot.incoming/"
```

The trailing slash copies the directory contents. Keep existing RuntimeRoot files until the transfer succeeds, then activate
with the relevant guest processes stopped. Current transfers skip a post-copy checksum pass; validate signed inputs locally.
Do not add `--delete`, `--checksum`, or `--inplace` to a partial runtime
payload. A cache already transferred to the phone can be activated by a local rename; it need not be downloaded and uploaded again.
Evidence is in `tmp/lsd-services/rsync-usb-shim-deploy.log` and `rsync-usb-springboard-final.log`.

### Log stream

The test device exposes SSH on port 2222. Start capture before launch in a separate terminal and stop it after the process exits.
The SSH options below match this test setup and do not record its host key in the Mac's known-hosts file.

```sh
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2222 root@iphone-0x11-cua-duy.local \
  'log stream -p amfid' > tmp/device-log/amfid.log 2>&1
```

For kernel TXM failures as well, use this broader capture instead:

```sh
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2222 root@iphone-0x11-cua-duy.local \
  > tmp/device-log/security.log 2>&1 <<'REMOTE'
log stream --style compact --level debug --predicate \
  'process == "amfid" OR (process == "kernel" AND (composedMessage CONTAINS[c] "TXM" OR composedMessage CONTAINS[c] "AMFI"))'
REMOTE
```

An AMFI profile success is only one stage. Also check `F_ADDFILESIGS_RETURN`, actual mapping permissions, and the eventual
execution result. A successful `mmap` return can still be `r--` when TXM refused executable association.

## Simulator dyld diagnostics

Simulator dyld diagnostics are disabled by default. To enable them, include the desired variables in the environment
passed to the guest launch. For example:

```text
DYLD_PRINT_TO_STDERR=1
DYLD_PRINT_LIBRARIES=1
DYLD_PRINT_SEGMENTS=1
DYLD_PRINT_INITIALIZERS=1
DYLD_PRINT_LOADERS=1
DYLD_PRINT_SEARCHING=1
DYLD_PRINT_APIS=1
DYLD_PRINT_NOTIFICATIONS=1
```

They show cache and segment mapping, dependency searches, loader selection, initializers, API calls, and image
notifications. `DYLD_PRINT_TO_STDERR=1` sends output to stderr, visible in Xcode's debug console or `devicectl --console`.
The loader preserves explicit diagnostic settings, including `DYLD_PRINT_TO_FILE`, without forcing an output destination.
Native environment strings are untouched. Dyld still applies the real AMFI policy's `allowEnvVarsPrint` gate.

These flags are presence switches: assigning `0` still enables them. To disable a category, omit its variable from the
guest environment. `DYLD_PRINT_BINDINGS=1` logs every fixup. The first loose-library launch with that flag emitted
867,477 console lines (49 MB) and ended with SIGKILL before `df` ran. The capture does not establish the kill's cause.

An earlier device test with diagnostics enabled on 2026-10-24 (PID 5630) printed 2,983 console lines and ran `df` to exit `0` after host-cache removal.
The log shows the working cache at `0x120000000` and loose libraries overriding cached images; this was not a cache-only test.
The concurrent SSH capture contained no AMFI signature rejection or TXM failure. Runtime files were preserved during the
in-place app update. Evidence is in `tmp/dyld-diagnostics/console.log`, `launch.json`, and `security.log`; the earlier verbose
attempt is retained as `bindings-console.log` and `bindings-launch.json` in the same ignored directory.

## Launch stopped and attach LLDB

Do not provide `DYLD_SHARED_REGION=private` through `--environment-variables`, `DEVICECTL_CHILD_DYLD_SHARED_REGION`, or the
app's native launch scheme. It affects native dyld before `main()` and caused a separate pre-handoff failure.

In terminal one, launch stopped with console capture. This command remains running until the app exits:

```sh
xcrun devicectl device process launch --device "$DEVICE" --terminate-existing --start-stopped --console --timeout 600 \
  --json-output tmp/device-log/launch.json -- com.kdt.LiveSimBooter > tmp/device-log/console.log 2>&1
```

In terminal two, find the live `LiveSimBooter` PID, then open LLDB:

```sh
DEVICE=00008150-0006505C1428401C
xcrun devicectl device info processes --device "$DEVICE" --json-output tmp/device-log/processes.json
xcrun lldb --no-lldbinit
```

Use the current CoreDevice identifier and PID in the following LLDB commands. The UUID shown is from the recorded test,
not a required constant. `target create` must point at the executable matching the installed app.

```text
settings set -g target.require-hardware-breakpoint true
settings set -g target.parallel-module-load false
settings set -g target.memory-module-load-level partial
target create --arch arm64 tmp/device-build/Build/Products/Debug-iphoneos/LiveSimBooter.app/LiveSimBooter
device select 25FDAD7E-026E-51E2-9C23-60AD659ABFCE
device process attach -p PID
```

**Wait for LLDB's asynchronous `Process PID stopped` notification before entering any further command.** An early `(lldb)`
prompt does not establish that attach has completed. Do not queue `bt`, `image list`, expressions, or even Python
`GetState()` polling during this interval. In the 2026-10-26 lsd investigation, entering Python while attachment was still
initializing caused `ScriptInterpreterPythonImpl::EnterSession` to select a frame, create the Objective-C runtime and block
in `AppleObjCRuntime::GetObjCVersion` on a module mutex. This was a debugger-side lock stall, not a guest crash.
The sample is `tmp/lsd-services/lldb-attach-sample.txt`.

Set serial module loading and partial in-memory module loading before creating/attaching the target, as above. They reduce
module-loading contention but do not make it safe to issue commands before the stopped notification. Keep the global
hardware-breakpoint requirement as well, including for debugger-created internal breakpoints. Once attachment reports the
process stopped, ordinary inspection and the Python value API below may be used. For example:

The current Apple LLDB (`lldb-2103.0.34.103`) exposes **`target.parallel-module-load`**. The spelling
`target.experimental.parallel-module-loading` is silently accepted without changing it; `settings show target.parallel-module-load`
must report `false`. A later PosterBoard attachment again blocked in ModuleList/language-runtime initialization when `process status`
was entered before the asynchronous stopped notification. That command is not safe polling during attach. Terminating only that
known LLDB client detached and resumed the guest; no guest-code crash was established by the debugger stall.

Always detach and verify the target resumed before finishing device work, and confirm your debugserver session ended. Do not leave
the host or a guest stopped/attached after a diagnostic task. If the client is hung, terminate only your own identified debugger
and independently check the device state, preserving unrelated debug sessions.

```text
breakpoint set -n 'lsb::mmap' -c 'location == (void*)0x120000000'
breakpoint set -H -a 0x129d4ff78
continue
```

At the first cache mapping, inspect the **whole** proposed range `[0x120000000, 0x135508000)` before continuing into `MAP_FIXED`.
`memory region 0x120000000` must report an unmapped region extending beyond the end; otherwise walk each returned region
until the end and stop the test if any is mapped. Do not infer the full range is free just from its first page.

```text
memory region 0x120000000
bt 8
breakpoint disable 1
continue
memory region 0x120000000
memory region 0x12b2f3fff
```

At cached `libSystem_initializer`, both TEXT endpoints were in the same `r-x` region starting at `0x120000000`.
The initializer's absolute address is specific to the recorded cache; recompute it after changing inputs.
Set the global hardware requirement before target creation and attach, not just `-H` on individual user breakpoints:
LLDB also creates internal breakpoints. Hardware resources are limited; check insertion errors and reduce active breakpoints
if necessary rather than assuming every requested breakpoint was installed.

```text
image list -o -f
breakpoint set -H -s libsystem_c.dylib -n getmntinfo
continue
bt
continue
```

LLDB should recognize simulator libraries automatically through the app's debugger bridge. Do not manually inject modules
when evaluating whether that bridge works. Both the mixed cache/loose run and the later cache-only run listed 49 named modules.
In the cache-only run, all 46 dependencies had cache-backed image headers and addresses. The legacy image array had 48 entries;
dyld_sim is published separately. Module count alone therefore does not distinguish cached dependencies from loose overrides.

The cache-only run also hit `__pthread_init` at `0x12b2688d8` and `getmntinfo` at `0x12b09e984`. These addresses belong to
the recorded cache UUID. All 219 loose dylibs had been moved outside RuntimeRoot for that test and were restored afterward.
Reproduce that distinction with a recorded move manifest and restoration step, rather than assuming the currently installed
runtime has no loose files.

To read published metadata without executing functions in the target, use LLDB's Python value API:

```text
script target = lldb.debugger.GetSelectedTarget()
script info = target.FindFirstGlobalVariable('sActiveProcessInfo').Dereference()
script print(hex(info.GetChildMemberWithName('sharedCacheBaseAddress').GetValueAsUnsigned()))
script print(info.GetChildMemberWithName('processDetachedFromSharedRegion').GetValueAsUnsigned())
script host = target.FindFirstGlobalVariable('sBridge').GetChildMemberWithName('hostSnapshot')
script print(hex(host.GetChildMemberWithName('dyldImageLoadAddress').GetValueAsUnsigned()))
```

Use `memory region` on that saved native dyld address after handoff; the successful test showed `---`.
The debugger metadata pages can remain readable independently, so readable metadata does not imply the host cache is still loaded.
For image-source checks, cached Mach-O headers carry `MH_DYLIB_IN_CACHE` (`0x80000000`); count those separately from total LLDB modules.
Avoid calling host libc, pthread, or Objective-C expressions after cache detachment.

## Compact image format compatibility

The debugger reads native dyld's compact archive through the device's own reader, so the archive must match that reader even
when the loaded libraries come from a different simulator runtime. iOS 26.1 uses an Apple Archive `CKSF` field with a POSIX
`cksum`; newer hosts use a `com.apple.dyld.crc32c` extended attribute. Sending the newer representation to the tested iOS 26.1
reader produced `aarDecoderError.badMagic(34)` and LLDB's `Unable to create dyld_process_info`. Using `CKSF` everywhere is not a
compatible workaround: the newer task-reading path requires the CRC32C attribute.

The bridge now validates native dyld's existing archive before removing its cache and retains the detected encoding. Later
simulator image notifications use the matching encoding with app-owned storage. Unsupported or malformed native metadata
fails preparation before detachment. The compact descriptor stays populated; clearing it previously left the legacy reader
with only three scavenged images and did not provide complete simulator discovery.

Validate this behavior with a fresh attach after the guest reaches its `--debug-delay` interval. Select the current device and
attach to the simulator probe's `LiveProcess` PID, without a pre-created module target, `target modules add`, or descriptor
edits. Keep the global hardware-breakpoint setting enabled before attach:

```text
settings set -g target.require-hardware-breakpoint true
settings set -g target.parallel-module-load false
settings set -g target.memory-module-load-level partial
device select CURRENT_COREDEVICE_IDENTIFIER
device process attach -p CURRENT_SIMULATOR_PID
```

Wait for `Process CURRENT_SIMULATOR_PID stopped`, then inspect:

```text
image list -o -f
breakpoint set -H -n LSBKill
breakpoint set -H -n LSBTransportKill
breakpoint set -H -n finishWait
```

On the replacement iOS 26.1 phone, the fresh attach selected `spawn-probe`, listed 48 modules, and resolved these named
breakpoints automatically. The native-cache range was inaccessible, and the rebuilt simulator cache retained executable
TEXT with zero dirty pages. The [multiprocess results](multiprocess.md#ios-26-automatic-image-discovery-2026-10-25) record the
process IDs, completion checks, and transcript. Keep the cached runtime and local shim/probe symbols matched when repeating
the test; image count alone does not verify that every dependency came from the cache.

### Larger SpringBoard image snapshots

The SpringBoard closure exceeded the original **1,024-image** compact snapshot limit. Native SpringBoard **97190** failed
with `Cannot publish debugger image snapshot` before its debug constructor. `kCompactImageInfoMaxImages` is now **2,048**
distinct images, including dyld_sim; the serialization buffers and object-reference tables derive their limits from that
one constant. Overflow still returns `E2BIG` without replacing the previous valid snapshot or advancing its initialization
state. This limit controls debugger metadata publication, independently of the cache's mapped size.

Focused tests cover 1,025, 1,353 and 2,048 images in both native archive formats, including 32 segments per image, long
Unicode paths, independent CoreFoundation decoding, checksum validation, and recovery after overflow. The worst tested
archives occupy 15,337,882 bytes (POSIX) and 15,337,914 bytes (CRC32C). The two snapshot buffers plus metadata reserve
36,610,816 bytes of static storage; that is a storage bound, not a measurement of resident dirty pages.

After installing the larger-capacity build, native SpringBoard **97287** passed snapshot publication and reached the
temporary `springboard-debug` constructor stop before main. This verifies progress past the metadata limit, not SpringBoard
boot or full image-discovery correctness. Evidence is `tmp/lsd-services/springboard-capacity-guest.log` and
`console-springboard-capacity.log`. The full `bash Tests/run.sh` suite passed with native IOSurface/Metal access after the
stack and capacity changes, including **74,434** compact metadata checks; evidence is `tests-integrated.log` in the same
directory. The initial sandbox-only run failed at `IOSurfaceLock` and is preserved separately as
`tests-integrated-sandbox.log`.

## Internal breakpoint investigation

LLDB temporarily enables pthread thread-creation breakpoints while stepping over its dyld notification breakpoint.
`breakpoint list -i` can show those locations disabled at a later stop even though LLDB already inserted and removed them.
The default debugger therefore writes cache code even when all explicit user breakpoints use `-H`.

Enable protocol and breakpoint logging before target creation/attach when investigating this behavior:

```text
log enable -T -p -f tmp/device-log/packets.log gdb-remote packets
log enable -T -p -f tmp/device-log/breakpoints.log lldb break dyld process
settings set -g target.require-hardware-breakpoint true
```

Packet timestamps are Unix epoch seconds from the Mac; convert them to the device log's timezone before comparing events.
Mac/device clock skew was not measured in these runs, so converted timestamps alone cannot establish precise cross-device ordering.
Look inside `jMultiBreakpoint` batches as well as individual packets. `Z0`/`z0` insert/remove software breakpoints;
`Z1`/`z1` insert/remove hardware breakpoints. Verify the corresponding replies, rather than inferring installation from a breakpoint list.

The controlled 2026-10-24 comparison observed these cached pthread locations:
`0x12b269a74`, `0x12b26ab5c`, and `0x12b26e618`.

| Run | Actual internal breakpoint requests | Cache TEXT dirty pages | TXM errors |
| --- | --- | --- | --- |
| Default internal breakpoints, PID 4631 | Three `Z0` requests followed by `z0`; all `OK` | Two pthread pages | None |
| Global hardware requirement, PID 4638 | `Z1`/`z1` at all three hooks, two cycles; all `OK` | Zero | None |
| Full default replay, PID 4645 | Internal `Z0` insertions succeeded | The same two pthread pages at all three stops | None |

The software insert/remove cycle occurred at `12:28:37.279–.286 +0700`; the first hardware cycle at `12:32:38.911–.918 +0700`.
Both the hardware run and full default replay reached cached `libSystem_initializer`, `__pthread_init`, and `getmntinfo`,
with all 46 dependencies cached and 49 recognized modules. The hardware run kept TEXT clean at all three stops; the default
replay retained the same two dirty pthread pages. Both exited `0`, with no AMFI rejection. The global hardware requirement
therefore avoids the observed debugger writes into cache TEXT.

None of the three runs reproduced the earlier `TXM selector: 38 | 23` event. The dirty-page difference does not prove those
writes caused that event. The first traced default run omitted the original run's later pthread and `getmntinfo` stops;
the full default replay included those stops and still produced no TXM error.
These tests used fresh processes with the same installed cache file; they did not compare fresh file objects or cold boots.
The original kernel event does not identify the affected PID, and its LLDB session had no packet capture.
All 219 loose dylibs were restored and independently checked against the move manifest after testing.

The comparison is summarized in `tmp/txm-breakpoints/result.json`. Evidence is in `tmp/txm-breakpoints/baseline/`, `tmp/txm-breakpoints/hardware-only/`, and `tmp/txm-breakpoints/baseline-full/`,
including packet, breakpoint, LLDB, and security logs. These experiment artifacts remain outside Git.

The subsequent [original-base retest](shared-cache.md#original-base-hardware-only-retest) reproduced `TXM 38 | 23` with
zero software-breakpoint requests and zero dirty TEXT pages. At `0x180000000`, `mmap` still returned `r--`; execution faulted
in cached `LibSystemHelpers::version()` before the cached images or pthread hooks were published. The restored cache at
`0x120000000` then ran df successfully. This separates the reproducible original-base mapping failure from the earlier,
unreproduced relocated-cache event.

## Confirm a normal launch

After the LLDB run, repeat without `--start-stopped` or an attached debugger:

```sh
xcrun devicectl device process launch --device "$DEVICE" --terminate-existing --console --timeout 60 \
  --json-output tmp/device-log/normal-launch.json -- com.kdt.LiveSimBooter /bin/df > tmp/device-log/normal-console.log 2>&1
```

Check the filesystem-table output and `result.terminationResult.exitCode` in both launch JSON files. A successful launch request
alone does not mean `df` completed. Record any AMFI/TXM rejection and crash alongside the exit status.

The completed cache-only test exited `0` in both modes. Its LLDB capture contained one `TXM selector: 38 | 23` event at
`12:20:51.144`; cached pthread pages were subsequently executable/dirty and internal LLDB thread-creation breakpoints were
present. The cause of that event was not established. The fresh normal-launch capture contained no TXM errors or AMFI
rejections. Keep those two observations separate when comparing future runs.

Early host `dlopen` experiments used `--probe-dlopen-dyld-sim` to print the handle and `dlerror()` before custom mapping.
That diagnostic argument is no longer implemented; its historical result is not a substitute for testing the custom loader.

## SpringBoard attach over Wi-Fi

On 2026-10-26, LLDB itself overflowed its stack while attaching to SpringBoard's simulator images. Its Objective-C runtime
initialization attempted `CanJIT`, memory allocation and frame unwinding, which recursively initialized the same runtime.
Set this before attaching to avoid that observed recursion while retaining ordinary stack unwinding:

```text
settings set -g target.process.disable-language-runtime-unwindplans true
```

With this setting, the hardware-only Wi-Fi attach to native SpringBoard PID 2555 completed. Its main thread was in
`mach_msg`, CoreFoundation, GraphicsServices and `UIApplicationMain`, above `SBSystemAppMain` and `start_sim`.
LLDB recognized TextInputUI and both SpringBoard images; detach resumed the guest normally. Loading the large image set
from process memory still took several minutes, and Objective-C expression support was not validated. This workaround
does not change the app or simulator binaries. Evidence: [successful transcript](../tmp/lsd-services/lldb-wifi-textinputui-mainloop.txt)
and [debugger crash analysis](../tmp/lsd-services/lldb-runtime-recursion.md).

A later Siri-shader investigation encountered two CoreDevice device-selection timeouts. Direct attachment to SpringBoard
**3374** through an SSH-forwarded research debugserver connected, but initial simulator-image preloading continued for more
than ten minutes. The [Mac LLDB sample](../tmp/siri-shaders-stage/lldb-ssh-connect.sample.txt) records this image-loading path;
the trial obtained no shader breakpoint values or shader-creation result. A connected remote debugger is not equivalent
to a completed, usable initial image load.

That owned trial was cleaned up: host **3351** was terminated, remote debugserver **3401** stopped, and LLDB exited.
The production shim from `tmp/lsd-services/libLiveSimSpawn-extensionkit-cdhash.dylib` was restored as RuntimeRoot's
`usr/lib/libLiveSimSpawn.dylib`, with SHA-256
`7c5379a864d9e4338a338e3eecfe99a1e0882525672fd55a635e9d2e05d817c9` verified. See the
[restoration log](../tmp/siri-shaders-stage/restore-production-wifi.log). Subsequent host **3450** runs used the production
shim; their standalone Siri resource inspection and continuing SpringBoard assertion are recorded in
[SpringBoard services](springboard-services.md#feature-flag-gate-isolation).

The later scoped diagnostic in SpringBoard **3722** captured the actual Metal error without waiting for LLDB's image
preload. It preserved the original method arguments/results and exposed the NSError discarded by a NULL error argument:
the default library loaded, but `siriChipVert` specialization failed because MTLCompilerService lookup returned error
**3, "No such process"** (`MTLLibraryErrorDomain` code 3). This observation came from the temporary diagnostic wrappers,
not the unsuccessful **3374** shader-breakpoint attempt. See the
[captured error](../tmp/siri-metal-diagnostic/guest-wifi.log) and [scope/ownership checks](../tmp/siri-metal-diagnostic/README.md).

The following production run used the compiler-registration fix without the diagnostic wrappers.
Host **3884** / SpringBoard **3908** launched compiler processes **3925** and **3928**, passed the previous shader assertion,
then failed at required loading of the missing **AutoFillUI.framework** executable. The
[production guest log](../tmp/siri-metal-diagnostic/guest-production-fix-wifi.log) includes older appended output: evaluate
the section beginning `pid=3908`. The [host console](../tmp/siri-metal-diagnostic/console-production-fix-wifi.log) records
both compiler launches. After the separate AutoFillUI executable deployment, host **3976** / SpringBoard **3998** displayed
the populated home screen with remaining icon/widget gaps; see the [screenshot](../tmp/autofillui-stage/springboard-wifi.png).
App-registration totals, checksum-skipped resource publication and the latest rendering limits are recorded in
[SpringBoard services](springboard-services.md#stock-application-population-and-registration).

## Retained-cache image discovery on iPhone 17 (2026-10-27)

A fresh late attach to guest launchd native PID 11328 on iPhone18,2 / iOS27.0 24A435 discovered 256 modules with
the native cache retained. The image list included the guest RuntimeRoot `launchd_sim`, `dyld_sim`, `libLiveSimSpawn.dylib`,
and simulator cached libraries. Guest `libsystem_kernel.dylib` at `0x16a9ee000` resolved `mach_msg2_trap +8`; its UUID
was `B07862D0-C2DE-3AED-898C-88190E38064E`. Native workqueue entry `0x19fc42908` remained in an `r-x` mapping.
Retaining the native mapping therefore does not prevent the debugger bridge from publishing simulator images.

The debugger emitted a missing `class_getMethodImplementation` lookup warning and read the guest Objective-C runtime
from process memory. Module discovery and ordinary named stack symbols were verified; full Objective-C stepping was not.
Attach also observed the guest-owned native-workqueue hardware breakpoint on one thread. Do not remove it as an unknown
breakpoint. The inspection used hardware-only breakpoints, serial module loading and ignored `EXC_BAD_ACCESS` configured
before attachment, waited for the actual stopped notification, and performed no expression evaluation in the target.
After detach, launchd accepted and completed a new job bootstrap; independent SSH inspection showed it running and no
debugserver remained. Module UUIDs and bases are recorded in `tmp/lsd-metadata-default/results/lldb-retained-modules.json`.

For current CoreDevice versions, device screenshots are available without attaching a debugger:

```sh
xcrun devicectl device capture screenshot --device "$DEVICE" --destination tmp/device-log/screen.png --timeout 30
```

This captured a1320×2868 PNG on the iPhone17; the older `idevicescreenshot` service was unavailable there. The simulator
FPS HUD was visible in `tmp/lsd-metadata-default/results/hud-fps.png` after the guest render server reported mode7.
