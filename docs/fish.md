# Fish as an interim shell

Unmodified Fish 4.9.3 source, built for iOS Simulator, runs noninteractive external commands through the brokered
`posix_spawn` and `waitpid` paths on `iphone-0xf-26-cua-duy.local` (iOS 26.1, build `23B50`). The runtime supplies the required
access-check and wait-option compatibility. Two df children and the shell exited 0 in the completed device test.
This baseline does not establish that all Fish execution paths avoid fork, or provide full job control or a kernel PTY.

The native host now has **IOSurface**, **Fish**, and **Settings** tabs. The shell is configured as a simulator-launchd job through
`/usr/libexec/terminal-fish`; the host UI does not spawn Fish directly. The helper receives its terminal through the broker,
then replaces itself with pristine Fish using `POSIX_SPAWN_SETEXEC`, retaining the launchd job's logical identity. It disables
job control and uses the existing [fake terminal](fake-pty.md) and native keyboard. Switching tabs keeps the runtime and
terminal session alive. The 2026-10-26 device trial reached a Fish prompt through xpcproxy → terminal-fish → Fish, with
logical job PID 10516 retained across native PIDs 10516, 10517 and 10518. Fish spawned `df` (native PID 10554), printed
`DF_STATUS=0`, and the broker recorded wait status `0x0`. Evidence:
[`console-path.log`](../tmp/terminal-launchd-tests/console-path.log). The later cache-base trial is recorded below.

## Optional Procursus commands

Fish searches the installed `data/var/jb/{usr/bin,bin,usr/sbin,sbin}` directories before the runtime's standard command
directories. Use `command -s ls` to see which file Fish selected, or `command printf` to select an external command over a builtin.
See [Procursus setup and APT verification](procursus.md) for installation and the supported package operations.

Every simulator guest redirects `/var/jb` and `/private/var/jb` for `stat`, `access`, and `faccessat`. The native host and native
helpers keep their own filesystem view. Each guest caches the physical bootstrap prefix at startup and resolves the target on
each matching call, or its parent for `AT_SYMLINK_NOFOLLOW`, so subsequent package and symlink changes are visible.

Settings contains one **Silent background audio** switch, off by default, with no separate status row. An explicit opt-in
is saved and starts native silent playback that mixes with other audio; turning it off stops playback and deactivates the
audio session. Interruptions or errors appear in the footer. This is a native host option, not simulator audio support or
a guarantee that every guest remains runnable in the background.

Fish uses the native keyboard independently of the IOSurface tab's [guest hardware-keyboard route](backboardd.md#physical-keyboard-forwarding).
A connected hardware keyboard or iPhone Mirroring can suppress the native software keyboard; the user confirmed that
connection during the missing-keyboard report. The terminal's actual text scroll view (`_display`) now sets
`keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive` and `alwaysBounceVertical = YES`, allowing a downward
drag to dismiss the software keyboard even when the terminal has little scrollable content. The build passed and was
installed at 20:08 on 2026-10-26; the physical interactive-dismiss gesture has not been verified. Evidence:
[build](../tmp/cpu-investigation/interactive-host-build.log) and [installation](../tmp/cpu-investigation/interactive-install.log).

