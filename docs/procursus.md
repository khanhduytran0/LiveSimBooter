# Procursus in the guest

Install the Procursus 1900 `iphoneos-arm64` bootstrap in the iOS 26.0 (23A343) guest. Fish then searches its command directories,
and APT can fetch the signed repository index and install packages under the guest data directory.

## Install or patch

Use the current LiveSimBooter host, LiveProcess extension, guest spawn shim, matching fork observer, terminal helper and
`/usr/libexec/app-registration` from the [setup guide](agent-setup.md).
Power off the simulator in LiveSimBooter Settings. On the Mac, install CMake, Ninja and `dpkg-deb` alongside Xcode and Python 3.8 or later.
The phone needs native SSH, Python 3, `ps` and `tar`. Use the same signing identity and bundle identifier as the native host.

```sh
python3 Tools/install-procursus.py --host mobile@device.local --port 2222 \
    --identity "$LSB_SIGNING_IDENTITY" --identifier "$LSB_BUNDLE_ID" --work "$LSB_WORK/procursus"
```

The installer downloads the [pinned bootstrap](https://apt.procurs.us/bootstraps/1900/bootstrap-iphoneos-arm64.tar.zst) and the
APT 2.9.4 build inputs. To reuse an original local archive, add `--bootstrap /path/to/bootstrap-iphoneos-arm64.tar.zst`.
To reuse an approved signing helper, add `--cms-helper /path/to/sign-code-directory`. A root SSH login also works. The receiver
drops to mobile before writing. If several store App Groups contain the runtime, pass `--runtime-root` with the full device path
to the selected group's `LSB/iOS/23A343/RuntimeRoot`.

- **Fresh installation.** The Mac rebuilds APT's acquisition library and verification method and signs all bootstrap Mach-O files.
  The phone publishes the prepared tree at `data/var/jb`. Internal absolute package symlinks become relative, and
  `RuntimeRoot/var/jb -> ../../data/var/jb` lets guest dyld resolve the packages' embedded library paths.
- **Existing installation.** The installer checks the installed APT and libapt-pkg versions. It patches the APT pair, re-signs
  binaries that still match the original archive bytes, and repairs the shipped internal symlinks. Local package data, settings,
  additional packages, and other changed or previously signed binaries stay in place. The installer prints a backup directory
  containing each replaced entry at its path relative to `data/var/jb`. Keep it until the guest tests pass. Updating or reinstalling
  the two APT packages through dpkg can overwrite the patch.

To deploy the same build/signing identity to another phone, pass a signed output directory from `Tools/prepare-procursus.py`
using `--prepared` in place of the signing options. Use `Tools/build-procursus-apt.py --help` and
`Tools/prepare-procursus.py --help` for the separate local build and signing steps.

## Verify in Fish

Start the simulator and run these commands from Fish.

```fish
command -s apt
/var/jb/bin/ls -d .
apt update
apt-cache policy cmake
apt list --installed
```

The path from `command -s` should be inside this simulator's `data/var/jb`. The host sets an `APT_CONFIG` default for guest jobs
at startup. APT's `Dir` points to simulator data, its dpkg chroot directory is `/`, and it passes `--root=<data>` to dpkg.
The `DPKG_ROOT` child default also directs standalone `dpkg-query` to the guest database. The spawn shim resolves dpkg's
early configuration reads under `/var/jb/etc/dpkg/` into the guest bootstrap. For a maintainer script loading or unloading
a single LaunchDaemon plist, it routes the package's `launchctl` to the simulator's `/bin/launchctl` and resolves the plist
inside that transaction's `DPKG_ROOT`. When the bootstrap's CA bundle is present,
the host gives the HTTPS method its physical path for TLS verification. APT's patched worker uses `posix_spawn` for its
two protocol pipes. The verification method launches the guest's stock `/var/jb/usr/bin/gpgv` with the selected keyrings.

The `uikittools.postinst` trigger calls `uicache -a` for `/var/jb/Applications`. In a chrootless package transaction the
shim sends that invocation to the guest registration helper. The helper scans the selected `DPKG_ROOT/var/jb/Applications`
and checks the guest LaunchServices records before registering a bundle or removing a stale entry. A bundle ID already
registered at another location is a conflict. You can run the same scoped reconciliation directly in Fish:

```fish
"$DYLD_ROOT_PATH/usr/libexec/app-registration" --dpkg-trigger
```

For automated verification, use `apt -o APT::Update::Error-Mode=any update`. Otherwise a failed fetch can print a warning
and still exit zero. Check that `apt-cache policy cmake` shows a candidate from `https://apt.procurs.us`.

On iPhone 15 and iPhone 17, an update from empty lists downloaded and authenticated the Procursus 1900 release and the
`iphoneos-arm64` index. On iPhone 17, a cold boot also passed plain `apt update`. `apt-cache policy cmake` reported 3.30.2,
and `apt list --installed` matched all 67 entries in the guest's dpkg database. The verifier rejected an altered release
with `BADSIG` and a different keyring with `NO_PUBKEY`.

On iPhone 17, `apt install cmake` exited 100 during dependency resolution. Neither `firmware (>= 14.0)` nor its alternative
`libexpat1` was available. APT also reported unsatisfied dependencies for the bootstrap's `essential`, `libkrw0`, `shshd` and
`snaputil` packages. It never ran dpkg. Fish then returned 127 for `cmake --version`.
The installer skips the archive's `prep_bootstrap.sh`. An earlier test, before guest `vfork` support, stopped at its first external command.

### Run a package repair in Fish

By default dpkg tries to chroot before running a maintainer script. iOS rejects that operation. Inspect APT's plan first.
For a transaction whose scripts can run with `DPKG_ROOT`, pass `--force-script-chrootless` to dpkg for that invocation only.
Scripts still interpret absolute paths themselves, and guest launchd shares the phone's network ports.

```fish
apt -s --fix-broken install
apt -o DPkg::Options::=--force-script-chrootless --fix-broken install
dpkg --audit
apt-get check
```

For the 1900 bootstrap, the proposed repair removes `shshd` and `snaputil` and installs `openssh-client`, `openssh-sftp-server`,
`openssh-server` and `libkrw0-tfp0`. OpenSSH's stock job requests ports 22 and 2222. To reproduce the device test, use a copy of the
bootstrap under guest data. Set `DPKG_ROOT` to the copy and `APT_CONFIG` to a config whose package database, archives and dpkg root
all point into the copy. Stage its `com.openssh.sshd.plist` before confirming the repair. Set the job's program to an inert guest
executable and both sockets to `127.0.0.1` on distinct unused high ports. Add
`-o DPkg::Options::=--path-exclude=/var/jb/Library/LaunchDaemons/com.openssh.sshd.plist` to the repair command to preserve that plist.

On iPhone 15 (host 23B50, guest 23A343) the CLI-only session ran the original `apt`, `dpkg`, `dpkg-deb` and Dash with development
signatures. The APT acquisition library and its `gpgv` method retained the installer patches. All four package archives and the
`shshd.prerm`, OpenSSH `extrainst_` and `uikittools.postinst` scripts were unmodified. The first repair stopped at line 7 of
`shshd.prerm` with `Cannot fork` and APT exited 100. Retrying the same transaction once completed both removals, all four installs
and the `uikittools` trigger. APT exited 0, `dpkg --audit` printed nothing and `apt-get check` exited 0.

Guest launchd unloaded the staged `us.diatr.shshd` job and registered `com.openssh.sshd` from the test root. The two test listeners
bound only to loopback and were released when the test job was unloaded. The native service registrations and both the native and
live guest dpkg status files were unchanged.

The repair's `uicache -a` trigger found no package apps in its test root. A separate disposable root contained one new `.app`. An independent query
verified its exact URL and installed state in guest LaunchServices, and found the same single record after a second run.
Removing the `.app` and rerunning the helper removed that record. All 168 unrelated records had the same fingerprint throughout.

### Guest vfork

The native-support profile stages `vfork()` before entering libc, alongside the existing snapshot `fork()`. On 23A343
both public functions invoke the same private stub for Darwin syscall 2 (`fork`). The shim checks the caller and operation
at that stub, and the observer resumes the matching zero-return branch. The child gets its own address space and the parent
can return once the snapshot is acknowledged, before the child calls exec. LibSystem receives flag 1 for `vfork` and skips user `pthread_atfork`
callbacks. The parent rechecks registrations after parking peer threads. A request that needs mutable shared-cache
data from an additional callback owner returns `ENOTSUP`, because its prepare callback has not protected that data.

On iPhone 15, both `vfork` children (one calling `_exit`, the other execing a new image) returned status 37 with no user
callbacks. Ordinary `fork` dispatched its registered handlers and returned the same child status. Unmodified Dash ran a
non-final external command, read its status and continued. The generic fork fixture also accepts `--vfork` to exercise
an exec from a loose dylib with a shared file offset and a close-on-exec descriptor.

## Stock APT and fork

On iPhone 15 (host 23B50, guest 23A343), signature-only copies of the 1900 archive's `apt`, `libapt-private` and
`libapt-pkg` loaded together. With the guest package database and the launchd-provided `APT_CONFIG`,
`apt -s --fix-broken install` finished dependency resolution, printed `Failed to fork`, and exited 100.
The package database remained byte-identical. The previously patched acquisition library reaches the install summary
before hitting a fork in the `ischroot` check. The two builds can fail at different points.

The opt-in fork snapshot follows a return PC in a loose dylib to its Mach-O image and includes the mapped segments
of libraries loaded from guest packages and installed app bundles. On iPhone 15 the broker copied the executable,
ten loose libraries (including the calling `libapt-pkg`) and the full 8 MiB caller stack at the same virtual addresses
in a fresh observer. It accepted 43 ranges totaling 13,205,504 bytes. An observer can have an occupied address or a
higher native VM address floor. One APT job exhausted all four observer attempts. A fresh APT job copied all ranges
on its first attempt.

### Experimental allocator and libc checkpoint

The standard shim build includes a matched fork observer. On the native-support cache, executables in the
guest's `data/var/jb` use this path automatically. The existing interactive Bash fork mode keeps its own selection.
For an explicit test from other guest executable locations, set `LSB_FORK_SNAPSHOT_PROBE=resume`. This path uses the pinned scalable
malloc zone in 23A343. It preserves the caller's heap, active stack, loose image mappings, startup vectors and
callee-saved registers. The snapshot broker preserves the protections and inheritance of each VM leaf. A one-shot
Unix socket authenticated with both native audit tokens allows the observer to accept while it still has one thread.

The observer validates and adopts the parent's copied zone as its default allocator. It translates the copied region
cookies to the child process's entropy and keeps its own original zone registered for its existing allocations.
The parent's prepared thread transfers supported open file descriptions via `SCM_RIGHTS`. The observer restores
the descriptor numbers and close-on-exec flags. It prepares its own libSystem hooks and takes the zero-return
branch of the guest's actual libc `fork`. This dispatches the child handlers before the application resumes.
The observer retains its own native pthread and authenticated broker connection. The two registrations present at
startup match the parent's native and guest CoreFoundation registrations. User lookup adds a third registration
from guest `libsystem_info`. Its DNS prepare handler locks a mutex and its child handler clears inherited resolver
state and unlocks it. For a newly registered guest-cache owner, the trial validates its Mach-O UUID and section
bounds on both sides. It transfers supported mutable C data sections after the parent's prepare handlers run.
It preserves bindings and neighboring images on the same page and rejects conflicting initialized observer state.
A separate libSystem-only executable called `getpwnam("mobile")` before and after fork. The child lookup returned
UID 501 within its six-second timeout and the parent reaped exit 37.

With this trial, the signature-only `apt -s --fix-broken install` completed twice on iPhone 15. Guest launchd
and Fish reported exit 0. In the first run each fork transferred 49 ranges (70,221,824 bytes) and staged all three
atfork registrations. APT reported four installs and two removals. In the second run the first child executed
the Procursus HTTP method and was reaped with `SIGINT`. The second child exited 127 without a replacement image,
consistent with the optional `ischroot` lookup failing. APT also warned that its EDSP output path resolved to the guest data directory.
No packages were changed by the simulation.

Build the independent tests with `sh Tests/GenericForkBuild.sh iphonesimulator tmp/generic-fork`. Sign the guest executables
and the fixture dylib. Run `fork-resolver` for the user lookup and `fork-stack-bounds` for the copied stack, pthread bounds,
lower guard and parent wait status. Run `fork-fixture` by path with its dylib beside it. It calls fork from the dylib and tests the callbacks from
`pthread_atfork`, private globals/heap/stack, shared file offset, close-on-exec and the status reaped by the parent.
The macOS control and the iPhone 15 resume trial both reaped status 37 after the child executed the fixture a
second time. The parent kept its global, heap and stack values and read byte `C` at the shared file offset. The
child callback ran once. The iPhone 15 trial used cache UUID `9a2fb463-a774-53ed-a661-ff3f3fcac717` at
`0x480000000`. Run with `LSB_FORK_SNAPSHOT_TRACE=1` for range, allocator, registry and wait-status diagnostics.
Before loading a package executable, LiveProcess reserves vacant low address ranges up to `0x164000000` in
the parent. These reservations have no access or physical pages. They place its loose images and allocator
metadata above the normal starting range of a fresh child's heap. A fork can still fail when
the observer cannot place every required region at the same address
(`EADDRINUSE`) or a new allocation appears between inventory and libc prepare (`EAGAIN`). It currently supports
one scalable default zone in the donor. An immediate nested fork from the adopted child before exec is unsupported.

### Intermittent fork failures during unpack

On iPhone 15 (host 23B50, guest 23A343), the 1900 `dpkg-deb` executable called `fork` from `subproc_fork+0xc`.
Unpacking a package starts separate archive-reader and decompressor processes and, for control-file extraction, a tar
process. A ten-package APT transaction exercises this path many times. [Upstream extraction code](https://github.com/guillemj/dpkg/blob/1.22.6/src/deb/extract.c#L259-L299)

- **`EAGAIN`.** Three failing donors each gained a 1 MiB tiny malloc region during the XPC call that registers their
  range list with the host. The child had already checked the original list. The new region was present
  before the donor entered libc's fork handlers, and the locked coverage check caught it. The donor now rechecks the
  allocator after the XPC call, before giving the child its broker token. On a new uncovered span it closes the socket,
  reaps that child, and rebuilds the offer once. The locked check still guards the final snapshot.
- **`EADDRINUSE`.** One donor needed the 8 MiB stack interval `[0x16efb0000, 0x16f7b0000)`. Four consecutive native
  children rejected it before starting guest dyld, exhausting the old limit. A separate run recorded both ranges on
  12 early rejections. Each donor stack overlapped the prospective child's live native pthread stack. Each was above
  that child's VM floor. Early rejections can now use up to eight placements. The full observer preflight still stops on
  its fourth placement failure, and a late broker error still reaches the caller.

The normal build completed three ten-package reinstalls on disposable copies and 16 consecutive stock
`dpkg-deb --control` extractions. One extraction detected a new 1 MiB region, rebuilt the offer, and passed the
locked check. Another used the eighth child after seven early stack rejections. Both exited zero. The independent
callback fixture also passed its `fork`, `vfork`, exec and wait checks. No debugger was attached.

#### Reclaiming the fork child's stack

When the donor's stack overlaps the lower part of the observer's native main stack, the single-threaded observer can move to a guarded
1 MiB scratch stack and release the overlapping pages. It checks the native pthread bounds against the actual writable stack mapping and
keeps the upper pages containing the native startup frames and kernel argument strings. It reserves the donor's exact range and a lower
guard page. The broker rechecks vacancy and maps the donor stack copy-on-write at its original address.

If the stacks are disjoint, the observer stays on its native stack until the final handoff. If it reclaimed pages, it briefly uses the
preserved upper pages to unmap scratch. Both paths publish the donor bounds in the child's native pthread at the instruction sequence
that switches SP and resumes the saved registers. The donor and observer use checkpoint protocol v21. For a general guest resume,
the donor verifies that the copied mapping matches its complete pthread stack, and the observer reserves a guard page directly below it.
A donor that overlaps the original stack top or an unrelated occupied mapping still needs a new placement.

On iPhone 15 (native 23B50, guest 23A343, cache `9a2fb463-a774-53ed-a661-ff3f3fcac717` at `0x480000000`), stock Procursus
`dpkg-deb --control` completed 12 extractions and reaped all 36 children. The extracted `control` and `md5sums` files matched
the archive byte-for-byte in every run. Three children reclaimed a real stack overlap on their first placement. In one run,
parent 34768 accepted child 34772, which released `[0x16a95c000, 0x16aef0000)` and resumed with the parent's 8 MiB stack at
`0x16a6f0000`. The parent's wait for that same PID returned status 0. The session used guest launchd without SpringBoard or a debugger.

An independent fork fixture checked the stack from the resumed child's own code. After reclaiming an overlap, child 35115 reported
the same 8 MiB pthread bounds as its parent, with its SP inside them. It modified an inherited stack local and exited 37. The parent
reaped 37 and retained the original value. Before v21, children with disjoint stacks resumed on the copied stack but still reported the
observer's original pthread bounds.

On the same iPhone 15, the v21 probe passed for 15 disjoint stacks and one reclaimed stack. In a disjoint child (PID 35639),
the observer's original stack ended at `0x16f2f4000`. After `fork()` returned zero, pthread reported the parent's 8 MiB range
`[0x16cbe0000, 0x16d3e0000)`, containing the actual SP `0x16d3da180`. The page below it was inaccessible, a stack write stayed
private to the child, and the parent reaped exit 37. The child went directly to the donor stack without allocating scratch.
Ten stock `dpkg-deb --control` extractions on that build matched the archive and reaped all 30 children with status zero.

On iPhone 17 (native 24A435, same guest and cache UUID at `0x7180000000`), the v21 probe passed for nine disjoint stacks and three
reclaimed stacks. Each child reported the donor's bounds and an inaccessible lower guard, and each parent reaped exit 37 with its
stack local intact. Ten stock control extractions matched the archive and all 30 children exited zero.

### iPhone 17

On native 24A435, the first stock APT dry run stopped before remapping with `EAGAIN`. The native pthread
registry existed and accepted new handlers, but its `os_once` word was still `0x2e0a5`. On arm64,
libplatform can publish a generation value after the initializer returns, then later change it to all ones.
The checkpoint recognizes both completed states and still rejects an initializer in progress.
[Apple's libplatform implementation](https://github.com/apple-oss-distributions/libplatform/blob/main/src/os/lock.c#L1077-L1176)

With that change, stock `apt -s --fix-broken install` on the 23A343 native-support cache at `0x7180000000`
completed both forks. The observer started with one matching atfork registration and staged the parent's second
registration from guest `libsystem_info`. In the first successful run each fork offered 49 ranges (70,352,896 bytes).
The parent reaped `SIGINT` and exit 127. Fish and guest launchd reported exit 0, with four installs, two removals,
and the same EDSP path warnings as on iPhone 15. Two of four independent APT jobs on the final build completed.
The other two exhausted all four observer attempts because required addresses were occupied or below the child's
minimum VM address. Both reported `Failed to fork` and exited 100.
The independent fork/exec fixture reaped exit 37 with the expected shared file offset. The resolver test also
reaped 37 after both processes resolved `mobile` to UID 501.

### Running repair from the Fish tab

In Fish, `apt --fix-broken install` also inherits the terminal's job-control state. The fork observer defers its
terminal-stop worker until libc finishes its child handlers. The donor parks its own worker at the verified
`kevent64` wait, and the child installs its own implementation of default terminal-stop signal dispositions.
This preserves guest job control when the parent's shim and the child's shim load at different addresses.

After the layout and terminal fixes, four invocations of the literal command in the same Fish tab reached
`Continue anyway? [y/N]` on iPhone 17 without setting any fork variables. The installed copy used its existing acquisition library. A separate
run with the archive's signed-only APT and both original libraries reached the same prompt on the same terminal.
The tests answered `n`, so status 1 was the requested abort. Package downloads and dpkg changes after confirmation
were outside this test.
