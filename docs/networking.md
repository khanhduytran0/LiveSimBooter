# Simulator networking

The guest uses the native kernel's sockets and the existing configd bridge. Working DNS or a TCP connection does not
establish that Network.framework's path evaluator accepts the route. The 23A343 simulator evaluator uses a routing
socket for this check; its ordinary path evaluation does not follow the native NECP client path.

## Routing reply PID compatibility

`Guest/ProcessIdentity.c` supplies the native PID to one pinned `getpid()` call inside Network.framework's
`nw_route_get_ifindex`. That function sends `RTM_GET_SILENT` and compares the reply's PID and sequence with its request.
XNU replaces the request PID with the actual native sender PID and changes the reply type to `RTM_GET`. Consequently,
a logical guest PID differing from the current native process makes the stock simulator code discard an otherwise valid
reply. Its next nonblocking read fails, leaving the route's interface index unset. See
[XNU's routing-socket implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/net/rtsock.c#L359-L379).

The adapter applies only to the exact install path or its configured physical RuntimeRoot path, the original plain-arm64
Network image UUID `37E59D23-280C-366B-A7AD-3C26D894508E`, and return offset `0xac7984`. The validator checks executable
section bounds and the unchanged getpid call, saved PID, and reply type/PID/sequence comparisons. It excludes relocated
ADRP/GOT operands. All twelve pinned instructions match both the original 23A343 image and the rebuilt cache based at
`0x110000000`, UUID `73ED978A-FD25-3166-A57A-34E21BB7E5B8`; Network's preferred address there is `0x11fcc1000`.
Other Network callers keep logical identity, and image removal or failed revalidation clears the exception. Routing
permissions, reply contents, interface state and connectivity decisions remain stock.

## Diagnostic evidence, 2026-10-28

On development iPhone16,2 running native iOS 26.1 / 23B50 with the arm64 iOS Simulator 26.0 / 23A343 guest and the cache
above, the initial diagnostic session used native host PID 6414. There was no LLDB attachment for the network probes.
Before the identity correction:

- `getaddrinfo` and DNS-SD resolved `example.com`; a numeric TCP connection to `1.1.1.1:80` succeeded.
- HTTP and HTTPS NSURLSession requests failed immediately with `NSURLErrorDomain -1009`. NWPath was unsatisfied with
  IPv4/IPv6/DNS false, and SCNetworkReachability reported flags zero.
- The guest NWI state was present, with IPv4 reachability `0x2` and `en0` flags `0x5` / reachability `0x2`. The existing
  configd mirror therefore had usable network information.
- The bounded routing probe ran as logical PID 6546 / native PID 6547. Both logical- and native-PID requests succeeded
  without socket, ioctl, write or read errors. Both replies contained native PID 6547, sequence matched, route error zero,
  and interface index 20 (`en0`). The logical comparison failed despite the valid route.

Focused ProcessIdentity tests cover the exact callsite, adjacent/unrelated callers, aliases, UUID/instruction rejection,
section bounds, slides and unload cleanup. Strict arm64 simulator compilation passes. After deploying the adapter, NWPath
became satisfied and an HTTP request to `example.com` returned status 200 with 577 bytes in about 270 ms. HTTPS exposed a separate library-alias problem below; that HTTP result alone does not establish
working TLS or Safari.

Ignored evidence is under `tmp/safari-network-20261028/`: `probe-baseline.log`, `store-results.log`, `route-results.log`,
and `necp-analysis/` containing the original-image disassembly, stock/cache instruction comparison and unsigned probe source.
These artifacts are historical evidence, not setup dependencies.

## Missing Network library alias

The stock 23A343 `/usr/lib/libnetwork.dylib` is a symlink to
`/System/Library/Frameworks/Network.framework/Network`. The cache input builder stages canonical images without this
alias, while the runtime stager omits links to cache-only code. Security dynamically loads the alias and resolves
`nw_protocol_options_access_handle` and `nw_protocol_metadata_access_handle`. A missing alias therefore affects TLS
configuration and metadata even though the canonical Network framework is already loaded and HTTP works.

A bounded no-debugger probe, logical PID 6801 / native PID 6802, confirmed alias `dlopen` failed with “no such file, not
in dyld cache”. Loading the canonical path succeeded, and both accessors resolved in the guest Network image at
`0x11fcc1000`. Before correcting this lookup, the explicit TLS verification callback was never invoked and CFNetwork
aborted at `_tlsProcessSaveHandshakeProperties` with its `_tlsTrust` assertion. The same assertion affected trustd and
Safari; it is not evidence that a certificate should be accepted.

The narrow compatibility adapter requires the configured RuntimeRoot and the canonical image in the active guest
cache. It translates this exact alias only when both alias and canonical loose files are absent, preserving loose
overrides, symlink ambiguity, pre-constructor forwarding, caller-sensitive dlopen behavior, mode and errno. No TLS
verification result is replaced.

The deployed alias probe then resolved both names to the same guest Network image and accessors. Without trustd,
NSURLSession reached its normal server-trust challenge but evaluation failed with `NSOSStatusErrorDomain -26276`, followed
by `NSURLErrorDomain -1202`. Starting the stock guest `/usr/libexec/trustd` service made the same default-handling HTTPS
probe evaluate the real four-certificate chain successfully (`SecTrustResult = 4`), return HTTP 200 / 577 bytes, and exit 0.
It ran as logical PID 6927; native trustd was PID 6918. No certificate anchor, policy or acceptance result was overridden.
Stock trustd is now included in graphical defaults; explicit service selections remain explicit.

Safari native PID 6944 then rendered its restored `example.com` tab on screen. No LLDB was attached. The guest used the
same cache and development-device detachment policy described above; native host PID was 6827. Guest binaries and shim
were development-signed with the configured identity and `com.kdt.LiveSimBooter` identifier; runtime originals remained
unchanged and the previous shim was kept outside RuntimeRoot for rollback. HTTPS probe success and this visible page are
bounded checks, not a claim that all Safari features work. See the [notification-loop results](logging.md#webkit-notification-loopback)
for the separate CPU/memory issue.

Local evidence: `tls-handles-result.log`, `alias-https-without-trustd.log`, `alias-https-with-trustd.log`, and
`safari-alias-screen.png` under the ignored experiment directory above. Each appended log contains older attempts as well;
identify the final result by its reported process ID rather than treating earlier failures as current.

The final native build cold-started through bare `--launchd-services` (the normal default selection): native host 7009,
launchd 7014, and trustd 7045. `launchctl print` showed the generated trustd job running with its stock Mach service and
environment. A fresh default-handling HTTPS probe, logical PID 7111, again verified the four-certificate chain, received
HTTP 200 / 577 bytes, and exited 0. Safari native PID 7100 displayed `example.com` again. The integrated local suite,
strict guest-shim build, native signed build, and `git diff --check` passed. No debugger was attached; thermal pressure
remained 0. Logs and screenshots are in `session-final/`, `trustd-default-state.log`, `final-https-thermal-debugger.log`,
and `safari-default-screen.png` under the ignored experiment directory.

Touch stress testing was not performed in this run: installed idb rejected physical-device touch, and DeviceHub interaction
was unavailable while the Mac was locked. Screenshots worked but did not contain the user's temperature overlay; pressure
0 is the native thermal-pressure state, not a temperature in degrees. Tests were bounded and no repeated launch/input
loop was left running.
