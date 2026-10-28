# Move a simulator runtime to another iPhone

This runbook complements [agent setup](agent-setup.md). Keep the guest pinned to iOS Simulator 26.0 / 23A343 even when the physical
phone changes. Current storage is `<selected SideStore or AltStore App Group>/LSB/iOS/23A343/{RuntimeRoot,data}`.
There is no automatic migration from Documents, an older App Group, or a File Provider backup. The manual procedure below covers
both changing phones and restoring an existing runtime into the new storage location.

## Historical migration evidence

As of 2026-10-26, the full local stage and finalization passed: 494,754 entries, 163 app-registration candidates,
and all applicable signing/section-preservation checks, with four recorded dependency gaps. USB rsync of the full runtime and
prepared extras completed, and the new runtime was activated on the destination iPhone 17. The runtime transfer covered
15,099,025,144 bytes; `RuntimeRoot.before-full-20261026` retains the previous root. No post-copy checksum pass was performed,
following the user's request. There is no in-app installer or automatic device-to-device migration.

The production-device host/shim policy retains the native cache and redirects its workqueue entry with a hardware breakpoint.
This passed concurrent dispatch work, timers, and launchd spawning df without LLDB. After applying and signing the pinned
cache/helper metadata-name patch, the iPhone18,2 running iOS 27.0 / 24A435 reached a home screen with app icons, confirmed by
the user. lsd started and 647 guest metadata files were created. The migration initially omitted SimulatorHID; the plugin has
been restored for a separate touch-input retest. Input and broad application compatibility are not yet established by this boot.

Both the previous working host and the latest host with the wide native stack guard disabled ran `df` with exit status 0 after
receiving the two simulator root variables. The wide guard still produced cache-reservation `mmap` error `EACCES`, including after
explicitly releasing its reservation, and is now opt-in with `LSB_ENABLE_NATIVE_STACK_RESERVATION=NO` by default.

Normal simulator launchd attempts on the new phone were terminated with `CODESIGNING / InvalidPage` at native `start_wqthread`
after native-cache detachment. Native Metal helpers initialize on A19 Pro. An empty launchd test attached before handoff with
hardware-only LLDB breakpoints reached `dispatch_main`; a later interrupt found the simulator exception server, signal thread,
and workqueue wait. This debugger-assisted result does not verify a normal graphical boot or establish why normal launch differs.

A signed, directly linked native dylib probe loaded successfully, but its `__bsdthread_register` interposer was never called.
Read-only disassembly showed the native pthread call at `0x190118910` branching to an `ADRP`/`ADD`/`BR` island at `0x197d14b00`,
which computes the original registration function's address directly. There is no GOT lookup for ordinary dyld interposition to
redirect. This explains the negative probe; it does not establish that all native interposition is disabled. The later successful
retained-cache handoff leaves that kernel registration intact and redirects workqueue entry instead.

The 2026-10-27 hardware-breakpoint experiment also ran without LLDB. A mapped-function control inherited a task-default
breakpoint on a new ordinary pthread, returned 84 after redirection, then returned its original 42 after the owned thread slot
was removed. Exact task-state restoration left the next thread unaffected. Code-signing flags stayed `0x22003305`, with
`CS_DEBUGGED` clear throughout. The actual launchd trial confirmed successful arming and readback at native `start_wqthread`
before workqueue creation, but recorded no handled exception and exited with wait status 9. Earlier attempts reported
`CODESIGNING / InvalidPage` at the same address. Breakpoint-versus-fetch priority and actual workqueue inheritance on this
device remain unseparated; the control alone does not establish a working handoff.

