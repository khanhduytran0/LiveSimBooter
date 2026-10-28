# Runtime handoff design

The native host runs UIKit and the process broker. Each guest starts separately in [LiveProcess/main.c](../LiveProcess/main.c),
receives its launch plan, locates `RuntimeRoot` in the App Group, maps `usr/lib/dyld_sim` and the requested executable, and
enters the simulator runtime. LiveProcess uses C for its native startup and releases native XPC objects before handoff.
The original starter delegates, controller, and storyboards remain in `unused/`; the new host UI lives in `Host/`.

The native host caches MobileGestalt's `EffectiveProductionStatusAp` before launching guests. Only an exact Boolean false
permits native-cache detachment; missing, malformed, or true results keep the native cache and hardware workqueue bridge.
This is a device production-status check, not an iOS-version or Developer Mode check. The development iPhone used for detach
tests has custom boot arguments and relaxed TXM enforcement; those results do not establish support on ordinary iOS 26 phones.

The authenticated XPC check-in reply carries that status separately from the guest's environment. LiveProcess validates the
policy before switching TLS: non-production defaults to detach/software bridging, while production/unknown defaults to
retain/hardware bridging and rejects an explicit detach request. Explicit retention remains available on development devices.
No MobileGestalt object or native function pointer crosses the handoff. See
[retained-cache workqueue redirection](launchd.md#retained-native-cache-with-hardware-redirection) for its device evidence.

A normal icon launch waits for **Start** on the IOSurface tab before starting the broker and graphical runtime. Explicit guest
arguments, including `--launchd-services`, `/bin/df`, and `--terminal`, retain automatic startup for development tests. Startup is
once-only; the IOSurface, Fish, and Settings tabs persist after it, and changing tabs does not start another guest.

## Preparing the handoff

[DyldSimLoader.cpp](../LiveSimBooter/DyldSimLoader.cpp) validates the Mach-O role, registers the embedded signature through
`F_ADDFILESIGS_RETURN`, reserves the address range, and maps segments and zero-fill. It accepts thin arm64 images or an arm64
universal slice. The executable's `__PAGEZERO` is excluded from its reservation. The mapped signed headers are preserved.

The loader builds persistent kernel-style arguments for the requested program, including its actual path in `argv[0]` and `executable_path`.
It sets `DYLD_ROOT_PATH` and `DYLD_SHARED_CACHE_DIR` to RuntimeRoot paths, removes stale host executable metadata, and replaces
inherited `ptr_munge` values with a fresh nonzero random token. The apple vector is writable because simulator libpthread
consumes and clears the token. [Dyld diagnostics](debugging.md#simulator-dyld-diagnostics) are disabled by default; explicit
diagnostic variables and output destinations are copied unchanged into the simulator environment. Native launch settings are untouched.

Successful handoff messages are also quiet by default. Set `LSB_TRACE_HANDOFF=1` to print the LiveProcess entry message, main-stack
relocation/preservation details, and the retaining/detaching-cache banner. Only the first exact value `1` enables this trace;
launchd preserves the normalized setting in its own configuration and child environment. The loader captures it before changing
TLS or crossing the cache handoff. Preparation failures and an explicitly requested LLDB-stop message remain visible.

[MainThreadBootstrap.cpp](../LiveSimBooter/MainThreadBootstrap.cpp) supplies an aligned, zeroed 16 KiB app-owned block for
the main pthread and TSD. It initializes only self, errno, and the kernel thread ID after validating the host layout.
It does not copy host TLS keys, cleanup callbacks, or thread-list pointers. The selected support libpthread needs a
224-byte prefix and 768 TSD slots, larger than the host object; simulator pthread completes initialization.

[HostPreparation.cpp](../LiveSimBooter/HostPreparation.cpp) snapshots the host AMFI policy for the pinned simulator's flags
before handoff. The callback returns the saved result only for the same flags and returns `ENOSYS` otherwise.
It fabricates no policy permissions and retains no host function pointer.

## The one-way transition

[SharedCacheHandoff.cpp](../LiveSimBooter/SharedCacheHandoff.cpp) queries the shared region, prepares the debugger transition,
switches the thread's TSD base with Darwin's platform syscall, and verifies it. In the diagnostic detach mode it performs
`__shared_region_check_np(NULL)` from app-resident assembly, without returning into an unmapped libSystem syscall stub.
Before successful detachment, failures restore host TSD and debugger state for host-side error reporting.

In either mode, all 67 helper callbacks execute from the app. [LocalPOSIX.cpp](../LiveSimBooter/LocalPOSIX.cpp) uses BSD
syscalls, [LocalMach.cpp](../LiveSimBooter/LocalMach.cpp) uses Mach traps and local message clients, and
[LocalRuntime.cpp](../LiveSimBooter/LocalRuntime.cpp) supplies memory/string operations, errno, recursive lock bridging,
and stderr formatting. These paths are built without builtins, stack protectors, exceptions, or RTTI.

The version 18 [helper ABI](../LiveSimBooter/SimSyscallHelpers.h) and argument block are passed to dyld_sim's prepare entry.
For the supported `LC_MAIN` path, its returned `start_sim` wrapper receives the same argc/argv/environment/apple vectors,
calls the guest program, and exits through the simulator runtime. A raw exit handles an unexpected wrapper return without host cleanup.
A null prepare result exits `1`; unsupported startup glue exits `126`; failed restoration of host TSD exits `127`.
Existing host frames remain below the simulator frames. The implementation does not reset the stack or remove those frames.

Only the current main thread's storage is replaced during this transition. Existing host threads are not migrated;
the native host's UIKit and broker workers remain in the separate host process. The local callback bridge implements
the pinned dyld's usage, not a complete replacement for host libc or pthread.

## New guest pthreads

Cache detachment does not replace XNU's recorded pthread entry point. The simulator's second `__bsdthread_register` fails
with `EINVAL`, leaving the native startup PC registered. An interactive Fish worker consequently started in the unmapped
host cache even though the main guest thread was running correctly.

[Guest/ThreadBootstrap.c](../Guest/ThreadBootstrap.c) interposes the simulator's registration and creation syscall wrappers.
It preserves the registration result. For the pinned layout (16 KiB pthread storage, TSD offset `0xe0`, Mach-self slot offset
`0x18`), a rejected repeat registration records the simulator startup PC for subsequent explicit pthread creation. A
successful native registration keeps the ordinary creation path. Unsupported layouts and creation modes fail explicitly.

The fallback asks the real `__bsdthread_create` to create the custom-stack thread suspended. XNU still initializes its
pthread port, TSD, QoS, argument registers, and stack. The shim fetches and validates that never-run thread's state, changes
only its PC to the simulator's pthread entry, signs the replacement PC when required by the returned arm64 thread-state
flags, and resumes it unless the caller requested suspension. It retains the other signed register fields. A failure
terminates the suspended thread before returning an error; unrecoverable cleanup exits `126` so libpthread cannot free
storage still referenced by a live kernel thread.

This supports explicit pthread creation with the selected simulator libpthread's custom-stack ABI. Kernel workqueue
registration remains native; the current default redirects workqueue entry using the retained-cache bridge linked above.
The shim is loaded in the simulator after the native single-thread guard; it does not introduce a native worker during handoff.

The host tests cover registration validation, state preservation, PC signing, requested suspension, failures, and cleanup,
including ASan/UBSan runs. An LLDB experiment on the phone redirected a suspended Fish worker and reached another worker
creation. The integrated interposer then enabled interactive Fish to reach its prompt and run `df` successfully. Hardware
breakpoints caught `LSBThreadCreate` and a new worker at simulator `_pthread_start` (`0x12b26e618`), with the host region
still inaccessible and the low simulator cache executable with zero reported dirty pages. Interactive child statuses,
Ctrl-C, editing, and clean Ctrl-D exit are recorded with the [terminal validation](fake-pty.md#device-checks-and-diagnostics).

## Debugger publication

[DebuggerBridge.cpp](../LiveSimBooter/DebuggerBridge.cpp) hands LLDB from native dyld notification to an app-resident notifier
and publishes simulator state through `TASK_DYLD_INFO`. The kernel allows only one metadata-pointer relocation, which native
startup may already have used. In the detach path, the bridge maps fresh anonymous metadata pages at the registered address
after detaching the cache. Native cache code then remains inaccessible. The retained-cache path preserves native mappings;
see [debugging](debugging.md) for the current publication and attachment behavior.

[CompactImageInfo.cpp](../LiveSimBooter/CompactImageInfo.cpp) publishes AppleArchive image snapshots before notifications,
including paths, UUIDs, addresses, segments, process state, and CRC. Recent debugserver versions need this compact snapshot
to name simulator-loaded libraries. Cached images use their actual segment coordinates and permit 4 KiB image-header alignment.
The app remains in the list for source breakpoints. Publication uses bounded app-owned storage for 2048 images and two buffers.
Asynchronous external process monitors, cache Atlas APIs, and the cache image bitmap are not implemented.

## Local verification

```sh
sh Tests/run.sh
python3 Tests/audit-local-runtime.py

xcodebuild -project LiveSimBooter.xcodeproj -scheme LiveSimBooter -configuration Debug -sdk iphoneos \
  -destination 'generic/platform=iOS' -derivedDataPath /tmp/LiveSimBooter-build CODE_SIGNING_ALLOWED=NO build
```

The host tests use synthetic images and substitute dangerous handoff operations. They do not detach the Mac test process's
cache or establish device signing acceptance. The import audit checks the local callback/handoff code at `-O0` and `-O2`;
it can also inspect an actual Xcode arm64 object directory. Device behavior requires the separate [debugging workflow](debugging.md).
Focused local tests cover the icon/explicit-argument startup decision and default-off handoff tracing, including launchd
configuration regeneration and duplicate or malformed environment values. The iPhone 17 cold-launch screenshot showed Start
with no guests, and two unattended reboots returned to SpringBoard with no handoff banners; see [device verification](guest-control.md#device-verification-2026-10-27).

The loader tests also exercise short and large Fish-shaped startup vectors in isolated host test processes. They verify
the argc/header prefix, contiguous argv/environment/apple layout, exact counts and null terminators, root overrides,
one final fresh writable pointer token, and independent copies of every input string. The native preparation mocks alter
the original strings and churn allocations before entry. This guards the handoff layout but does not reproduce the one
observed on-device startup failure in which libpthread reported a zero pointer token.

The implementation follows the pinned Apple dyld source described in [the architecture references](architecture.md#debugging-and-implementation-references). Apple-derived files
retain their APSL notices; the project's separate original code uses MIT.
