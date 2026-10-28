# Guest haptics

The experimental bridge gives guest CoreHaptics an authenticated, session-owned native haptic relay. A public CoreHaptics
engine produced a physically confirmed transient on the iPhone 15 with the pinned guest runtime. The separately verified keyboard-pattern correction below fixes AHAP loading
and keyboard input; it does not itself establish physical haptic output.

## Native relay and capability projection

Haptics default off. Enable the native Settings option and apply it to a fresh simulator session, or launch the native host
with exactly `LSB_EXPERIMENTAL_HAPTICS=1`. The broker snapshots the choice for that session. A guest environment variable
cannot enable a host service. Disabled sessions preserve the staged stock capability profile and do not load the optional
Objective-C guest haptic helper. See [setup](agent-setup.md) for helper staging and [host services](simulator-host-services.md) for the service inventory.

[HapticServer](../Host/HapticServer.m) publishes only `com.apple.audio.hapticd` into the session's simulator bootstrap namespace.
It admits current simulator jobs through the broker's full native audit-token check and owns at most 64 peer connections.
Its NSXPC listener uses Apple's `xpc_connection_enable_sim2host_4sim` before activation. The optional guest helper calls
the same API for exactly the inactive `com.apple.audio.hapticd` client connection: both ends must select mode 2.
Plain simulator mode 1 does not match this relay, and a local native mode-0/mode-2 control also failed with XPC error 4097.
Stock `NSXPCListener` constructs each peer; no libxpc memory layout or NSXPC implementation address is patched.
The typed server/callback protocols in [PrivateHaptics](../Host/PrivateHaptics.h) follow the pinned CoreHaptics interface,
including the native shared-memory ring reply. Event timing and pattern playback remain in CoreHaptics/native audiomxd.

Each admitted peer gets a genuine **host-owned** auxiliary AVAudioSession, task identity token, and native NSXPC connection.
The relay replaces the guest's audio-session ID, sharing flag, bypass flag and process token before configuring audiomxd;
`BypassAudioSession` is always false on that native leg. Unrelated pattern/audio options are preserved. The host is the actual
native client, so native foreground/background and actuator policy still apply to it. Guest identity remains available for
relay admission; it is not substituted into a forged native audit token. Disconnect, native interruption or simulator shutdown
invalidates both legs and releases the native session. Admission and cleanup run off main; shutdown never waits on main.

The optional [HapticCompatibility](../Guest/HapticCompatibility.m) constructor handles the simulator's intentionally empty
AudioSession implementation. It checks the original 23A343 CoreHaptics UUID, method encoding and initial instructions,
then adapts only `doInit:sessionIsShared:options:bypassAudioSession:error:` with session ID zero and the default haptic service.
That bypass applies to the **local constructor**; the relay replaces it with its real host session before any native request.
Other services and nonzero session IDs keep their original constructor behavior. The helper is separate from the C shim
and links Foundation/CoreHaptics only for enabled sessions. The original method remains responsible for engine state,
player construction, callbacks and error reporting.

Pinned 23A343 CoreHaptics (`2AFD3C06-3A36-35CE-8940-B381C1AFFC0B`)
`-[AVHapticClient setupConnectionWithOptions:error:]` uses the fixed service and the
`CHHapticServerInterface`/`CHHapticClientInterface` protocols. Its capability path reaches
`MGGetBoolAnswer("gqDnklGQnpv5ilgh5uHckw")` through `PlatformUtilities_iOS::ProductSupportsClosedLoopHaptics`.
In pinned simulator libMobileGestalt (`878A27E4-22E8-3E39-8CE9-62FEB8301B36`), callback `+0xec80` reads
`capabilities.DeviceSupportsClosedLoopHaptics` through the SimCapabilities reader at `+0x2ccbc`, which loads
`SIMULATOR_CAPABILITIES` once per process. The supplied profiles omit this key.

