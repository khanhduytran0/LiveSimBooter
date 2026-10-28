# Loading iOS binaries with the Simulator runtime

The loader accepts plain arm64 programs with either `LC_BUILD_VERSION` platform `IOS`/`IOSSIMULATOR`, or the legacy
`LC_VERSION_MIN_IPHONEOS` command. Exactly one platform declaration is required. The existing PIE, `LC_MAIN`, signature,
segment, and arm64 ABI checks still apply. This avoids rebuilding a compatible device executable or rewriting its platform
load command; the runtime's normal development-signing procedure is still required.

`Tools/patch-ios26-platform.py` applies the corresponding change to the exact iOS 26 `23A343` `dyld_sim`. It recognizes the
original binary, the unsigned relocated-cache variant, and this project's existing signed relocated-cache variant by full
SHA-256. It also checks every original instruction before writing a new output and a manifest. Inputs are never modified,
existing outputs are refused, and the output must be signed separately.

```sh
python3 Tools/patch-ios26-platform.py --source tmp/dsc-relocated/signed/dyld_sim \
  --output tmp/ios-platform/dyld_sim-patched
cp tmp/ios-platform/dyld_sim-patched tmp/ios-platform/dyld_sim-signed
codesign --force --sign D7610EDC80D34A9B1A3D3C8563D66A5CD4716293 --identifier com.kdt.LiveSimBooter \
  --timestamp=none --digest-algorithm=sha256 tmp/ios-platform/dyld_sim-signed
python3 Tools/sign-sim-executable.py --source /path/to/original-ios-program --output tmp/ios-platform/program-signed \
  --identity D7610EDC80D34A9B1A3D3C8563D66A5CD4716293 --identifier com.kdt.LiveSimBooter
```

The executable signer now requests 16KB code-signing pages explicitly. Otherwise `codesign` chooses 4KB pages for the
legacy iOS 7 Bash binary, which the existing CodeDirectory/CMS helper deliberately does not support. This changes the
signature format, not the executable's platform, minimum OS, or SDK declaration.

## What changes inside dyld_sim

There are two separate decisions in Apple's dyld:

- [`Process::getMainPlatform`](https://github.com/apple-oss-distributions/dyld/blob/dyld-1323.3/dyld/DyldProcessConfig.cpp#L700)
  derives the process platform from its main image. The patch turns an iOS main process into an iOS Simulator process while
  retaining its original minimum OS and SDK versions. This selects the Simulator cache and runtime behavior.
- [`Header::loadableIntoProcess`](https://github.com/apple-oss-distributions/dyld/blob/dyld-1323.3/mach_o/Header.cpp#L2077)
  checks each dependency's platform. The patch additionally admits iOS dependencies into an iOS Simulator process. Existing
  Simulator and allowed host-library paths retain their original checks.

The 11 instruction changes reuse the macOS `DYLD_FORCE_PLATFORM` block and the macCatalyst-to-macOS dependency fallback.
Those cases cannot be reached through the app's supported main-image platform set; macOS and macCatalyst programs remain
rejected by the app loader. The patch does not make all platform checks return success. Per-image platform declarations
remain iOS, while `dyld_get_active_platform()` reports iOS Simulator for the running process. The prior cache-base patch
is retained byte-for-byte outside these platform changes.

## Validation and limits

Host loader tests cover modern iOS/Simulator and legacy iOS declarations, wrong platforms, duplicate declarations,
and malformed legacy commands. The 11 patcher tests passed, covering exact write boundaries,
hash/instruction/header checks, refusal to overwrite files or symlinks, source preservation, and failed-write cleanup.

On 2026-10-25, normal launches on `iphone-0xf-26-cua-duy.local` (iOS 26.1 `23B50`) completed with the patched loader and the
existing iOS 26 cache at `0x120000000`. No debugger was attached:

| Guest | Result |
| --- | --- |
| Existing Simulator `df`, PID 79573 | Printed the filesystem table and exited 0. |
| iOS executable plus iOS dylib, PID 79577 | Printed `main=2 dylib=2 active=7` and exited 0. Both images retained `LC_BUILD_VERSION IOS`. |
| Original-platform Bash, PID 79579 | Printed `bash-ios=3.2.57(1)-release` and `answer=42`, then exited 37 (`0x2500`). |

Bash came from `/Users/duy/Developer/spironolactone/work/sshtar/bin/bash`. Its original and signed
`LC_VERSION_MIN_IPHONEOS` command bytes match exactly: iOS 7.0 minimum, SDK zero. No `vtool` command or rebuild was used.
The executable was signed without entitlements and with `CS_EXECSEG_MAIN_BINARY` cleared. The modern iOS probe and dylib
were compiled for plain arm64 iOS 26.0, SDK 27.0, and their platform commands were also left unchanged. All signatures use
identity `D7610EDC80D34A9B1A3D3C8563D66A5CD4716293` and identifier `com.kdt.LiveSimBooter`.

The patched dyld signature passed `codesign --verify --strict` and rehashing all 38 code pages. Its executable-segment flags
remain zero. Its deployed SHA-256 is `1caa843ab8c98da45a3794b99e66b2d3719e0855936b0ec69ceafcca81426a2b`; the original-platform
Bash hash is `db269f0143a2c0a5e484fb6bf4fc30e9432d1a0fa83f59da02058cd66636f41f`. Remote readback matched both signed files.
The temporary probe and its dylib were removed from the phone. `bin/bash-ios` remains available alongside the previously
converted `bin/bash`, and the patched dyld_sim remains active.

Evidence lives in `tmp/ios-platform/console-df-compat.log`, `console-ios-platform-unlocked.log`, `console-bash-ios.log`,
`unchanged-bash-platform.json`, `sign-bash.json`, `sign-probe.json`, and `deployed.sha256`. The simultaneous
`security-probes.log` capture matched no amfid or kernel TXM event. The first launch attempt was rejected while the phone was
locked and did not execute a guest. These results do not measure new VM protections; cache detachment and low-address mapping
follow the already established runtime handoff.

This is an ABI compatibility option for the existing Simulator runtime, not an implementation of all device frameworks.
Missing symbols, private framework differences, unsupported system calls, encrypted executables, and arm64e remain separate
constraints. A stock arm64e device image is not compatible merely because its platform is iOS. The current shell limitation
around `fork`, `exec`, and full job control is also unaffected.

## UIKitester arm64e inspection

The native iOS 26.1 UIKitester bundle was inspected from an immutable copy. Both its executable and widget are arm64e,
with `DYLD_CHAINED_PTR_ARM64E_USERLAND24` fixups. Changing only the main executable's CPU subtype to plain arm64 allowed
the host loader's local header/layout preflight to pass; it was not signed, installed or executed on the phone.
The main image contains 31,426 authenticated fixups. The pinned simulator dyld recognizes the pointer format but its arm64
fixup implementation reports `architecture does not support PAC` when asked to sign one. Header relabeling therefore does
not make this UIKitester build runnable. This conclusion comes from fixup enumeration and dyld disassembly, not a device
crash. A matching plain-arm64 build or separate arm64e ABI support is still required.
