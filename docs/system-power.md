# Virtual system power

The earlier backboardd experiment reached a deliberate SystemWake assertion because `IORegisterForSystemPower` returned
`IO_OBJECT_NULL`. This was a separate issue from simulator launchd ownership, HID registration, or framebuffer transport.
`Guest/libLiveSimSpawn/SystemPowerCompatibility.c` implements an opt-in C adapter for an always-awake guest. Local lifecycle and native
interposition tests pass. The phone trial with backboardd PID 81599 passed the previous SystemWake assertion without its
debugger bypass, created a CAWindowServer swapchain, and entered rendering before a separate Metal shader lookup abort.
Device-side provider teardown has not been verified. The LLDB bypass below is historical diagnostic evidence only.

## Observed failure

The pinned iOS 26 (23A343) SystemWake binary calls `IORegisterForSystemPower` from
`-[SWSystemSleepMonitorProvider registerForSystemPowerOnQueue:withDelegate:]`. The provider has already stored its queue
and weak delegate, marked itself registered, and acquired its unfair lock when registration fails.

| SystemWake image offset | Operation |
| --- | --- |
| `0x4588` | Call `IORegisterForSystemPower(weakDelegate, &notificationPort, SWSystemPowerCallback, &notifier)` |
| `0x458c` | Store the returned connection at `self + 0x20` |
| `0x4590` | Branch to the failure path if that connection is zero |
| `0x486c` | Explicit `brk #0`, reporting `_lock_systemPowerConnection != IO_OBJECT_NULL`, source line 567 |

The device's SystemWake base was `0x13c840000`, so the observed breakpoint was `0x13c84486c`, method offset `+904`.
The separate duplicate-registration assertion is at image offset `0x4768`; it is not the failure observed here.
Evidence is in `tmp/framebuffer-server/lldb-initial.txt`.

The matching IOKit implementation has three relevant failure points:

| IOKit image offset after call | Operation and result to inspect |
| --- | --- |
| `0xb2e0` | `IORegistryEntryFromPath(0, "IOPower:/IOPowerConnection/IOPMrootDomain")`; root service in `w0` |
| `0xb300` | `IOServiceOpen(rootService, mach_task_self(), 0, &connection)`; kernel result in `w0`, connection at `sp + 0xc` |
| `0xb33c` | `IOServiceAddInterestNotification(..., "IOAppPowerStateInterest", ...)`; kernel result in `w0` |

Failure in any of these paths destroys the temporary notification port, closes an opened connection, releases any notifier,
and returns zero. The caller's notification-port output is written only on success. A zero registration result alone does
not establish whether the root service was hidden, opening it was denied, or interest registration failed.

The final hardware-breakpoint trial with native backboardd PID 81249 established the failing call. At cached IOKit
`0x12701a2e0`, root-domain lookup returned the valid service name `0x631b`. At `0x12701a300`, immediately after
`IOServiceOpen`, `w0` was `0xe00002e2` (`kIOReturnNotPermitted`) and the connection output at `sp + 12` was zero.
The system-power user-client open is denied; the specific entitlement or policy responsible was not established.
Evidence: `tmp/framebuffer-server/lldb-final.txt`.

Sim26Booter's `PATCHING.md` records the same SystemWake method failure in backboardd and SpringBoard on 2025-09-27, attributing
it to an unidentified entitlement. That is useful corroboration, not proof of the exact failing call or entitlement here.

## Why the assertion bypass is temporary

At this particular assertion, continuing at SystemWake image offset `0x45b8` follows the existing object-release, unlock,
stack-check and return sequence. The device test did this at `0x13c8445b8` and reached backboardd's main run loop. A direct
return from the assertion would skip that cleanup and leave the unfair lock held.

This does not create a valid registered provider. In particular, `-[SWSystemSleepMonitorProvider invalidate]` later calls
`IODeregisterForSystemPower`, `IONotificationPortDestroy`, and `IOServiceClose` unconditionally. Its notification-port field
remains null after the failed registration. The pinned `IONotificationPortDestroy` immediately reads `port + 8` at IOKit
image offset `0x750c`; it does not accept null. Invalidation after the bypass can therefore crash.