[DeviceConfiguration](../Host/DeviceConfiguration.m) queries the corresponding native MGCopyAnswer once per host lifetime.
Only an actual CFBoolean true enables the projected capability. False, absent and malformed responses publish false.
Only this key changes; simulator model, geometry and other capabilities remain intact, and the staged original stays immutable.
Disabling haptics republishes that original profile, removing any earlier generated true value.

## Optional playback probe

Build the unsigned arm64 simulator probe with the same SDK setup as the guest helpers:

```sh
sh Tools/build-haptic-probe.sh "$LSB_WORK/guest/lsb-haptic-relay-probe"
```

Add `--add-binary "/usr/local/bin/lsb-haptic-relay-probe=$LSB_WORK/guest/lsb-haptic-relay-probe"` to the runtime staging
command in [setup](agent-setup.md). The stager signs it using the destination app's identity and identifier. For an existing
runtime, use [sign-sim-executable.py](../Tools/sign-sim-executable.py) with those same signing settings before copying it;
this preserves `MH_EXECUTE` while clearing `CS_EXECSEG_MAIN_BINARY`. Stop affected guests before replacing an installed binary.

Enable haptics for the simulator session, stage `libLiveSimHaptics.dylib`, keep the native host foregrounded, and hold the phone.
Discover the **current native host PID** from the native process list, then run this in guest Fish:

```sh
/usr/local/bin/lsb-haptic-relay-probe --expected-host-pid CURRENT_NATIVE_HOST_PID
```

The probe first verifies that the service peer is that host PID. A missing or different peer stops the test before engine
construction; do not substitute native audiomxd's PID. It then uses the public CoreHaptics initializer, plays one transient,
and stops. Exit status 0 confirms accepted playback and cleanup; a person must separately confirm physical vibration.
It has a 20-second deadline and never requests the unsafe direct-native bypass described below.

## Evidence and limits

The iPhone 15 / iOS 26.1 trials with the 23A343 guest ran without LLDB. Native MG returned an exact true Boolean;
after projection the guest reported `closed-loop-gestalt=1` and `supports-haptics=1`. Native and guest endpoint lookups succeeded.
Subsequent probes isolated these boundaries:

1. The stock public engine initializer fails with CoreHaptics **-4815 InvalidAudioSession**: the simulator's auxiliary
   AVAudioSession has ID zero. Setting `playsHapticsOnly` after construction is too late.
2. A temporary, pinned connection-mode diagnostic reached native audiomxd directly. Sending its private bypass option
   without a real audio session caused a native audiomxd null dereference during start. Do not repeat that experiment.
3. A typed diagnostic created a genuine native auxiliary session under the extension's actual audit identity, kept it alive
   through engine start/play/stop, then destroyed it. All API operations succeeded and audiomxd remained stable. The user
   felt no vibration: native logs explicitly marked the extension `BackgroundRunning` and muted its haptic channel.
4. The relay therefore moves **ownership**, not guest identity, to the foreground host. On 2026-10-28, the normal public
   `initAndReturnError:` succeeded through the optional helper, followed by start, one transient, stop and exit status 0.
   The user repeated the command while holding the phone and confirmed a felt vibration. The native logs no longer showed
   the haptic background mute; the audio-only mute was expected from `playsHapticsOnly`.

The successful run used host PID 4076 and guest probe PID 4144 (repeat 4149), without LLDB. Its preflight NSXPC query
verified that the peer PID was the native host. Shared-memory/event playback then used the stock guest CoreHaptics engine,
not an independent host vibration call. This establishes the public engine path; ordinary UIKit feedback and keyboard
haptic preferences still need separate interaction checks. `LSB_HAPTIC_TRACE=1` in the native host environment enables
bounded peer/connection lifecycle logs; failures remain bounded and visible without this tracing flag.

The earlier direct-native endpoints and connection-mode patch are not the production path. No native AudioSession service
is published to guests by the relay implementation. Research sources, binaries and native logs are under ignored
`tmp/haptic-continue-20261028/`; these local artifacts are not setup prerequisites.

