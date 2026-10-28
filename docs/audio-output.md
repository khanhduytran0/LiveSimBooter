# Experimental simulator audio output

Enable **Settings → Experimental audio → Audio output** to restart the simulator with the bridge enabled. It defaults off;
the preference persists independently of Silent background audio. Exactly `LSB_AUDIO_OUTPUT=1` in the native host launch
environment forces it on for diagnostics. The broker supplies the per-session guest flag.

The optional `libLiveSimAudio.dylib` exposes a fixed output-only CoreAudio HAL device to simulator guests when
`LSB_AUDIO_OUTPUT=1`. It keeps guest AudioQueue, RemoteIO, converters and mixers in simulator libraries. The host
owns native AudioQueue playback; native audio implementations never execute on a guest thread.

The format is 48 kHz, stereo, interleaved Float32, in 512-frame blocks. One lazy guest worker mixes at most eight
IOProcs and copies samples into an 8192-frame shared ring acquired from the authenticated native broker. The worker
uses an absolute clock, catches up after small scheduling delays and resets after a delay exceeding four blocks.
While idle it waits on a condition variable. The native callback only reads the ring and enqueues its native buffer.
Neither render path performs broker IPC. A full ring drops new frames; an empty ring produces silence. Samples are
bounded and non-finite values are rejected. This is not adaptive resampling or guaranteed glitch-free playback.

AudioQueue builds a private simulator aggregate around the fixed output. Each aggregate has its own device/stream
IDs and immutable composition referencing the physical output UID. Unknown virtual properties fail; unrelated HAL
objects retain stock forwarding. Default input is explicitly absent. Static format/topology listeners are accepted;
running-state notifications and route changes are unsupported. Stop waits for callback completion and final stream
closure, and callback-initiated destruction defers reclamation until the callback returns.

Build the separate guest shim with `Tools/build-audio-output-shim.sh` and the probe with
`Tools/build-audio-output-probe.sh`, then sign and stage the executable as
`RuntimeRoot/usr/local/bin/lsb-audio-output-probe`. Fish includes this directory in `PATH`; run
`lsb-audio-output-probe` for AudioQueue, or add `--direct` to drive
the virtual HAL directly. `LSB_AUDIO_TRACE=1` enables bounded diagnostic
logging for unknown virtual properties and forwarded system queries. Both environment switches require exact `1`.

## Evidence and limits

- The user confirmed the direct HAL tone was audible on the development phone. That first test exited 0 with 111
  callbacks in 1.4 seconds; it preceded the absolute-clock correction. It proves guest PCM transport and native
  playback, not stock framework integration.
- Stock AudioQueue subsequently passed after aggregate composition and the legacy `duid` UID translation were implemented.
  Queue creation/Start returned 0, 136 callbacks completed, and the probe exited 0. The native queue consumed 68,608 frames
  with zero underrun frames; the user confirmed its tone was audible.
- Separate unmodified-API AVAudioEngine and AVAudioPlayer probes each played a finite one-second tone, reported successful
  playback completion, and exited 0. The user confirmed both were audible. AVAudioEngine rendered 51,712 frames with
  512 underrun frames; this is a working basic output path, not a guarantee of glitch-free playback. Arbitrary guest apps
  and direct RemoteIO use are not yet individually verified.
- These tests used the 23A343 guest on iPhone16,2 / native iOS 26.1 build 23B50, with no debugger attached. The unchanged
  cache was based at `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`. Native host PID 3981 served
  the successful AudioQueue test. Helpers and probe were separately signed with the configured development identity
  and app identifier, staged after stopping guests, and tested with the host foregrounded.
- Focused native tests pass for the exact opt-in gate and native forwarding, fixed format and input absence, short
  output buffers, unsupported selectors, aggregate ownership and identities, transport failure, PCM callbacks,
  nonblocking short writes, repeated Start, Stop completion, callback self-destruction, stale IDs and bounded IOProc
  registration. An eight-block callback test checks absolute timestamp increments. Strict arm64 simulator compilation
  also passes. These are separate from signature acceptance and device playback tests.
