# Guest launchctl

`Guest/LaunchctlControl.c` runs stock simulator `/bin/launchctl` inside the existing simulator launchd session. It inherits
the guest bootstrap port, runtime environment, and spawn shim; each command uses the broker's ordinary spawn/wait path.
`Tools/guest-launchctl.py` submits literal arguments over native SSH and retrieves stdout, stderr, and exit status. Commands
do not use a shell. Ordinary requests execute only `/bin/launchctl`; the reserved IPA-install request described below
executes the fixed guest `/usr/local/bin/lsb-app-install` helper.

## Setup

Build with `Tools/build-launchctl-control.sh`. Sign its `tmp/launchctl-control/launchctl-control` output and the matching
runtime's stock `bin/launchctl` with `Tools/sign-sim-executable.py`, using the application's signing identity and identifier.
That signer retains MH_EXECUTE and the simulator platform while clearing CS_EXECSEG_MAIN_BINARY. Place the signed files
at `RuntimeRoot/usr/libexec/launchctl-control` and `RuntimeRoot/bin/launchctl`. The matching iOS 26 launchctl needs only
libSystem, libobjc, and CoreFoundation; the worker needs only libSystem.

Install this plist as `RuntimeRoot/System/Library/LaunchDaemons/com.kdt.LiveSimBooter.launchctl-control.plist`, and add its
label when using an explicit `--launchd-services` selection. The graphical default now includes this worker, installd and
installcoordinationd, filling its 24 selected-service slots. Explicit labels replace the default additional service list,
so retain the other labels needed for the current boot.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
    <key>Label</key><string>com.kdt.LiveSimBooter.launchctl-control</string>
    <key>ProgramArguments</key><array><string>/usr/libexec/launchctl-control</string></array>
    <key>WorkingDirectory</key><string>/private/var/mobile/</string>
    <key>StandardOutPath</key><string>/private/var/mobile/Library/Logs/launchctl-control.log</string>
    <key>StandardErrorPath</key><string>/private/var/mobile/Library/Logs/launchctl-control.log</string>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><false/>
</dict></plist>
```

The worker requires simulator platform 7 and the host's sibling `RuntimeRoot` / `data` directories under the selected
store App Group's `LSB/iOS/23A343` directory. It creates
`data/var/run/launchctl-control`. Directory components are opened without following symlinks, and a held worker
lock prevents two instances from consuming the same queue. Native SSH needs Python 3 and write access to that directory.

## Commands

For command-line tests, start the native app with `--launchd-cli`. The guest starts the Fish terminal and launchctl
worker under launchd. On the full runtime, guest LaunchServices, preferences, notifications and the two container
managers have Mach service entries and start on demand. The profile omits SpringBoard, backboardd and native graphics helpers.

```sh
xcrun devicectl device process launch --device "$LSB_DEVICE" -- "$LSB_BUNDLE_ID" --launchd-cli
```

Supply the actual SSH destination and physical data path; the tool has no device or App Group default. Port 2222
is the default. SSH uses the user's normal host-key configuration; additional SSH options can be passed with `--ssh-option`.

```sh
python3 Tools/guest-launchctl.py --host root@device.local \
  --data-root /private/var/mobile/Containers/Shared/AppGroup/APP-GROUP-ID/LSB/iOS/23A343/data \
  --output-dir tmp/guest-launchctl -- print system
