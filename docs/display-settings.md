# Native Settings and guest display controls

The native **Settings** tab provides **Performance HUD** and **Visualize touches** controls for the running simulator.
Opening the tab or pressing Refresh reads the current guest values. Controls show confirmed readback, remain disabled
while a request is pending, and report failures in the section footer. There is no separate Status row.

**Silent background audio** is a separate native switch, off by default. It keeps the host's audio session active by
playing silence and can increase battery use. It does not depend on the simulator display helper.

Device, Haptic passthrough and Audio output choices are editable before the first Start and after Power Off. These controls
show saved choices separately from the running session. Changing one during a session offers **Restart** or **Later**;
Later saves it for the next start/restart, and multiple deferred changes apply together. Changing a choice back to the
current session value clears its restart requirement. A host launch-environment override still locks its corresponding switch.
The live HUD and touch-marker controls require a running simulator; an unavailable display helper does not block saving
device, haptic or audio choices.

The **Simulator session** section contains **Power Off** and **Restart**. Power Off waits for the owned guest and helper processes
to exit, clears framebuffer/terminal connections, and restores Start on IOSurface. The native host and saved data remain.
Restart uses the same confirmed process-death drain, then boots with the saved choices. Controls are unavailable during a
session transition. Ordinary child launches do not toggle their availability. A requested Power Off or Restart pauses new
launch dispatch and waits for any already-submitted child to establish its audited identity and exit monitor before draining.
If process retirement fails or times out, the host reports the failure and does not overlap old and new guests. Unchanged
status notifications do not redraw Settings; actual updates replace only the affected sections without animation.

## Session-settings verification, 2026-10-29

The cold-start UI test used iPhone16,2 on native iOS 26.1 / 23B50 with simulator iOS 26.0 / 23A343 and the existing cache at
`0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`. No debugger was attached. Before Start, process inspection found
only the native host. The user confirmed that Haptic passthrough and Audio output were editable while off and that Start
booted SpringBoard. The initial implementation toggled Settings availability during individual daemon launches; this was
separate from a session restart and caused visible flashing. After the availability and redraw fixes, the user confirmed stable
controls during boot, Later preserving the running session, and Restart returning to SpringBoard. The native host stayed at
PID 8533 (historical test identity); the rebuilt host UUID was `9AE0BA4F-3F9E-3F25-8BF7-BACE0EA82BB2`.

Power Off restored the Start button. Independent process inspection then found only that same host, with no guest or native
Metal helper remaining. Starting again from IOSurface returned to SpringBoard and reconnected Fish, confirmed by the user.
Native thermal pressure was 0 and no debugger was attached. Runtime binaries and the cache were
unchanged; this test deployed the rebuilt native app and retained the working native Metal backend.

Focused local tests cover saved-versus-active configuration, combining deferred changes, malformed profile preflight,
confirmed-death shutdown, receiver reuse on Start, stale callbacks, failed drains and unchanged-model notification suppression.
They also cover Restart/Power Off before and after child CHECKIN, including exec replacements and failure paths that must
keep the next session from starting. The integrated `sh Tests/run.sh` suite and signed native build passed. These tests do
not substitute for device confirmation of each UI operation.

## Native UI localization

[Host/Localizable.xcstrings](../Host/Localizable.xcstrings) contains the native UI's English source text and translator comments.
Add translations there with Xcode; labels, accessibility text, status messages and errors use `NSLocalizedString`.
Keep formatting arguments intact, and keep protocol identifiers, preference keys, accessibility identifiers and terminal escape
sequences untranslated. UI behavior must use state rather than comparing translated text. The catalog belongs only to the
native host target, not LiveProcess or the supplied simulator runtime.

## Controls and lifetime

| Mode | Performance HUD |
| ---: | --- |
| 0 | Off |
| 1 | Basic |
| 2 | Backdrop |
| 3 | Particles |
| 4 | Full |
| 5 | Frequencies |
| 6 | Power |
| 7 | FPS only |
| 8 | Display |
| 9 | Glitches |

The HUD is transient render-server state and resets when backboardd restarts. Off clears only the HUD flag; modes 1–9
set the corresponding zero-based render-server value and enable that flag. Other render-server diagnostic flags are preserved.

Visualize touches writes the Boolean `BKDigitizerVisualizeTouches` in the guest `com.apple.backboardd` preferences suite.
Stock backboardd observes changes through its defaults/KVO path, so markers update without restarting it. This setting is
persistent guest preference state. Bootstrap preserves an existing Boolean, including false, and seeds true only when the
key is absent. It rejects an existing value of the wrong type.

## Launchd-owned helper and request protocol

Graphical bootstrap creates the fixed job `com.kdt.LiveSimBooter.display-settings`, running
`/usr/libexec/display-settings --serve` with `RunAtLoad=true`, `KeepAlive=false`, and `EnableTransactions=false`.
Its output goes to guest `/private/var/mobile/Library/Logs/display-settings.log`, backed by
`data/Library/Logs/display-settings.log` under the selected App Group's `LSB/iOS/23A343` directory.

The native host never calls guest CoreAnimation entry points. [GuestDisplaySettings.m](../Host/GuestDisplaySettings.m)
exchanges fixed-size records with the guest worker through the owned, private
`data/var/run/display-settings` directory. [DisplaySettings.h](../ProcessSupport/DisplaySettings.h) defines
version 1: a 56-byte request, 64-byte response, and 40-byte ready record. The only operations are read, HUD mode 0–9,
and touch Boolean 0–1. Requests contain no executable path, arbitrary command, preference domain, or preference key.

