# Socket-backed terminal

A normal app launch now starts the graphical simulator launchd session and presents persistent IOSurface and Fish tabs.
Launchd starts the fixed terminal helper, which replaces itself with `/bin/fish -i -N -P` using the existing SETEXEC transport.
An initialization command disables job control, enables `fish_use_posix_spawn`, and selects a simple `fish> ` prompt. The Fish
executable remains unchanged. The earlier direct-terminal mode reached an interactive prompt on the iPhone, accepted editing
keys, ran `df` and Bash, interrupted a running child, and exited cleanly on Ctrl-D. The launchd-owned Fish tab has now also
reached its prompt and run `df` with exit status 0 on the iPhone. The newer lifecycle-label and toolbar layout changes still
need device verification; the two launch paths' results are recorded separately below.

The terminal runs in the native UIKit host. LiveProcess remains C, releases native XPC state before its single-thread check,
and detaches the host cache as before. The guest additions are C interposers compiled into `libLiveSimSpawn.dylib` and use
the Simulator's libc after handoff. The terminal does not create a native kernel PTY.

## Launch and files

The runtime needs the existing cache, dyld_sim, updated spawn shim, the [pristine Fish build](fish.md) at `bin/fish`, and the
signed helper at `usr/libexec/terminal-fish`. `sh Tools/build-terminal-fish.sh` builds the arm64 simulator helper into
`tmp/terminal-fish/`; sign and stage it using the runtime's existing procedure. Keep the app foregrounded. A normal app launch
opens the IOSurface tab; select Fish for the terminal. Explicit commands keep their console behavior unless prefixed with
the legacy diagnostic option `--terminal`:

```sh
# Default graphical launchd session, with IOSurface and launchd-owned Fish tabs.
xcrun devicectl device process launch --device 00008130-00021D122680011C --terminate-existing --console \
  -- com.kdt.LiveSimBooter

# Legacy direct-shell diagnostic: this bypasses simulator launchd ownership.
xcrun devicectl device process launch --device 00008130-00021D122680011C --terminate-existing --console \
  -- com.kdt.LiveSimBooter --terminal /bin/fish -i -N -P -C 'status job-control none; set -g fish_use_posix_spawn 1'

# Run the existing process test without the fake terminal.
xcrun devicectl device process launch --device 00008130-00021D122680011C --terminate-existing --console \
  -- com.kdt.LiveSimBooter /bin/spawn-probe --repeat 2 --verify-statuses
```

The launchd helper sets `TERM=xterm-256color`, `HOME` and `CFFIXED_USER_HOME` to the owned `data` directory, changes
into that directory, and uses the physical RuntimeRoot `bin`, `usr/bin`, `usr/sbin` and `sbin` directories in `PATH`. It checks
the simulator platform, attached terminal descriptors and canonical sibling `RuntimeRoot`/`data` paths under `LSB/iOS/23A343` before SETEXEC.
The explicit `--terminal` diagnostic retains its earlier `RuntimeRoot/home/mobile` home and `bin:usr/bin` path setup.
Explicit shell arguments choose their own job-control policy; the provided Fish arguments disable it. This is not a general
`chroot` implementation. The loader leaves `DYLD_PRINT_*` diagnostics off unless supplied explicitly.

## Launchd-owned Fish and tabs

`Host/LaunchdConfiguration.m` generates `com.kdt.LiveSimBooter.terminal-fish` with program `/usr/libexec/terminal-fish`,
`RunAtLoad=true`, `KeepAlive=false`, and working directory `/private/var/mobile/`. Both graphical launchd modes include it.
The normal icon launch and bare `--launchd-services` select eleven stock services, including SpringBoard and IconServices;
backboardd, cfprefsd and this terminal helper are the three fixed jobs. Explicit service labels still replace only the
stock selection. There is no additional host-to-guest terminal RPC and no host-direct Fish fallback for this path.

