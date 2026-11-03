# Source licensing

LiveSimBooter's original code is available under the [MIT License](../LICENSE), copyright 2026 Duy Tran.
The following Apple-derived files and their modifications remain under the [Apple Public Source License 2.0](../LICENSES/APSL-2.0.txt):

- `LiveSimBooter/DyldSimLoader.cpp` — adapted simulator mapping and launch flow.
- `LiveSimBooter/SharedCacheHandoff.cpp` — adapted shared-region removal flow.
- `LiveSimBooter/SimSyscallHelpers.h` and `LiveSimBooter/SimSyscallHelpers.cpp` — adapted syscall-helper ABI and table.
- `LiveSimBooter/CompactImageInfo.cpp` — adapted compact image snapshot format.

Their Apple copyright/license notices and dated modification notices are retained. The source is in this repository at those
paths; upstream is [Apple dyld at fd8d0c4d52320ebf64db34f3cb280310d905c5ae](https://github.com/apple-oss-distributions/dyld/tree/fd8d0c4d52320ebf64db34f3cb280310d905c5ae).
APSL §4 permits combining covered code with separately licensed code in a larger work. MIT does not replace APSL for the
covered portions. Distributing or externally deploying modified covered code carries APSL's notice, license-copy and source
availability requirements; executable-only distributions also need a notice identifying how to obtain that covered source.
The licenses here do not grant rights to separately supplied Apple simulator binaries, runtime images, or SDKs.

The optional Procursus APT build applies [source patches](../Tools/procursus/) to APT 2.9.4. The patched APT sources use
GPL-2.0-or-later. APT's [copyright file](https://github.com/Debian/apt/blob/2.9.4/COPYING) lists the upstream notices.
