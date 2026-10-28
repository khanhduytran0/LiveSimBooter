# Set up LiveSimBooter with an agent

This guide pins the guest to **iOS Simulator 26.0, build 23A343, arm64**. It uses tracked tools and original runtime inputs;
it does not require another developer's ignored `tmp/` directory. There is no in-app installer and no single command that
provisions a signing account, installs the runtime, packages the native backend, transfers files, and verifies the phone.
The commands below separate those steps so an agent can record exactly which one succeeded.

Read [AGENTS.md](../AGENTS.md) before changing the project. Older documents contain historical PIDs, paths, identities, and
experiments; use the values for your own machine and device here. The guest version is fixed; the physical phone's OS is a
separate input. The app targets iOS 26.0+, with recorded development tests on iOS 26.1 and iOS 27 devices.
For moving a completed setup to another phone, also follow [the migration runbook](migration.md).

## Prerequisites and configuration

You need an Apple Silicon Mac, full Xcode with iPhoneOS and iPhoneSimulator SDKs, Python 3.8 or newer, and an original installed
23A343 `.simruntime`. Obtain that runtime through your own Xcode/runtime installation; Apple binaries are not in Git.
The runtime must include its original `Contents/Resources/update_dyld_sim_shared_cache` and standalone dylibs. Cache-extracted
images or a newer runtime are not substitutes for the pinned originals. The cache tool verifies their hashes.

The device needs pairing/Developer Mode and a development signing identity/profile valid for that device. Both the app and
LiveProcess extension require the same two store App Groups and `com.apple.developer.kernel.increased-memory-limit` in their **profiles
and signed executables**. A capability written only into the entitlements file is insufficient. Xcode's extension UI may not
offer Increased Memory Limit; arrange valid provisioning for the extension identifier before installing. This guide does not
edit a device provisioning database or assume custom profile-validation behavior.

Allow substantial free space. The pinned cache inputs occupy about 3 GB, its signed output about 2 GB, and the broad resource/app
payload occupies many more gigabytes. Keep room for one build, its signed staging tree, and one rollback, rather than many caches.
The setup inventory reports sizes before copying. SSH, `rsync`, and `iproxy` are optional external development tools; the app
does not provide native-device SSH access. Guest launchctl control additionally needs native SSH with Python 3 on the device.

From the repository root, create a new local setup directory and fill these values. These are shell conveniences, not a
configuration file read by the app:

```sh
LSB_WORK="$PWD/tmp/setup-26.0"
LSB_RUNTIME_SOURCE='/absolute/path/iOS 26.0.simruntime/Contents/Resources/RuntimeRoot'
LSB_SIGNING_IDENTITY='YOUR_40_HEX_CERTIFICATE_SHA1'
LSB_TEAM='YOUR_TEAM_ID'
LSB_BUNDLE_ID='your.registered.LiveSimBooter'
LSB_EXTENSION_ID='your.registered.LiveSimBooter.LiveProcess'
LSB_TEAM_SUFFIX=".$LSB_TEAM" # Debug; Release defaults to an empty suffix.
LSB_APP_GROUP_SIDESTORE="group.com.SideStore.SideStore$LSB_TEAM_SUFFIX"
LSB_APP_GROUP_ALTSTORE="group.com.rileytestut.AltStore$LSB_TEAM_SUFFIX"
LSB_DEVICE='YOUR_CURRENT_COREDEVICE_IDENTIFIER'
LSB_CAPABILITIES='/Library/Developer/CoreSimulator/Profiles/DeviceTypes/iPhone 15 Pro Max.simdevicetype/Contents/Resources/capabilities.plist'
mkdir -p "$LSB_WORK"
xcodebuild -version
xcrun --sdk iphoneos --show-sdk-path
xcrun --sdk iphonesimulator --show-sdk-path
xcrun devicectl list devices
security find-identity -v -p codesigning
```

Configure these source settings consistently, using identifiers registered to your team:

| Setting | Files/settings to update |
| --- | --- |
| Team and app identifier | Both configurations of the `LiveSimBooter` Xcode target |
| Team and extension identifier | Both configurations of the `LiveProcess` Xcode target |
| Extension discovery identifier | `LSB_EXTENSION_IDENTIFIER` in `ProcessSupport/SpawnPlan.h` |
| Shared App Groups | `APP_GROUP_SIDESTORE`, `APP_GROUP_ALTSTORE`, and `DEVELOPMENT_TEAM_SUFFIX` Xcode settings; both targets' `*.entitlements` files |
| Provisioned capabilities | Both targets' signed entitlements and embedded provisioning profiles |

The store group build settings are:

```ini
APP_GROUP_SIDESTORE = group.com.SideStore.SideStore$(DEVELOPMENT_TEAM_SUFFIX)
APP_GROUP_ALTSTORE = group.com.rileytestut.AltStore$(DEVELOPMENT_TEAM_SUFFIX)
```

Debug sets `DEVELOPMENT_TEAM_SUFFIX=.$(DEVELOPMENT_TEAM)`; Release leaves it empty, matching LiveContainer's store-signing setup.
Use the exact expanded identifiers authorized by the installed store and both native targets' profiles; the suffix must not duplicate
a dot or team ID. The runtime reads the actual signed `com.apple.security.application-groups` entitlement through a shared C helper,
so a store's re-signing step can change the suffix without leaving an Info.plist value stale. Confirm the final signed groups on both
the app and extension, along with their profiles. Merely matching a store's name grants no
container access. Keep that store installed with access to the selected group if its ownership is intended to preserve runtime
storage across uninstalling LiveSimBooter; this is not an independent backup or a guarantee after removing all group owners.

