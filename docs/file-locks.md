# Guest file locks during native suspension

Two native LiveProcess crash reports on iPhone16,2 / iOS 26.1 (23B50) identified real suspension failures at
2026-10-26 19:16:21 +0700. Native RunningBoard terminated the launchctl-control worker (PID 10755) for holding
`SimulatorData/var/run/launchctl-control/worker.lock`, and containermanager (PID 10749) for holding
`SimulatorData/Library/MobileContainerManager/references.sqlite3`. Both reports use termination code `0xDEAD10CC` and
state that the extension was suspended while holding a shared file lock. The saved inventory is
`tmp/shm-write-measure/lock-crash-inventory.txt`.

The native RunningBoard binary, UUID `8551BB47-6666-33AB-860C-D87869E8D452`, checks
`com.apple.runningboard.can-suspend-locked` in `_lock_lockedFilePathsIgnoring:` at image offset `0x66d4c`.
The query at `+0x672f4` requires an attribute length of exactly one byte; `+0x6731c` reads that byte, and `+0x67320`
accepts any nonzero value before the SQLite and `F_GETLKPID` lock checks. This was verified against the matching native
DeviceSupport binary, rather than the simulator RunningBoard image. WebKit's
[SQLiteFileSystem implementation](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/platform/sql/SQLiteFileSystem.cpp)
uses the same attribute with a single byte `0xff`.

`Guest/FileLockCompatibility.c` provides an opt-in guest adapter, enabled by `LSB_SIMULATE_FILE_LOCK_SUSPENSION=1`.
It captures the canonical, owned `SIMULATOR_SHARED_RESOURCES_DIRECTORY`, whose final component is now `data` under the pinned
`LSB/iOS/23A343` storage root. The crash evidence above predates this storage rename.
Before an acquiring `flock` or record-lock operation, it checks the actual descriptor: an owned regular file with one
hard link, a canonical `F_GETPATH` below that directory, and matching descriptor/path device and inode. This resolves
the `/var` and `/private/var` aliases and rejects symlinks escaping the root, sibling prefix matches, and unrelated files.
It uses `fsetxattr` on that same descriptor, with `XATTR_CREATE`, only when the attribute is absent. Existing values,
including explicit zero or malformed values, remain unchanged.

The adapter covers the public and fixed-argument `fcntl` entry points, including their no-cancel variants, and the
ordinary/OFD acquiring commands and timeout variants. Darwin arm64 variadic forwarding preserves the original third
argument slot and native command decoding. A safe self-memory read identifies `F_RDLCK`/`F_WRLCK` without dereferencing
invalid caller pointers. Unlocks, queries, unsupported commands, and calls before initialization retain native behavior.

Annotation is best effort and occurs before acquisition to avoid a post-acquisition suspension window. Its failure does
not change the requested lock, native result, or errno. The annotation can remain after a failed acquisition or later
unlock; it is persistent file policy metadata, not lock state. No lock is replaced, released, or silently granted, and
the adapter does not ensure that another guest waiting on a suspended owner will make progress before that owner resumes.

The trace-enabled simulator shim compiles and links with all five interpositions. Focused descriptor, lock, forwarding,
timeout/OFD, and errno tests pass, as do enabled/disabled `DYLD_INSERT_LIBRARIES` tests of the real symbol bindings.
The complete test suite also passed (`tmp/shm-write-measure/full-suite.log`). After deploying the new shim, guest code
created the exact one-byte `ff` attribute on both reported files: `worker.lock` and `references.sqlite3`. A read-only device
query confirmed their values in `tmp/shm-write-measure/lock-xattrs.json`; no SSH/root annotation was used.
A subsequent background/foreground trial with silent audio off kept the host, guest launchd, SpringBoard, backboardd,
containermanager and the control worker alive, with no new `0xDEAD10CC` report. Process snapshots and crash inventory are
`tmp/shm-write-measure/background-off-processes.json` and `after-background-crashes.json`. This verifies one background
cycle; it does not establish that each process entered kernel suspension or that every guest lock site is covered.