Setting the failed connection result to a fabricated nonzero integer is also insufficient: the normal registration path
immediately passes the still-null notification-port field to `IONotificationPortSetDispatchQueue`.

The LLDB continuation is useful for observing the next unrelated blocker. It is not a permanent binary patch, evidence of
working sleep/wake monitoring, or a substitute for cleanup testing. Addresses above apply only to the verified cache build.

## C adapter: an explicitly awake virtual machine

For the present simulator experiment, the narrow useful model is an always-awake guest. Registering a monitor creates
a guest-local subscription that never receives a host sleep notification. It does not register with the phone's power
root domain nor acknowledge, delay, cancel, or otherwise control the phone's sleep. This preserves the existing pure-C guest
shim boundary and requires no Objective-C method replacement.

The exact environment value `LSB_SIMULATE_SYSTEM_POWER=1` enables registration before SystemWake initializes. All other values
forward registration unchanged. The backboardd launch configuration supplies this value in the launch plan and both the
bootstrap `Self` and `Child` environments, overriding conflicting inherited copies. Empty, df, and lifecycle configurations
do not add it. Cleanup recognizes existing subscriptions even if the environment changes afterward.
The adapter covers the complete local subscription lifetime rather than changing global IOKit error results:

1. `IORegisterForSystemPower` allocates a real `IONotificationPortRef` using `IONotificationPortCreate`, plus local opaque
   connection/notifier handles recorded in a 64-entry table. The real notification port lets existing queue/run-loop setup
   and `IONotificationPortDestroy` retain their normal implementation. Its receive right receives no power events.
2. The opaque handles are actual reserved Mach names, allocated as owned dead-name rights, rather than magic integers that
   could alias unrelated IOKit objects. They identify only virtual subscriptions; they are never sent to an IOKit kernel MIG
   routine. The table records the separate connection and notifier lifetimes and publishes outputs only after full success.
3. A scoped `IODeregisterForSystemPower` recognizes its virtual notifier, releases that owned handle exactly once, and zeros
   the caller's notifier field. Unknown notifiers go to the original implementation. Deregistration does not destroy the
   separately caller-owned notification port or close the connection.
4. A scoped `IOServiceClose` recognizes only its virtual connection and releases that handle. Unknown connections continue
   to the original kernel operation. Closing and deregistering must work in either order without double-freeing the table
   entry. The entry is reusable only when both owned handles have been released. A failed right release preserves ownership
   for retry. Passing a known connection as a notifier, or a known notifier to close, returns `kIOReturnBadArgument`.
5. `IOAllowPowerChange` and `IOCancelPowerChange` on a known virtual connection report an unsupported operation, since this
   initial model never issues a sleep notification or an acknowledgement ID. Unknown connections retain native behavior.
   The adapter must not report that a real kernel sleep transaction was acknowledged.

The callback and `refcon` are borrowed inputs. With no queued notifications, the adapter does not retain, invoke, or inspect
either. The caller owns the returned notification port and destroys it using normal IOKit semantics. A failure before
publication cleans up every temporary port/right and returns zero with null outputs. Capacity exhaustion fails in the
same way, without falling back to host power registration. The adapter uses a C mutex and reserves a slot before allocating;
it does not hold that mutex across notification-port initialization. It adds no worker or Objective-C object beyond the
queue setup the caller already performs. Native notification-port destruction handles asynchronous dispatch-source cleanup.

Five functions are interposed: `IORegisterForSystemPower`, `IODeregisterForSystemPower`, `IOServiceClose`, `IOAllowPowerChange`,
and `IOCancelPowerChange`. Normal notification-port functions stay unchanged. `Tools/build-spawn-shim.sh` includes the adapter
and links IOKit; the power functions absent from the simulator SDK headers have normal C declarations in the adapter header.

