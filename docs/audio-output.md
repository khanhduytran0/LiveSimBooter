# Experimental simulator audio output

Enable **Settings → Experimental audio → Audio output** to save the bridge setting. While running, choose Restart or Later;
while powered off, it applies on the next Start. It defaults off and persists independently of Silent background audio.
Exactly `LSB_AUDIO_OUTPUT=1` in the native host launch
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

JIT-less WebContent startup and audible WebAudio output are verified on the pinned runtime. The generated guest `Bootstrap.Child` environment now
persistently supplies `JSC_useJIT=false`, `GIGACAGE_ENABLED=false`, and `SIMULATOR_MEMORY_WARNINGS` pointing to
`<data>/var/run/LiveSimBooter-memory-warnings`. These apply before guest jobs initialize; the native host environment is
unchanged. Disabling Gigacage also disables its heap-isolation mitigation. The memory-warning file is empty: simulator
libdispatch opens it with `O_EVTONLY` and watches `NOTE_ATTRIB`, but no native memory-pressure relay is implemented.

The current nine-site launchd identity patch fixes ExtensionKit's routine 716 alive check-in by translating the audited
native PID into the committed logical guest PID. It preserves full audit-token validation and authorization checks. See
[the identity adapter](launchd.md#native-identity-and-service-ownership). JavaScriptCore's environment option must reach
WebContent before JSC initializes; setting it only on the UI application is insufficient. See the original
[option reader](https://github.com/apple-oss-distributions/WebKit/blob/2331c43da3e7123e51ba8682d7ec023b8591460e/Source/JavaScriptCore/runtime/Options.cpp)
and [worker bootstrap](https://github.com/apple-oss-distributions/WebKit/blob/2331c43da3e7123e51ba8682d7ec023b8591460e/Source/WebKit/Shared/EntryPointUtilities/Cocoa/XPCService/XPCServiceEntryPoint.mm).

The 2026-10-28 cold-session test used iOS Simulator **26.0 / 23A343**, arm64, on development iPhone16,2 running native
**26.1 / 23B50**. The cache remained at `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`. Native host PID
5595 launched the signed test app (logical PID 5664 / native PID 5665); WebContent was logical PID 5667 / native PID 5669. The app used the
default `WKProcessPool` with no private pool configuration or application JIT setter. Local-page navigation and a JavaScript
callback completed, and the actual child's `_isJITEnabled:` query returned `enabled=0`.

That first session's GPU processes (native PIDs 5672, 5676 and 5678) each aborted after roughly 2.3 seconds with CoreAnimation
exit code 7, decoded in the pinned QuartzCore as "Metal failed to specialize fragment function". The compiler-registration
adapter now also admits the exact guest WebKit GPU executable,
`/System/Library/ExtensionKit/Extensions/GPUExtension.appex/com.apple.WebKit.GPU`, so its own PID domain can launch
`MTLCompilerService`. The original GPU executable was restored and the temporary diagnostic injection removed before retesting.

The subsequent cold session used native host PID 5737, guest launchd native PID 5741, UI logical PID 5807 / native PID 5808,
WebContent native PID 5811, GPU native PID 5814 and compiler native PIDs 5818 and 5821. With the same default pool and disabled
JIT, repeated finite WebAudio tones reached `state=running` at 48 kHz and completed. The user confirmed they were audible.
When the GPU's audio output later closed, the native queue reported 256,512 rendered frames and 1,536 underrun frames;
this verifies the basic WebAudio path, not uninterrupted playback under all workloads.

Selecting text initially crashed the UI process (native PID 5808) with SIGSEGV. GPU PID 5814 and WebContent PID 5811 then
exited 0 after the UI process died; these were not repetitions of the earlier GPU code-7 abort. The TextToSpeech override
below resolved the reproduced selection failure. Local WebAudio evidence is retained under
`tmp/webcontent-20261028/startup/results/` and `tmp/webcontent-20261028/compiler-fix/results/`, including the broker console
and `web-audio-full.log`; these ignored logs are not setup inputs.

### Text selection and TextToSpeech

An LLDB capture with hardware-only breakpoint policy caught SIGSEGV in native UI PID 5864 at a call through address zero in cached TextToSpeech
(UUID `70B4AAB0-019B-3631-97CD-65F434AC0A2E`). Its stub at image offset `0x1cf008` called the weak SwiftSQLite import
`_$s11SwiftSQLite5TableVMa`. The current 1,353-image cache omitted SwiftSQLite and contains zero at this import's GOT
address, `0x174c857f0`. The correctly signed loose SwiftSQLite library was already present on disk.

The cache records missing weak dependencies and does not redo cached-image fixups when the provider later appears;
see the pinned [prebuilt loader dependency and fixup paths](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/PrebuiltLoader.cpp#L398-L462).
This development cache accepts a loose TextToSpeech override, which restores its original binding metadata. All 58 other
TextToSpeech dependencies are cached, all 64 SwiftSQLite imports exist in the stock provider, and that provider's ten
dependencies are cached. The fix stages only the original 23A343 TextToSpeech executable at its normal framework path,
signed with the configured host identifier and no native entitlements; its Mach-O type, platform and sections remain intact.
Original runtime inputs and the cache are unchanged.

After stopping the simulator, installing that override and starting a fresh session, the user confirmed responsive text
selection without crashes or freezes, and working text-to-speech. This test used native host PID 5929, guest launchd native
PID 5933 and UI logical PID 6002 / native PID 6003 on the same 23A343 cache and iPhone16,2 / 26.1 / 23B50 baseline.
The diagnostic debugger was detached before restart; verification used no debugger. The app also completed navigation,
reported WebContent native PID 6006 with JIT disabled, and completed repeated WebAudio tones. A read-only native task-memory
inspection, without pausing the UI process, confirmed loose TextToSpeech at `0x10cc60000` and SwiftSQLite at `0x10c900000`.
The repaired TextToSpeech slot at offset `0x265dc8` held `0x10c93b81c`, exactly SwiftSQLite's exported accessor at offset
`0x3b81c`. The inspection released its task right. Evidence is under `tmp/webcontent-20261028/tts-override/`, including
`results/mapped-images.json`; the earlier failure capture is `tmp/webcontent-20261028/results/lldb-text-selection.txt`.

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
