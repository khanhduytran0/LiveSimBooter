# CoreSimulator host services

The following service names were observed in a Mac CoreSimulator registration dictionary supplied during the audio/haptics
investigation. Numeric Mach port names and object addresses were session-local and are intentionally omitted. This is a
reference inventory, not a list of services currently implemented or forwarded by LiveSimBooter.

```text
IndigoHIDRegistrationPort
com.apple.CoreSimulator.NearbyInteraction
com.apple.CoreSimulator.SimCaptureService
com.apple.CoreSimulator.SimFramebufferServer
com.apple.CoreSimulator.SimIdentityService
com.apple.CoreSimulator.host.bootstrap_port
com.apple.FSEvents
com.apple.IOSurface.Remote
com.apple.PowerManagement.control
com.apple.SystemConfiguration.PPPController
com.apple.SystemConfiguration.configd
com.apple.SystemConfiguration.configd_host
com.apple.audio.audiohald
com.apple.audio.coreaudiod
com.apple.coreservices.lsuseractivity.simulatorsupport
com.apple.fpsd.sim
com.apple.gputools.service.simulator
com.apple.metal.simulator
com.apple.metal.simulator.Carousel
com.apple.metal.simulator.HeadBoard
com.apple.metal.simulator.PineBoard
com.apple.metal.simulator.SBRendererService
com.apple.metal.simulator.SpringBoard
com.apple.metal.simulator.backboardd
com.apple.storekitservice.sim2host
```

## Audio reference

[SimRun's pinned notes](https://github.com/unixzii/SimRun/blob/f49aea096320dffcea948bbbf74337d02c795800/docs/simns.md#L88-L96)
describe forwarding the Mac's `com.apple.audio.audiohald` port into the guest namespace. That supports AudioQueue, RemoteIO,
AVAudioEngine, AVAudioPlayer and AVPlayer on its Mac host. System sounds additionally use the guest
`systemsoundserver-simd` job (`com.apple.audio.SystemSoundServer-Sim`). The notes also describe re-registering after a native
audio-server restart, and the lifetime of the server's audio assertion. They are evidence about SimRun, not device validation
of this project's audio output.

The pinned 23A343 simulator CoreAudio is a desktop HAL client. A native iPhone audio service cannot be assumed to implement
the same Mach protocol merely because both platforms have CoreAudio. Keep service forwarding and any compatibility adapter
scoped to a verified protocol, and distinguish accepted playback APIs from physically observed output.
