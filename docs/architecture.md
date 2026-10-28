# Architecture and compatibility

LiveSimBooter runs arm64 iOS Simulator programs as separate native iPhone processes. It shares the device's kernel;
it does not emulate a CPU or boot a separate kernel. The supported guest baseline is **iOS Simulator 26.0, build 23A343**.
Runtime patches and private ABIs are pinned to that build. Start with [setup](agent-setup.md) for inputs, provisioning,
signing and installation, and [the documentation index](README.md) for current results and unresolved issues.

## Process boundaries

| Component | Responsibility |
| --- | --- |
| [Host](../Host/) | Native UIKit tabs, extension launch broker, process ownership, IOSurface/framebuffer and Indigo services. |
| [LiveProcess](../LiveProcess/main.c) | Pure-C extension entry: receive a plan, restore descriptors/state, prepare a simulator or selected native role. |
| [Loader](../LiveSimBooter/) | Signed Mach-O mapping, startup vectors, pthread/TSD bootstrap, dyld callbacks and debugger publication. |
| [Guest shim](../Guest/) | Simulator-side process, identity, path and framework compatibility adapters. |
| [ProcessSupport](../ProcessSupport/) | Shared protocols, validation, descriptor actions, child-status tables and terminal state. |
| [NativeMetal](../NativeMetal/) | Separately hosted native GPU service using the prepared simulator serializer/backend libraries. |

The native host retains its own UIKit and shared cache. Guest apps use the simulator's UIKit/frameworks in their own
LiveProcess instances. LiveProcess's C entry has no Objective-C imports or extension context; a host-selected native Metal
role may subsequently load its Objective-C backend. Private APIs are declared and called through typed C/Objective-C APIs.

The broker discovers the extension through `NSExtension` and launches instances through PlugInKit. A synchronous XPC pipe
delivers owned arguments, environment, file descriptors and supported process attributes. No `NSExtensionItem` carries the
plan. The broker matches the native extension PID and full kernel audit identity before releasing it; guest-supplied PIDs
cannot claim another job. The child retains a broker Mach send right, releases native XPC objects and checks its thread count
before handoff. READY acknowledges preparation, not successful guest execution. See [multiprocess](multiprocess.md).

## Runtime storage and loading

The selected SideStore or AltStore App Group contains `LSB/iOS/23A343/RuntimeRoot` and `LSB/iOS/23A343/data`.
RuntimeRoot holds supplied code and resources; data holds mutable guest state. Both native targets require matching
store-group provisioning. A shared C helper reads actual signed group entitlements before guest handoff, including a suffix
changed by store re-signing. The host prefers an accessible group containing `LSB`, otherwise one containing `Apps`,
with SideStore before AltStore for ties or an empty setup. All native consumers share that process-lifetime selection.
Only the pinned 23A343 runtime is selected; there is no runtime-version picker.

The broker resolves the actual container and canonicalizes the runtime path; the child receives it in the authenticated launch plan.
Its native service name is `<selected actual store group>.LiveSimBooter.spawn`. There is no automatic storage migration or merge.
Restore the two roots manually with the host and guests stopped; see [migration](migration.md). Keeping another entitled owner
of the selected group installed can preserve shared storage when this app is removed, but an independent backup is still needed.

The loader maps `RuntimeRoot/usr/lib/dyld_sim` alongside the selected executable. It accepts thin arm64 images and arm64
slices of universal files, validates segments and zero-fill, registers embedded signatures using `F_ADDFILESIGS_RETURN`,
and reserves space without overwriting resident images. It excludes executable `__PAGEZERO` from the allocation.

| Launch role | Accepted file type | Additional boundary |
| --- | --- | --- |
| dyld_sim | `MH_DYLINKER`, or experimental `MH_DYLIB` variant | The signed header is preserved in memory. |
| Main program | `MH_EXECUTE`, or experimental `MH_DYLIB` variant | Plain arm64, PIE, `LC_MAIN`, valid signature and segment layout. |

Acceptance by this loader does not establish signature acceptance or dyld_sim compatibility. A header-only conversion of
a program to `MH_DYLIB` lacks the `LC_ID_DYLIB` that dyld_sim expects for that main-image type. Any metadata modification
must precede signing. Use the [tracked executable signer](../Tools/sign-sim-executable.py), not an assumed interchangeable
dylib-signing recipe.

Programs need exactly one supported platform declaration: iOS Simulator, iOS, or legacy `LC_VERSION_MIN_IPHONEOS`.
The pinned [iOS compatibility patch](ios-platform.md) allows compatible iOS programs/dependencies without rewriting their
platform commands. It does not supply missing APIs, decrypt executables, or support arm64e, macOS or Mac Catalyst programs.

The cache is rebuilt from matching original dylibs at the project's selected low address; headers alone cannot relocate
an existing simulator cache. The image allocator avoids the cache's reservation. Signed-cache preparation and address-space
constraints are separate from the presence of framework resources; both are needed. See [shared-cache design](shared-cache.md)
and [runtime staging](agent-setup.md). Apple runtime binaries and generated signing/cache artifacts remain outside Git.