The failed experiment removed the native cache. Production/unknown devices enable retention and hardware bridging.
Detachment now requires an exact native non-production MobileGestalt result supplied by the host at check-in; environment
overrides cannot grant it. See [handoff policy](runtime-handoff.md). Arming can follow native registration, but must precede worker
creation; the owned slot must persist for reused workers. See [the launchd trial](launchd.md#ios-27-hardware-breakpoint-trial)
and [retained-cache results](launchd.md#retained-native-cache-with-hardware-redirection).

Temporary probe processes and paused guests were stopped. The normal package with the wide guard disabled was restored and its
installation confirmed; independent process scans found no owned probe processes or debugserver sessions. The full transfer and
direct `df` baseline are complete. The subsequent patched-cache boot reached the home screen; remaining input validation is
recorded separately below.

## Record the source and destination

Record the runtime build, cache base and signing receipt, patched loader/launchd/shim versions, Fish and helper versions, native
app build, and simulator device type. The default simulated iPhone 15 Pro Max profile need not match the physical destination.
Keep one working source runtime and its mutable state until the new device passes verification.

Identify the destination with `xcrun devicectl list devices`. Install the correctly signed native app first. Both app and extension
profiles must include this device, their distinct identifiers, both expanded store App Groups, and Increased Memory Limit. Preserve the native
Metal backend under `LiveProcess.appex/Frameworks/NativeMetal`; these three libraries do not belong in `RuntimeRoot`.

Set task-local values; do not commit actual device identifiers, container UUIDs, keys, or credentials:

```sh
LSB_DEVICE='CURRENT_COREDEVICE_IDENTIFIER'
LSB_USB_UDID='CURRENT_USBMUX_DEVICE_UDID'
LSB_TEAM='YOUR_TEAM_ID'
LSB_TEAM_SUFFIX=".$LSB_TEAM" # Debug; Release defaults to an empty suffix.
LSB_APP_GROUP_SIDESTORE="group.com.SideStore.SideStore$LSB_TEAM_SUFFIX"
LSB_APP_GROUP_ALTSTORE="group.com.rileytestut.AltStore$LSB_TEAM_SUFFIX"
LSB_SSH_USER='YOUR_NATIVE_DEVICE_SSH_USER'
LSB_SSH_PORT='YOUR_LOCAL_FORWARD_PORT'
LSB_DEVICE_SSH_PORT='YOUR_NATIVE_DEVICE_SSH_PORT'
LSB_SSH_DEST="$LSB_SSH_USER@127.0.0.1"
LSB_RUNTIME_STAGE='/absolute/path/to/completed/stage/RuntimeRoot'
LSB_DATA_STAGE='/absolute/path/to/reviewed/data' # An older stopped SimulatorData root is also a valid source.
```

The native-device SSH service is externally provided. It is separate from an optional sshdog server running inside the simulator.
Do not assume that a previous local port still forwards to the intended phone. With a compatible libusbmuxd `iproxy`, use a
dedicated foreground terminal and bind only loopback:

```sh
iproxy --local --udid "$LSB_USB_UDID" --source 127.0.0.1 "$LSB_SSH_PORT:$LSB_DEVICE_SSH_PORT"
```

Inspect `iproxy --help` if the installed version uses different syntax. Verify the native SSH host key/device identity over this
connection before transferring. Do not create a second forward if a verified one already owns the port.

## Discover the actual App Group

The expanded App Group identifiers are stable; their filesystem UUIDs are not. Both targets use these Xcode settings:

```ini
APP_GROUP_SIDESTORE = group.com.SideStore.SideStore$(DEVELOPMENT_TEAM_SUFFIX)
APP_GROUP_ALTSTORE = group.com.rileytestut.AltStore$(DEVELOPMENT_TEAM_SUFFIX)
```

Debug uses `DEVELOPMENT_TEAM_SUFFIX=.$(DEVELOPMENT_TEAM)` and Release uses an empty suffix. The shared C helper reads actual
signed group entitlements at runtime, including suffix changes made when a store re-signs the app. Check the actual expanded values
in build settings, both final signed executables, and both embedded profiles. They must agree with the installed store's groups.
Adjust the shell variables above to those final identifiers before running commands. CoreDevice can inspect
each entitled group without assuming a physical path:

```sh
for LSB_GROUP_ID in "$LSB_APP_GROUP_SIDESTORE" "$LSB_APP_GROUP_ALTSTORE"; do
  xcrun devicectl device info files --device "$LSB_DEVICE" --domain-type appGroupDataContainer \
    --domain-identifier "$LSB_GROUP_ID" --no-recurse
done
```

The host considers only groups accessible with its current entitlements. Select in this order:

1. A group containing an existing `LSB` directory.
2. Otherwise, a group containing the store's `Apps` directory.
3. Otherwise, the first accessible group, SideStore before AltStore.

SideStore also wins ties within the first two steps. An existing `LSB` directory wins even if the other group has `Apps`.
Avoid leaving an empty `LSB` marker in the wrong group before choosing the destination. Store files under the selected
group's `LSB/iOS/23A343` subdirectory; leave the store's `Apps` and other files alone. These markers and the storage path components must
be actual directories, not symlinks. The selection is fixed for the host process lifetime; quit it before changing the layout.

Obtain the canonical physical group path from the host's `[broker] RuntimeRoot=...` startup record, or inspect container metadata
through the already authorized native SSH connection. The latter must match `MCMMetadataIdentifier` exactly to the configured
group identifier; require one result for each identifier being resolved. Do not guess from directory timestamps or copy a UUID
from an older run. If metadata access is unavailable, use the host/CoreDevice route rather than changing device permissions.

Set `LSB_SELECTED_APP_GROUP` to the selected identifier and `LSB_DEVICE_GROUP` to its discovered physical directory. Set
`LSB_DEVICE_STORAGE="$LSB_DEVICE_GROUP/LSB/iOS/23A343"`; this is the directory containing `RuntimeRoot` and `data`.
List both groups' existing contents and available storage before copying.
Only build 23A343 is selected. The directory hierarchy does not provide a runtime-version picker or permission to substitute another build.

The shared group is intended to retain these files while its owning SideStore or AltStore installation remains installed and
entitled to that same group. Removing every owner, changing signing teams/group identifiers, or deleting the store's data can
still lose access or storage. Keep an independent backup; using a store group is not an unconditional uninstall guarantee.

## Move an existing runtime into store storage

The `LSB/iOS/23A343/{RuntimeRoot,data}` build was installed on the iPhone16,2 destination on 2026-10-28. Integrated tests,
separate guest-shim/helper builds, the signed iPhoneOS build, and local signature checks passed. Both native signed executables
carry both store groups and Increased Memory Limit. Host UUID: `FE13AAD0-FC4F-3CA3-9CCB-9006030987AA`;
extension UUID: `49324F7C-32BF-39A9-A045-EEE115AC3EF6`. Entitlement discovery uses `xpc_copy_entitlement_for_self`.
The subsequent native production-policy build is recorded below.
After the user restored both roots manually, the four updated guest binaries were staged to inactive paths and activated with
rollback copies. No cache rebuild or runtime/data migration was performed. No post-copy checksum pass or debugger was used.
Both phones reached SpringBoard: the iPhone18,2 used retained-cache mode, while the development iPhone16,2 required detachment
because its native mapping overlaps the guest reservation. That development phone has custom boot arguments and relaxed TXM;
this result does not imply general iOS 26 detach support. Fish ran df and read display settings from the renamed data root on
the iPhone16,2 diagnostic run. Initial normal terminal launches reported ENOTTY; subsequent launches worked, and final normal
helper verification is recorded separately below. Local build, signing, deployment, and test evidence is under ignored
`tmp/runtime-layout/`; it is not a fresh-setup dependency.

The final iPhone16,2 / iOS 26.1 / 23B50 cold boot used host UUID `BBF2B0D4-D115-3845-839B-F6C69DA3C8E2`,
extension UUID `402EED62-9D38-3DB1-AA66-A41FDBC00E3F`, and shim UUID `D0EAF2B8-0BB2-36BA-921B-E37BB5EB9168`.
The native host returned exact `CFBoolean false` for `EffectiveProductionStatusAp`. With no cache-mode overrides, both launchd
scopes resolved to detach/software mode. The normal terminal helper reached its Fish prompt, spawned df with status 0, and read
HUD/touch settings with status 0. A screenshot confirms SpringBoard. The initial ENOTTY was not reproduced in this final run;
no speculative PTY retry was added. Host PID 904 and guest launchd PID 908 belong only to this recorded run, not future setup commands.
The unchanged guest cache is 23A343, base `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`.
Temporary diagnostic helpers were replaced, and no debugger was attached. Bounded native logs contained no AMFI signature
rejection for these launches. Policy/build/deployment and normal-helper evidence is in ignored `tmp/runtime-policy/results/`.

Stop the old and destination LiveSimBooter hosts and their guests/native helpers before moving code or mutable state. Do not
uninstall the old app to discover whether a group survives. Preserve the complete old `RuntimeRoot` and matching `SimulatorData`
outside any group that will be removed; a reviewed File Provider backup or local copy can serve as the source.

After installing the new build and discovering its selected group, prepare this layout manually:

```text
<selected store App Group>/
  Apps/                         # Store-owned, if present; do not modify.
  LSB/
    iOS/
      23A343/
        RuntimeRoot/
        data/
```

Copy the existing `RuntimeRoot` into `LSB/iOS/23A343/RuntimeRoot.incoming`, and the old `SimulatorData` contents into sibling
`data.incoming`. Publish them as `RuntimeRoot` and `data` only after the transfers succeed. This is an explicit rename of the
mutable root; do not create `data/SimulatorData`. If the source already uses `data`, copy that root instead. Backup wrappers such
as `LiveSimBooter` or `Sim26` are not runtime search locations: select their two actual roots rather than nesting the wrapper.
The app neither moves files nor merges runtimes. Keep the original backup until boot, app launch, input, and terminal checks pass.

The directory rename also requires current guest code. Rebuild and sign `usr/lib/libLiveSimSpawn.dylib` and the three
`usr/libexec` helpers `terminal-fish`, `display-settings`, and `launchctl-control` using the setup guide, then replace them in the
inactive `RuntimeRoot` before booting. Older copies validate the old `SimulatorData` basename; updating only the native host
would leave terminal, settings, control, and file-lock compatibility out of sync. The cache does not need rebuilding for this rename.

Review physical paths in generated configuration after the move. Existing guest data is not automatically rewritten for a new
container UUID. Regenerate supported configuration deliberately and preserve a stopped rollback pair; do not perform a blanket
replacement in binaries or databases.

## Finish a portable local payload

Use the explicit full scopes in [agent setup](agent-setup.md#stage-resources-apps-and-signed-loose-code), including `Developer` and
`RestoreVersion.plist`. Wait for a successful tool exit and its final `stage-manifest.json`. A directory with thousands of signed
files is still incomplete while the stage runs: final symlinks and app registration JSON are published last.

The finished payload must include the current patched `dyld_sim`, patched `launchd_sim`, guest shim, Fish, terminal helper,
control worker, app-registration helper, capabilities, and the separately signed cache. Include both external CoreSimulator
clients with their `Info.plist` files: `Library/PrivateFrameworks/SimFramebuffer.framework` and
`Library/Frameworks/SimulatorHID.framework`. They are outside the stock runtime root and require explicit `--add-binary`
inputs plus resource copies in [the staging recipe](agent-setup.md#stage-resources-apps-and-signed-loose-code). The host's
`SIMULATOR_HID_SYSTEM_MANAGER` points to the latter bundle; missing it can leave graphics working without touch delivery.
Keep original resources and signed uncached code together. Never overlay the original runtime wholesale onto this tree,
which would restore stock loaders and unsigned code.

Include the container-metadata filename patch from [agent setup](agent-setup.md#build-and-inspect-the-cache): run
`Tools/patch-ios26-container-metadata.py --source INPUT_CACHE --output NEW_CACHE` before signing the cache, and use its
`--mach-o` mode on the original MobileInstallationHelperService before passing that copy to `setup-ios26-runtime.py --add-binary`.
The tool validates five original 23A343 cache `__cstring` sections and the one matching loose helper, replacing the stock name
with `.com.kdt.LiveSimBooter.container-metadata.v1.plist` without changing its length. Both outputs require new signatures.
The device probe demonstrated exact-basename `EPERM`, and the kernel logged a sandbox `System Policy` denial for creating that
stock basename. The exact policy definition remains unidentified. The patched cache/helper pair then passed guest metadata
creation and lsd startup, reaching the home screen without LLDB. Cache UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8` and base
`0x110000000` were preserved; the replacement changes data bytes and therefore requires re-signing. Keep an older cache/helper
with its corresponding data: a data root whose metadata uses the stock basename needs a separately reviewed migration rather
than an incidental filename rewrite during transfer.

A local hard link can attach an already signed immutable cache without consuming another 2 GB, provided both paths are on the
same filesystem. After linking, do not rewrite, re-sign, chmod, or chown either path: they share an inode. Normal rsync copies the
file contents to the device. Retain the original signature/layout receipt; this is not a new signing operation.

`RuntimeRoot` and `data` are separate transfer roots:

- `RuntimeRoot` holds code, cache, bundles, resources, and generated job definitions.
- `data` holds mutable databases, preferences, logs, guest control state, and optional SSH configuration. Choose deliberately
  between a fresh data root, a stopped snapshot from the old setup, and retaining an existing destination data root. Do not merge live
  databases or overwrite destination state as an incidental consequence of copying code.

Regenerate configuration containing physical paths for the new `LSB/iOS/23A343` storage directory. In particular, [sshdog staging](sshdog.md) uses
`Tools/stage-sshdog.py --device-group "$LSB_DEVICE_GROUP"` to create its launch job, Fish hook, and key configuration. That option
takes the actual group directory and appends `LSB/iOS/23A343` itself; do not pass `LSB_DEVICE_STORAGE` to it. Supply the intended public
authorization keys and host key explicitly; preserve their restrictive modes. Review generated plists/JSON/Fish hooks for the old
container UUID. Do not blanket-replace text inside binaries or databases. Job executable/log paths should remain guest paths except
where a documented adapter requires a physical host path.

## Transfer to inactive directories

Stop the destination host and its simulator/native helper processes before publication. Detach any debugger and verify the target
resumed before stopping it normally. Keep the old runtime and data in place during transfer; choose new inactive directory names.

With the verified USB connection still running, a typical pair of transfers is:

```sh
LSB_DEVICE_GROUP='/private/var/mobile/Containers/Shared/AppGroup/DISCOVERED_UUID'
LSB_DEVICE_STORAGE="$LSB_DEVICE_GROUP/LSB/iOS/23A343"
LSB_RSYNC_SSH="ssh -p $LSB_SSH_PORT"
ssh -p "$LSB_SSH_PORT" "$LSB_SSH_DEST" "mkdir -p '$LSB_DEVICE_STORAGE'"
rsync -rlpt --partial -e "$LSB_RSYNC_SSH" "$LSB_RUNTIME_STAGE/" \
  "$LSB_SSH_DEST:$LSB_DEVICE_STORAGE/RuntimeRoot.incoming/"
# Only when a reviewed data stage is intended for this destination:
rsync -rlpt --partial -e "$LSB_RSYNC_SSH" "$LSB_DATA_STAGE/" \
  "$LSB_SSH_DEST:$LSB_DEVICE_STORAGE/data.incoming/"
```

The trailing slash copies each root's contents. Use `--dry-run --itemize-changes` first when destination contents are uncertain.
Do not add `--delete`, `--checksum`, or `--inplace`; a partial transfer must not delete rollback files or modify active mapped code.
Honor the user's transfer-verification preference. The current workflow skips a post-copy checksum pass; local signing and layout
validation still apply. A dropped connection can resume into the same inactive directory using the same source artifacts.

After both intended transfers exit successfully, inspect their exit codes, required paths, filesystem ownership, and permissions.
Files uploaded by a privileged SSH account must be readable by the native app, and mutable data must be writable by that app's
device account. Preserve private-key permissions. Do not apply a blanket world-writable mode or alter the host app bundle.

Publish within `LSB_DEVICE_STORAGE` with reviewed same-filesystem renames while every affected process is stopped: preserve an existing `RuntimeRoot` under
a unique rollback name, then rename `RuntimeRoot.incoming` to `RuntimeRoot`. If replacing data, preserve its existing root separately
and rename `data.incoming` to `data`. There is no atomic transaction spanning both root renames; do not launch between them.
If keeping destination data, leave it in place. On a fresh destination with no staged data, let the host create its required data
directories and copy only explicitly prepared configuration afterward.

## Boot, register, and retain rollback

Launch `/bin/df` first using [the setup smoke-test command](agent-setup.md#transfer-and-boot). It requires the newly discovered
`LSB_DEVICE_STORAGE`: pass `DEVICECTL_CHILD_SIMULATOR_ROOT="$LSB_DEVICE_STORAGE/RuntimeRoot"` and
`DEVICECTL_CHILD_SIMULATOR_SHARED_RESOURCES_DIRECTORY="$LSB_DEVICE_STORAGE/data"` to CoreDevice. Direct mode does not inject
those variables, and omitting them can trigger stock libsystem_featureflags' missing-root trap. Graphical mode already supplies them.
The destination test reproduced that trap without the variables, then printed the filesystem table and exited 0 with them using
both the previous working host and the latest default-off guard build. The migrated runtime therefore has a passing `df` baseline;
the later metadata-name patch also reached the home screen, but the 163 registration candidates are not a claim that every app works.
Check the broker's actual exit record, then boot graphical launchd with the native host foregrounded.
Check the cache mapping, helper readiness, framebuffer, Fish tab, and one known app. Use the generated app-registration manifest
inside guest launchd after lsd starts; rsync completion alone does not register icons. See [guest control](guest-control.md).

Record actual results separately from copy success. Full-runtime staging is not proof that every included application or optional
daemon works. Keep deferred application failures deferred instead of adding unrelated compatibility changes during migration.
If the baseline fails, stop the new session and restore the matching runtime/data rollback pair. Delete old copies only after the
replacement is verified and the user no longer needs them. Stop only the USB forward and debugger sessions created for this task.

The iPhone 17/iOS 27 migration subsequently passed launchd spawning df without a debugger by retaining the native cache and
redirecting its workqueue entry with a hardware breakpoint. This is now the default and requires the matching rebuilt host/shim;
see [the setup note](agent-setup.md#transfer-and-boot) and [measured results](launchd.md#retained-native-cache-with-hardware-redirection).
The full cache and native mapping fit together on the tested boot. After the filename-specific sandbox denial was isolated,
the signed cache/helper patch allowed lsd startup and produced 647 guest metadata files. The user confirmed the home screen
with app icons on iPhone18,2 / iOS 27.0 / 24A435 without LLDB. The initial migration lacked the external SimulatorHID plugin;
restoring the signed plugin and its Info.plist then restored touch markers, home-page swipes and opening Contacts, confirmed by the user. Evidence is in
`tmp/lsd-metadata-default/results/`, including `sandbox-policy-kernel.log`, `patched-lsd-status.log`, and the guest logs.