The host selects an accessible group containing an `LSB` directory first, otherwise one containing the store's `Apps`
directory. SideStore precedes AltStore when both qualify or neither contains either directory. It stores the two roots at
`<selected group>/LSB/iOS/23A343/RuntimeRoot` and `<selected group>/LSB/iOS/23A343/data`. Only the pinned 23A343 build is selected;
there is no runtime-version picker. There is no automatic move from Documents or an older group: follow the
[manual move](migration.md#move-an-existing-runtime-into-store-storage), including renaming old `SimulatorData` to `data`, before booting.
The native broker uses `<selected actual store group>.LiveSimBooter.spawn`; the C child tries its entitled store groups and receives
the selected runtime path from the broker.

Do not globally override `PRODUCT_BUNDLE_IDENTIFIER` on an xcodebuild command:
the enclosing app and extension need different identifiers. Internal guest job labels containing `com.kdt.LiveSimBooter`
are protocol labels, not the app's signing identity; do not blanket-replace them. Rebuild host, extension, and shim together
after changing shared constants. No personal certificate hash, device name, unlock code, or container UUID belongs in a commit.

The default simulated display is iPhone 15 Pro Max, `iPhone16,2` / `D84`, 1290 × 2796 at scale 3. Use its capabilities plist,
independently of the physical phone's model. The staged path is
`RuntimeRoot/Library/Developer/CoreSimulator/DeviceType/capabilities.plist`; the host and C child inject `SIMULATOR_CAPABILITIES`
for that physical file. The [device picker](device-types.md) keeps capabilities, identity and framebuffer geometry together.
Stage its seven original profiles below to enable Home-button/Home-indicator iPhones and iPads; iPhone 8/8 Plus remain experimental on this OS.

Normal startup and device-picker apply publish the staged stock capability values unchanged. The active file is serialized as
a binary plist and replaced atomically; staged originals remain untouched. The unfinished native haptic bridge
is disabled by default: `LSB_EXPERIMENTAL_HAPTICS=1` in the **native host launch environment** enables both fixed hapticd
endpoint publication and projection of `DeviceSupportsClosedLoopHaptics` from the actual native MobileGestalt answer.
Only exact CFBoolean true enables that capability; unknown or malformed answers become false. Other profile fields and original
staged profiles remain unchanged. Engine initialization has succeeded only with an additional temporary diagnostic adaptation;
engine start still fails and physical playback is unverified. This opt-in is not a setup requirement; see [haptic validation](keyboard-haptics.md).

## Build and inspect the cache

The tracked [cache inventory](../Tools/setup-ios26-cache-inputs.json) freezes 1,350 original runtime images; the stock builder
also imports three `/usr/lib/system` host-support dylibs from the Mac. Their paths/hashes are recorded separately in the build
manifest. They are not falsely presented as fixed 23A343 inputs, and another Mac OS can produce a different cache requiring
device validation.

```sh
python3 Tools/setup-ios26-cache.py --runtime-root "$LSB_RUNTIME_SOURCE" --validate-only \
  > "$LSB_WORK/cache-preflight.json"
python3 Tools/setup-ios26-cache.py --runtime-root "$LSB_RUNTIME_SOURCE" --output-dir "$LSB_WORK/cache" \
  > "$LSB_WORK/cache-result.json"
```

This stages independent copies, applies four exact changes to a private original builder, ad-hoc signs that **Mac build tool**,
and invokes its arm64 simulator builder at `0x110000000` with local symbols stripped. It produces
`cache/unsigned/dyld_sim_shared_cache_arm64`, `stage-manifest.json`, `build.log`, and `validation.json`. It refuses existing
output, original-input hash mismatches, missing/extra cached images, missing required runtime metadata, and executable or
writable mappings crossing `0x180000000`. Read-only metadata/LINKEDIT may cross that boundary. This is layout validation,
not signature acceptance or proof that the phone can execute the cache. See [cache mapping evidence](shared-cache.md).

The tracked helper has been exercised end to end against the pinned originals: it built a new 1,353-image cache with the
expected six mapping ranges and preserved exports/function starts. That fresh unsigned build was inspected locally; it was not
separately deployed. Its redundant copied inputs and cache were removed after retaining the manifest, build log, and validation report.

Apply the pinned container-metadata filename patch before signing. A clean probe on the iOS 27 device could create ordinary
files and atomic temporary files, but creating or renaming to `.com.apple.mobile_container_manager.metadata.plist` returned
`EPERM`. A kernel sandbox report independently records `System Policy ... deny(1) file-write-create` for that exact basename;
the specific policy definition has not been identified. The patch uses
`.com.kdt.LiveSimBooter.container-metadata.v1.plist`, also 50 bytes, in five validated cache images. It checks the image paths,
UUIDs, complete `__TEXT,__cstring` hashes, literal boundaries, and mapping protections, so matching 23A343 cache rebuilds may
have different file offsets and cache UUIDs. It preserves the input and refuses existing outputs or repeated patches.

```sh
mkdir -p "$LSB_WORK/patched-cache"
python3 Tools/patch-ios26-container-metadata.py \
  --source "$LSB_WORK/cache/unsigned/dyld_sim_shared_cache_arm64" \
  --output "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64" \
  > "$LSB_WORK/cache-metadata-patch.json"
```

The separate MobileInstallation helper needs the same replacement; prepare it in the next section. The signed cache/helper pair
passed a no-LLDB test on iPhone18,2 running iOS 27.0 / 24A435: lsd started, 647 guest metadata files were created under the new
name, and the user confirmed a home screen with app icons. The patch changes guest filenames, not native sandbox policy or
container authorization. Keep the cache, helper, and guest data's metadata naming scheme consistent; an existing data root
using stock names requires a separate migration plan. See [the device evidence](launchd.md#container-metadata-filename-compatibility).

Apply the [keyboard haptics correction](keyboard-haptics.md) to preserve AHAP resource URLs before signing. This is needed
for the tested iOS 27 host's system keyboard patterns and is pinned to the same 23A343 CoreHaptics image/cache layout.
The haptics patch currently requires a reservation size of `0x7e194000`. Different host-support dylibs on another Mac may
produce a different span: stop at the failed preflight and audit that layout rather than weakening the check or using a
partially patched cache. The fresh builder reproduction matched this span; the complete new patch/sign pipeline was tested
on the existing working cache, not independently redeployed from that fresh builder output:

```sh
python3 Tools/patch-ios26-keyboard-haptics.py \
  --source "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64" \
  --output "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64.haptics" \
  > "$LSB_WORK/cache-haptics-patch.json"
```

Sign that patched cache to a distinct new output; the signer can replace its destination, so choose a fresh filename:

```sh
mkdir -p "$LSB_WORK/signed-cache"
python3 Tools/sign-dyld-cache.py "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64.haptics" \
  "$LSB_WORK/signed-cache/dyld_sim_shared_cache_arm64" \
  --identity "$LSB_SIGNING_IDENTITY" --identifier "$LSB_BUNDLE_ID" --team "$LSB_TEAM" \
  > "$LSB_WORK/cache-signing.json"
python3 Tools/setup-ios26-cache.py --check-cache "$LSB_WORK/signed-cache/dyld_sim_shared_cache_arm64" \
  > "$LSB_WORK/cache-signed-layout.json"
```

The signer uses the selected keychain identity without exporting its key. macOS may ask for key access. It generates the
validated contiguous 16 KiB/v0x20400 signature and fixes the signature-size field before hashing. Ordinary `codesign` alone
is not the cache signing recipe. The final layout check does not replace the signer's CMS/page-hash checks.

## Prepare the loader and guest tools

Patch copies of the original loader and launchd before signing. The older base-patch tool also writes a `0x120000000` builder;
**use only its patched dyld_sim here**. The full-cache builder above already has the `0x110000000` configuration. The patched
loader reads the actual cache base from its header. Apply platform compatibility to the unsigned base-patched loader so its
pinned input hash matches:

```sh
python3 Tools/patch-ios26-cache-base.py \
  --builder "$LSB_RUNTIME_SOURCE/../update_dyld_sim_shared_cache" \
  --dyld-sim "$LSB_RUNTIME_SOURCE/usr/lib/dyld_sim" --output-dir "$LSB_WORK/loader-base"
python3 Tools/patch-ios26-platform.py --source "$LSB_WORK/loader-base/dyld_sim" --output "$LSB_WORK/dyld_sim"
python3 Tools/patch-ios26-launchd-identity.py --source "$LSB_RUNTIME_SOURCE/sbin/launchd_sim" --output "$LSB_WORK/launchd_sim"
sh Tools/build-spawn-shim.sh "$LSB_WORK/guest/libLiveSimSpawn.dylib" "$LSB_WORK/guest/spawn-probe"
sh Tools/build-terminal-fish.sh "$LSB_WORK/guest"
sh Tools/build-display-settings.sh "$LSB_WORK/guest"
sh Tools/build-launchctl-control.sh "$LSB_WORK/guest"
sh Tools/build-app-registration.sh "$LSB_WORK/guest"
LSB_MI_HELPER_DIR='/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/com.apple.MobileInstallationHelperService.xpc'
LSB_MI_HELPER="$LSB_MI_HELPER_DIR/com.apple.MobileInstallationHelperService"
python3 Tools/patch-ios26-container-metadata.py --mach-o \
  --source "$LSB_RUNTIME_SOURCE$LSB_MI_HELPER" --output "$LSB_WORK/MobileInstallationHelperService" \
  > "$LSB_WORK/helper-metadata-patch.json"
```

These commands do not deploy or sign guest executables. `dyld_sim` retains `MH_DYLINKER`; executables retain `MH_EXECUTE`.
For executables the staging tool uses [sign-sim-executable.py](../Tools/sign-sim-executable.py), which removes only
`CS_EXECSEG_MAIN_BINARY` from the generated CodeDirectory and re-signs it. Guest signatures use the app's identifier and
carry no application entitlements. Original simulator mock entitlement sections (`__TEXT,__ents_der` and related data)
remain inside the Mach-O for the guest identity adapters; they are not extra device privileges or external sidecar files.
The metadata tool's `--mach-o` mode accepts only the matching 23A343 MobileInstallationHelperService UUID and original
`__cstring` section. Its one literal patch invalidates the signature; the `--add-binary` override below makes the runtime
stager sign this patched copy instead of restoring the stock executable. `--check-only` validates either input without writing.

### Supply Fish

The ordinary Fish tab needs an arm64 simulator `/bin/fish`. A native macOS executable is not a drop-in replacement.
The tested unmodified source is Fish **4.9.3**, archive SHA-256
`20998a25f73217ddcc19f499055fd587e9912d1ad6e7109120fbcf2871f0b98c`, with Rust **1.91.1**, target
`aarch64-apple-ios-sim`, static PCRE2, no default features, and minimum iOS 26.0. Obtain the original release and dependencies
through your normal trusted source/toolchain setup; the repository does not bundle or automatically download them.

With that source extracted at `$LSB_WORK/fish-source` and the Rust target installed, the equivalent build recipe is:

```sh
cat > "$LSB_WORK/simulator-clang.sh" <<'SH'
#!/bin/sh
exec /usr/bin/xcrun --sdk iphonesimulator clang -target arm64-apple-ios26.0-simulator "$@"
SH
chmod +x "$LSB_WORK/simulator-clang.sh"
CARGO_TARGET_DIR="$LSB_WORK/fish-target" \
CARGO_TARGET_AARCH64_APPLE_IOS_SIM_LINKER="$LSB_WORK/simulator-clang.sh" \
CC_aarch64_apple_ios_sim="$LSB_WORK/simulator-clang.sh" \
CARGO_PROFILE_RELEASE_LTO=false CARGO_PROFILE_RELEASE_DEBUG=1 \
IPHONEOS_DEPLOYMENT_TARGET=26.0 PCRE2_SYS_STATIC=1 PREFIX=/usr \
cargo +1.91.1 build --locked --release --no-default-features --target aarch64-apple-ios-sim \
  --manifest-path "$LSB_WORK/fish-source/Cargo.toml" --bin fish
LSB_FISH="$LSB_WORK/fish-target/aarch64-apple-ios-sim/release/fish"
```

Use `--offline` once Cargo dependencies are already available. Inspect the result's architecture/platform/dependencies;
the recorded build loads only libiconv and libSystem. The terminal helper disables job control and uses the simulated
spawn/SETEXEC/wait path. Generic fork/execve and kernel PTYs remain unsupported. See [Fish](fish.md).

## Stage resources, apps, and signed loose code

The runtime stager inventories the selected original trees before copying; default scopes include the app/system/library
resources and command directories, excluding the original caches, stale bundle seals, and cache-resident Mach-Os. It builds
a **new** `RuntimeRoot`, signs uncached code, and preserves original simulator executable sections. Do not blindly copy the
whole stock root over a signed staging tree: that restores unsigned executables and cache overrides.

The full migration recipe below explicitly adds `Developer` and `RestoreVersion.plist` to the default selection. Supplying
any `--scope` replaces the defaults, so repeat the complete list. The optional CMS helper argument reuses a prebuilt
`Tools/sign-code-directory.m` executable that the keychain already recognizes. Leave the array empty to compile a new helper
once for this stage; neither option suppresses a keychain authorization prompt if access is needed.

Graphics also needs the simulator-platform SimFramebuffer client distributed with CoreSimulator, outside the runtime root.
Input needs the separate SimulatorHID plugin from the same CoreSimulator platform tree. Use the tested 1171.7 frameworks at
the paths below, or previously verified copies, and keep each `Info.plist` alongside its executable. Do not substitute the
macOS SimFramebufferServer. The first full migration omitted SimulatorHID: graphics displayed a home screen, but touch input
was unavailable. Restoring the signed plugin and Info.plist restored input; the user verified Home Screen page swipes and opening Contacts.
See [the framebuffer protocol](framebuffer.md) and [Indigo prerequisites](backboardd.md#host-indigo-input-and-presentation-preparation).

```sh
LSB_CMS_OPTIONS=()
# Optional: LSB_CMS_OPTIONS=(--cms-helper '/absolute/path/to/approved/sign-code-directory')
LSB_CORESIM_IPHONEOS='/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A/Resources/Platforms/iphoneos'
LSB_FRAMEBUFFER_FRAMEWORK="$LSB_CORESIM_IPHONEOS/Library/PrivateFrameworks/SimFramebuffer.framework"
LSB_HID_FRAMEWORK="$LSB_CORESIM_IPHONEOS/Library/Frameworks/SimulatorHID.framework"
python3 Tools/setup-ios26-runtime.py --runtime-root "$LSB_RUNTIME_SOURCE" \
  --cache-manifest Tools/setup-ios26-cache-inputs.json --dry-run --allow-missing-dependencies \
  --scope Applications --scope System/Applications --scope System/Library --scope System/Cryptexes \
  --scope Library --scope usr --scope bin --scope sbin --scope private/etc --scope etc \
  --scope Developer --scope RestoreVersion.plist > "$LSB_WORK/runtime-inventory.json"
python3 Tools/setup-ios26-runtime.py --runtime-root "$LSB_RUNTIME_SOURCE" \
  --cache-manifest Tools/setup-ios26-cache-inputs.json --stage --output-dir "$LSB_WORK/runtime" \
  --scope Applications --scope System/Applications --scope System/Library --scope System/Cryptexes \
  --scope Library --scope usr --scope bin --scope sbin --scope private/etc --scope etc \
  --scope Developer --scope RestoreVersion.plist --allow-missing-dependencies "${LSB_CMS_OPTIONS[@]}" \
  --identity "$LSB_SIGNING_IDENTITY" --identifier "$LSB_BUNDLE_ID" --capabilities "$LSB_CAPABILITIES" \
  --add-binary "/usr/lib/dyld_sim=$LSB_WORK/dyld_sim" \
  --add-binary "/sbin/launchd_sim=$LSB_WORK/launchd_sim" \
  --add-binary "/usr/lib/libLiveSimSpawn.dylib=$LSB_WORK/guest/libLiveSimSpawn.dylib" \
  --add-binary "/bin/spawn-probe=$LSB_WORK/guest/spawn-probe" \
  --add-binary "/usr/libexec/terminal-fish=$LSB_WORK/guest/terminal-fish" \
  --add-binary "/usr/libexec/display-settings=$LSB_WORK/guest/display-settings" \
  --add-binary "/usr/libexec/launchctl-control=$LSB_WORK/guest/launchctl-control" \
  --add-binary "/usr/libexec/app-registration=$LSB_WORK/guest/app-registration" \
  --add-binary "$LSB_MI_HELPER=$LSB_WORK/MobileInstallationHelperService" \
  --add-binary "/bin/fish=$LSB_FISH" \
  --add-binary "/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer=$LSB_FRAMEBUFFER_FRAMEWORK/SimFramebuffer" \
  --add-binary "/Library/Frameworks/SimulatorHID.framework/SimulatorHID=$LSB_HID_FRAMEWORK/SimulatorHID" \
  > "$LSB_WORK/runtime-stage.json" 2> "$LSB_WORK/runtime-stage.log"
cp "$LSB_FRAMEBUFFER_FRAMEWORK/Info.plist" "$LSB_WORK/runtime/RuntimeRoot/Library/PrivateFrameworks/SimFramebuffer.framework/Info.plist"
cp "$LSB_HID_FRAMEWORK/Info.plist" "$LSB_WORK/runtime/RuntimeRoot/Library/Frameworks/SimulatorHID.framework/Info.plist"
mkdir -p "$LSB_WORK/runtime/RuntimeRoot/System/Library/Caches/com.apple.dyld"
cp "$LSB_WORK/signed-cache/dyld_sim_shared_cache_arm64" \
  "$LSB_WORK/runtime/RuntimeRoot/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64"
python3 Tools/setup-ios26-device-types.py --device-types-root /Library/Developer/CoreSimulator/Profiles/DeviceTypes \
  --output-dir "$LSB_WORK/device-types"
ditto "$LSB_WORK/device-types/RuntimeRoot/Library/Developer/CoreSimulator/DeviceTypes" \
  "$LSB_WORK/runtime/RuntimeRoot/Library/Developer/CoreSimulator/DeviceTypes"
```

Read the inventory and any blockers before staging. The broad stock payload includes diagnostic/XCTest executables with unresolved
dependencies. The example explicitly uses `--allow-missing-dependencies` to retain that broad
payload while reporting those gaps; it does not make those programs runnable. Omit the flag for a strict dependency-complete subset.
Unsupported architectures/platforms still block staging; there is no unsigned fallback. Stock iOS-platform images are allowed because
the documented dyld_sim variant supports them, while irrelevant Mac-only host tools are recorded and omitted. Contained dangling stock
links are reported and preserved as relative links, not redirected into the native filesystem.

The default 23A343 inventory contains about 13 GB of selected payload: 3,825 code files, 490,636 resources, 249 symlinks, and 254 app
bundles, of which 163 have registerable executables. Adding `Developer` supplies five code files, 23 resources, and three links,
about 1.2 MB, including the two XCTest support libraries omitted by the defaults. Four unresolved references remain in the full
selection: two absent AppleInternal frameworks and `@rpath/XCTest.framework/XCTest` used by VisualTestKit and xctest.
These counts exclude added local helpers and the cache. The subsequent full migration stage completed with `Developer`,
`RestoreVersion.plist`, and ten explicit local overrides:
3,840 code files, 490,661 resources, 252 symlinks, and the generated registration manifest, totaling 494,754 entries. Its source
payload was 13,049,057,305 bytes before the separately attached 2.064 GB signed cache. All applicable local codesign/CMS checks and
original executable-section preservation checks passed with zero errors; the four dependency gaps remain explicitly recorded.
The completed stage lists 163 registerable apps. The full runtime and prepared extras finished USB rsync and were activated on the
destination iPhone 17. The runtime transfer covered 15,099,025,144 bytes; the previous root was retained as
`RuntimeRoot.before-full-20261026`. No post-copy checksum pass was run, as requested. This transfer result does not establish that
all 163 apps launch; see [migration validation](migration.md#boot-register-and-retain-rollback) for the current execution results.

An explicit subset of `--scope` values can narrow a diagnostic payload, but such a subset is not a complete graphical runtime. The default scopes
also include `System/Cryptexes`, needed by stock framework aliases. Keep stock fonts, ICU data, framework/bundle `Info.plist` files,
shaders, CoreImage plugins, feature flags, wallpaper,
and app resources together. Copying only the cache does not supply these assets. Apps without an executable may be placeholders.

The stager preserves regular-file modes and contained symlinks, but does not recreate empty directories, directory ownership/modes,
or extended attributes. The readable stock `private/var` tree contains empty seed directories rather than resources; its
`db/modelmanagerd` directory was not readable during inspection. Mutable guest state belongs in the separate `data` root.
Wait for a successful exit and `stage-manifest.json` before finalizing or transferring: app registration JSON and symlinks are
published at the end. The tool does not resume a partially completed output directory.

The broad cache is the measured 1,353-image closure, not every framework Apple ships. Newly needed code outside it is signed as
loose code. This avoids pretending that the initial closure proves every installed app or daemon works.

For optional SSH-based guest control and app registration, add the following two plists before transfer. Only the control worker
will be selected at initial boot; the registration job will be bootstrapped once afterward:

```sh
python3 - "$LSB_WORK/runtime/RuntimeRoot" <<'PY'
import pathlib, plistlib, sys
root = pathlib.Path(sys.argv[1])
jobs = [
    ('System/Library/LaunchDaemons/com.kdt.LiveSimBooter.launchctl-control.plist',
     'com.kdt.LiveSimBooter.launchctl-control', ['/usr/libexec/launchctl-control']),
    ('usr/share/LiveSimBooter/app-registration.plist', 'com.kdt.LiveSimBooter.app-registration',
     ['/usr/libexec/app-registration', '--manifest', '/usr/share/LiveSimBooter/applications.json']),
]
for relative, label, arguments in jobs:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    job = dict(Label=label, ProgramArguments=arguments, WorkingDirectory='/private/var/mobile/',
               RunAtLoad=True, KeepAlive=False,
               StandardOutPath='/private/var/mobile/Library/Logs/' + label + '.log',
               StandardErrorPath='/private/var/mobile/Library/Logs/' + label + '.log')
    with path.open('xb') as output:
        plistlib.dump(job, output)
PY
```

## Build and package the native host

```sh
sh Tests/run.sh > "$LSB_WORK/tests.log" 2>&1
xcodebuild -project LiveSimBooter.xcodeproj -scheme LiveSimBooter -configuration Debug \
  -destination 'generic/platform=iOS' -derivedDataPath "$LSB_WORK/native-build" \
  DEVELOPMENT_TEAM="$LSB_TEAM" CODE_SIGN_IDENTITY="$LSB_SIGNING_IDENTITY" build > "$LSB_WORK/native-build.log" 2>&1
python3 Tools/prepare-native-metal.py --runtime-root "$LSB_RUNTIME_SOURCE" \
  --output-dir "$LSB_WORK/native-metal" --self-test > "$LSB_WORK/native-metal.json"
LSB_BUILT_APP="$LSB_WORK/native-build/Build/Products/Debug-iphoneos/LiveSimBooter.app"
LSB_STAGED_APP="$LSB_WORK/package/LiveSimBooter.app"
ditto "$LSB_BUILT_APP" "$LSB_STAGED_APP"
LSB_NATIVE_BUNDLE="$LSB_STAGED_APP/PlugIns/LiveProcess.appex/Frameworks/NativeMetal"
mkdir -p "$LSB_NATIVE_BUNDLE"
for library in MetalSerializer MTLSimImplementation MetalNativeProbe; do
  cp "$LSB_WORK/native-metal/$library.dylib" "$LSB_NATIVE_BUNDLE/$library.dylib"
  codesign --force --sign "$LSB_SIGNING_IDENTITY" --identifier "$LSB_BUNDLE_ID" --timestamp=none "$LSB_NATIVE_BUNDLE/$library.dylib"
done
codesign --force --sign "$LSB_SIGNING_IDENTITY" --preserve-metadata=identifier,entitlements \
  --timestamp=none "$LSB_STAGED_APP/PlugIns/LiveProcess.appex"
codesign --force --sign "$LSB_SIGNING_IDENTITY" --preserve-metadata=identifier,entitlements --timestamp=none "$LSB_STAGED_APP"
codesign --verify --strict --deep "$LSB_STAGED_APP"
codesign -d --entitlements :- "$LSB_STAGED_APP"
codesign -d --entitlements :- "$LSB_STAGED_APP/PlugIns/LiveProcess.appex"
```

Check both embedded profiles as well, using `security cms -D -i .../embedded.mobileprovision`; their team, app identifiers,
both expanded App Groups, device authorization, development debugging permission, and requested memory capability must match the signatures.
Do not re-sign the extension with the app's identifier. Native backend libraries receive no application entitlements.
The backend packaging step is required for graphics and is not an automatic Xcode build phase.
See [NativeMetal/README.md](../NativeMetal/README.md) for the host/guest library distinction.

Keep the experimental native stack-exclusion segment disabled for the baseline package. On the destination iPhone 17, that
wide variant still failed the simulator-cache reservation with `mmap` returning `EACCES`, even after releasing the reservation.
`LSB_ENABLE_NATIVE_STACK_RESERVATION=NO` is the default; the wide reservation requires explicit opt-in. Production/unknown devices
retain the native cache and redirect its registered workqueue entry through the guest's hardware-breakpoint bridge.
Rebuild both native app and guest shim for these defaults. On the iOS 27 phone, this passed dispatch workloads and launchd
spawning df without LLDB, then reached backboardd framebuffer presentation. After the cache/helper metadata-name patch,
lsd started and the user confirmed a home screen with app icons. Restoring the omitted SimulatorHID plugin restored input,
verified by page swipes and opening Contacts. Broader application validation remains separate. See [migration findings](migration.md).

An additional no-debugger hardware-breakpoint trial on 2026-10-27 verified mapped-function redirection and breakpoint removal,
but arming the detached native workqueue entry still ended with `CODESIGNING / Invalid Page` and no delivered breakpoint.
The failed trial removed the native cache; it is distinct from the now-default retained-cache path. See the
[workqueue trial](launchd.md#ios-27-hardware-breakpoint-trial) for the evidence and limits.

## Transfer and boot

Install the staged app using the current device identifier, with the phone unlocked:

```sh
xcrun devicectl device install app --device "$LSB_DEVICE" "$LSB_STAGED_APP" --json-output "$LSB_WORK/install.json"
```

Discover both entitled groups and select the destination using the [storage selection rules](migration.md#discover-the-actual-app-group).
Set `LSB_SELECTED_APP_GROUP` to its expanded identifier. For a **fresh storage directory**, copy directly into that group:

```sh
xcrun devicectl device copy to --device "$LSB_DEVICE" --source "$LSB_WORK/runtime/RuntimeRoot" \
  --destination LSB/iOS/23A343/RuntimeRoot --domain-type appGroupDataContainer --domain-identifier "$LSB_SELECTED_APP_GROUP" \
  --timeout 3600 --json-output "$LSB_WORK/runtime-copy.json"
```

Prepare the `LSB/iOS/23A343` parent directories if the transfer tool requires them. The app does not migrate Documents or an old group.
For an existing setup, stop the host and all guests first, preserve the active runtime, and publish into the selected group's
`LSB/iOS/23A343` directory using the inactive-transfer workflow below. Avoid memorized UUIDs and preserve sibling `data`,
which contains mutable guest state. Do not place runtime files in the store's `Apps` directory.

If native SSH is already available, an incremental inactive transfer can use `rsync -rlp --partial` over the configured Wi-Fi
or loopback-only USB SSH connection. Do not add `--delete` or `--checksum` to a partial transfer. Keep the source and destination
trailing-slash semantics explicit, activate only after the command succeeds, and retain a rollback. The current workflow does not
require a post-copy checksum pass; local patch/signing validation is still required. SSH setup itself is outside this repository.

First discover the destination's canonical App Group directory as described in [migration](migration.md#discover-the-actual-app-group),
then set `LSB_DEVICE_GROUP` to that actual path and `LSB_DEVICE_STORAGE` to its `LSB/iOS/23A343` subdirectory. Direct executable mode
does not inject the two simulator root variables that graphical launchd mode supplies. Pass them explicitly for the small-executable smoke test; otherwise stock libsystem_featureflags
can trap because `SIMULATOR_ROOT` is absent:

```sh
LSB_DEVICE_GROUP='/private/var/mobile/Containers/Shared/AppGroup/YOUR_DISCOVERED_UUID'
LSB_DEVICE_STORAGE="$LSB_DEVICE_GROUP/LSB/iOS/23A343"
DEVICECTL_CHILD_SIMULATOR_ROOT="$LSB_DEVICE_STORAGE/RuntimeRoot" \
DEVICECTL_CHILD_SIMULATOR_SHARED_RESOURCES_DIRECTORY="$LSB_DEVICE_STORAGE/data" \
xcrun devicectl device process launch --device "$LSB_DEVICE" --terminate-existing --console \
  --json-output "$LSB_WORK/df-launch.json" -- "$LSB_BUNDLE_ID" /bin/df > "$LSB_WORK/df-console.log" 2>&1
```

In the earlier iPhone 17 migration, both the previous working host and the then-latest default-off guard build printed the filesystem table
and reported `df` exit status 0 with these two variables supplied. Omitting them reproduced the missing-root feature-flags trap.
Repeat that check after restoring files to the selected store group; copy success does not establish guest execution.

On the tested iOS 27/24A435 device, removing the native cache caused a code-signing fault when a kernel workqueue entered
the previously registered native `start_wqthread`. Retaining the native cache with hardware redirection passed real workqueue
tasks and launchd spawning df without LLDB. The native host now caches MobileGestalt `EffectiveProductionStatusAp` and sends
it through authenticated child check-in. Only an exact Boolean false enables development cache detachment; production/unknown
keeps retention and hardware bridging, and rejects an explicit detach request. The tested iPhone 15 has custom boot arguments
and relaxed TXM enforcement; its detach success is not evidence that stock iOS 26 permits this. Developer Mode alone is not
this status. Explicit `LSB_RETAIN_HOST_CACHE=1` remains available on a development device. See [handoff policy](runtime-handoff.md).
Set `DEVICECTL_CHILD_LSB_DEBUG_STOP_CHILDREN=0` and
`DEVICECTL_CHILD_LSB_DEBUG_STOP_BEFORE_HANDOFF=0` for an unattended test. The handoff rejects a guest cache
reservation overlapping the current native cache; do not assume a historical ASLR address. Keeping the existing native
mapping does not copy its whole cache into each process. See [retained-cache evidence](launchd.md#retained-native-cache-with-hardware-redirection).
The later cache/helper metadata-name patch resolved the observed lsd failure and reached a home screen with app icons on
iPhone18,2 / iOS 27.0 / 24A435 without LLDB. The full migration initially omitted SimulatorHID; restoring it also restored touch delivery, verified by Home Screen
page swipes and opening Contacts. The home screen is not proof that every staged application works.

The host may remain open after the guest exits; inspect its broker exit record. A normal icon launch, or a command with no
arguments after the bundle identifier, now waits for **Start** on the IOSurface tab. Tap it to boot graphical launchd. The tabs
remain available before startup and persist afterward. For an unattended development test, pass `--launchd-services` with no
labels to start the same default selection automatically. Other explicit guest arguments, such as `/bin/df` or `--terminal`,
also retain automatic startup. The default selection comes from
`Host/Broker.m`: it includes SpringBoard, assistantd, configd_sim, systemstatusd, kbd, and the on-demand PosterBoard LaunchAngel.
The generator adds backboardd, cfprefsd, the launchd-owned Fish helper, and the display-settings worker. Explicit `--launchd-services LABEL...` **replaces**
the additional default list. Keep the native app foregrounded for baseline tests.

Handoff success logs are disabled by default. To inspect guest entry, main-stack relocation, and the retained/detached cache
choice, prefix the development launch command with `DEVICECTL_CHILD_LSB_TRACE_HANDOFF=1`. Launchd propagates this exact opt-in to
its children; preparation failures remain visible without it. The Start policy and trace propagation have focused local tests;
[device verification](guest-control.md#device-verification-2026-10-27) also covers the Start screen and default-off banners.
Repeat the boot checks below on your destination device.

For app icons, the staged registration manifest is `RuntimeRoot/usr/share/LiveSimBooter/applications.json`. Run the signed
`/usr/libexec/app-registration --manifest /usr/share/LiveSimBooter/applications.json` as a job **inside the existing simulator
launchd**, after lsd is running. It logs individual registration results; copying bundles alone does not prove registration.
Use the [guest launchctl control setup](guest-control.md) to add its worker job and bootstrap the registration job. The worker
label is opt-in and consumes one selected-job slot; current defaults plus the worker fit the 20-label limit. Bootstrap one-off
registration jobs afterward rather than adding every diagnostic job to the initial selection. In job plists, use Boolean
`<true/>`/`<false/>` for launchd policies and guest paths for logs/working directories. The `launchctl bootstrap` plist argument
and the SSH client's `--data-root` are physical paths. Do not mix these path domains.

For the optional control setup, this is the current default selection plus its worker. Use it instead of the no-argument launch,
and review `Host/Broker.m` if the defaults have changed since this guide:

```sh
xcrun devicectl device process launch --device "$LSB_DEVICE" --terminate-existing --console -- "$LSB_BUNDLE_ID" \
  --launchd-services com.apple.notifyd com.apple.lsd com.apple.runningboardd com.apple.containermanagerd \
  com.apple.fontservicesd com.apple.containermanagerd.system com.apple.distnoted.xpc.daemon com.apple.configd_sim \
  com.apple.sleepd com.apple.systemstatusd com.apple.SpringBoard com.apple.iconservices.iconservicesagent \
  com.apple.TextInput.kbd com.apple.assistantd com.apple.PosterBoard com.apple.DragUI.druid com.kdt.LiveSimBooter.launchctl-control
```

After the worker starts, configure your actual SSH endpoint and group path, then submit the one-off job:

```sh
LSB_SSH_DEST='YOUR_SSH_USER@YOUR_DEVICE_HOST'
LSB_SSH_PORT='YOUR_SSH_PORT'
LSB_DEVICE_GROUP='/private/var/mobile/Containers/Shared/AppGroup/YOUR_DISCOVERED_UUID'
LSB_DEVICE_STORAGE="$LSB_DEVICE_GROUP/LSB/iOS/23A343"
python3 Tools/guest-launchctl.py --host "$LSB_SSH_DEST" --port "$LSB_SSH_PORT" \
  --data-root "$LSB_DEVICE_STORAGE/data" --output-dir "$LSB_WORK/guest-control" -- print user/501
python3 Tools/guest-launchctl.py --host "$LSB_SSH_DEST" --port "$LSB_SSH_PORT" \
  --data-root "$LSB_DEVICE_STORAGE/data" --output-dir "$LSB_WORK/guest-control" -- bootstrap user/501 \
  "$LSB_DEVICE_STORAGE/RuntimeRoot/usr/share/LiveSimBooter/app-registration.plist"
```

Collect `data/Library/Logs/com.kdt.LiveSimBooter.app-registration.log` and its exit status. Do not resubmit an uncertain
launchctl operation: retrieve its original request ID as described in [guest-control.md](guest-control.md).

## Verify and record the result

Success is a sequence of observations, not the completion of a copy command:

1. Inspect app/extension provisioning and signature metadata, cache/signing manifests, and the selected source runtime build.
2. Observe `df` exit 0 and `/bin/spawn-probe --verify-statuses` with spawn/wait/SIGCHLD/SIGTERM results.
3. Observe simulator launchd plus baseline daemons and successful endpoint registrations. The host bridge for selected configd_sim
   supplies only native `com.apple.SystemConfiguration.configd`; the `_sim` services remain guest-owned.
4. Inspect the actual IOSurface image and Fish prompt. From Fish, run `df` and check `$status`. A screenshot is not proof of input delivery;
   try a guest UI action and keep its process alive long enough to observe completion.
5. Check app registration results and launch an app. A successful launch request alone is not proof of a visible, responsive app.

Capture a bounded native amfid/TXM log when debugging signatures or mappings. For LLDB, use the matching native executable and configure
these settings before attachment:

```text
settings set -g target.require-hardware-breakpoint true
settings set -g target.parallel-module-load false
settings show target.parallel-module-load
settings set -g target.memory-module-load-level partial
settings set platform.plugin.darwin.ignored-exceptions EXC_BAD_ACCESS
```

The tested Apple LLDB uses `target.parallel-module-load`; the spelling `target.experimental.parallel-module-loading` silently leaves
that setting enabled. Confirm the displayed Boolean is false. Wait for the actual `Process ... stopped` notification after attaching:
do not issue `process status`, backtraces, expressions, or Python polling while asynchronous attach is still initializing. A prompt alone
does not mean attach completed. The workqueue bridge uses intentional `EXC_BAD_ACCESS`; ignoring it is appropriate for ordinary execution
tests, not a substitute for the bridge's dedicated diagnostics.

Before finishing, detach your debugger, verify the guest resumed, and confirm your remote debugserver session ended. If the LLDB client
deadlocks during module/language-runtime initialization, terminate only that known client and verify detachment/resume independently.
Do not leave the device frozen after collecting a stack trace.
See [debugging.md](debugging.md) for detailed commands. A successful signature registration or mmap can still leave TEXT `r--`;
inspect actual protections when diagnosing a fault. Never pass `DYLD_SHARED_REGION=private` to native app startup.

Common first failures: missing runtime files or profile capabilities; unsigned loose code shadowing the cache; a stale shim/control
ABI; unavailable native backend files; wrong device capabilities/assets; incorrect logical/native PID assumptions; launchd policies
serialized as integers; or a stopped/suspended native host. Record the failing stage, errno, app/guest PIDs, source/cache manifest,
and device OS before changing another variable. Preserve a successful baseline and describe untested behavior explicitly.

The setup helpers do not configure developer accounts, download Apple runtimes or Rust dependencies, install device SSH, select
a device, transfer or launch code, seed private simulator databases, or promise a complete simulator boot. They make the local
preparation reproducible; the device steps and their evidence remain part of setup.
