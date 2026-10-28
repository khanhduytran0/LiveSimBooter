# Guest SSH with Fish

The guest experiment uses [NyaMisty/sshdog_ios v1.1](https://github.com/NyaMisty/sshdog_ios/releases/tag/v1.1), tag commit
`9c7cb8917e1872c73e4717cfa59bbdef2970ad7c`. Its Darwin implementation uses `posix_spawnp`; terminal sessions additionally
use a helper process followed by `POSIX_SPAWN_SETEXEC`. The release's support for Fish on native iOS does not establish
interactive SSH support in this simulator environment.

The device run with the installed RSA key completed public-key authentication and ran non-PTY Fish commands, including
`df -h` and `launchctl list`. The explicit status-only wait4 compatibility mode below allowed Go to wait successfully for
the command. Interactive SSH currently fails when the server tries to open `/dev/ptmx` with `operation not permitted`.
The earlier EC-key attempt failed because the Mac-generated PEM used explicit curve parameters unsupported by the
release's Go parser. The staging recipe below uses the working RSA PKCS#1 PEM format.

## Prepare the signed executable

Keep downloaded binaries, keys and generated configuration under ignored `tmp/`. The checked-in staging tool contains no
runtime binary or private key. Download the [v1.1 release asset](https://github.com/NyaMisty/sshdog_ios/releases/download/v1.1/sshdog):

```sh
mkdir -p tmp/sshdog-input
curl -fL https://github.com/NyaMisty/sshdog_ios/releases/download/v1.1/sshdog -o tmp/sshdog-input/sshdog.original
```

The inspected original has SHA-256 `9aaec8f86fde670c5141754e98f6853c149a117e195ada9e0babb4963a11608c`. It is a thin arm64
MH_EXECUTE, built with Go 1.18.8 for iOS 12.0 / SDK 14.5. Keep that platform command intact; this requires the project's
[iOS platform compatibility](ios-platform.md) support. Its libSystem, CoreFoundation and Security dependencies are in the
current guest cache. This recipe stages the original release and does not claim a reproducible rebuild with newer Go:
upstream's replacement `exec.Cmd` layout and linknames explicitly depend on Go 1.18 internals.

Sign with the existing app identity and identifier, using a fresh output path:

```sh
python3 Tools/sign-sim-executable.py --source tmp/sshdog-input/sshdog.original \
  --output tmp/sshdog-input/sshdog.signed --identity "$LSB_SIGNING_IDENTITY" \
  --identifier com.kdt.LiveSimBooter > tmp/sshdog-input/signature.json
```

This preserves MH_EXECUTE, adds no entitlements, and clears CS_EXECSEG_MAIN_BINARY. Signing may require access to the
existing keychain identity. Prepare matching signed simulator `/bin/launchctl` and the [guest control worker](guest-control.md)
separately. The already working [Fish executable](fish.md) must be installed at `RuntimeRoot/bin/fish`.

## Keys and staging

Supply an existing client **public** key as `authorized_keys`; keep its private key on the client. The staging tool requires
at least one public key and rejects private keys, empty authorization files and OpenSSH key-option prefixes. Upstream
sshdog does not enforce those options and otherwise allows unauthenticated connections when no nonempty authentication
configuration exists. It listens on all interfaces, so install the complete authorization configuration before starting it.

Generate a host identity once, then reuse it for later stages:

```sh
mkdir -p tmp/sshdog-keys
ssh-keygen -q -t rsa -b 2048 -m PEM -N '' -f tmp/sshdog-keys/ssh_host_rsa_key
```

Do not rerun that command over an existing key. The unencrypted RSA PEM stays in the private server configuration with
mode 0600. If replacing an earlier ECDSA experiment, stop the job and retire the old ECDSA key files explicitly; creating
a new stage does not remove stale files on the phone.

```sh
python3 Tools/stage-sshdog.py --binary tmp/sshdog-input/sshdog.signed \
  --authorized-keys "$LSB_CLIENT_PUBLIC_KEY" --host-key tmp/sshdog-keys/ssh_host_rsa_key \
  --device-group /private/var/mobile/Containers/Shared/AppGroup/APP-GROUP-ID \
  --ssh-host device.local --port 2223 --output tmp/sshdog-stage
```

The tool verifies the existing signature and key formats, then writes a new stage under ignored `tmp/`. It never signs,
deploys, starts a daemon, overwrites an existing stage, or reads a client private key. Its `stage.json` records the files and
destination roots. The optional `client/known_hosts` pins the supplied host key for the named host and port; keep it on
the Mac. Only the `RuntimeRoot` and `data` trees are deployment payloads.

The `--device-group` argument names the selected store App Group itself; the tool appends `LSB/iOS/23A343` when constructing
physical paths. Copy those two trees under `<selected store group>/LSB/iOS/23A343/`. Ensure the server configuration is readable by guest
uid 501, with its host private key and authorized_keys mode 0600 and config directory mode 0700. Create
`data/Library/Logs` if needed. No checksum pass after copying is required by this workflow.

The generated environment sets `SSHDOG_SHELL` to the **physical** Fish executable, sets HOME/CFFIXED_USER_HOME to the
physical data path, and points XDG_CONFIG_HOME to the staged Fish configuration. `SSHDOG_SHELL` is only a pathname;
appended `-C` arguments fail upstream's `os.Stat` check and cause fallback to `/bin/sh`. The Fish file instead sets
`status job-control none` and `fish_use_posix_spawn`. The server reads `./config`, so its working directory matters.
No `config/daemon` marker is installed: launchd owns the foreground process. Port 2223 keeps it separate from native phone
SSH on 2222; upstream stores ports in a signed 16-bit value, so the tool accepts only 1–32767.

The generated job also sets `LSB_WAIT4_STATUS_ONLY=1` for Go's `os/exec` wait path, which always supplies a rusage pointer.
The guest shim samples this exact value at initialization. It still waits for the broker's actual terminal event and
returns the child's real normal or signal exit status; after a positive reaped PID it clears rusage to indicate **uncollected**
usage. Those zeros are not measured CPU or resource usage. Pending `WNOHANG` and failed waits leave that buffer unchanged.
Without this explicit opt-in, nonnull rusage remains unsupported with `ENOTSUP` and does not consume the child's status.

## Start through the guest launchctl

Keep the control job running, then bootstrap the original generated plist directly. Do not add sshdog to the host's
selected-service sanitizer, which replaces WorkingDirectory and would prevent reading `./config`.

```sh
python3 Tools/guest-launchctl.py --host root@device.local --port 2222 \
  --data-root /private/var/mobile/Containers/Shared/AppGroup/APP-GROUP-ID/LSB/iOS/23A343/data \
  --output-dir tmp/guest-launchctl -- bootstrap user/501 \
  /private/var/mobile/Containers/Shared/AppGroup/APP-GROUP-ID/LSB/iOS/23A343/RuntimeRoot/System/Library/LaunchDaemons/com.kdt.LiveSimBooter.sshdog.plist
```

The bootstrap plist argument is a **physical path** because launchctl opens that file. Inside the plist, Program is the
guest `/usr/local/libexec/sshdog`, while WorkingDirectory and log paths use guest `/private/var/mobile/Library/...` paths
that simulator launchd translates. Do not replace those job fields with physical App Group paths: they can be translated
twice. Use `print system` or `list` for diagnostics; use `user/501` for the tested bootstrap/bootout domain. Once registered,
`kickstart -k user/501/com.kdt.LiveSimBooter.sshdog` requests a restart, and `bootout user/501/com.kdt.LiveSimBooter.sshdog`
removes it without restarting the simulator session.

Start validation without a PTY and inspect the daemon log alongside command output:

```sh
ssh -T -i "$LSB_CLIENT_PRIVATE_KEY" -o IdentitiesOnly=yes \
  -o UserKnownHostsFile=tmp/sshdog-stage/client/known_hosts -o StrictHostKeyChecking=yes \
  -p 2223 mobile@device.local 'echo sshdog-fish-ready; df -h'
```

The SSH username does not select a separate guest account; the daemon runs as its existing guest uid. Upstream's channel
exit-status field does not consistently reflect ordinary child failures: the tested `exit 23` reached Go as status 23 but
the SSH client returned 0. Inspect command output and the daemon log instead of treating SSH exit 0 as proof of success.

## Device checkpoint

The promoted guest launchctl worker bootstrapped the original sshdog plist in `user/501` with exit 0. Guest sshdog **5201**
loaded the installed RSA key, listened on port 2223, authenticated the client, and received the non-PTY Fish command.
Its first `posix_spawnp` returned EPERM at the shim's `getcwd` stage, before a Fish process was created. The daemon log is
`tmp/app-launch-fix/daemon-latest.txt`; the successful bootstrap status is request
`64cf219bf2504093a219582337e0b883` under `tmp/app-launch-fix/promoted-results`.

LLDB then inspected Go's cached `os/user` record and confirmed HomeDir was `/var/mobile`, despite the physical HOME
environment value. The upstream launcher uses that account home for its per-thread directory change, explaining why
the subsequent spawn cwd lookup encountered the sandboxed native path. Evidence is
`tmp/app-launch-fix/lldb-sshdog-cwd.txt`. The guest account-home compatibility fix now maps uid 501's `/var/mobile` home to
the physical SimulatorData directory. The subsequent spawn reached FD transfer, where Go's internal close-on-exec
descriptor caused `xpc_fd_create` to fail with EINVAL. The FD snapshot now omits descriptors that close on exec unless
an ordered file action needs their original value as a DUP2 source, FCHDIR input, or explicit INHERIT.

With both fixes installed, guest sshdog **6833** authenticated the client, spawned Fish **6836**, and Fish spawned df **6837**.
The SSH output contains `sshdog-fish-ready` followed by the filesystem table from `df -h`; see
`tmp/sshdog/nonpty-fd-fixed-retry.log`. The daemon then reported `wait: operation not supported` because Go requested rusage;
see `tmp/sshdog/fd-fixed-retry-daemon.log`. This proves remote command execution and output, but not successful wait completion
for that run.

With `LSB_WAIT4_STATUS_ONLY=1`, Fish **7622** spawned df **7624**, printed the filesystem table and `external-status=0`, and
Go completed its wait with `Finished execution. Err: <nil>`. Evidence is `tmp/sshdog/nonpty-wait-fixed-retry.log` and
`tmp/app-launch-fix/wait-success-app-current.txt`. A further command ran guest `launchctl list` through Fish, showing the
simulator job list without restarting the session; see `tmp/sshdog/ssh-launchctl.log`.

The failure-status check printed `before-exit-23`, and Go correctly reported `exit status 23`, confirming that status-only
mode preserves nonzero child status. The SSH client nevertheless returned 0 because of the upstream channel-status bug;
see `tmp/sshdog/ssh-exit23.log` and `tmp/sshdog/ssh-checks-daemon.log`. Unit tests separately cover signal exits, pending
waits, errors, and restoration of strict mode.

## Interactive limitations

The interactive SSH attempt returned client status 255 and `PTY allocation request failed on channel 0`;
`tmp/sshdog/ssh-pty.log` records that result. The daemon identifies the first failure as
`unable to open "/dev/ptmx": operation not permitted` in `tmp/sshdog/ssh-checks-daemon.log`. Unlike the host's existing
inherited socket terminal, upstream allocates `/dev/ptmx`, requests a slave pathname, grants/unlocks it, and opens the slave.
The current [fake PTY](fake-pty.md) does not allocate
those devices or provide openable slave paths. The SSH PTY path also asks for POSIX_SPAWN_SETSID and a controlling terminal;
SETEXEC replacement alone does not implement those session semantics. Some Go terminal ioctls and thread-directory
changes use raw syscalls, so libc interposition alone does not cover them.

Upstream's Darwin resize function returns Unsupported and the session handler has no window-change case. The wait4 opt-in
above provides terminal status with uncollected usage only; it does not implement stopped/continued notifications or a
controlling terminal. Non-PTY command output does not establish interactive Fish support.

A later regression run with the full SpringBoard cache at `0x108000000` authenticated successfully, but two Fish launches
stopped at the existing loader guard, `Simulator cache overlaps a resident executable image`, before guest entry. The SSH
client still returned zero because of the release's exit-status limitation. The earlier command-mode success does not prove
all native address layouts can run Fish with this larger cache; collision handling remains separate loader work. Evidence:
`tmp/app-launch-fix/workspace-shm/ssh-command-regression.log` and `ssh-command-regression-2.log`.
