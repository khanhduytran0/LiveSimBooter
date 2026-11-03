# Procursus in the guest

Install the Procursus 1900 `iphoneos-arm64` bootstrap in the iOS 26.0 (23A343) guest. Fish then searches its command directories,
and APT can fetch and verify the repository index.

## Install or patch

Use the current LiveSimBooter host, LiveProcess extension, guest spawn shim and terminal helper from the [setup guide](agent-setup.md).
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
at startup so APT reads its sources, indexes and dpkg status from guest data. When the bootstrap's CA bundle is present,
the host gives the HTTPS method its physical path for TLS verification. APT's patched worker uses `posix_spawn` for its
two protocol pipes. The verification method launches the guest's stock `/var/jb/usr/bin/gpgv` with the selected keyrings.

For automated verification, use `apt -o APT::Update::Error-Mode=any update`. Otherwise a failed fetch can print a warning
and still exit zero. Check that `apt-cache policy cmake` shows a candidate from `https://apt.procurs.us`.

On iPhone 15 and iPhone 17, an update from empty lists downloaded and authenticated the Procursus 1900 release and the
`iphoneos-arm64` index. On iPhone 17, a cold boot also passed plain `apt update`. `apt-cache policy cmake` reported 3.30.2,
and `apt list --installed` matched all 67 entries in the guest's dpkg database. The verifier rejected an altered release
with `BADSIG` and a different keyring with `NO_PUBKEY`.

Package installation and upgrades remain untested. The installer skips the archive's `prep_bootstrap.sh`. Its Dash exits 2
on line 3 when it tries to fork `/var/jb/usr/libexec/firmware`.