```

Keep those options and replace the arguments after `--` for subsequent commands:

```text
list
print system
kickstart -k user/501/com.apple.lsd
bootstrap user/501 /private/var/mobile/Containers/Shared/AppGroup/APP-GROUP-ID/LSB/iOS/23A343/data/Library/YourJobs/example.plist
bootout user/501/com.example.job
```

Use `print system` for diagnostics and `user/501` for the tested bootstrap/bootout domain. Do not infer the mutation target
from the diagnostic command's `system` spelling. The bootstrap **plist argument is a physical path** because launchctl
opens the file. Inside that plist, working directories and log paths use guest paths such as `/private/var/mobile/Library/...`;
physical App Group paths in those job fields can be translated a second time. The SSH client's `--data-root` is also a
physical path because native SSH accesses the App Group directly. See [guest sshdog](sshdog.md) for a complete example
that preserves a daemon's working directory by bootstrapping its original plist directly.

Submit as the app's filesystem user (normally `mobile`, UID 501). The client creates a mode-0700 `client.lock` directory;
submitting as `root` can leave a root-owned lock that the native app cannot inspect when preserving an interrupted reboot
request. The 2026-10-30 transport test hit this permission error after all guests had exited. Preserve that request's evidence
before repairing its ownership or moving it aside; do not repeat an unknown command merely to clear the lock.

The initial device trial with the temporary worker completed `print system`, `list`, and `bootstrap`/`bootout user/501`
with exit 0. The promoted worker subsequently completed `list`, bootstrap of both the original sshdog and application-launch
plists, and bootout of the application-launch probe with exit 0, without restarting the host or simulator launchd.
Records are under `tmp/app-launch-fix/promoted-results`: `91db0a23abba4d2f9e5e03bd24069f55` (list),
`64cf219bf2504093a219582337e0b883` (sshdog bootstrap), `4375fd94d09f42a5b4dcc2c8949b9a3c` (application-launch bootstrap),
and `d9b31729d60b48249caa3e5553d99099` (probe bootout). Those completed requests have zero spawn/wait errors and no timeout.
This validates job control; it does not imply that every bootstrapped program completed its intended work.

## Reserved IPA installation request

Native **Settings → Applications → Install IPA…** shares this queue. Its sole reserved command is
`--lsb-install-ipa <absolute physical archive path>`, with exactly one path argument. The worker replaces that verb with
`/usr/local/bin/lsb-app-install <path>`; it cannot launch an arbitrary program. Ordinary launchctl arguments retain their
existing behavior. The helper validates the exact host-staged `data/Library/LiveSimBooter/Imports/<32hex>/payload.ipa`
location, invokes guest InstallCoordination, and checks the installed LaunchServices record. See
[guest application installation](app-installation.md) for building, staging, services, result schema and current validation limits.

Install requests have a 180-second worker deadline plus five seconds to observe SIGKILL, rather than launchctl's 60 seconds.
The native importer polls for up to 190 seconds. Killing the helper cannot cancel a daemon operation already in progress:
a timeout preserves `active`, source and queue ownership for inspection, even if the helper was reaped. The worker stops
instead of consuming another request. Native Refresh only reads the saved operation and never retries the installation.
Do not submit the same IPA through a second client to work around an unknown outcome.

The helper's `LSB_INSTALL_RESULT ` JSON line is separate from the worker's spawn/wait receipt. Successful spawn or exit
alone is insufficient: native import requires both a confirmed receipt and the validated result. Completed receipt/log
files remain available after the native importer removes its own staging copy and matching queue lock. The original
selected document is never modified. The first IPA control request succeeded on iPhone 15 with helper exit 0 and verified
installed LaunchServices paths; see [installation evidence](app-installation.md#installed-executables-and-remaining-validation).
The installed app subsequently launched after restarting guest SpringBoard, without a full simulator reboot. Automatic
live icon refresh and correct per-app home selection remain separate issues recorded there.

## Restart the simulator session

With the matching host, LiveProcess and guest shim installed, `launchctl reboot userspace` requests a new simulator session
inside the existing native host. The persistent tabs remain open. The host regenerates the launchd bootstrap configuration,
boot UUID, Metal helpers, IOSurface/framebuffer/HID services and launchd-owned Fish using the original host arguments and environment.
It preserves the data root; this is a restart, not a data reset. A normal launchd exit or crash does not trigger automatic restart.

Native Settings also provides **Restart** and **Power Off**. Restart and the guest userspace reboot both apply any saved
device, haptic and audio choices deferred with Later. Power Off drains the same owned processes but does not enqueue a
replacement session. The host broker remains alive; tapping Start on IOSurface starts a new session without registering a
second broker receiver. Settings remain editable while powered off. Neither action reboots or powers off the physical iPhone.

The 23A343 launchd adapter pins UUID `C3EC5C45-8C85-3D11-B59C-62C4248F5EF9`, instruction contexts and exact callsites. It records
an accepted reboot only at the successful routine-821 reply, then notifies the broker at launchd's final clean `exit(0)`.
The stock simulator launchctl does not recognize the literal `userspace` argument and falls back to its SYSTEM reboot flag;
the adapter accepts that and the USERSPACE form, while excluding halt, recovery and dry-run flags. This never calls a native
phone reboot API. The broker authenticates the current root launchd's full audit token and waits for actual process-death
notifications before constructing new services. Missing exit evidence or an unresolved extension launch stops the restart
and requires reopening the host; it does not create an overlapping session.

After the old generation has exited, the host locks the control queue and archives interrupted `active`, `request` and
`client.lock` entries under `data/var/run/launchctl-control/reboot-interrupted.<UUID>/`. It preserves logs and receipts
and never replays the old command or fabricates success. The client issuing the reboot may therefore lose its result; inspect
the new boot UUID, launchd PID and output independently instead of resubmitting the reboot. Other interrupted-worker cases
still require the inspection described below. Wire protocol version 2 requires rebuilding the host, extension and shim together.

### Device verification (2026-10-27)

Pinned 23A343 arm64 guest on iPhone18,2 / iOS 27.0 build 24A435, retaining the native cache, with no LLDB attachment.
The existing 1,353-image cache stayed at base `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`;
no runtime/cache rebuild was needed. Rebuilt host/extension and wire-v2 shim were signed and installed together. The shim's
previous device copy was retained outside RuntimeRoot; transfers followed the requested no-checksum policy. The deployed shim
UUID is `EE703BAB-57F0-39D1-B644-3539B4B2438C`, host UUID `DB9F467F-F2B8-3D3E-8846-EA47123C6953`, and extension UUID
`830129AF-ED8E-396A-B2E5-F175A42EDDD3`.

A no-argument cold launch displayed Start on IOSurface and had only native host PID **13152**, with no LiveProcess guests.
The explicit `--launchd-services` route started automatically. Native host **13156** then survived two sequential
`launchctl reboot userspace` commands:

| Session | Native launchd PID | Boot UUID |
| --- | --- | --- |
| Initial | 13160 | CAE85E1B-91B6-4690-BF3C-F022F54A2C50 |
| First reboot | 13244 | F5A939A8-9B3A-4D84-8B43-DE31EBA37835 |
| Second reboot | 13341 | A0BA9040-7F2F-4F6B-98BC-C7FEB54E8B21 |

Each root launchd armed the broker and exited with wait status zero. Process inventories after each reboot shared only the
native host PID with the previous inventory. New framebuffer surfaces and Indigo registration succeeded; a screenshot and
the user confirmed black → spinner → SpringBoard. Post-reboot control queries completed with exit zero and showed SpringBoard,
backboardd, Fish and the control worker running. These launchctl listings expose logical guest PIDs; the table above uses native PIDs.
Both issuing control workers were interrupted, and their request/client-lock evidence was archived without replay; a new control
request worked after each reboot. The three handoff banners occurred zero times with default tracing disabled.

The user subsequently reported that app transitions returned to SpringBoard without opening the app. Closing FLEX did not
help, and no matching new crash report established a cause. A controlled repeat kept the same artifacts: cold-start native
host **13450**, verify app launching, then issue two consecutive userspace reboots and verify app launching after each.
The user confirmed all three app-launch checks passed. Native launchd changed **13454 → 13525 → 13674**; the cold boot UUID
was `1F59B69C-DFEB-4BB3-8519-4E097B03A31A` and the final UUID was `1CE62762-5CA4-4FC9-9A33-FC052289CDC5`.
The initial and final process inventories shared only native host **13450**. No debugger was attached. At the user's request,
the app-launch report is considered resolved for now; the earlier failure was not reproduced and no separate cause or fix
was established. Evidence is in `tmp/userspace-reboot/app-launch/` and `app-launch-cold-baseline-console.log`.

Local validation includes the integrated suite, focused reboot authentication/drain/callback tests, control-queue preservation
and lock tests, startup policy, handoff-flag propagation, and iPhoneOS build/signature checks. Evidence and packages are under
ignored `tmp/userspace-reboot/`; the setup does not depend on these experiment files.

## Results and interrupted requests

Each call prints a fresh request ID and saves `<ID>.request.json`, `<ID>.json`, and `<ID>.log` under `--output-dir`. The final
JSON contains spawn/wait errno, raw wait status, decoded exit/signal, and timeout state. The client returns the command's
exit status, 124 for a command timeout, 125 for unknown/transport failures, or 75 when submission is rejected before publication.
Logs remain on the phone; local downloads are capped at 16 MiB and explicitly report truncation.

The client holds a queue ownership record until completion. Complete requests are published atomically; a second client
cannot overwrite a pending request. Arguments are passed byte-for-byte as UTF-8, with 1–63 nonempty arguments and a 16 KiB
request limit. The worker kills ordinary launchctl commands after 60 seconds and allows five more seconds to observe
termination; reserved IPA installation uses 180 seconds plus the same grace period. SSH client polling defaults to 90
seconds and can be changed with `--timeout`; native IPA import uses 190 seconds.

If SSH disconnects or polling expires, the client never submits that command again. Retrieve the same operation using the
same connection/output options plus `--request-id ID`, with no command arguments. An interrupted worker, unresolved wait,
or failed result publication leaves `active` and its evidence intact and stops the worker. Inspect the request, its logs,
the live child, and `client.lock/owner.json` before manually clearing the queue and restarting the control job. Do not delete
these files just to retry an operation whose side effect may already have occurred. Completed logs/status can be removed
after collection.

This controls guest launchd jobs and the fixed IPA helper; it does not provide a general command shell, a guest SSH server,
or new launchd policy.
Restarting the control job through its own queue can interrupt the reply, so use another management path for that job.