- The final native Settings build was launched without forced audio/haptic environment variables. The user enabled both
  switches and confirmed both session restarts succeeded; both preferences persisted as Booleans. Test executables live
  in `RuntimeRoot/usr/local/bin`, which the launchd-owned Fish environment includes in `PATH`.
- Microphone input, configurable device formats, scheduled starts, dynamic routes, full guest AudioSession policy,
  system sounds and haptic playback are outside this HAL output bridge. Recovery from native audio-server interruption
  is not implemented; restart the simulator session after such a failure. Simulator system clicks additionally use
  `systemsoundserver-simd`; the haptic/native AudioSession bridge is a separate component.

## WebKit test status

JavaScriptCore accepts `JSC_useJIT=false`, but it must reach WebContent before JSC initializes; setting it only on the UI
application does not establish that its separate worker inherited it. WebKit also exposes the private process-pool
`JITEnabled` setting, which sends `disable-jit` during worker bootstrap. See the original
[option reader](https://github.com/apple-oss-distributions/WebKit/blob/2331c43da3e7123e51ba8682d7ec023b8591460e/Source/JavaScriptCore/runtime/Options.cpp)
and [worker bootstrap](https://github.com/apple-oss-distributions/WebKit/blob/2331c43da3e7123e51ba8682d7ec023b8591460e/Source/WebKit/Shared/EntryPointUtilities/Cocoa/XPCService/XPCServiceEntryPoint.mm).

An arm64 simulator test app was signed, registered and launched on the same iPhone 15 baseline. It sets the pool's
`JITEnabled=NO` before constructing WKWebView and requires the child's `_isJITEnabled:` reply before enabling its finite,
user-triggered WebAudio tone. Its scene and WKWebView initialize, but WebContent exits before the page loads or the query
completes. Native logs record SIGTRAP after `launch_extension_check_in_live_4UIKit` reports routine 716 returning 1; that
assertion's logging branch returns normally, so it does not identify the trapping instruction. Crash-report quota prevented
a fresh stack. **WebKit audio and the resulting child JIT state remain unverified.**

The UI process also exposed a separate missing `SIMULATOR_MEMORY_WARNINGS` path. Pinned simulator libdispatch uses that
environment variable to open a file with `O_EVTONLY` and watch `NOTE_ATTRIB`; it never reads file contents. An empty owned
file was supplied through guest launchctl for the second diagnostic session, but WebContent still terminated. This is a
test-session setting, not a native memory-pressure relay or a persistent setup change.

## Pinned provenance

The HAL ABI and setup sequence were inspected in original iOS Simulator **26.0 / 23A343** inputs, left unchanged:
AudioToolbox UUID `16A74A51-4CD9-39EE-A6F4-6423357C7208`, CoreAudio UUID
`E1DD10CB-BF7A-322C-BEB3-8E3A035C34A9`. Image-relative evidence:

- CoreAudio `HALC_ProxySystem` looks up `com.apple.audio.audiohald` at `0x26e23c..0x26e250`; this is a Mac HAL Mach
  service with shared IO state, not a native `audiomxd` NSXPC protocol that can be forwarded verbatim.
- AudioToolbox `AggregateDevice::buildOrRebuild` requires aggregate composition at `0x12aac..0x12b58` and validates
  child channel counts through `0x130c8`. The physical child must not expose a self-referencing composition.
- AudioToolbox `AQMEIO_HAL::StartIO_Primitive`, `0x121754..0x121814`, uses ordinary Start when device `dspd` is absent.
- CoreAudio `AudioObjectAddPropertyListenerInternal`, `0x34818`, uses the same four-argument ABI as the public entry.

Local analysis and device logs remain ignored artifacts, not setup dependencies. Revalidate these private assumptions
before changing the pinned runtime.
