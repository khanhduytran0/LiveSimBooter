# Guest haptics

An opt-in experimental path connects guest CoreHaptics to a native `com.apple.audio.hapticd` endpoint. It has not reached
physical playback: the current diagnostic initializes an engine but loses its connection when starting it. The separately
verified keyboard-pattern correction below fixes AHAP loading and keyboard input;
it does not establish physical haptic output.

## Native service and capability projection

Set `LSB_EXPERIMENTAL_HAPTICS=1` in the **native host launch environment** to enable this investigation. Only that exact value
enables it. By default the host publishes the staged stock capability values unchanged, does not query the native haptic capability,
and does not look up or publish the hapticd endpoint. Setting an environment variable in Fish or a guest does not enable it.
Cold-relaunch the native app without the variable to restore the staged original profile; ordinary userspace reboot preserves
the native host's launch environment. Preserve the original staged profiles when using this experiment.

With this opt-in during graphical startup, [the native broker](../Host/Broker.m) looks up exactly `com.apple.audio.hapticd` in its native
bootstrap namespace. If that returns a valid send right, the broker registers it in this session's simulator launchd bridge
namespace after the critical display endpoints. The lookup reference is released after registration. A missing service,
registration rejection, or unknown timeout is logged and does not fail baseline display startup; the request is not retried.
Guests cannot supply arbitrary native service names. Their NSXPC connections retain the real native extension audit identity,
and native hapticd retains its own authorization policy.

Pinned 23A343 CoreHaptics `-[AVHapticClient setupConnectionWithOptions:error:]` uses this service name by default and the
`CHHapticServerInterface`/`CHHapticClientInterface` protocols. Its hardware-capability path is not an unconditional simulator
false: `+[AVHapticPlayer supportsHaptics]` reaches `MGGetBoolAnswer("gqDnklGQnpv5ilgh5uHckw")` through
`PlatformUtilities_iOS::ProductSupportsClosedLoopHaptics`. In the pinned simulator `libMobileGestalt.dylib`
(`878A27E4-22E8-3E39-8CE9-62FEB8301B36`), the callback at image offset `0xec80` reads the plaintext
`capabilities.DeviceSupportsClosedLoopHaptics` key through the SimCapabilities reader at `0x2ccbc`. That reader loads
the existing `SIMULATOR_CAPABILITIES` plist once per process. The supplied device profiles omit this key.

[DeviceConfiguration](../Host/DeviceConfiguration.m), under the same opt-in, queries native `MGCopyAnswer` for the corresponding hashed question
once per native host lifetime. Only an actual CFBoolean true enables the projected capability. False, unavailable, and
non-Boolean responses publish false, including when the input was a restored generated profile containing an old true.
Cold startup and device-picker apply adapt just this one key while atomically publishing the active capabilities plist.
The selected simulator model, geometry, and other capability values remain unchanged; the original staged file is untouched. Normal userspace
reboots retain the same physical-device result. No guest interposer, shared-cache patch, or protocol-layout change is used
for this projection. Native query status and optional service-registration status are logged separately.

The iPhone 15 / iOS 26.1 trials with the 23A343 guest ran without LLDB. Native lookup, guest registration, and the guest's
own hapticd lookup succeeded. The unadapted profile returned `closed-loop-gestalt=0` and `supports-haptics=0`; after projection,
the host reported an exact native CFBoolean true and the guest reported both values as 1. These results establish discovery
and capability propagation, not a working feedback path.

The bounded diagnostic then found three separate boundaries:

1. Public `-[CHHapticEngine initAndReturnError:]` fails with CoreHaptics error **-4815**, `InvalidAudioSession`. The pinned
   implementation obtains an auxiliary AVAudioSession whose session ID is zero in this guest. Setting `playsHapticsOnly`
   after construction does not avoid that initializer check.
2. A typed private `initWithOptions:error:` call with the stock `BypassAudioSession` option avoids audio-session creation,
   but engine initialization fails with **NSCocoaErrorDomain 4097**, connection to `com.apple.audio.hapticd`.
3. An explicitly enabled, temporary interposer changes only the inactive connection to that exact service from simulator
   XPC wire mode 1 to native mode 0. With this adaptation, engine initialization succeeds, but the stopped handler receives
   reason -1 and `startAndReturnError:` fails with **NSCocoaErrorDomain 4097**. No pattern was played. The reason for this
   later connection loss is still under investigation.

The temporary connection adaptation is not part of the guest shim or setup overlay. It is loaded as an ordinary diagnostic
dylib dependency and enabled from `main`, preserving the production inserted-library policy. Before accessing its named
connection layout, it verifies the pinned libxpc UUID `8E65DA13-E624-392F-8392-B80BF6FEC65E`, the original setter/getter
instructions, and inactive state/mode. It neither changes unrelated services nor fabricates an audio-session identifier.
The generic `xpc_connection_enable_sim2host_4sim` API selects mode 2, which is different from the native mode required by
this unmodified service. The probe logs connection creation to distinguish another connection from a later protocol or
service rejection; the evidence so far does not identify the final cause.

The current capability and endpoint publication are unfinished experimental implementation, not a claim that normal UIKit
feedback works. In the pinned UIKitCore, even its haptics-only feedback engine creates the audio-session-based engine first,
then applies `playsHapticsOnly`; it also needs a narrowly scoped construction adaptation if the native transport is proven.
Capability/endpoint publication remain opt-in together so ordinary guests do not discover a physical feature whose bridge
cannot yet complete a request. The bounded probe mutes audio and
requests one transient; even an accepted player request would still require observer confirmation of a felt pulse.

Broker tests cover default/invalid opt-in exclusion, lookup/registration errors, unknown timeouts without retry, send-right ownership, full audit identity,
headless exclusion, and independence from display startup. DeviceConfiguration tests cover exact CFBoolean handling,
all catalog profiles, unchanged identity/geometry, immutable staged inputs, stale true removal, malformed plists, and repeat
publication. They also verify stock-value preservation for every catalog device, actual plist Booleans after serialization,
and restoration of stock values after disabling the opt-in. Active profiles are serialized once for atomic publication, so their
encoding may change while staged input bytes remain untouched. Ignored `tmp/haptic-bridge/` holds the diagnostic source,
binaries, disassembly and local logs; these are experiment artifacts, not setup prerequisites.

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
