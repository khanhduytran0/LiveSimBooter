# Lazy sign

Native Settings → Code signing has an opt-in **Lazy sign** switch and a P12 import row. Tap **Import P12…** to select a P12/PFX
file and enter its password. After import, the row shows **Imported**; tapping it replaces the identity, and its × button removes
it. The switch defaults off. Changes apply on the next start/restart; a running session offers Restart or Later.
Use an identity accepted for the installed host's signing identifier/team. Importing a certificate does not grant provisioning
capabilities or make AMFI accept an otherwise unauthorized identity.

For Apple Development identities issued by WWDR G3, the signer supplies Apple's bundled public G3 intermediate as a certificate-only
Keychain item and in the final CMS chain. This is conditional on the leaf's issuer; it adds no private key or trust override.

The original document is untouched. The host validates one identity with memory-only `SecPKCS12Import`, then stores the P12,
password and certificate metadata as one app-private, device-only Keychain item. Ordinary metadata reads do not fetch the key
payload. Credentials are snapshotted per simulator session and sent only to the host-selected native signer. They never enter
the guest environment, runtime tree or guest IPC replies. A different phone requires another import.

## Registration and signing

The normal loader always tries `F_ADDFILESIGS_RETURN` first. Only a real failure triggers the optional retry. Both native
mapping of dyld_sim/main executables and dyld_sim's app-local fcntl callback use it, covering normal dependency/dlopen and
cache registration paths. Direct arbitrary guest `fcntl` calls are not interposed by this feature.

When enabled, dyld_sim's app-local `stat` callback also preflights supported files before dyld records their inode and mtime.
It bounds-checks the signature metadata, tries real signature registration, and repairs only a failed registration. After
repair it returns the verified replacement's actual metadata, so subsequent mapping checks and image deduplication use the
new inode. This avoids inventing an identity for an atomically replaced file. General guest `stat`/`fstat` remain unchanged;
when disabled, the dyld callback forwards directly without opening or reading the file.

The app-local client uses bounded synchronous Mach messages because this can run before guest TLS, constructors or XPC are
available, and after native-cache detachment. The host supplies its private endpoint and the bootstrap reply's genuine audit
identity. The host authenticates the requesting managed guest's complete audit token and confines the opened file to this
session's canonical RuntimeRoot or data tree. Device/inode, size, mode and modification/change times identify the requested
generation. Requests are serialized, with a bounded queue and idempotent reuse of an already published replacement.

Security code runs in a separate native **Signer** LiveProcess, loaded from a fixed bundled dylib. This role retains native
libraries and never installs guest TLS. Guests cannot select the role, backend path, identity or signing identifier. Only the
native host's full audit identity can connect to its XPC listener. The signer receives a read-only FD and makes one private
scratch clone for Security's file-based `SecCodeSigner` writer. It hashes the unchanged original and assembles the final
signature in memory using `CMSEncoder`. Its XPC reply contains only the unpadded signature data, with no signed-output FD
or intermediate output file. The scratch file is removed on normal success or failure. APFS cloning avoids copying its
unchanged file data when supported.

The final signature uses the host bundle identifier, no signature entitlements, a contiguous 16 KiB SHA-256 CodeDirectory,
and no `CS_EXECSEG_MAIN_BINARY`. Original guest `__TEXT,__ents_der` sections remain unchanged for the existing entitlement
snapshot policy. The requested arm64 fat slice is signed independently; other slices remain untouched. The pinned single-file
arm64 simulator cache is also supported. Its signature is rebuilt into the existing allocation, preserving the original
header and `codeSignatureSize`; no signature-size fixup is needed when that allocation is sufficient.

The host validates the reply's audit identity, version, data type, size and signature structure against the requested allocation.
Only after signing succeeds and the source generation still matches does it clone the original runtime file to a named sibling
with `CLONE_NOOWNERCOPY`. It writes the signature data and zero-padding only within that allocation; the reply cannot supply
replacement executable bytes or change the file size. Unchanged data stays shared through APFS CoW. A bounded data-copy
fallback remains for original files that cannot be cloned.
The host flushes the signature and verifies actual kernel signature registration before publication. The host's staging name
must remain present during registration because amfid reopens it.
It temporarily preserves the original under
`LSB/iOS/23A343/SigningBackups/<UUID>`, with a companion plist recording the relative destination and signature span, then
atomically replaces the path with a new inode. Successful publication and final identity verification remove that backup
and plist. A failure after replacement retains the backup; cleanup failures are logged without rolling back an already
published binary. Existing backups from older builds are not automatically removed. Existing mappings/open descriptors retain their original inode. The requester
opens the exact returned generation, registers its signature, then replaces its own FD while preserving flags and offset.
Signing, validation or registration failures retain the original loader error; there is no ad-hoc fallback or success bypass.