Each request carries a random ID, worker generation, and deadline. Files must be owned, single-link regular files of the
exact expected size; directory traversal uses validated canonical paths and no-follow opens. Atomic exclusive publication
prevents one request or result replacing another. The worker sleeps on directory vnode events when idle.

The host waits eight seconds. A timeout means the outcome is unknown, since an in-flight operation may still finish.
Refresh checks the same request ID; it does not repeat the setter. New setters remain disabled until that uncertainty is
resolved. A restarted worker rejects abandoned or old-generation requests without applying them again. A ten-second guest
watchdog bounds a stalled operation; an already published valid result is reused without another apply.

## Guest operations and errors

[DisplaySettingsRenderServer.c](../Guest/DisplaySettingsRenderServer.c) uses the guest bootstrap's fixed
`com.apple.CARenderServer` endpoint. Its four typed MIG operations match iOS 26.0 build 23A343 QuartzCore: IDs 40259–40262,
covering flags and the HUD float value. Each RPC has a one-second send/receive timeout, an owned reply port, and strict
reply ID, size, NDR, port, error, and value validation. This avoids the stock convenience getters' ambiguous zero return
on transport failure. Recheck this private ABI before changing the runtime.

Touch updates use guest `NSUserDefaults`, synchronize, and validate Boolean readback. A setter succeeds only when its
requested value is observed. `EIO` can mean preference synchronization failed or readback disagreed; it is not treated as
successful persistence. `ENOTCONN` means the render endpoint is unavailable, `ESTALE` means the helper generation changed,
and `ETIMEDOUT` requires checking the unresolved request rather than repeating it. Invalid or unavailable fields display
Unavailable instead of a guessed value. The CLI JSON separates the operation error from HUD and touch read errors.

The preference path needs both the pinned CoreFoundation native-PID preflight compatibility and the exact shared-counter
object adapter. The stock user object is `apple.sim.cfprefs.user.daemonv`: 23A343 uses `snprintf(size=31)`, truncating its
version digit. [NotificationSharedMemory.c](../Guest/NotificationSharedMemory.c) maps only that exact name to the owned
`data/tmp/cfpreferences-user-v1.shm`. The real daemon creates a 16 KiB writable page; clients open it read-only.
No additional sandbox exception is granted. XML preference files are valid: the earlier nil reads occurred before plist
parsing because the shared counter page was missing. After replacing the shim, restart cfprefsd and its clients to discard
the daemon's cached fallback page.

## Build and stage

With the variables from the [setup guide](agent-setup.md), build the unsigned simulator helper:

```sh
sh Tools/build-display-settings.sh "$LSB_WORK/guest"
```

Include this override in the setup guide's complete `setup-ios26-runtime.py --stage` command:

```sh
--add-binary "/usr/libexec/display-settings=$LSB_WORK/guest/display-settings"
```

The staging tool signs the helper as `MH_EXECUTE` using the app identifier, without application entitlements, and removes
`CS_EXECSEG_MAIN_BINARY` through the tracked executable signer. Rebuild and sign the guest shim as part of the same setup.
Stop affected guests before replacing either mapped binary; preserve the working runtime until the new one is tested.
There is no in-app installer. Rebuild the native app to include its Settings UI and generated launchd job.

For diagnostics, run the following **inside the simulator guest**, for example from its Fish terminal:

```sh
/usr/libexec/display-settings read
/usr/libexec/display-settings hud 7
/usr/libexec/display-settings touches 0
```

The CLI prints JSON with requested and observed values and exits nonzero on operation failure. It has a ten-second watchdog
and refuses to apply settings outside the simulator platform. The native UI uses the fixed worker protocol, not shell commands.

## Verification, 2026-10-27

The device run used an iPhone18,2 host on iOS 27 build 24A435 and guest iOS 26.0 build 23A343. The 1,353-image simulator cache
was based at `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`. The final session had native host 12095 and cfprefsd
12128; these PIDs are historical, not setup inputs.

The user confirmed the initial FPS-only HUD; a later screenshot established the Basic HUD. After the preference compatibility
fix, the native Visualize touches control stopped reporting an error, and the user explicitly confirmed both live toggle directions.
This verifies those UI operations; it does not claim that every HUD mode was visually checked or that preferences survive
an interrupted disk flush. Local evidence is in [the Settings console](../tmp/lsd-metadata-default/results/settings-preferences-fixed-console.log),
[the preference probe](../tmp/lsd-metadata-default/results/preferences-shm-fixed-probe.log), and
[the FPS screenshot](../tmp/lsd-metadata-default/results/hud-fps.png). These ignored artifacts may be absent from a fresh checkout.

Focused tests cover protocol validation and file safety, exclusive writers, worker restart/no-replay behavior, malformed and
late responses, native UI model state, bounded render RPCs, actual Boolean plist serialization, and preservation of a false
touch preference. Shared-memory tests include a separate producer/client process, read-only access, counter visibility,
daemon reopen, unlink lifetime, namespace isolation, and native forwarding for unsupported names. Native UI syntax and the
arm64 simulator helper/shim builds passed. These tests are included in [Tests/run.sh](../Tests/run.sh); device behavior remains
a separate check.
