# LiveSimBooter

An experimental native iPhone host for an arm64 iOS Simulator runtime. The native app owns UIKit, process launches,
and display/input services; simulator programs run in separate LiveProcess instances using the device’s kernel.
It does not emulate a CPU or boot a separate kernel. See [the architecture](docs/architecture.md) for the process boundaries.

## Setup

Start with the [agent-assisted setup guide](docs/agent-setup.md) and [AGENTS.md](AGENTS.md).
The supported guest baseline is **iOS Simulator 26.0, build 23A343**. Runtime patches and private interfaces are pinned
to that build; a different iOS 26.x or iOS 27 runtime is not an interchangeable input.

There is no in-app runtime installer. Supply the original runtime, signing identity, matching provisioning profiles,
and device access, then use the tracked setup/build/sign/copy steps. Generated binaries and runtime files stay outside Git.
Runtime and mutable state live at `LSB/iOS/23A343/{RuntimeRoot,data}` inside the selected SideStore or AltStore App Group.
This layout selects the pinned build; there is no runtime-version picker. Both native targets need the same
two group entitlements. [Device migration](docs/migration.md) covers selection, manual moves from older locations, and rollback;
there is no automatic migration. Keep the owning store installed and entitled to that group to retain shared storage when uninstalling this app.

## Running

After setup, open the app and tap **Start** on the **IOSurface** tab. Keep the native host in the foreground for baseline use.
The persistent **IOSurface**, **Fish**, and **Settings** tabs share one simulator session; switching tabs does not restart guests.
Fish belongs to simulator launchd, and Settings provides the guest performance HUD and touch-marker controls.
Device, haptic and audio choices can be saved while the simulator is off. During a session, choose **Restart** or **Later**
to apply them. Settings also provides **Power Off** and **Restart**; Power Off returns IOSurface to Start without closing the host.
The [device picker](docs/device-types.md) changes the simulated model and resolution on the next start or restart.
Silent background audio is optional and disabled by default.

Explicit guest arguments, such as `--launchd-services` or `/bin/df`, retain automatic startup for development tests.
Guest `launchctl` can manage individual jobs and request a simulator userspace reboot; see [guest control](docs/guest-control.md).
Production devices retain the native shared cache and redirect workqueue entry through a hardware breakpoint.
Cache detachment is restricted to devices reporting non-production status; see [handoff policy](docs/runtime-handoff.md).

## Status and limits

The matching runtime has reached SpringBoard with app icons on iPhone18,2 running iOS 27.0 build 24A435.
Device tests have verified Contacts, touch and keyboard input, native Metal presentation, interactive Fish, and display settings.
App launching also passed a cold-start check and two consecutive userspace reboots; these results do not establish complete app compatibility.

Home Screen → Edit → Edit Wallpaper supports color/gradient rendering. PosterBoard’s lock-screen path and direct activation
remain broken with black output; the editor’s clipped Done button is also deferred. See [wallpaper status](docs/springboard-services.md#current-wallpaper-status).

Generic `fork`/`execve`, native PTYs, stopped/continued wait events, and full job control are not implemented.
The supported process-replacement path uses `POSIX_SPAWN_SETEXEC`. Missing daemons, resources, and private API differences
still limit applications; the observed Spotlight last-character crash is deferred. This remains a partial simulator environment.

## Documentation

- [Architecture](docs/architecture.md) and [runtime handoff](docs/runtime-handoff.md): loading, callbacks, threads, and debugger metadata.
- [Processes](docs/multiprocess.md), [launchd](docs/launchd.md), and [SpringBoard services](docs/springboard-services.md): lifecycle and compatibility.
- [Framebuffer](docs/framebuffer.md), [IOSurface](docs/iosurface.md), and [Metal](docs/metal.md): graphics transport and native helpers.
- [Fish terminal](docs/fake-pty.md), [display settings](docs/display-settings.md), and [guest control](docs/guest-control.md): interactive use.
- [Shared cache](docs/shared-cache.md), [iOS binary compatibility](docs/ios-platform.md), and [debugging](docs/debugging.md): preparation and evidence.

The [documentation index](docs/README.md) includes the remaining topics and historical experiments.
Use the setup guide for current commands; older notes describe earlier cache layouts and device configurations.

## Development checks

On an Apple Silicon Mac with Xcode:

```sh
sh Tests/run.sh
python3 Tests/audit-local-runtime.py
```

Build the native host and guest shim separately using [the setup guide](docs/agent-setup.md).
Local tests do not establish device signature acceptance, working mappings, or a successful UI operation.

## License

Original project code is available under the [MIT License](LICENSE), copyright 2026 Duy Tran.
Apple-derived portions retain the [Apple Public Source License 2.0](LICENSES/APSL-2.0.txt), their notices, and separate obligations.
See [source licensing](docs/licensing.md) for the covered files, upstream source, and distribution/source-availability explanation.
These licenses do not grant rights to separately supplied Apple runtime binaries or SDKs.