Focused [relay tests](../Tests/HapticRelayTests.m) exercise exact Boolean/identity replacement, preservation of caller options,
real NSXPC mode-2 admission off main with a full audit token, mixed 64-bit/double/error reply signatures, and the actual XPC
shared-memory reply. Broker tests cover session/generation admission and endpoint lifetime; DeviceConfiguration tests cover
stock preservation, typed native capability projection and restoration. The device observation above is separate from these local checks.

The final build was launched without forced feature environment variables. The user confirmed that both Haptic passthrough
and Audio output applied from native Settings with simulator restarts; both preferences persisted as Booleans. Ordinary
UIKit/keyboard feedback remains separate from the confirmed public CoreHaptics probe.

## Keyboard-pattern URL correction on the iOS 27 host

The iOS Simulator 26.0/23A343 CoreHaptics image (`2AFD3C06-3A36-35CE-8940-B381C1AFFC0B`) crashed when Contacts
opened its keyboard on iPhone18,2 running iOS 27.0/24A435. The guest reads the native system keyboard AHAP files under
`/Library/Audio/Tunings/Generic/Haptics/Patterns`. Their relative `AudioResources/Key*.caf` references are valid, but stock
`+[CHHapticPattern patternForKey:error:]` parses the AHAP into a dictionary and loses the source directory. The dictionary
initializer returns error -4830; the stock caller then stores the pattern identifier through nil + 0x18.

Controls isolated path resolution from AHAP version or missing resources: all eight original keyboard dictionaries failed,
while both absolute waveform paths and `initWithContentsOfURL:error:` succeeded for the same files. No audio events need
to be removed and no global working-directory change is needed.

[patch-ios26-keyboard-haptics.py](../Tools/patch-ios26-keyboard-haptics.py) changes the pinned system-pattern loader to pass
its existing path through `NSURL` and the public URL initializer. Existing ownership, cleanup, NSError handling and pattern
identifier assignment are preserved; a nil guard handles other initializer failures. App dictionary initializers are untouched.
The tool validates the original image UUID, section extents, instructions, selector/class references, branch targets and
mapping protections before producing a separate output. It refuses unexpected, partial or already patched inputs. Its current
layout profile is the tracked 1,353-image cache at `0x110000000`, reservation size `0x7e194000`.

Apply this after the container-metadata patch and before signing:

```sh
python3 Tools/patch-ios26-keyboard-haptics.py --source "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64" \
  --output "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64.haptics" > "$LSB_WORK/cache-haptics-patch.json"
python3 Tools/sign-dyld-cache.py "$LSB_WORK/patched-cache/dyld_sim_shared_cache_arm64.haptics" \
  "$LSB_WORK/signed-cache/dyld_sim_shared_cache_arm64" \
  --identity "$LSB_SIGNING_IDENTITY" --identifier "$LSB_BUNDLE_ID" --team "$LSB_TEAM"
```

The device test changed 29 bytes across seven instruction spans, signed the new cache using the same app identity and
identifier, and published it with affected guests stopped and a rollback retained. Post-copy checksums were skipped as
requested. Cache UUID remained `73ED978A-FD25-3166-A57A-34E21BB7E5B8`; the new CodeDirectory hash is
`ed8d1b7d245abdacdd12a81e97a5ca7b8a299352`. Host11690 / launchd11694 retained the native cache and used hardware
workqueue redirection without LLDB. All eight stock `patternForKey:error:` initializations succeeded with nil NSError.
The user then confirmed the simulator keyboard works. This verifies pattern construction and UI input, not native haptic
actuator playback.

Sixteen focused patcher tests cover valid patching, immutable inputs, malformed layouts, unexpected instructions and
references, branch safety, duplicate patches and output collisions. Local evidence is under ignored
`tmp/lsd-metadata-default/`: `haptic-probe/path-analysis.md`, `results/haptic-probe-v2.log`, `results/haptic-probe-v3.log`,
and the patch/signing manifests. These local files are evidence, not setup dependencies.