The broker accepts the exact helper path only from the authenticated running simulator launchd, or through SETEXEC by its
direct child whose captured executable identity is exactly `/usr/libexec/xpcproxy_sim`. It preserves the genuine sender audit
token checks and rejects a second active terminal lineage. After the original ordered spawn actions, attachment adds a
staging socket FD and final `dup2` operations for 0/1/2 followed by closing the staging FD. The operation is transactional;
failure preserves the request and closes newly created resources. Existing terminal-state environment entries are replaced
with the new session's state path.

Simulator launchd constructs the physical proxy path with a doubled separator at `RuntimeRoot//usr/libexec/xpcproxy_sim`.
The broker originally rejected it before taking the immutable executable-identity snapshot, leaving `peerIdentityError=EINVAL`
and rejecting the terminal helper's SETEXEC with `EACCES`. It now accepts that one physical-root boundary spelling, including
the strict `/var` alias of `/private/var`, while retaining traversal rejection and opened-descriptor containment checks.
The stored identity is still the canonical logical `/usr/libexec/xpcproxy_sim`. The helper recognizer accepts the same
specific boundary spelling; other duplicate components, doubled separators under unrelated alias roots and escaping symlinks remain rejected.

`Guest/TerminalFish.c` then requests only `POSIX_SPAWN_SETEXEC` for Fish. The existing replacement protocol preserves the
logical job PID, parent ledger and wait ownership across xpcproxy → helper → Fish. Terminal ownership follows that lineage
and its descendants. Simulator launchd, Metal helpers and unrelated launchd jobs do not become terminal jobs merely because
a global terminal session exists. A failed replacement exits the helper with an error; it does not spawn an ordinary child
as a fallback. When the final terminal process exits, the host reader drains to EOF and the tab reports that it is closed.

`Host/ViewController.m` keeps both tab controllers and their runtime connections alive. It loads the hidden terminal consumer
early so Fish's cursor/status queries can receive replies before that tab is selected; only the visible tab acquires keyboard
focus. The broker publishes the terminal-change notification before starting its reader, and the session retains up to
256 KiB of output until a consumer binds, including output drained just before EOF. Switching away cancels active simulator
contacts and pauses drawable presentation. Dirty updates continue into the display's owned baseline, so returning to IOSurface
can present the current image without restarting backboardd or losing pending frame acknowledgments.

The terminal parses output and answers device/cursor queries immediately. It coalesces screen painting on the main queue
and skips painting while the Fish tab is hidden. Reopening the tab displays the latest screen, including output received
before its first appearance or just before EOF. Painting updates the existing text storage in one editing transaction,
without animation, and scrolls only when the cursor leaves the viewport. Identical attributed text skips the storage edit,
but still checks layout and cursor visibility after resizing. Replacing the text view's entire `attributedText` and always
calling `scrollRangeToVisible:` caused visible jumps during Fish autosuggestions in the initial coalesced-rendering trial.
`Tests/TerminalScreenTests.m` checks burst coalescing, hidden output and
resize, final output without another callback, and reentrant replies/presentation changes; these are local behavioral tests,
not a measured device performance result.

The focused broker/configuration suites cover authenticated attachment, xpcproxy/helper/Fish replacement, subtree isolation,
concurrent-session rejection, default icon-launch job selection and preserved explicit diagnostic modes. TerminalSession tests
cover transactional final actions, FD/environment failure rollback, late output binding, EOF and closure notification.
TerminalFish tests check platform/TTY/data-root guards, environment setup, SETEXEC-only replacement and failure reporting.
The full `sh Tests/run.sh` suite passed in [the integrated log](../tmp/terminal-launchd-tests/full-suite.log), and the native
app build succeeded. [The icon-default broker run](../tmp/terminal-launchd-tests/broker-icon-default.log) also passed.
The subsequent broker path-boundary regression passed, and the device run described below verifies the new launch path.

## Transport and guest controls