The native host/signer XPC protocol is **version 2**; version 1 output-FD replies are rejected. Rebuild and deploy both native
components together. The guest-facing Mach repair messages remain version **1**, so no new guest transport change is required
for signature-data replies. The current host and extension use transport ABI **9**, bootstrap ABI **7**; the child ledger remains ABI **5**.
Do not mix those with older guest shims. Xcode builds and signs the native backend through `Tools/build-native-signer.sh`.
It belongs under `LiveProcess.appex/Frameworks/NativeSigner`, separately from the runtime and native Metal backend.

## Limits and verification

Lazy sign requires an existing signature allocation. A truly unsigned file without `LC_CODE_SIGNATURE`, a malformed file,
an unsupported format or an insufficient allocation is rejected rather than changing a layout dyld has already parsed.
Supported Mach-O file types are `MH_EXECUTE`, `MH_DYLIB` and `MH_DYLINKER`; `MH_BUNDLE` is currently unsupported.
The current cache support is for the pinned single-file simulator cache, not arbitrary split caches or other runtime versions.
It is a signature repair path, not a runtime installer, relocation engine or platform conversion tool.

A signer crash invalidates its endpoint without taking down the host. Already signed programs can still launch; further
signature repairs require a simulator restart. Each request has a timeout. A timed-out request is not automatically resent
because publication may already have completed. There is no automatic rollback of a file another process may have opened;
restore a recorded backup only with affected guests stopped.

Local tests cover input bounds, exact-layout signatures, authentication, malformed replies, timeouts, FD semantics, atomic
publication, backups, failed AMFI registration and secure credential storage. Disposable test identities successfully signed
thin/fat executables and a 277 MiB pinned cache on the Mac, with `codesign --verify` passing and unchanged bytes outside the
signature allocation. macOS CMS testing required a disposable keychain; iOS uses memory-only import. On-device signature
acceptance must be verified separately; importing a P12 or a successful local build is not that evidence.

iOS Security can omit the requirements blob in its scratch signature. The parser accepts CodeDirectory and CMS with optional
requirements, and materializes an empty requirements set when absent. It still rejects unexpected/duplicate slots and rebuilds
the final special-slot hashes itself.

## iPhone 17 verification, 2026-10-31–11-01

The device was iPhone18,2 on native iOS 27.0 / 24A435, with the pinned Simulator 26.0 / 23A343 runtime. The unchanged cache
reservation starts at `0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`. The refreshed RuntimeRoot was activated from an
inactive clone, retaining the prior root for rollback and preserving `data`; no transfer checksum pass was performed.

No debugger was attached for these signing tests. Each negative fixture changed one CMS byte in a separate test binary,
leaving executable code and the working `/bin/df` untouched. The executable repair ran as guest/native PID 16983, printed the
df table and exited 0. A subsequent launch exited 0 without another signing request. The dependency repair ran as guest/native
PID 17307: its first `dlopen` signed the invalid dylib, returned the expected value 42 and exited 0. AMFI logged profile validation
for the staged signatures; fetched test outputs also passed `codesign --verify`, with only the existing signature allocation changed.
Original test files were retained in `SigningBackups`. Killing native Signer PID 17038 left host PID 17037 alive; a new session
started a new signer successfully. These are historical process identities, not reusable device commands.

This exposed and fixed three device-only differences: the missing public intermediate, iOS's optional requirements blob, and
amfid's pathname reopen. Cross-container APFS cloning also returned `EPERM`; bounded positioned copying now handles that case
without requiring pathname or xattr access to the signer's private output. A dyld-only stat preflight fixes the first-load inode
check while keeping real file identities. Device repair of a shared cache and device fat-binary signing remain unverified.

The final graphical session used native host 17337, Signer 17338, launchd 17342, backboardd 17385 and SpringBoard 17386.
With Lazy sign enabled and no cache-policy override, it retained the native cache, registered the native configd bridge,
and displayed SpringBoard with app icons and the Calendar widget. Launchd-owned Fish ran the repaired df probe and reported
exit 0. The earlier configd restart loop came from native helper planning clearing pending bootstrap flags; signer and Metal
helper plans now preserve them. No debugger was attached, and the final process inventory contained no debugger sessions.
The integrated test suite and signed device build passed. This graphical check does not establish that every migrated app works.

That graphical check used artifact UUIDs host `98A6ED09-FD76-3706-ACBA-6E1B0039EC34`, LiveProcess `5D9814B5-0F12-3321-B98E-88F1D84EE9E7`,
and NativeSigner `CA7E8B67-50F6-3DEC-A118-EA9709629786`. The runtime/cache was not rebuilt for this final check.