The pinned SystemWake provider calls `IONotificationPortSetDispatchQueue` at image offset `0x459c`. Its invalidate path calls
deregister at `0x4cb8`, notification-port destroy at `0x4cc0`, and connection close at `0x4ccc`. It does not directly retain or
release either virtual handle through `IOObjectRetain`/`IOObjectRelease`. Before using this adapter generally, check all
additional users of the returned IOKit handles: direct `IOObjectRetain`/`IOObjectRelease` or arbitrary `IOConnectCall*`
operations require either explicitly
supported virtual ownership behavior or a documented rejection. The first target is the verified SystemWake registration,
queue setup and invalidation sequence. It does not implement power assertions, host suspension handling, guest sleep/wake
transitions, or powerd services. The host app can still be suspended normally. As with native IOKit handles, using a copied
handle after its ownership was released is unsupported; the adapter does not retain unbounded stale-handle tombstones.

## Why the normal activity completion should run

The pinned simulator SystemWake already makes its activity provider local. At image offset `0x8834`,
`-[SWSystemActivityProvider declareSystemActivityWithName:]` increments a local assertion-ID counter and returns success with
system state zero. `releaseAssertionID:` at `0x8878` is a no-op. The sleep monitor starts at state zero; its predicate methods
at `0x1424`, `0x1460`, and `0x149c` classify that as awake, with no requested or imminent sleep.

With those values, the normal activity-acquisition worker records its assertion ID, marks the assertion valid, and calls
`callbackAcquisitionHandlerWithError:nil` at `0x695c`. No synthetic wake notification is needed. This supports the expectation
that a valid registration lets backboardd's boot activity completion continue normally. It is not device proof of a rendered
boot logo: DataMigration waits, Metal initialization, and framebuffer presentation remain separate requirements.
The local disassembly used for this analysis is `tmp/backboardd/systemwake-power-disassembly.txt`.

## Validation and remaining phone check

`Tests/SystemPowerCompatibilityTests.c` passes on the Mac normally, with ASan/UBSan, and with TSan. It uses real Mach rights
and real IOKit notification ports while substituting only the original power calls and allocation-failure hooks. Coverage
includes exact opt-in gating and native forwarding, null outputs, allocation rollback at each stage, failed-release retry,
capacity exhaustion, independent and concurrent teardown, 400 concurrent registration cycles, run-loop source validity,
dispatch-source cancellation, and absence of callbacks. No test calls the native host power-registration function.

A second build of the same test with `LSB_SYSTEM_POWER_INTERPOSE_TESTING` loads the standalone adapter through
`DYLD_INSERT_LIBRARIES`. Calls through all five actual IOKit symbols reach the adapter and complete a real notification-port
lifecycle. Both power tests are included in `Tests/run.sh`. `LaunchdConfigurationTests` verifies the backboardd opt-in in all
three environment locations, conflicting duplicate removal, preservation of a similarly prefixed unrelated key, and absence
from the default empty, df, and lifecycle configurations.

The isolated adapter links successfully for arm64 iOS Simulator; its header is C++ compatible. The complete spawn shim and
spawn probe also build successfully with `Tools/build-spawn-shim.sh` into a fresh `tmp/system-power/full-shim.*` directory.
The build has not been identity-signed or deployed, and no full host-app build was part of these checks.

The subsequent phone trial used the rebuilt power shim with stock backboardd PID **81599**. It passed the earlier SystemWake
assertion through the normal path and created CAWindowServer and its swapchain. The render thread then aborted in
`CA::OGL::MetalContext::create_fragment_shader` with `Metal failed to specialize fragment function`; the payload says
`Function fixed_frag_lpf_cpf was not found in the library`. The stack includes solid-background rendering and display update,
so this is later than the prior power-registration blocker. Evidence is `tmp/framebuffer-server/lldb-power-gpu.txt`.

This trial did not verify provider invalidation or teardown on the phone, nor a submitted or displayed frame. Later compiler
and matching QuartzCore resource fixes let stock backboardd **82143** present a real spinner frame to the host image view;
see [the first stock frame](backboardd.md#first-stock-backboardd-frame). Device provider teardown remains unverified. The
resource inventory is `tmp/metal-iosurface/quartzcore-resources.json`; the matching stock shader library contains the required function.