`Host/TerminalSession.m` creates an `AF_UNIX/SOCK_STREAM` socketpair. The host keeps the nonblocking master endpoint and
transfers duplicates of the other endpoint as guest descriptors 0, 1, and 2 through the existing spawn plan. Real sockets
provide stream reads/writes, readiness, backpressure, EOF, and descriptor inheritance. stdout/stderr share the same endpoint.
Partial host writes queue up to 1 MiB and resume through a dispatch write source. Cancellation handlers retire the dispatch
registrations before their descriptors close. Read/write callbacks process bounded amounts before yielding to the main queue,
so a continuously writing guest cannot monopolize keyboard and signal handling.

A 16 KiB file beside RuntimeRoot holds the shared termios, window size, foreground identifier, and slave socket identity.
Its path is passed in `LSB_FAKE_PTY_STATE`. The guest constructor maps it through the ordinary App Group filesystem; spawned
descendants inherit the environment entry and socket open description. Matching the socket's device/inode identifies aliases
after `dup`, fileport transfer, and descriptor renumbering without globally treating stdin or every socket as a terminal.
Unrelated descriptors retain native behavior. The host removes the state file when the session closes; already mapped state
remains available until the corresponding guest exits.

`ProcessSupport/FakePTY.c` implements the shared records and host line discipline. `Guest/FakePTY.c` interposes `isatty`,
termios access, foreground-group access, window ioctls, and tty naming for matching descriptors. `TCSANOW` updates supported
attributes; canonical input implements line editing, echo, CR conversion, and selected signal characters. Noncanonical input
supports `VMIN=1, VTIME=0`. Output applies `OPOST/ONLCR` when selected. `ttyname` reports `/dev/lsbtty` as a label; that path
is not an openable device node.

The host forwards VINTR/VQUIT and resize notifications through the broker's existing audited signal path. With job control
disabled, it targets all live jobs belonging to that terminal, not a kernel foreground process group. The stored foreground
identifier satisfies the shell's terminal queries without creating a native foreground group or supplying stopped/continued
job control. Initial foreground selection calls `LSBGuestProcessGroup()` directly, keeping it consistent with the existing
logical session model across SETEXEC. Calling `getpgrp()` from inside the interposing shim bypassed its own interposition
and seeded the native group instead; Fish then saw a different group from its own logical `getpgrp()` result.
The [creation-mask tracker](creation-mask.md) lets the existing spawn path capture a known mask after Fish starts workers.