The earlier integrated terminal reached a prompt on iPhone, accepted editing keys, spawned `df` with exit 0 and
original-platform Bash with exit 37, interrupted a looping child, and exited 0 on Ctrl-D. A builtin command substitution
also produced 42. Those results used the earlier terminal/cache configuration; they do not verify the new tab path.
The [terminal results and limits](fake-pty.md#device-checks-and-diagnostics) contain its LLDB evidence and input/signal
ordering limitation. The completed noninteractive results below also remain historical baselines.

The earlier explicit command-mode launch used the broker's initial RuntimeRoot working directory:

```sh
xcrun devicectl device process launch --device 00008130-00021D122680011C --terminate-existing --console \
  -- com.kdt.LiveSimBooter /bin/fish -N -P -c 'status job-control none; set -g fish_use_posix_spawn 1; ./bin/df -h'
```

Keep the native host in the foreground. `./bin/df` is relative to RuntimeRoot; the runtime does not provide a general `chroot`.

## Full-cache allocation boundary

After the 1,353-image SpringBoard cache was introduced at `0x108000000`, Fish could fail before executing any guest code:
`Simulator cache overlaps a resident executable image`. The loader had reserved image memory anywhere while retaining a
second whole-file parse mapping. The [root-aware allocator](shared-cache.md#low-image-allocation-and-the-0x110000000-rebuild)
now releases that temporary view, reserves a free low gap without overwrite, and reports the exact exhausted window.

The next trial found no contiguous `0xa8c000` bytes between native image `0x106c18000` and cache `0x108000000`. The apparent
space below the native image is protected PAGEZERO extended by ASLR; ordinary VM allocation cannot reclaim it. This is an
address-space preparation failure, not a Fish assertion or a fork/PTY failure.

The same cache closure has therefore been rebuilt and signed at `0x110000000`, leaving twice the low-address room.
Offline validation confirms that all code and writable mappings remain below `0x180000000`; only read-only metadata and
LINKEDIT extend above it. The existing dyld_sim is unchanged because it reads the cache base from the header.
Build, rebasing and signing checks passed, as documented in the [cache results](shared-cache.md#offline-cache-and-signing-validation).
After activation on the iPhone, host PID 10621 booted simulator launchd 10625, SpringBoard 10645 and backboardd 10646.
Launchd-owned Fish reached its prompt promptly as native PID 10656, without the previous image preparation failure.
The loader reported reservation `0x110000000–0x18e194000`; the bounded amfid/TXM capture contained no identified rejection.
Evidence: [`console-110.log`](../tmp/terminal-launchd-tests/console-110.log) and
[amfid/TXM capture](../tmp/terminal-launchd-tests/cache-110-amfid-txm.log). This verifies startup with the new cache;
the `df` exit-status test above belongs to the preceding trial, and live mapping extents/protections were not remeasured.

## Converted Homebrew baseline

The first experiment used the locally installed arm64 Homebrew Fish 4.9.3 and its PCRE2-8 and PCRE2-32 dependencies. Copies
were marked for iOS Simulator 26.0 with `vtool -set-build-version iossim 26.0 26.0`; the PCRE2 load paths and install IDs were
changed to `/usr/lib/libpcre2-8.0.dylib` and `/usr/lib/libpcre2-32.0.dylib` before signing. The copies were signed with identity
`D7610EDC80D34A9B1A3D3C8563D66A5CD4716293` and identifier `com.kdt.LiveSimBooter`, then deployed as:

- `RuntimeRoot/bin/fish`
- `RuntimeRoot/usr/lib/libpcre2-8.0.dylib`
- `RuntimeRoot/usr/lib/libpcre2-32.0.dylib`

The executable retains `MH_EXECUTE`, has no entitlements, and has `CS_EXECSEG_MAIN_BINARY` cleared by the existing executable
signer. libiconv and libSystem come from the rebuilt iOS 26 cache at `0x120000000`. Local source binaries were preserved;
converted copies and logs are in ignored `tmp/fish-probe/`.

Guest PID 79345 printed `fish=4.9.3` and `answer=42`, then deliberately exited 37. The broker reported raw status `0x2500`.
Evidence is `tmp/fish-probe/console-builtins.log` and `sign-fish.json`. This normal launch did not exercise an external command.

Dyld diagnostic variables are now off by default; the loader no longer injects the `DYLD_PRINT_*` set on every launch.
After the updated native app was built and installed, guest PID 79363 printed `fish=4.9.3 diagnostics-quiet` and exited 0
without the earlier dyld output flood. Evidence is `tmp/fish-probe/console-quiet.log` and `build-quiet.log`.
See [dyld diagnostics](debugging.md#simulator-dyld-diagnostics) for the current opt-in procedure.

## Executable preflight failure

The next run, PID 79347, disabled job control, enabled `fish_use_posix_spawn`, and attempted `./bin/df -h` from RuntimeRoot.
Fish rejected the command before any child request reached the broker: it reported that the file exists but is not executable,
then returned status 126 (`0x7e00` in the broker). The output is in `tmp/fish-probe/console-df.log`.

A separate guest probe, PID 79353, checked the actual file and access calls. Its current directory was RuntimeRoot, and its
UID, effective UID, and GID were 501. `stat` returned mode `100755` and owner/group 501 for df:

| Requested access | `access` | `faccessat(..., AT_EACCESS)` |
| --- | --- | --- |
| `F_OK` | Succeeded | Succeeded |
| `R_OK` | Succeeded | Succeeded |
| `X_OK` | Failed with `EPERM` | Failed with `EPERM` |
| `R_OK \| X_OK` | Failed with `EPERM` | Failed with `EPERM` |

Thus the normal execute-access query rejects this file despite its executable mode bits. The custom loader's successful
mapping and execution of df does not imply that the kernel will approve an ordinary executable-access check. This probe
exited 0; evidence is `tmp/fish-probe/console-access.log` and `access-probe.c`.

## Source prototype result

The initial source prototype used the checksum-verified official Fish 4.9.3 archive, a local Rust simulator standard library,
and bundled static PCRE2 with JIT disabled. Its two source changes narrowed non-job-control waits to `WNOHANG` and provided
an `EPERM`-only executable preflight fallback for canonical, readable arm64 executables inside RuntimeRoot. Two isolated
helper tests passed. Those changes were experimental; the final direction keeps upstream Fish source unchanged.

Prototype guest PID 79377 launched two df children, PIDs 79378 and 79379. Both exited 0, the shell received SIGCHLD, and the
shell itself exited 0. The recorded output is `tmp/fish-probe/console-port-df.log`. This proves the limited prototype flow,
not the unmodified build or all Fish process behavior. The prototype source and patch remain in ignored `tmp/fish-build/`;
its signed executable is preserved separately as `tmp/fish-probe/fish-port-signed`. Obsolete compilation output was removed.

## Unmodified Fish and runtime compatibility

A separate simulator build now comes from a fresh extraction of official Fish 4.9.3: all 2,183 distinct regular release files
and symlinks match the archive. It retains upstream `access(X_OK)` checks and `waitpid` calls with
`WNOHANG | WUNTRACED | WCONTINUED`. The build targets arm64 iOS Simulator, minimum 26.0 and SDK 27.0, with static PCRE2;
its only dylib dependencies are libiconv and libSystem. The recipe and source comparison are recorded in
`tmp/fish-build/build-pristine-fish.sh` and `PRISTINE.txt`; the executable is under `target-pristine/`.

Guest PID 79403 used this pristine build to spawn df PIDs 79404 and 79405. Both returned status 0, SIGCHLD was delivered,
and Fish exited 0. This was a normal run without LLDB; its evidence is `tmp/fish-probe/console-pristine-df.log`.
`sign-pristine.json` records signing with the same runtime identity/identifier, no entitlements, and
`CS_EXECSEG_MAIN_BINARY` cleared. The full runtime test suite passed in `tmp/fish-probe/tests-runtime.log`.

A second pristine run, Fish PID 79407, spawned Bash PID 79408 with `--noprofile --norc -c 'exit 37'`. The broker recorded
child status `0x2500` and sent SIGCHLD; Fish printed `bash-child-status=37`, then exited 0 after checking that result.
Evidence is `tmp/fish-probe/console-pristine-bash-status.log`. This verifies nonzero child status as well as df's successful exits.

The deployed Fish and shim SHA-256 values match their signed local files, recorded in `tmp/fish-probe/deployed-phone.sha256`
and `deployed-local.sha256`. The two loose PCRE2 dylibs and the temporary PTY/access probes were removed from the phone;
the current Fish uses static PCRE2. Local patched-build output and pristine compilation intermediates were also removed.
The pristine release executable, source trees, patches, recipes, dependency/toolchain caches, signed copies, and logs remain.

The compatibility lives in the runtime, independently of Fish source:

- The guest wait shim accepts `WUNTRACED` and `WCONTINUED` instead of rejecting a wait that also requests terminal status.
  These sequential Fish tests exercised normal and signal exits. A running child with no pending event returns 0 under
  `WNOHANG`. A terminal status is reaped once, followed by `ECHILD`. The fake terminal now also reports observed stops
  and continuations for broker-delivered signals. [Interactive Bash tests](bash.md#interactive-bash-on-the-fake-pty)
- The executable-access interposer acts only after native `EPERM` on a valid mode containing `X_OK`. It requires a canonical,
  readable regular file strictly inside canonical `DYLD_ROOT_PATH`, plain arm64 `MH_EXECUTE`/`MH_PIE`, valid header bounds,
  and execute permission for the relevant real/effective UID and groups. Native read/write checks still apply. Unsupported
  directory-relative `faccessat` requests and flags keep the original result. The [full scope](multiprocess.md#executable-access-checks)
  separates this preflight from the loader's actual image/code-signature validation.

These tests ran with job control disabled. Their original Darwin 25 creation-mask fallback required a single-threaded guest
at every spawn. The new [creation-mask tracker](creation-mask.md) initializes before guest workers start and interposes
`umask`, so later reads no longer temporarily change the process mask or reject worker threads. Native preparation and cache
detachment still require a single thread. Calls that bypass `umask` interposition do not update the tracked value.

The original Fish tests did not exercise stopped or continued jobs. Full fork and exec emulation remain separate work, described in
the [Bash notes](bash.md#process-model-questions). The guest and native host PTY probes there both rejected new PTY allocation;
the new terminal supplies socket descriptors and guest controls instead. Shell pipelines, background jobs, substitutions,
and other paths that may fork or start threads need separate validation; the completed device results establish sequential external commands.