Detailed ignored captures and test artifacts are under `tmp/lazy-sign-20261031/`, including `console-probe17-copy.log`,
`console-probe17-repeat.log`, `console-dlopen17-stat.log`, `amfi17-final.log`, `device-signature-verification.log`,
`console-complete17-unlocked.log` and `graphical17-complete-later.png`.

## CoW staging and cleanup, 2026-11-01

On the same iPhone 17/runtime/cache, `CLONE_NOOWNERCOPY` still returned `EPERM` when cloning the signer's unlinked output.
The flag-only trial used the fallback and exited 0 as guest/native PID 17557; its new backup and staging files were removed.
This does not identify the exact failing policy. [Public XNU](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/vfs/vfs_syscalls.c)
performs sandbox and source metadata checks before applying
ownership options, and already omits ownership copying for non-root callers.

Cloning the original shared-container file instead succeeded. Guest/native PID 17589 ran the repaired df clone and exited 0;
the host reported `original clone + signature splice`, with no full-file data-copy fallback. The fetched output passed
`codesign --verify`; every byte outside the original signature allocation was unchanged, and the backup directory matched
its pre-test contents. No debugger was attached. Existing historical backups were left untouched.

The final build also repaired a fresh invalid dylib at first `dlopen` using the original clone path. Guest/native PID 17610
resolved the exported function, returned its expected value 42 and exited 0. Its signature and unchanged bytes were verified,
and no new backup or staging files remained. Host UUID was `295A807F-E359-3372-9155-87BBAC3696A9`; LiveProcess and NativeSigner
UUIDs remained those of the graphical check above. The host stage temporarily permits writing and restores the original mode
before registration; focused tests cover original modes 0444 and 0555. The writable FD is closed before AMFI registration.
The final graphical session (native host 17621) returned to SpringBoard with icons and the Calendar widget; screenshot
`graphical-final.png` and `device17-final-state.json` record the restored session and absence of debugger processes.

The host now removes a transaction's original backup and plist after publication and final identity verification. Failure
after replacement retains the backup; cleanup failures are logged without turning a successfully published signature into
a failure. Focused tests cover both cleanup failures, queued requests, preserved old descriptors and bounded signature writes.
Ignored evidence is under `tmp/lazy-sign-cow-20261101/`, including `console-noowner.log`, `console-cow.log`,
`noowner-verification.log`, `cow-verification.log`, `console-dlopen-final.log`, `dylib-verification.log` and `security.log`.

## Signature-data XPC reply, 2026-11-01

The native service now calls `LSBCreateRuntimeSignature` with the read-only original FD and returns its unpadded superblob
as protocol-v2 XPC data. One private Security scratch file remains; the service's intermediate working/output file and output-FD
reply are removed. The core checks the original generation after signing and preserves its bytes, permissions and seek position.
The host rejects old output-FD replies and malformed signature structures, then writes only signature data and zero-padding
into its original-file clone. This removes the former whole-file output comparison: the service can no longer supply alternate
code bytes, and writes remain confined to the requested allocation. Real kernel registration still precedes publication.

On iPhone18,2 / native iOS 27.0 build 24A435, using the unchanged 23A343 runtime and cache UUID
`73ED978A-FD25-3166-A57A-34E21BB7E5B8` at `0x110000000`, a fresh invalid-CMS df clone was repaired and exited 0 as guest/native
PID 17836. A fresh invalid-CMS dylib repaired at first `dlopen` returned 42 and exited 0 as PID 17850. Both used CoW cloning,
passed fetched-output `codesign --verify`, preserved every byte outside their signature allocation, and left no new backup
or host staging file. No debugger was attached. The simulator was off before testing and the completed test session was stopped.

The installed host UUID is `EB205EEC-6265-3E95-950E-74E9C832FBCD`; NativeSigner is `1BE1018A-B225-3609-A9C9-E04AABE2DA51`;
LiveProcess remains `5D9814B5-0F12-3321-B98E-88F1D84EE9E7`. The signed build and integrated local suite passed, including
malformed replies, signature bounds/layout, source changes, zero-padding, read-only modes and cleanup. Disposable-identity
tests also verified real in-memory CMS generation, one-scratch cleanup and unchanged original bytes/mode/offset.
Ignored evidence is under `tmp/lazy-sign-xpc-20261101/`: `console-executable.log`, `console-dlopen.log`,
`executable-verification.log`, `dylib-verification.log`, `security.log`, `tests-final.log` and `device17-final-state.json`.