Interactive Fish also starts pthread workers. XNU retains the native pthread startup PC after cache detachment, and rejects
the simulator's repeat registration. The shim's [thread-start bridge](runtime-handoff.md#new-guest-pthreads) creates each
supported new pthread suspended through the real syscall, redirects its initial PC to simulator libpthread, and resumes it.
This preserves kernel thread/TSD initialization while avoiding entry into the unmapped native cache. It supports the pinned
custom-stack pthread ABI; the separate [launchd workqueue bridge](launchd.md) handles the supported workqueue startup path.

## Native display and input

`Host/TerminalView.m` provides software-keyboard input, hardware arrow/control keys, and a shortcut row with Escape, Tab,
Ctrl-C, Ctrl-D, arrows, and Return. The row is inside the terminal view, whose bottom stays above both the safe-area bottom
and keyboard top; it no longer uses an `inputAccessoryView` that can overlap the floating tab bar. A separate lifecycle
label changes from waiting to attached or closed without polluting the guest's output. The controller rechecks its binding
when the tab appears, retains failure output after EOF, and disables keyboard focus without a live input handler. These
status/layout changes pass local iPhoneOS syntax checks and await device verification. The display parses UTF-8, cursor
movement, erasure, basic insert/delete operations, and
device/status queries, with bounded scrollback. Size changes update the shared window and ask the broker to send SIGWINCH.
Raw output is also mirrored to a connected `devicectl --console` stream. A usable pipe/TTY on the host app's stdin can feed
the same session, but `devicectl` did not forward stdin on the tested iPhone; keyboard input and the opt-in test FIFO work independently.
Console mirroring currently writes synchronously to stdout. A stalled attached console can therefore stall the host's main
queue even though socket I/O is bounded; ordinary on-screen use without an attached console is unaffected.

The renderer is a basic ANSI screen. It is not a complete VT/xterm implementation: colors and unsupported modes have no
visual effect, Unicode cell widths are incomplete, and full-screen terminal applications need separate work.

## Limits and validation

- `openpty`, `posix_openpt`, `/dev/ptmx`, and `/dev/tty` are not virtualized. Only the inherited socket endpoint is a fake terminal.
- Guest flush/drain controls, `TCSADRAIN`, `TCSAFLUSH`, flow control, breaks, and unsupported terminal ioctls return `ENOTSUP`.
- Canonical EOF on an empty line, software flow control, suspend characters, timed reads, and nondefault minimum-length reads
  are unsupported. In Fish's noncanonical mode Ctrl-D is an ordinary input byte that Fish can interpret itself.
- Full job control, stopped/continued wait events, generic `fork`, and generic `execve` remain unsupported; the broker supports
  the separate-process `POSIX_SPAWN_SETEXEC` replacement used by the terminal helper.
  Pipelines, external command substitutions, background jobs, and arbitrary shell applications need separate execution tests.
- Input and signal delivery are separate operations. Text followed immediately by Ctrl-C in the same input burst can leave
  text already queued in the slave socket when SIGINT reaches Fish; Fish may display that text afterward. A separate Ctrl-C
  after the line appears clears it. Host queue flushing cannot remove bytes already queued in the socket; an acknowledged
  flush/control protocol or guest read interposition is not implemented.

`Tests/FakePTYTests.c` covers shared state, alias matching, native-descriptor forwarding, termios/window controls, line editing,
echo, output translation, signal characters, and explicit unsupported operations. Native session tests exercise inherited
endpoints, dispatch I/O, raw input, resize/signals, and EOF cleanup. Screen tests exercise split UTF-8/escape sequences,
cursor movement, editing, bounded state, and parser callbacks that reenter input handling. These are host tests and do not
replace the separate device checks below.

## Device checks and diagnostics

On 2026-10-28, the refactor ran without LLDB on iPhone16,2 / iOS 26.1 (`23B50`) with the unchanged iOS Simulator 26.0
(`23A343`) runtime. Its cache remained at `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`; only the native app,
extension and separately built guest shim were replaced with development-signed artifacts. A 200-line Fish builtin burst
received while the tab was hidden appeared when the tab opened, and the user confirmed responsive typing/deletion.
Fish's `df` child exited zero. The first UI trial exposed flashing and a vertical jump when an autosuggestion disappeared;
after the in-place text-storage correction, repeated typing/deleting over a full `launchctl list` page stayed positioned
without flashing, confirmed by the user. The final build removed temporary render tracing; the user also confirmed that
swipe-dismissal and reopening of the keyboard preserved a visible, stable prompt while typing/deleting. The screen tests
do not exercise UIKit's layout behavior; this is separate device
evidence, not a CPU or frame-time benchmark. Logs and before/after screenshots are under ignored `tmp/refactor-20261028/`.

On 2026-10-26, the graphical host **10396** started the launchd-owned terminal job with logical PID **10516**. Its xpcproxy
instance **10516** replaced itself with helper **10517**, then Fish **10518**, preserving the logical job identity. Fish
reached `fish> ` and accepted interactive input. The command `df -h >/dev/null; echo DF_STATUS=$status` launched **10554**;
the broker recorded wait status `0x0`, Fish printed **`DF_STATUS=0`**, and its prompt returned. Evidence is
[`console-path.log`](../tmp/terminal-launchd-tests/console-path.log) and the older-layout
[`fish-tab-live.png`](../tmp/terminal-launchd-tests/fish-tab-live.png). This verifies Fish and its child wait path, not
SpringBoard helper restart or Spotlight recovery. The newer terminal status/toolbar changes have not been installed in
that captured run.

On 2026-10-25, the earlier direct-terminal mode and worker shim ran on `iphone-0xf-26-cua-duy.local` (iOS 26.1, build `23B50`):

- Fish PID `79627` reached the prompt and accepted typed commands. `ls` was absent from the test runtime; typing `df` spawned
  PID `79629`, printed its output, and produced exit status 0.
- A fresh Fish launch, PID `79649`, accepted raw test input through the opt-in FIFO and printed `interactive-ok`.
- `df -h` spawned PID `79695`, printed its filesystem table, and returned `df-status=0` through Fish's wait path.
- Original-platform `bash-ios` spawned PID `79699`, exited 37, and returned `bash-status=37`. Its iOS platform command
  remained unchanged; see the [dyld compatibility results](ios-platform.md).
- Backspace and arrow editing produced `edit-ok`. A separate Ctrl-C cleared a rendered unfinished line. The same-burst
  text/Ctrl-C ordering limitation above was also reproduced.
- A Bash builtin loop, PID `79711`, terminated on Ctrl-C with raw wait status `0x2`; Fish reported `interrupt-status=130`
  and returned to its prompt. `echo substitution=(math 20 + 22)` then printed `substitution=42` using Fish builtins.
- Resizing the native view updated the terminal and submitted SIGWINCH. Ctrl-D at the final prompt exited Fish `79649`
  with raw wait status `0x0`. SSH inspection then confirmed removal of both the terminal state file and its input FIFO.

Hardware-only LLDB breakpoints caught `LSBThreadCreate` and then a new worker at simulator libpthread's `_pthread_start`,
address `0x12b26e618`. The host region `0x180000000–0x300000000` remained inaccessible (`---`); the simulator cache range
starting at `0x120000000` was `r-x` with zero dirty pages reported. LLDB named Fish, dyld_sim, the shim, and simulator pthread.
The simultaneous amfid/TXM capture, saved as `tmp/fake-pty/security-interactive.log`, completed with no matching events.

Evidence is in `tmp/fake-pty/console-key-tests.log` and `tmp/fake-pty/interactive-lldb.txt`. The final `Tests/run.sh` suite
and signed iPhone Xcode build passed. After the I/O fairness and parser reentrancy fixes, the final installed build also
passed a fresh launch: host PID `79724`, Fish PID `79725`, a new prompt, and `final-terminal-ok`. Original-platform Bash
PID `79728` exited with raw status `0x2500`, and Fish printed `final-status=37`. This final run is recorded in
`tmp/fake-pty/console-final.log`.

The first attempts exposed two startup failures before a prompt: one libpthread zero-token trap, and a later worker starting
at the old host pthread PC. The short and large startup-vector tests did not reproduce token corruption. Redirecting a
suspended worker under LLDB isolated the stale entry point; the integrated thread bridge then enabled the successful runs above.
The isolated zero-token report remains a separate unresolved observation.

These diagnostic environment switches are available and normally off:

- `LSB_DEBUG_STOP_BEFORE_HANDOFF=1` makes LiveProcess send itself `SIGSTOP` after reporting READY and building its launch
  arguments, before entering the simulator loader. This permits attaching LLDB while native code is still available. The
  setting is removed from the guest environment, so guest-spawned children do not inherit the stop request.
- `LSB_TERMINAL_INPUT_FIFO=1` makes the native host create a mode-`0600` FIFO at `<terminal-state-path>.input` and print its
  exact path as `[terminal] input FIFO=...`. Writing raw bytes to it feeds the same line-discipline/input path as the keyboard.
  It avoids relying on console stdin forwarding during device tests. The host removes the FIFO when the terminal closes.
- `LSB_TERMINAL_CONSOLE_OUTPUT=1` in the native host environment also mirrors terminal output to its stdout, for example a
  connected devicectl console. Ordinary UI sessions leave this off so console backpressure cannot stall UIKit. Diagnostic
  mirroring remains synchronous and can block if that console stops consuming output; the Fish tab receives output either way.

The FIFO and stop switch are test controls; neither is required for ordinary on-screen use. Runtime `DYLD_PRINT_*`
diagnostics remain off unless explicitly supplied.
