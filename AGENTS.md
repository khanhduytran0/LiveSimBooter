# Agent guide

LiveSimBooter is an experimental native iPhone host for an arm64 iOS Simulator runtime. Start with
[the setup guide](docs/agent-setup.md), then read the relevant topic in [docs/README.md](docs/README.md).
The setup baseline is **iOS Simulator 26.0, build 23A343**. Runtime patch offsets and several private ABIs are pinned to it.
Do not substitute an iOS 26.x update or iOS 27 runtime because its marketing version looks compatible.

## Setup and local state

- There is no in-app runtime installer. Use the tracked `Tools/setup-ios26-*` commands and the documented build/sign/copy steps.
  Apple runtime files, signing identities, provisioning profiles, and device access are supplied by the person setting up the project.
- Inspect `git status` before editing. Preserve unrelated changes and coordinate file ownership when working with other agents.
- Keep binaries, source downloads, signing outputs, provisioning exports, logs, and intermediate manifests under ignored `tmp/`.
  Do not commit runtime binaries, private keys, account data, device unlock codes, or machine-specific paths.
- Treat paths and PIDs in older experiment notes as historical evidence. `tmp/` links in those notes may not exist in a fresh checkout;
  the setup guide and tracked setup tools must not depend on them.
- Configure the local team, app and extension identifiers, and both store App Groups together. The `APP_GROUP_SIDESTORE` and
  `APP_GROUP_ALTSTORE` Xcode settings, both targets' signed entitlements/profiles, and runtime group identifiers must agree.
  Shell variables in the setup guide do not rewrite these source settings.
- Runtime storage is `<selected store App Group>/LSB/iOS/23A343/{RuntimeRoot,data}`. Prefer an available group containing
  `LSB`, then one containing `Apps`, with SideStore before AltStore for ties or an empty setup. Only the pinned build is selected;
  there is no runtime-version picker or automatic migration from older locations. Manually restore `RuntimeRoot` and rename
  the old `SimulatorData` root to `data` as described
  in `docs/migration.md`. Preserve the backup until verified.
- Keep original runtime inputs immutable. Build into a new directory and keep one known-working device runtime until the replacement is tested.
  Inspect free space before making another multi-gigabyte cache or resource tree. Do not clean unrelated files to make room.

## Architecture boundaries

- `Host/` retains the native cache and owns UIKit, the broker, framebuffer presentation, and public IOSurface service.
- `LiveProcess/main.c` remains pure C. Simulator guests prepare synchronously, release native XPC objects, and check thread count.
  Production/unknown devices retain the native cache and redirect workqueue entry with a hardware breakpoint. Only a native
  non-production MobileGestalt result permits cache detachment; do not infer this from the OS version or Developer Mode.
  Detached code must not call removed host-cache implementations; retained native code must not run against incompatible guest TLS either.
- Native Metal helpers use separate host-selected LiveProcess roles and keep native libraries. Guests cannot choose arbitrary native roles.
- `Guest/` contains simulator interposers. `ProcessSupport/` contains shared wire formats and validation. Preserve ownership of FDs,
  Mach rights, immutable identity snapshots, full audit tokens, and logical/native PID distinctions.
- Fish in the ordinary UI belongs to simulator launchd through `/usr/libexec/terminal-fish`; tab changes must not spawn a host-owned replacement.
- Declare private C/Objective-C APIs in headers and call them normally. Avoid hand-written selector dispatch for private methods.
- Use named structs and layout assertions for wire/memory layouts. Explain pinned patch provenance and validate original bytes before patching.
- Preserve existing license notices. Apple runtime inputs are separately supplied and are not licensed by this repository's source license.

## Working and verification

- Use roughly 160 characters per source line; do not reflow unrelated code solely to enforce the limit.
- Keep compatibility changes narrow: exact API, path, runtime, caller, or operation where evidence requires it. Do not turn a failed query
  into a blanket entitlement or identity success. Preserve errno and native forwarding behavior outside the supported case.
- For startup/interposition changes, check pre-constructor execution and TLS availability. A harmless-looking logging or environment read
  can call into native state before the simulator has initialized.
- Run focused tests for the changed behavior. `sh Tests/run.sh` is the integrated local suite; some tests use real native IOSurface/Metal
  frameworks and may need normal local process access outside an agent sandbox. Build the host and guest shim as separate artifacts.
- After protocol/control-page layout changes, update the ABI version and rebuild every producer/consumer together.
- Run `git diff --check`. A local build or test pass is not evidence of device signature acceptance, working mappings, or a successful UI operation.
- Plist policies must retain their actual types. In particular, `@(!value)` boxes an integer; use `condition ? @NO : @YES` for `RunAtLoad`
  and test `CFBooleanGetTypeID()` after serialization. `boolValue` alone misses this launchd failure.

## Device work

- Discover the current CoreDevice identifier and App Group path. Never reuse a recorded container UUID or PID blindly.
  SSH is an optional externally configured development channel; this app does not install an SSH server on the phone.
- Stop affected guests before replacing mapped executables, the shim, or cache. Transfer to an inactive location and preserve a rollback copy.
  Do not use `rsync --delete` against an active or partial runtime. Honor the user's requested transfer-verification policy.
- Keep the native host foregrounded for baseline tests. Do not assume a retained extension assertion or attached debugger prevents suspension.
- Never set `DYLD_SHARED_REGION=private` in the native app's environment. Guest diagnostics are opt-in; unset `DYLD_PRINT_*` rather than setting them to `0`.
- Use hardware-only LLDB breakpoints and set `target.parallel-module-load` to `false` before attachment. On the tested Apple LLDB,
  `target.experimental.parallel-module-loading` silently does not change that setting; verify with `settings show target.parallel-module-load`.
  Configure ignored `EXC_BAD_ACCESS` before attachment when testing the intentional workqueue bridge; use the documented bridge diagnostic
  modes if the exception itself is under investigation. Wait for the actual stopped notification before **any** further command, including
  `process status` or Python state polling: asynchronous attach can still be initializing the language runtime under module locks.
- Before finishing device work, detach your debugger and verify that the target resumed and your debugserver session is gone.
  Never leave a guest or host stopped/attached accidentally. If LLDB itself deadlocks, terminate only your known debugger client,
  then independently verify device resume/detachment; do not kill the guest to hide a debugger-side failure.
- Record runtime build, cache base/UUID, artifact provenance, process identity, debugger use, and actual exit/mapping/UI results.
  A successful `launchctl kickstart` only proves command acceptance; inspect the new process and its output separately.
- Do not modify provisioning databases or device security state as an implicit setup step. The normal setup requires profiles granting the requested capabilities.
