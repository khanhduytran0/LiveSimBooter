# Bash baseline and remaining process support

## Procursus Bash 5.2.21

With the [matched fork trial shim and observer](#running-the-stock-bash-snapshot-trial) installed, type `bash` in the Fish tab.
Fish finds the [Procursus bootstrap's](procursus.md) `usr/bin/bash`. The trial recognizes its arm64 executable by UUID
(`0579980b-285b-3e49-a3b1-ff5ea2099c97`) and enables the fork handler automatically on the simulator PTY.
For a noninteractive launchd job, set `LSB_FORK_SNAPSHOT_PROBE=early` and invoke that executable explicitly.

On iPhone 15 (host 23B50, guest 23A343, cache UUID `9a2fb463-a774-53ed-a661-ff3f3fcac717` at `0x480000000`),
plain `bash` entered an interactive shell. Its parent collected 0 from `df -P .`, 1 from `df` on a missing path, and 37 from
`(exit 37)`. Exiting Bash returned to the original Fish process. Noninteractive runs observed the same statuses for both the
Procursus executable and the pinned Bash 3.2.57 below.

On iPhone 17 (host 24A435, guest 23A343, the same cache at `0x7180000000`), the launchd-owned Fish also started
Procursus Bash without setting a probe variable. Bash returned the same three statuses and exited back to the original Fish.
Both Bash versions returned 0, 1 and 37 in separate noninteractive runs.

A fresh copy from the 1900 archive also passed. Its original SHA-256 was
`1f2c34b9231756f65b118f4b023bf95193e2c336b3290c3b3d0c57937baa495f`. With Lazy sign enabled, registration replaced
only the code signature. All 770,304 bytes preceding `LC_CODE_SIGNATURE`'s payload matched the archive after execution.

An external command usually starts two native LiveProcesses. The first returns from Bash's simulated `fork()` on
the copied stack. When it calls `execve()`, the broker launches the command in a replacement and preserves the
logical PID. An address collision can require another fork observer before the command starts.

Before optimizing native library binding on iPhone 17, five runs of `df -P .` with output redirected in an
already-running shell and no terminal took a median of 232 ms in Bash and 104 ms in Fish. RunningBoard recorded two extension launch requests for each Bash run.
The median interval between requests was 124 ms, followed by 105 ms until Bash collected the status.
Fish uses one `posix_spawn()`. A Bash loop of 1,000 `:` builtins took 1.38 ms.
An earlier interactive `df` trace on the same phone recorded three rejected observers, then one
successful fork and exec. The interval from the first rejection to command exit was 446 ms.

With size optimization limited to the binding code in Debug, the loader calculates export offsets without
constructing temporary buffers and writes the final trie once. With these changes and the early stack check below,
the same five-run test took 171 ms in Bash and 76 ms in Fish. In an independent warm test on the same phone and cache, binding preparation fell from 27.95 to
7.06 ms of thread CPU per launch, and interposition replay from 4.94 to 0.76 ms.

In a ten-command run on the iPhone 17 package fork path, all five rejected observers had a collision in the
parent's 8 MiB main-stack range. The first occupied page was 7.98 MiB below the saved stack pointer in four
cases, and 5.62 MiB below it in the fifth. The receipt records the start of each overlap. The general fork path
preserves the full inheritable mapping so the child can use saved pointers and grow its stack.
For this path, the donor now sends its required stack bounds to each prospective child. Before starting guest dyld,
the native observer can reject a range below its VM floor or overlapping its live main stack. An acknowledged rejection
uses one of the existing four attempts and leaves no guest PID to reap. A candidate that passes still undergoes
the full mapping checks. On iPhone 17, a diagnostic exercised both early rejection reasons. Each returned
`EADDRINUSE`, a subsequent `waitpid(-1, …, WNOHANG)` reported `ECHILD`, and the caller received no `SIGCHLD`.
An APT terminal run then rejected two candidates early and used the third. The shorter guarded stack used by
the pinned Bash continuation is described below.

The 5.2 handler recognizes standalone foreground external commands and a literal `(exit N)` group with no active shell functions.
Pipelines, background jobs, command substitution, redirection and custom traps are outside this profile. The broader Bash 3.2
job-control tests below apply to that executable only.

## Interactive Bash on the fake PTY

Build and install the [matched trial shim and observer](#running-the-stock-bash-snapshot-trial). Start the simulator and run this command in the Fish tab.

```fish
LSB_FORK_SNAPSHOT_PROBE=early "$DYLD_ROOT_PATH/usr/local/bin/bash" --noprofile --norc -i
```

The launchd-owned Fish starts Bash on its existing terminal. Bash enables monitor mode, moves its controlling terminal to a
close-on-exec descriptor (255 in the pinned build), and takes the foreground. Before resuming a copied shell, the observer
recreates that descriptor from an inherited alias of the same fake PTY. The broker keeps the job's logical process group
across the switch to its new executable. Type `exit` to return to the original Fish.

Start a long-running foreground command and press Ctrl-Z (or `^Z` on the second page of terminal shortcuts). Use `jobs -l`
to see the stopped job, `bg %+` to continue it in the background, and `fg %+` to give it the terminal again. Ctrl-C then
interrupts the foreground group. For snapshot and raw wait diagnostics, also set `LSB_FORK_SNAPSHOT_TRACE=1` when starting Bash.

The device test sent the suspend byte through the host input path on iPhone16,2 (host 23B50, guest 23A343).
For logical PID 10951, Bash 10937's three wait statuses were `0x127f` (SIGTSTP), `0x137f` (SIGCONT) and
`0x0002` (SIGINT). Native `ps` showed the stopped process and its heartbeat ceased. The foreground group returned to Bash
on stop, stayed with Bash after `bg`, and switched to the worker after `fg`. Bash reported 130 after Ctrl-C. Exiting Bash
returned to Fish 10890. This used stock Bash 3.2.57 (UUID `95da63f7-defa-3394-a33f-374d41f1190c`) with the native-support
cache at `0x480000000` (UUID `9a2fb463-a774-53ed-a661-ff3f3fcac717`) without a debugger.

A second Bash (11070) ran a two-process foreground pipeline. Both stages (11081 and 11082) stopped, resumed and then
exited on SIGINT. A separate background process group (11076) kept producing output through both terminal signals.
Bash reported 130 for `fg`; the two individual wait records each reported signal 2. Receipts from these runs are in
ignored `tmp/bash-jobcontrol-iphone15/rounds/`.

To try a background read in the interactive shell, run `read -r answer &` and then `jobs -l`. Bash reports
`Stopped (tty input)`. Run `fg %+` and enter a line. The read completes in the child and returns the terminal to Bash.
In the iPhone 15 tests, a foreground worker already blocked in `read` stopped with SIGTTIN (`0x157f`) when
another process called `tcsetpgrp` to give the terminal back to Bash. It received no input or SIGCONT before stopping.
After `fg`, it read all eight expected bytes and exited 42. With `TOSTOP` enabled, background writes including
`fwrite` followed by `fflush` stopped with SIGTTOU before emitting output. After `fg`, the buffered writer flushed its
payload and exited 43. Background writes still work with `TOSTOP` cleared.

Bash also observed a SIGTSTP blocked for three seconds before delivery and a SIGTSTP sent from outside the broker over SSH.
Both `kill -STOP` from Bash and a child's own `raise(SIGSTOP)` produced wait status `0x117f` and could be resumed with `fg`.
A native `kill -STOP` sent over SSH also works on iPhone 15. It woke a Bash waiting for its foreground worker and returned
the prompt without keyboard input. Across four cycles on the same worker, Bash reported `Stopped` and wait status
`0x117f` while the worker was still stopped. After each native `kill -CONT`, Bash received
`0x137f` and reported `Running`. A separate five-second Mach suspension and an LLDB attachment left the job listed as
`Running` and produced no job-control wait status. See [how these cases are distinguished](multiprocess.md#child-status-waiting-and-sigchld).
Device transcripts for these controls are under ignored `tmp/bash-hardstop-iphone15/`.
The final regression repeated Ctrl-Z, `bg`, `fg` and Ctrl-C on a two-process pipeline while an unrelated job kept running.
Bash returned 130. In a separate run without fork tracing, `df` returned 0. The launchd-owned Fish session was restored
with the matched trial shim and observer installed. Device transcripts are under ignored `tmp/bash-ttyio-iphone15/`.
The supported fork sites, heap size and descriptor bounds still apply.
[Terminal implementation](fake-pty.md) · [Process and wait semantics](multiprocess.md)

## Parenthesized subshells and command substitution on iPhone 15

The opt-in trial runs stock Bash’s `(command)` and `$(command)` fork paths. Use the signed trial shim and observer
together with `LSB_FORK_SNAPSHOT_PROBE=early`. Pass the [subshell acceptance script](../Tests/bash-fork-subshells.sh)
as one Bash `-c` argument. It checks the parent’s status, captured stdout, and stderr from a failing `df`.
[Build and staging instructions](#running-the-stock-bash-snapshot-trial).

| Command | On-device result |
| --- | --- |
| `(exit 37)` | Bash 9545 reaped child 9548 with raw status `0x2500`, continued, and printed `37`. |
| `out=$(df -P .)` | Bash 9571 started substitution child 9574. That shell forked and reaped `df` 9575. The outer Bash reaped 9574. Both waits returned 0. |
| Captured contents | `out` contained the `Filesystem` header, an internal newline, and the row for `/dev/disk2s8` mounted on `/private/var/mobile`. There was no trailing newline. |
| Exit and output are independent | A builtin emitted two lines and multiple trailing newlines, then exited 41. The parent retained the internal newline and observed status 41. A failing `df` left stdout empty, wrote its diagnostic to stderr, and returned 1. |

A nested substitution returned exactly `[nested]`. A substitution inside a pipeline delivered `inpipeline` and EOF to
the reader, with stage statuses 0 and 23. A nested group propagated exit 31 through both waits. With `set -e`, a
failing builtin stopped the group before its next command and the parent received 1. A separate group changed a shell
variable and its current directory. The parent retained its original values.

These tests used iPhone16,2 on host 23B50, guest 23A343, Bash UUID `95da63f7-defa-3394-a33f-374d41f1190c` and the
native-support cache at `0x480000000`, UUID `9a2fb463-a774-53ed-a661-ff3f3fcac717`. Guest launchd ran them without LLDB.

* For a parenthesized group, Bash calls `make_child` at +0x15a48 and `execute_in_subshell` in the child. Its `exit`
  builtin normally jumps to a context saved before the fork. The observer installs a child-local `sigsetjmp` target
  in Bash’s `top_level` buffer. For explicit exit and `errexit` it passes Bash’s own last status to the signed `exit_shell`.
* For command substitution, Bash calls `make_child` at +0x2f30c. The trial preserves both ends of that real pipe
  and the writable command string in the copied heap. Bash parses it, connects stdout, reads until EOF in the parent
  and trims trailing newlines. A copied shell that forks again snapshots the pinned Bash image at its inherited address.
  Each child keeps its own broker connection and a guard page below its stack, including when its parent already used a reduced stack.

Grouped bodies support simple commands, nested groups, and bounded lists joined with `;`, `&&` or `||`. The trial
accepts up to 64 syntax nodes in a group and four successive copied interpreters. Active function-return contexts,
process substitution and legacy backticks are outside the tested paths. Device receipts are
under ignored `tmp/bash-subshell-iphone15/`.

## Pipelines and background jobs on iPhone 15

Set `LSB_FORK_SNAPSHOT_PROBE=early` on the pinned, noninteractive Bash launchd job and pass its script as
one `-c` argument. This mode also accepts the foreground fork site. The trial build is required. The regular shim
keeps native fork behavior. [Build and stage the trial](#running-the-stock-bash-snapshot-trial).

Before Bash’s main function runs, the trial binds the executable’s allocation imports to a 4 MiB private arena.
The allocator’s block sizes, allocation flags and next-search offset live in that arena alongside the shell objects.
The arena starts at the Bash image base plus `0x3b800000`. Installation requires the whole range to be vacant.
At fork the broker copies the arena, image and caller stack at their original addresses using private CoW mappings.
The child validates the heap block chain before using it. Its own libc, TLS and broker connection remain active.

For a simple command in a pipe or background job, the saved caller of `make_child` is Bash+0x178d8.
The observer rebinds the copied Bash’s imported functions and data to its local guest runtime and resumes Bash on the
copied stack. Bash closes the unused pipe ends from its descriptor bitmap, calls `do_piping`, expands the command and
runs a builtin or reaches guest `execve`. Once the child reaches its first `getpid`, the parent returns from fork and can start the next
stage while the producer writes. The broker transfers the pipe descriptors, including open descriptors referenced by
the copied close bitmap. The observer restores their original close-on-exec bits before handing control to Bash.

The first iPhone 15 tests used iPhone16,2 host 23B50, guest 23A343, stock arm64 Bash 3.2.57 at `0x400000000`,
and native-support cache `9a2fb463-a774-53ed-a661-ff3f3fcac717` at `0x480000000`. Guest launchd ran the jobs without LLDB.

| Case | Device result |
| --- | --- |
| Background wait | Bash 7197 returned `$!` = 7201. The child waited for the parent to publish both PIDs, verified its identity and exited 37. The parent reaped 7201 with raw status `0x2500`. |
| Background signal | Bash 7290 reaped 7292 with raw signal 15. Its `wait "$!"` reported 143. |
| External pipeline | Bash 7227 piped `df -P .` into a reader. The reader saw 141 bytes and EOF. Bash reported stage statuses 0 and 24. |
| Streaming pipeline | Bash 7258 reaped writer 7260 and reader 7263. The reader checked all 393,289 patterned bytes and EOF. `PIPESTATUS` was 33, 24. |
| Heap isolation and multiple jobs | A builtin assigned a variable during expansion and wrote its value to the pipe. Bash 7563 observed that the variable was still unset in the parent. It then waited for background children in reverse launch order, observing 19 then 37. |
| Three stages | Bash 7653 piped a builtin through two more processes. The final stage read `triple`, and the parent observed all three statuses in order, 0, 17, 23. |

With `set -o pipefail`, `false | true` returned 1 and the parent reaped both builtin stages. In a separate
test the parent set `umask 077`. A piped `umask` printed `0077`, and the reader verified it.

This mode covers noninteractive simple commands, supported groups, and command substitutions on the pinned Bash.
Process substitution and arbitrary file redirections in the early child remain unsupported. The arena has a fixed size and a fork can still fail on a required
address collision. Only descriptor numbers below 128 are accepted for pipe ends and the close bitmap. An explicit
`CHLD` trap remains unsupported because a failed provisional observer can send a child notification.

To reproduce the builtin pipeline and background waits, pass the full contents of
[the pipeline and background acceptance script](../Tests/bash-fork-jobs.sh) to `-c` with `LSB_FORK_SNAPSHOT_PROBE=early`.
On-device logs are kept under ignored `tmp/bash-early-iphone15/`.

## Foreground external commands on iPhone 15

Set `LSB_FORK_SNAPSHOT_PROBE=exec` on a noninteractive guest Bash job to try foreground Mach-O commands.
Use the guest’s existing executable lookup. For example, put the runtime’s physical `usr/local/bin`, `bin` and
`usr/bin` directories on Bash’s `PATH`. Bash passes the resolved name, the actual argument vector and its exported
environment to the replacement. The guest’s normal `execve` checks execution permission and resolves its allowed paths.

The verified Bash is 3.2.57, UUID `95da63f7-defa-3394-a33f-374d41f1190c`, with guest 23A343 on iPhone16,2
host 23B50. The signed main executable started at `0x400000000`, the native-support cache at `0x480000000`,
UUID `9a2fb463-a774-53ed-a661-ff3f3fcac717`. These jobs ran through guest launchd without a debugger.

| Test | Device observation |
| --- | --- |
| Sequential external commands | Bash 5174 reaped `xmllint` with exit 0, Fish with exit 18 and Bash with exit 37. |
| Nested shells | Bash 5265 started Bash 5268, which reaped `df` 5270 with status 0. Bash 5265 then reaped 5268 with raw status `0x2900` (exit 41). |
| Command inputs | An exported value containing spaces, an empty argument, a quoted argument, PATH lookup, cwd-relative `./bash` and an open FD 7 survived fork and exec. |
| Redirections | `1>&2` sent child stdout to the original stderr. `2>&1 1>&2` sent both child streams to the original stdout. The parent kept its own FDs. |
| Errors and signals | An explicit missing path exited 127. A non-executable file exited 126. Bash reaped a SIGTERM-killed child with raw status 15 and reported 143. |
| Signal traps | Bash 5850 kept its own `USR1` handler and reaped its child with raw signal 30. When Bash 5901 ignored `USR1`, its child survived the signal and exited 29. |

In this mode the observer verifies the first `getpid` in the child branch of `make_child`, on the saved fork stack,
before acknowledging the fork. Bash can then apply redirections and build its argument vector while the parent
continues normally. A failure after that acknowledgement belongs to the child and is reported by `waitpid`.
The observer writes diagnostics to a close-on-exec copy of the original stderr. When Bash restores signals before exec,
the observer suppresses a release only for the inherited trap string at Bash’s verified `free` instruction. It leaves
the string in the parent’s allocator. The committed child uses its own signal dispositions in the short interval before
Bash resets its handlers. An active trap, a nonportable original handler, or an explicit `CHLD` trap is outside this path.

The first observer asks for the entire caller stack. If a mapping collides during preflight, the next observer can take
the live stack from the saved SP to the top, plus 256 KiB of downward growth, at the same addresses. The parent inventories
command objects again so separately referenced pages below that slice remain in the offer. The child reserves a no-access
guard immediately below the slice and keeps it until exec or exit. A conflict with that guard or any required page still
rejects the offer. Both `exec` and `early` use this retry for the fork frames they recognize.

In a device run of 17 jobs, Bash reaped 19 children. Sixteen used the full stack. Three recovered after a preflight collision
using a 288 KiB stack slice. All eight independent repetitions of the quoted-argument test passed. The tracked acceptance
script also passed, including nested waits for exits 0 and 37, the redirected diagnostic, and the final wait for exit 1.
With the final build, an explicit `CHLD` trap stopped the trial before it created a provisional child. A separate
non-Bash executable calling `fork()` received the native `EPERM` with the opt-in both set and unset.
Per-job outputs, raw waits and mapping bounds are under ignored `tmp/bash-general-iphone15/`.

In `exec` mode, background commands, pipelines and shell subshells still return `ENOTSUP` at their earlier fork site.
Use `early` mode for simple pipelines, background commands, supported groups and command substitution. File redirections requiring Bash word expansion,
an unresolved bare command name (Bash’s internal command-not-found branch), and text-script interpretation after
`ENOEXEC` also need more inherited shell state. Failed provisional spawns
can still generate guest-visible `SIGCHLD`. [Build and run the guest acceptance test](#running-the-stock-bash-snapshot-trial).

## Bash forks and waits for df on iPhone 15

Stock Bash 3.2.57 (UUID `95da63f7-defa-3394-a33f-374d41f1190c`) ran foreground `df` through its own child branch.
These runs used iPhone16,2 host build 23B50, simulator build 23A343, the signed Bash at `0x400000000` and native-support DSC
`9a2fb463-a774-53ed-a661-ff3f3fcac717` at `0x480000000`. Both guest executables came from the installed runtime and
test overlay. The jobs ran without LLDB.

| Foreground command | Bash PID | Logical child reaped by Bash | Raw wait status | Bash `$?` and launchd exit |
| --- | ---: | ---: | ---: | ---: |
| `df -P .` | 4371 | 4378 | `0x0000` | 0 |
| `df -P /no-lsb-test-fs-20261103` | 4406 | 4414 | `0x0100` | 1 |

Child 4378 resumed at Bash’s `fork` return PC `0x40002570c`. Its first `getpid` used the exact saved SP
`0x16b228bb0`. Bash then called `execve` with return address `0x400019570`, SP `0x16b228ac0`, three arguments,
and 60 environment entries. The replacement `df` printed the row for `/private/var/mobile`. In the error case
`df` printed its own missing-path diagnostic before the parent reported exit 1. One observer collided with the
parent’s stack in that run, so the provider discarded it before the checkpoint and retried with child 4414.
A single Bash process (PID 4509) also ran both commands consecutively. It reaped PID 4512 with raw status 0, then PID 4517
with raw status `0x100`, and continued after each wait. A control using the normal shim returned `EPERM` from native fork. The opt-in procedure is
[below](#running-the-stock-bash-snapshot-trial). The device logs are under ignored `tmp/bash-resume-iphone15/`.

## Public fork and execve on iPhone 17

Stock Bash 3.2.57 can use its `exec` builtin on the 23A343 guest. On iPhone18,2 running 24A435,
`bash -c 'exec "$DYLD_ROOT_PATH/bin/df" -P .'` printed the mounted filesystem and exited 0.
In a second test, Bash replaced itself with another Bash. Both reported logical PID 9775 and guest launchd recorded the
replacement's exit 37. A missing target left the original shell running with `shopt -s execfail`. Direct `execve` uses the
existing broker replacement path for guest Mach-O executables with a nonempty argument vector.

The guest shim captures the public `fork` caller's arm64 registers before entering libc. A provider may prepare a direct
child and open its rendezvous socket at that point. The pinned `__fork` call inside libc's prepare/parent handlers uses
non-cancelling socket I/O on the existing descriptor. The normal shim forwards to Darwin when no provider is installed.
The child in the controlled test resumes a signed, self-contained application frame. General child-side atfork handlers
and inherited pthread, malloc and XPC state still require reconstruction. [Apple's libc fork implementation](https://github.com/apple-oss-distributions/Libc/blob/main/sys/fork.c)

On the native-support guest cache `9a2fb463-a774-53ed-a661-ff3f3fcac717`, the parent received PID 10074 and ran its
prepare and parent atfork handlers once each. The child returned zero at the same caller instruction, changed a stack
local from `0x1357` to `0x369c`, and exited 37. The parent retained `0x1357` and reaped status `0x2500`.

The same caller also completed a fork followed by exec on the normal shim. Parent 10642 received child PID 10647.
The child returned zero on the copied stack, sent its register report, then switched back to its own initialized
bootstrap stack to call guest `execve("/usr/local/bin/bash", …)`. Bash reported `$$ = 10647` over the inherited
socket. The original parent reaped PID 10647 with status `0x2a00` (exit 42).

An ordinary Bash command followed by a parent-side `printf` still failed to fork. The baseline returned `EPERM` and
Bash exited 128. With the trial provider, the broker attempted all mapped segments of the actual Bash executable
(UUID `95da63f7-defa-3394-a33f-374d41f1190c`) and the calling thread's 8 MiB stack. Both runs offered six ranges
totaling 9,469,952 bytes. No range had `VM_INHERIT_NONE`.

| Bash PID | Device result at the real fork call |
| --- | --- |
| 10281 | The child already occupied the address of Bash's text and writable data. The broker returned `EADDRINUSE`. |
| 10385 | The broker installed all six ranges at their original addresses in child 10389. The trial returned `ENOTSUP` because the snapshot omitted heap and runtime state. |

Neither Bash parent reached the command after `df`. The trial child is an ordinary provisional spawn and can produce a
`SIGCHLD` during cleanup. General fork will need to keep this child out of the guest wait table until commit.
These runs used no debugger. Device outputs and job statuses are under ignored `tmp/fork-bash-20261103/`.

### Bash text above the guest cache

On the same iPhone 17 and cache UUID, three fresh parent/child pairs mapped the installed Bash binary’s entire
624 KiB `__TEXT` at `[0x7080000000, 0x708009c000)`. Each parent registered the original file signature, reserved
the vacant range without overwriting existing mappings, then mapped the signed file there. Each child received
the text at the identical address through the fork broker. All six tasks reported current `r-x`, maximum `rwx`,
and called Bash’s `all_digits` at `0x708000f850`. The three parents reaped the expected exit 37. This first experiment
exercised a function with no external references. Probe source and logs are under ignored `tmp/fork-high-text-20261103/`.

### Guest executables below the slid cache

With the native-support profile, the start of the broker's executable window is the common slide for a conventional
`0x100000000` PIE and the `0x180000000` DSC. On iPhone 17 the window started at `0x7000000000`.
Kernel inspection of launchd_sim, SpringBoard and Fish showed their respective signed main images at `0x7100000000`,
the signed DSC at `0x7180000000` (both current `r-x`), and DATA_CONST at `0x71dc544000` backed by the new slide-specific
DATA file. The cache retained UUID `9a2fb463-a774-53ed-a661-ff3f3fcac717`. The host leaves the old sidecar available for rollback.
See [cache selection and DATA preparation](shared-cache.md#automatic-base-selection-and-shared-data-preparation).

Bash 3.2.57 ran with its complete image at `0x7100000000–0x7100108000`. `exec df` printed the filesystem table
and exited 0. An exec from Bash to Bash retained logical PID 11119 and returned 37. In five separate fork trials,
the observer executable started at `0x713c000000`. It verified and released its own tagged reservation for the
parent's executable before the broker tried to install the snapshot. All five parents reached the same fork caller PC,
`0x710002570c`. Four observers accepted the whole image and the 8 MiB caller stack (9,469,952 bytes total).
One offer returned `EADDRINUSE` after the guard was released. Its sampled SP was vacant, but the trial did not
identify which page of the full offer collided. The caller stack still has its native placement. In that snapshot-only trial, successful copies
returned `ENOTSUP` to Bash.
These tests used no debugger. Device receipts are under ignored `tmp/guest-main-high-20261103/`.

On iPhone 15 (23B50), a signed leaf returned 42 at both `0x400000000` and `0x480000000` in a guest of the original
running session. That session's cache was already at `0x300000000`, so a new 8 GiB probe *inside the guest* started
at `0x37e14c000`, immediately past its cache. The test verified fixed executable mappings at the proposed addresses.
It did not boot SpringBoard with the DSC at `0x480000000`. See the ignored `tmp/guest-high-window-iphone15-20261103/` receipt.

### Bash stack collisions and the first child binding

On iPhone 17, separate LiveProcesses can place their native thread stacks at overlapping addresses.
One Bash parent offered `[0x16a8d4000, 0x16b0d4000)`. Its first observer already had a 16 KiB guard at
`[0x16aedc000, 0x16aee0000)`, immediately below its own main stack. The parent's saved SP was above the guard.
Checking only the saved SP misses that collision.

Before libc enters its atfork handlers, the trial observer now checks the entire offer. It holds the intended
caller-stack range with a fixed, non-overwriting reservation until the checkpoint. A collision causes that observer
to exit. The parent reaps its exact logical PID and tries up to four observers total. The broker checks the destination
again while both processes are suspended. This is a test provider. Its disposable children can still generate
guest-visible `SIGCHLD` before a fork has committed.

Seven Bash jobs successfully copied their main image and stack, including six that needed a fresh observer. In every
copy the bound `getpid` slot still held the parent's `LSBGetPID` address and the virtual address in Bash's
`shell_variables` pointer was vacant in the child.

Four follow-up jobs also offered the 16 KiB private page containing the variable context. All four children recovered
the same pointer and matched a word sampled in the parent. For the pinned Bash UUID, the observer checked the original
branch and stub instructions, confirmed the stale target matched the parent's interposer, then wrote the child's resolved
`getpid` address to the copied import at `0x710009c280`. Calling Bash's signed stub on the observer's own stack
returned the expected logical PID in all four runs. These iPhone 17 tests stopped before the child branch of `make_child` resumed on the copied stack.
The later iPhone 15 trial above followed that branch through `unset_bash_input(0)` and `sigprocmask` to `execve`.
Both series used guest 23A343, the same DSC at `0x7180000000`, and no debugger. Receipts are under
`tmp/fork-stack-preflight-20261103/`.

## Earlier Bash baseline

A noninteractive Bash baseline runs in LiveProcess on `iphone-0xf-26-cua-duy.local` (iOS 26.1, build `23B50`).
This establishes shell startup and builtins after native-cache removal. Full `fork`, `exec`, terminal creation, and interactive
job control are not implemented or established by this test.

A later [dyld platform-compatibility test](ios-platform.md) also ran the original iOS 7 platform version of this binary
without `vtool` or rebuilding it. That separately signed copy is `RuntimeRoot/bin/bash-ios`; its original
`LC_VERSION_MIN_IPHONEOS` bytes are unchanged. PID 79579 printed its version and 42, then returned exit 37. The earlier
converted `bin/bash` and its historical results below remain intact.

## Runtime file

The locally supplied source is plain arm64 Bash 3.2.57 from
`/Users/duy/Developer/spironolactone/work/sshtar/bin/bash`, originally tagged for iOS 7. A copy was changed with `vtool` to
`LC_BUILD_VERSION` platform `IOSSIMULATOR`, minimum OS 26.0, SDK 26.0, then signed with the existing runtime procedure:

```sh
mkdir -p tmp/bash-probe
xcrun vtool -set-build-version iossim 26.0 26.0 -replace \
  -output tmp/bash-probe/bash-simulator /Users/duy/Developer/spironolactone/work/sshtar/bin/bash
python3 Tools/sign-sim-executable.py --source tmp/bash-probe/bash-simulator --output tmp/bash-probe/bash-signed \
  --identity D7610EDC80D34A9B1A3D3C8563D66A5CD4716293 --identifier com.kdt.LiveSimBooter
```

The signed output retains `MH_EXECUTE`, has no entitlements, and clears `CS_EXECSEG_MAIN_BINARY`. It was copied to the App
Group's `RuntimeRoot/bin/bash`. Its only linked dependencies, `/usr/lib/libiconv.2.dylib` and `/usr/lib/libSystem.B.dylib`, are
already available in the rebuilt iOS 26 cache at `0x120000000`; no extra loose libraries were copied. The supplied source
binary remains unchanged. Build copies, signatures, and logs remain in ignored `tmp/bash-probe/`.

## Builtin-only device result, 2026-10-25

Guest PID 79320 ran Bash without startup files. It printed `bash=3.2.57(1)-release` and `answer=42`, completed a shell loop,
and deliberately exited 37. The native broker reported raw wait status `0x2500`. No external program, pipeline, command
substitution, or fork/exec path was exercised. This run was launched normally, without LLDB.

Evidence is in `tmp/bash-probe/console-builtins.log` and `tmp/bash-probe/sign-bash.json`. The latter records the output hash,
verified signed pages, cleared executable-segment flag, absent entitlements, and unchanged signing input. The later
`App terminated due to signal 2` line is the stopped native console session; the broker had already recorded Bash's exit 37.

## Terminal diagnostic

A separate simulator probe, PID 79331, inspected existing standard descriptors and attempted terminal opens without changing
terminal settings, session IDs, or process groups:

| Operation | Observed result |
| --- | --- |
| `isatty(0)` | False; stdin was not a TTY. |
| `isatty(1)` / `isatty(2)` | True for inherited console output descriptors. |
| `tcgetattr(1)` / `tcgetattr(2)` | Succeeded. |
| `tcgetpgrp(1)` / `tcgetpgrp(2)` | Failed with `EPERM`. |
| Open `/dev/tty` or `/dev/ptmx` | Failed with `EPERM`. |
| `posix_openpt(O_RDWR \| O_NOCTTY)` | Failed with `EPERM`. |

The diagnostic completed with exit 0. Evidence is in `tmp/bash-probe/console-pty.log`; the source and its behavior notes are
`pty-probe.c` and `PTY-README.txt` in the same directory. Inherited console TTY descriptors do not establish that the guest can
allocate a PTY or obtain a controlling terminal.

A subsequent LLDB probe in native host PID 79330 also found `posix_openpt(O_RDWR | O_NOCTTY) == -1` with `EPERM`.
A direct `/dev/ptmx` open returned -1, but that expression's errno capture was inconclusive and does not establish its error
code. Neither call opened a descriptor; the debugger was detached afterward. Evidence is in
`tmp/bash-probe/host-pty-lldb.txt`. Thus moving this allocation into the current native host did not make the tested PTY path
available.

## Process-model questions

These are constraints for future fork/exec emulation, not a selected implementation plan. Unmodified Fish now has a verified
noninteractive baseline through the existing `posix_spawn` path; whether every required execution path avoids fork remains
unverified. See the separate [Fish results](fish.md).

- A fork-like operation needs a consistent snapshot at identical guest virtual addresses, with signed executable mappings
  recreated and mutable pages copied privately or inherited through actual copy-on-write. Shared writable mappings alone
  would allow parent and child to modify each other's memory and would not provide fork semantics. Native fork also creates
  kernel process/task/thread state, as shown by [XNU's fork implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_fork.c).
- Resuming a cloned guest also requires its register state and stack, pthread/TSD storage, libc fork hooks, pointer-munging
  state, descriptors, and Mach rights to remain coherent. Copying their numeric values is insufficient across native tasks.
  A single-threaded restriction can reduce the initial scope but does not remove these requirements. [libSystem's initialization
  code](https://github.com/apple-oss-distributions/Libsystem/blob/main/init.c) coordinates prepare/parent/child hooks across runtime components.
- Replacing a guest image through a fresh LiveProcess instance needs a stable guest-visible PID and a broker transaction that
  rebinds its parent ledger, suppresses the retiring native process's exit notification, and transfers its existing simulated
  children. Launching another instance alone would expose a new process rather than implement `exec` semantics. Apple's
  [execve manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/execve.2.html)
  specifies PID preservation, descriptor inheritance, and signal-state changes during image replacement.
- Interactive Bash additionally needs usable terminal transport and session/process-group behavior. The terminal diagnostic
  establishes the current restrictions, not an implemented PTY or job-control solution.

The existing brokered `posix_spawn`, child `kill`, `waitpid`, and SIGCHLD paths are described in [multiprocess.md](multiprocess.md).
They are useful primitives for this work, but do not substitute for the missing fork and exec behavior of an unmodified shell.

## Testing a fork with `mach_vm_remap`

On 2026-11-03 a macOS arm64 probe launched a second copy of itself with `posix_spawn`. The existing exception rendezvous
gave the parent an authenticated control right for that child. The parent then allocated a fresh 16 KiB anonymous page
and remapped it into a vacant address at the same VA in the child, without `VM_FLAGS_OVERWRITE`. Ten fresh pairs
completed all 40 remaps and every child exited 0. With `copy=TRUE`,
each process retained its own value after both wrote. This held whichever process wrote first. With `copy=FALSE`, each
process saw the other's writes. Initial address collisions returned `KERN_NO_SPACE`, which makes
selecting a common free address a separate requirement. The probe tests the observed private/shared semantics. It does
not measure physical page allocation. Local evidence is under ignored `tmp/fork-remap-mac-20261103/`.

A second macOS arm64 probe used a controlled assembly frame. The parent wrote a stack local and saved `x19`,
copied that page into the child at the same VA, and waited for the child to park on a separate scratch stack.
It used `thread_set_state` to resume at the matching label in the **child's signed image**, with the copied SP,
saved `x19` and `x0 = 0`. The child reported those values, cleared its stack local and exited 37. The parent retained
the original local and reaped status `0x2500`. ASLR gave the two code images different addresses. This tested a
known continuation with a translated PC; it did not unwind a parent's saved return addresses or run libc's child hook.
All ten repeated process pairs passed. The signed fixture and receipts are under ignored `tmp/fork-remap-continuation/`.

The corresponding guest experiment uses the ordinary spawn and task-port paths. Only the disposable probe carries the
guest's `task_for_pid-allow` metadata. It checks the child's complete audit token before mapping its own page into that
child. A fork implementation can keep both rights in the native broker and authorize only the parent and newly allocated
child for that operation. Ordinary guest forks should never require a task-inspection entitlement.

On iPhone 15 Pro Max (native 23B50, guest 23A343), guest launchd ran that signed probe without a debugger. Both guests
mapped one 16 KiB page at `0x120000000` with RW current and maximum protections. With `copy=TRUE`, the parent wrote
`0x55667788`, the child read the original `0x11223344` and wrote `0x99aabbcc`, and the parent still read `0x55667788`.
With `copy=FALSE`, the child read the parent's write and the parent then read the child's write. The child's real wait
status and the launchd job's exit code were both 0. Their code, stack and pthread addresses differed despite running the
same executable. This device run exercised writable data only. The stack continuation above was tested on macOS.
The job was unloaded and the probe files removed from the phone. Local receipts are under ignored `tmp/fork-remap-20261103/`.

A separate probe on the same iPhone 15 remapped a signed 16 KiB `__TEXT` page containing a position-independent
function that returns `0x53b7`. Each child queried its own VM leaf and compared the function bytes with its separately
loaded image before calling the mapped address. At `0x120000000`, both `copy=TRUE` and `copy=FALSE` succeeded on both
runs. At the parent's original VA, each mode succeeded once and returned `KERN_NO_SPACE` once with fixed mapping and
no overwrite. All six successful mappings had current `r-x` and maximum `rwx`, and all six calls returned `0x53b7`.
These were low-address, file-backed signed pages on the development-configured 23B50 phone. Receipts are under
ignored `tmp/fork-exec-remap-20261103/`.

On iPhone 17 Pro Max (native 24A435, guest 23A343, native cache retained), the same signed probe ran six times without
a debugger. At `0x120000000`, all six copies and all six shared mappings executed. At the parent's original VA, one
copy and one shared mapping executed. The other ten fixed-address requests returned `KERN_NO_SPACE` (six) or
`KERN_INVALID_ADDRESS` (four). Each of the 14 successful mappings had current `r-x` and maximum `rwx` in the child,
and each call returned `0x53b7`. All 24 children exited normally. A fork implementation can reuse this kind of signed
text across tasks, but still needs a child layout that can accept the parent's addresses. Raw results are under
ignored `tmp/fork-exec-remap-iphone17-20261103/`.

### Replacing occupied addresses from a third process

On the same iPhone 17, a third guest verified both workers' task-control ports against their full audit tokens and suspended
the destination. Both workers had occupied the same two addresses. With anonymous destination pages, both remote deallocation
followed by fixed remap and direct `VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE` succeeded. The resumed destination executed the
source's signed function at `0x120000000` and wrote to its private copy of the source's data at `0x120010000`. The function
returned `0x53b7` and the source retained its original data.

When the destination had already executed a different signed function at `0x120000000`, direct overwrite returned
`KERN_PROTECTION_FAILURE`. Remote deallocation returned success, but the VM leaf remained with current and maximum protection
both `---`. A subsequent fixed remap returned `KERN_NO_SPACE` without overwrite and `KERN_PROTECTION_FAILURE` with it.
This was a signed alias outside either cache. Published XNU also marks the arm64 native executable's `__LINKEDIT` permanent.
[XNU native segment mapping](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c#L2362-L2367)

There is a separate address floor. In one pair, the source guest's main header was at `0x103018000` and the destination's
`TASK_VM_INFO.min_address` was already `0x104ad0000`. A candidate fork path would park the fresh extension immediately after
task-port capture in `LiveProcess/main.c`, before it maps guest images. Choose guest addresses above the supported native floors
and reserve them while free. Keep the native bootstrap, pthread/TSD and broker resources valid, and account for the parent’s
private dirty guest-cache pages when restoring guest state. The test replaced two pages and resumed an ordinary guest.
It did not clone the calling thread. Logs are under ignored `tmp/fork-remote-{replace,signed,vacated}-20261103/`.
[XNU PIE address floor](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c#L2222-L2266)

### Restoring the fork caller

The pinned 23A343 libc has an interception point below its atfork handlers. In the signed native-support cache
(`9a2fb463-a774-53ed-a661-ff3f3fcac717`), both `fork` and `vfork` use a stub whose `___fork` pointer is at preferred
VA `0x1e5f0eff8`. libc calls prepare before this stub, and uses the result to choose the parent/error or child hooks.
`vfork` supplies different handler flags. Replacing the public `fork` wrapper would skip that protocol, and replacing the
lower-level call must also reproduce its child-side native PID-cache reset. The prepare handlers can hold malloc, pthread,
dispatch and dyld locks. A request made in that window needs preallocated storage and a pre-established transport that
can run while those locks are held.

The broker would have to classify mappings before cloning them. Private dirty data and stack pages need independent
child writes. Guest `MAP_SHARED` regions must preserve sharing, and `VM_INHERIT_NONE` regions must be omitted or rebuilt
if the guest shim owns them. The `inheritance` argument to `mach_vm_remap` sets the destination entry's future fork
policy. It does not inspect the source and select the appropriate copy behavior. Reserve the original guest VAs while
running on a separate child bootstrap stack and abort on collisions with native mappings. Publish the new guest PID only
after the child is ready, and unwind its partial maps and job on failure.

Returning on the cloned stack also requires child-owned pthread state. In both native
23B50 and 24A435, pthread's prepare hook saves the calling pthread pointer in a **native** alloc-once entry. The child
hook reads that entry before reinitializing pthread. The fresh extension has its own native table, and the bridge gives
guest callers a separate one. Copying guest heap and stack pages therefore misses state required by the very first
child hooks. Even a guest whose application has one thread has a shim-owned Mach worker. Fork needs a consistent snapshot
while those workers are quiescent and fresh child-owned broker and ledger state before resuming the application.
The [iPhone 15 allocator and libc checkpoint](procursus.md#experimental-allocator-and-libc-checkpoint) ran both an
independent fork/exec fixture and stock APT using a single-threaded observer with its own native pthread.
Preserve FD numbers and shared open-file descriptions, including `FD_CLOEXEC` descriptors until exec. Guest launchd's
bootstrap/registered ports need explicit handling as well. An ordinary Darwin fork already gives its child a new Mach
namespace with selected ports inherited. Replaying an entire parent's Mach name table is neither the kernel contract nor
a substitute for reconnecting the shim. [XNU IPC task initialization](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/kern/ipc_tt.c#L173-L293)

Fresh native tasks can have different PAC state, which matters for saved native arm64e return addresses even when
the guest executable is plain arm64. XNU's fork path also handles VM inheritance, pmap and code-signing state.
[XNU VM fork](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_map.c#L13815-L13951)
and [arm64 PAC initialization](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/arm64/machine_routines.c#L2811-L2866)
describe these kernel-owned steps.

### Cooperative fork on iPhone 17

Transport v10 lets a parent offer an explicit list of ranges to one direct child. The child accepts once. The native
broker authenticates both guest connections and full audit tokens, suspends both tasks while taking the snapshot,
and reads each range's inheritance and protections from the kernel. The current broker walks each source VM leaf
within a range separately. An offer supports up to 64 ranges and 128 MiB total. Neither guest needs a task port for the other.

Private pages are copied on write, shared pages stay shared, and a source range with `VM_INHERIT_NONE` leaves the
destination alone. The broker rejects required collisions before starting any remaps. A child must exit if installation
fails partway through, because a newly installed signed mapping can become permanent. Build the native app, extension,
and guest shim together when changing the transport version.

On iPhone 17 Pro Max (24A435, guest 23A343, cache `9a2fb463-a774-53ed-a661-ff3f3fcac717`), a signed guest
without entitlements requested a snapshot onto a child that already had a signed page at `0x120020000`. With the source
page marked required, the broker returned `EADDRINUSE`. The child still executed its own page and verified that both
earlier destination slots were vacant. In a second child, the source page had `VM_INHERIT_NONE`. The marker survived
and the child resumed the parent's plain-arm64 continuation with a zero return at PC `0x120000034`, SP `0x120013fa0`.
`x19`, `d8`, FPCR and FPSR matched the saved values. The child changed its stack local from `0x1357` to `0x369c`.
The parent retained `0x1357`, received the logical child PID, and reaped status `0x2500` (exit 37).

An ordinary dynamically linked guest `fork()` on the same installed build returned `-1` with `EPERM`. Its registered
prepare and parent atfork callbacks each ran once. The cooperative operation uses a separate controller stack and a
newly spawned guest's bootstrap. Wiring libc's syscall boundary requires a rendezvous safe under the atfork locks,
restoration of child pthread/TSD and shim resources, and fork-specific inheritance of descriptors and signal state.

Build the optional device fixture against the matching guest shim produced by `Tools/build-spawn-shim.sh`.

```sh
xcrun --sdk iphonesimulator clang -target arm64-apple-ios26.0-simulator -Wall -Wextra -Werror \
    Tests/Guest/fork-continuation/ForkContinuation.c Tests/Guest/fork-continuation/ForkContinuation_arm64.S Guest/Shared/Fork/ForkContext_arm64.S \
    "$LSB_WORK/libLiveSimSpawn.dylib" -o "$LSB_WORK/fork-continuation"
```

Sign it using the ordinary simulator executable procedure and launch it under guest launchd with no arguments.
It reports its saved registers, VM collision result and real wait status. To exercise the public libc entry with the
same isolated frame, launch the fixture with the single argument `public` and leave `LSB_FORK_SNAPSHOT_PROBE` unset.
The fixture installs its own provider, calls `fork()` and checks the parent’s atfork counters, the child’s return value,
and the real wait status. Its child continuation uses raw write and exit. Device receipts for the original cooperative
run are under ignored `tmp/fork-continuation-iphone17/v10/{final,libc}/`.
Use `public-exec` with a signed Bash at `RuntimeRoot/usr/local/bin/bash` to test exec and wait across the same fork.

### Running the stock Bash snapshot trial

Use the pinned 23A343 guest and the matching host, extension and shim (transport v12, child-status v8, fake PTY v2).
Set `LSB_WORK` to an ignored staging directory.
Build the trial shim and its guest observer together.

```sh
LSB_FORK_SNAPSHOT_TRIAL=1 sh Tools/build-spawn-shim.sh "$LSB_WORK/trial/libLiveSimSpawn.dylib" "$LSB_WORK/trial/spawn-probe"
```

Sign the shim as a guest dylib and the observer as a guest executable. Stop the simulator, retain its normal shim,
and stage the trial shim at `RuntimeRoot/usr/lib/libLiveSimSpawn.dylib` and the observer at
`RuntimeRoot/usr/local/bin/lsb-fork-snapshot-child`. Follow the [guest signing and staging procedure](agent-setup.md).
Launch the pinned Bash from a guest launchd job. Set `LSB_FORK_SNAPSHOT_PROBE=exec` in that job’s environment,
and use these five `ProgramArguments` entries.

1. `/usr/local/bin/bash`
2. `--noprofile`
3. `--norc`
4. `-c`
5. The full text of [the guest acceptance script](../Tests/bash-fork-exec.sh).

Capture stdout and stderr, then inspect the guest job’s actual exit status. The inner shell should run `df` and exit
37. The outer shell should observe 37, run a failing `df` with stderr redirected to stdout, observe 1, and exit 0.
Separate stdout/stderr paths also let you verify that the failing command and the continuation diagnostics took
different routes.

For the original `df`-only test use `LSB_FORK_SNAPSHOT_PROBE=df` with
`"$DYLD_ROOT_PATH/bin/df" -P .; rc=$?; printf 'AFTER_DF pid=%d status=%d\n' "$$" "$rc"; exit "$rc"`.
This older mode delays the acknowledgement until after Bash reaches `execve`. Set the option to `1` for the snapshot-only
diagnostic, which returns `ENOTSUP` after a successful copy, or leave it unset for native fork.
Boot out the test jobs and stop the simulator before restoring the normal shim and removing the observer.