## Handoff and threads

The loader supplies persistent kernel-style argv/environment/apple vectors and the pinned version-18 dyld helper table.
It sets the runtime roots, removes stale native executable metadata, and supplies a fresh writable `ptr_munge` token.
App-owned main-pthread/TSD storage is initialized for the simulator rather than copying native TLS keys and thread lists.
dyld_sim prepares dependencies and returns `start_sim`, which runs the guest's `LC_MAIN` path. Successful handoff never
returns through native cleanup; unexpected wrapper return uses a raw exit. See [handoff details](runtime-handoff.md).

**Production/unknown devices retain the native cache.** Native pthread registration cannot simply be replaced by the simulator's
second registration. Explicit guest pthread creation redirects a never-run suspended thread; workqueue entry uses an inherited
hardware execution breakpoint to reach the simulator entry. The breakpoint remains installed for new and reused workers.
Only a cached native non-production MobileGestalt result allows detachment; guest environment values cannot grant it.
The [handoff policy](runtime-handoff.md) describes the authenticated check-in metadata. The current behavior and evidence are in
[retained-cache workqueue redirection](launchd.md#retained-native-cache-with-hardware-redirection); older detach experiments
in the handoff notes are not evidence of production-device support.

The dyld helper callbacks use app-local BSD syscalls, Mach traps, memory/string operations and synchronization support.
They do not call native libc/pthread against simulator TLS. The bridge targets dyld's needs rather than general host libc:
its fixed storage includes 4,096 lifetime errno-thread slots and 512 recursive mutex identities. Host AMFI policy is captured
before handoff for the pinned query; unsupported query flags return `ENOSYS`. Signing and kernel restrictions still apply.

## Processes, graphics and controls

Simulator `posix_spawn`/`posix_spawnp` requests return to the broker. Supported attributes and ordered descriptor actions are
replayed in a new LiveProcess. `POSIX_SPAWN_SETEXEC` prepares a replacement native process while keeping a stable guest PID
for parent wait/signal and launchd events. Shared child-status tables report actual terminal exits and support SIGCHLD.
Signals target direct simulated children, with positive self signaling handled natively. Cross-process signaling uses
synchronous IPC and is not async-signal-safe. Generic fork/execve, stopped/continued wait events and full job control are
not implemented. See [process semantics](multiprocess.md) and [launchd identity](launchd.md).

Graphical mode starts stock simulator launchd, selected matching jobs and stock xpcproxy_sim. The host registers the native
services into that isolated bootstrap namespace. Metal runs in separate native helpers: public, backboardd, and optionally
SpringBoard. Dedicated routes validate the owner's full audit token; the public Metal and IOSurface endpoints accept clients
holding those endpoints. Services, authorization and helpers belong to their simulator session. See [Metal](metal.md).

Apple's native IOSurfaceRemoteServer lives in the host. Framebuffer updates accumulate in a private GPU texture and copy
into the native display's drawable pool; source ownership lasts through GPU reads. Hidden tabs retain the latest frame
without requesting drawables. Touches and hardware keyboard input come only from the visible simulator display view and
are delivered through Indigo. See [framebuffer](framebuffer.md), [IOSurface](iosurface.md) and [input](backboardd.md).

Icon launches wait for **Start** on IOSurface; explicit guest arguments preserve automatic test launches. The persistent
Fish tab attaches to launchd's fixed terminal-fish job, which replaces itself with Fish. Its socket-backed terminal supports
selected termios/ioctl behavior, not a native PTY. Settings uses a fixed guest worker for HUD/touch preferences; background
audio is an explicit opt-in. See [terminal](fake-pty.md), [display settings](display-settings.md) and [guest control](guest-control.md).
An accepted guest userspace reboot drains the old processes and creates new services in the same host while preserving
the data root and tabs; an ordinary exit or crash does not request a restart.

## Debugging and implementation references

The debugger bridge publishes simulator images, compact AppleArchive snapshots and an app-resident notification function.
It adapts the snapshot format to native debugserver, retains source visibility for the loader and uses bounded storage.
This is not a full implementation of dyld's asynchronous monitor/cache Atlas APIs. Use [the debugging guide](debugging.md)
for hardware-only breakpoint requirements and detach caveats. `LSB_TRACE_HANDOFF=1` enables entry/stack/cache success logs;
failures stay visible by default. Local tests cannot establish device signing acceptance or complete simulator behavior.

The original mapping flow follows Apple's pinned [prepareSim](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/dyldMain.cpp#L300-L469).
The optional detach path follows [deallocateExistingSharedCache](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/SharedCacheRuntime.cpp#L1230-L1243).
Apple's macOS path [keeps bootstrap dyld outside the cache](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/dyldMain.cpp#L1191-L1195)
and simulator libpthread [adopts the main pthread](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/src/pthread.c#L2086-L2096).
The separate app startup and storage lifetimes here require the explicit thread/bootstrap adapters above.
