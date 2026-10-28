# Shared-memory storage measurements

The current notification and FrontBoard workspace adapters use ordinary files in `data/tmp` under the selected group's
`LSB/iOS/23A343` directory. The measurements below predate the rename from `SimulatorData`. The files' shared
mapping changes can reach storage. The existing App Group native POSIX SHM probe is documented in
[the launchd notes](launchd.md#frontboard-scene-reconnection-storage); it is not yet the production backing store.
FrontBoard's `FBW1`/`FBW2` buffers contain workspace connection state, not framebuffer pixels. Display frames use IOSurface.

On 2026-10-26, the iPhone16,2 / iOS 26.1 run was sampled seven times per window, about ten seconds apart. The collector
selected only exact LiveSimBooter and LiveProcess executable paths, checked native PID/start-time/UUID identity, captured
`proc_pid_rusage(RUSAGE_INFO_V4)`, inspected file-backed VM regions, and hashed the notification/workspace files in
16,384-byte chunks. It did not flush those production mappings. Evidence and collector sources are in
`tmp/shm-write-measure/`.

| Window | Elapsed time | Processes present at both endpoints | Disk-write counter delta | Logical-write counter delta |
| --- | ---: | ---: | ---: | ---: |
| Idle, `12:17:58.894–12:19:00.990` UTC | 62.10 s | 21 | 57,888,768 bytes | 69,278,518 bytes |
| App launch, `12:22:47.750–12:23:55.136` UTC | 67.39 s | 28 | 15,003,648 bytes | 18,236,417 bytes |

These totals are endpoint differences for processes with matching identity, covering all their file activity. Each window
also gained one process. The idle window's new process had zero recorded write bytes at the endpoint; the app-launch
window's new process had lifetime counters of 8,192 disk-write bytes and 98,304 logical-write bytes, excluded from the table.
Processes that start and exit entirely between samples are not accounted for. The idle window was shortly after boot,
so its larger total must not be interpreted as a controlled comparison of idle and interactive workloads.

The file-specific observations were small:

- `notification-center.shm` remained the same inode, 16,384-byte size, and identical content hash in all fourteen samples.
  Its mapping reported one dirtied page throughout; that is a sampled page state, not one new write per sample.
- During the idle window, the nine-byte `frontboard-workspace-2-com.apple.frontboard.syst.shm` remained unchanged.
- During app launch, `frontboard-workspace-1-com.apple.frontboard.syst.shm` was replaced between the first two samples:
  inode 7758371 / 13 bytes became inode 7758381 / 21 bytes. The replacement and its hash then remained stable.
- Native Metal control, child-status, and terminal mappings were also found. Multiple processes mapped the same backing
  objects, so summing all region counters would count shared pages repeatedly. Child-ledger/terminal dirty-page snapshots
  changed around launch; those snapshots do not provide cumulative write bytes or a disk-write count.

A separate scratch-file positive control validated the counters on the same phone. Writing 65,536 bytes and calling
`fsync` increased its process disk-write counter by exactly 65,536 bytes. Then 40,000 one-byte stores, repeatedly touching
one byte in each of four 16 KiB shared pages, increased logical writes by 65,536 bytes while disk writes remained unchanged.
After `mmap.flush()` and `fsync`, disk writes increased by another 65,536 bytes; logical writes gained another 12,288 bytes.
The scratch file was removed. This demonstrates page accounting and delayed writeback in that controlled test, not one
storage write per mapped store.

Neither rusage counter measures physical NAND programming, wear, filesystem compression, or write amplification. They
also cannot attribute process totals to an individual file. Equal hashes can miss writes of identical data and changes
that are undone between samples. The collected evidence therefore confirms file-backed memory can be written out, but
does not show a high write rate from the notification or FrontBoard buffers in these windows. It does not justify assigning
the much larger process-wide totals to these buffers.

The later native suspension failures concerned held file locks, not SHM write volume; see [file-lock compatibility](file-locks.md).
