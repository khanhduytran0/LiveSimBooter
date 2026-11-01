#!/usr/bin/env python3
"""Prepare the pinned 23A343 dyld_sim for a runtime-selected simulator cache base.

Input must be the original thin arm64 dyld_sim, SHA-256 pinned below. The output includes the existing
iOS platform compatibility patches and low-cache compatibility. Native LiveSimBooter validates the
prepared cache data file and writes the selected address to a marked __DATA_DIRTY record before entry;
changing the base does not modify signed TEXT or require signing dyld_sim again. Sign this output once.
--find-only discovers and validates the instruction contexts and section-free padding, reporting JSON
without writing a binary. Discovery is limited to this reviewed input; it is not a cross-version patcher.
"""

import argparse
import hashlib
import importlib.util
import json
import stat
import struct
from pathlib import Path

SOURCE_SHA256 = '47a6f5e9b44ae48182ec085b748f708c35254b894c4bbc51e3fe47bfe18c528a'
PREFERRED_BASE = 0x180000000
TEXT_PADDING = 0x4d540
CONFIG_VM_OFFSET = 0x7c200
CONFIG_FILE_OFFSET = 0x5c200
CONFIG_MAGIC = b'LSBDyldSlideV1'.ljust(16, b'\0')
CONFIG = struct.pack('<16sIIQQQ16s', CONFIG_MAGIC, 1, 64, PREFERRED_BASE, 0, 0, bytes(16))

# Original instruction neighborhoods from mapCachePrivate in the pinned input. Search the complete instruction section,
# not windows at the reviewed offsets. Each context must occur once, on an instruction boundary.
CONTEXTS = (
    ('initialize_slide', bytes.fromhex('e83b48f9e81300f9e81b4bf9f31f40f9687e00a9080080d239070034'),
     (('initialize_slide', 0x10),)),
    ('mapping_address', bytes.fromhex('130140f9fb2743a9290540f9ea1340f94901098b690209cb158941b9e80761b23a01088b68435df8'),
     (('cancel_slide', 0x08), ('mapping_base', 0x1c))),
    ('cleanup_address', bytes.fromhex('fa6741a9fc6fc6a8c0035fd6e00761b2e10761b2020080524302825204008052'),
     (('cleanup_base', 0x0c), ('cleanup_size', 0x10))),
)
REVIEWED_SITES = dict(initialize_slide=0x1c16c, cancel_slide=0x1c1a0, mapping_base=0x1c1b4, cleanup_base=0x1c47c, cleanup_size=0x1c480)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def branch(source, target):
    delta = target - source
    require(not (delta & 3) and -(1 << 27) <= delta < (1 << 27), 'Branch is not representable')
    return 0x14000000 | ((delta >> 2) & 0x3ffffff)


def adrp(register, source, target):
    pages = (target >> 12) - (source >> 12)
    require(-(1 << 20) <= pages < (1 << 20), 'ADRP is not representable')
    return 0x90000000 | ((pages & 3) << 29) | (((pages >> 2) & 0x7ffff) << 5) | register


def load_constant(register, value):
    return [(0xD2800000 if i == 0 else 0xF2800000) | (i << 21) | (((value >> (16 * i)) & 0xffff) << 5) | register for i in range(4)]


def macho_segments(original):
    require(len(original) >= 32 and struct.unpack_from('<IIII', original) == (0xFEEDFACF, 0x0100000C, 0, 7), 'Expected thin arm64 MH_DYLINKER')
    ncmds, sizeofcmds = struct.unpack_from('<II', original, 16)
    offset, end, segments = 32, 32 + sizeofcmds, {}
    require(end <= len(original), 'Truncated Mach-O commands')
    for _ in range(ncmds):
        require(offset + 8 <= end, 'Truncated load-command header')
        command, length = struct.unpack_from('<II', original, offset)
        require(length >= 8 and offset + length <= end, 'Invalid Mach-O command length')
        if command == 0x19:
            require(length >= 72, 'Truncated Mach-O segment')
            _, _, name, address, vmsize, fileoff, filesize, maxprot, initprot, nsects, _ = struct.unpack_from('<II16sQQQQiiII', original, offset)
            name = name.rstrip(b'\0')
            require(name not in segments, f'Ambiguous duplicate segment {name!r}')
            require(fileoff + filesize <= len(original) and address + vmsize <= 1 << 64 and 72 + nsects * 80 <= length, 'Invalid segment bounds')
            segment = dict(name=name, vmaddr=address, vmsize=vmsize, fileoff=fileoff, filesize=filesize,
                           maxprot=maxprot, initprot=initprot, sections=[])
            for index in range(nsects):
                section = struct.unpack_from('<16s16sQQIIIIIIII', original, offset + 72 + index * 80)
                section_name, owner, start, size, section_fileoff, _, _, _, flags, _, _, _ = section
                require(owner.rstrip(b'\0') == name and address <= start <= start + size <= address + vmsize, 'Invalid section VM bounds')
                if (flags & 0xff) not in (1, 0xc, 0x12):  # Mach-O zero-fill section types have no file bytes.
                    require(fileoff <= section_fileoff and section_fileoff + size <= fileoff + filesize and
                            section_fileoff - fileoff == start - address, 'Section/file mapping disagreement')
                segment['sections'].append(dict(name=section_name.rstrip(b'\0'), address=start, size=size, fileoff=section_fileoff, flags=flags))
            segments[name] = segment
        offset += length
    require(offset == end, 'Unexpected Mach-O command size')
    return segments


def unique_instruction_context(original, section, pattern, label):
    start, end = section['fileoff'], section['fileoff'] + section['size']
    matches = []
    position = original.find(pattern, start, end)
    while position >= 0:
        if not (position & 3):
            matches.append(position)
            require(len(matches) == 1, f'Ambiguous instruction context: {label}')
        position = original.find(pattern, position + 1, end)
    require(matches, f'Missing instruction context: {label}')
    return matches[0]


def find_tail_padding(original, segment, size, alignment, protection):
    require(segment['maxprot'] == protection and segment['initprot'] == protection, 'Unexpected padding segment protections')
    require(segment['sections'], 'Padding segment has no section boundaries')
    end = max(section['address'] + section['size'] for section in segment['sections'])
    # First suitably aligned location after every declared section, not a scan through an arbitrary run of zero instructions.
    address = (end + alignment - 1) & ~(alignment - 1)
    fileoff = segment['fileoff'] + address - segment['vmaddr']
    require(address + size <= segment['vmaddr'] + min(segment['vmsize'], segment['filesize']), 'Insufficient section-free tail padding')
    require(original[fileoff:fileoff + size] == bytes(size), 'Section-free tail padding is not zero')
    return dict(segment=segment['name'].decode('ascii'), section_end=hex(end), alignment=alignment,
                vm_address=hex(address), file_offset=hex(fileoff), size=size, zero_bytes_verified=True)


def discover_layout(original):
    segments = macho_segments(original)
    require(b'__TEXT' in segments and b'__DATA_DIRTY' in segments, 'Missing loader text/data segments')
    text = segments[b'__TEXT']
    instruction_sections = [section for section in text['sections'] if section['name'] == b'__text' and section['flags'] & 0x80000000]
    require(len(instruction_sections) == 1, 'Missing or ambiguous pure-instruction __TEXT,__text section')
    section = instruction_sections[0]
    sites, contexts = {}, []
    for label, pattern, members in CONTEXTS:
        position = unique_instruction_context(original, section, pattern, label)
        contexts.append(dict(name=label, file_offset=hex(position), vm_address=hex(section['address'] + position - section['fileoff']),
                             original_bytes=pattern.hex(), matches=1))
        for name, relative in members:
            sites[name] = dict(file_offset=hex(position + relative), vm_address=hex(section['address'] + position + relative - section['fileoff']))
    payload, _ = build_stub()
    return dict(contexts=contexts, sites=sites,
                text_padding=find_tail_padding(original, text, len(payload), 32, 5),
                data_configuration=find_tail_padding(original, segments[b'__DATA_DIRTY'], len(CONFIG), 256, 3))


def validate_reviewed_layout(discovery):
    # Discovery is independently useful evidence, but a new location is not authorization to transplant this pinned instruction stub.
    for name, expected in REVIEWED_SITES.items():
        site = discovery['sites'][name]
        require(int(site['file_offset'], 16) == expected and int(site['vm_address'], 16) == expected,
                f'Discovered {name} differs from the reviewed 23A343 layout')
    require(int(discovery['text_padding']['file_offset'], 16) == TEXT_PADDING and
            int(discovery['text_padding']['vm_address'], 16) == TEXT_PADDING, 'Discovered TEXT padding differs from reviewed layout')
    require(int(discovery['data_configuration']['file_offset'], 16) == CONFIG_FILE_OFFSET and
            int(discovery['data_configuration']['vm_address'], 16) == CONFIG_VM_OFFSET, 'Discovered DATA padding differs from reviewed layout')


def find_pinned_layout(original):
    require(hashlib.sha256(original).hexdigest() == SOURCE_SHA256, 'Unsupported original 23A343 dyld_sim SHA-256')
    discovery = discover_layout(original)
    validate_reviewed_layout(discovery)
    discovery['reviewed_layout_matches'] = True
    return discovery


def validate_padding(original, vmaddr, fileoff, size, name, protection):
    ncmds, sizeofcmds = struct.unpack_from('<II', original, 16)
    offset, found = 32, False
    require(32 + sizeofcmds <= len(original), 'Truncated Mach-O commands')
    for _ in range(ncmds):
        command, length = struct.unpack_from('<II', original, offset)
        require(length >= 8 and offset + length <= 32 + sizeofcmds, 'Invalid Mach-O command')
        if command == 0x19:
            _, _, segment, address, vmsize, file_offset, filesize, maxprot, initprot, nsects, _ = struct.unpack_from('<II16sQQQQiiII', original, offset)
            require(72 + nsects * 80 <= length, 'Truncated segment sections')
            if segment.rstrip(b'\0') == name:
                require(not found and address <= vmaddr and vmaddr + size <= address + min(vmsize, filesize), 'Padding outside file-backed segment')
                require(file_offset + vmaddr - address == fileoff and maxprot == protection and initprot == protection, 'Unexpected padding segment')
                for index in range(nsects):
                    start, count = struct.unpack_from('<QQ', original, offset + 72 + index * 80 + 32)
                    require(not (vmaddr < start + count and start < vmaddr + size), 'Padding overlaps a Mach-O section')
                found = True
        offset += length
    require(found and offset == 32 + sizeofcmds, 'Missing expected segment or unexpected load-command size')
    require(original[fileoff:fileoff + size] == bytes(size), 'Expected zero-filled segment padding')


def build_stub():
    # mapCachePrivate's x8 is the first mapping's preferred address, x19 its output structure.
    # Source: SharedCacheRuntime.cpp at apple-oss-distributions/dyld fd8d0c4d52320ebf64db34f3cb280310d905c5ae;
    # offsets/encodings are independently checked against the exact 23A343 binary and complete input hash.
    words = load_constant(9, PREFERRED_BASE) + [0xEB09011F, 0]  # cmp x8,x9; b.ne ordinary
    words += [adrp(9, TEXT_PADDING + len(words) * 4, CONFIG_VM_OFFSET),
              0xF9400129 | (((CONFIG_VM_OFFSET + 32) & 0xfff) // 8 << 10),  # ldr x9,[x9,#config.actualBase]
              0, 0xCB080129, 0x8B090108, 0xA9002668]  # cbz x9,missing; sub slide; add base; stp base,slide
    words += [branch(TEXT_PADDING + len(words) * 4, 0x1c170)]
    ordinary = TEXT_PADDING + len(words) * 4
    words[5] = 0x54000001 | (((ordinary - (TEXT_PADDING + 5 * 4)) >> 2) << 5)
    words += [0xA9007E68, branch(ordinary + 4, 0x1c170)]
    missing = TEXT_PADDING + len(words) * 4
    words[8] = 0xB4000009 | (((missing - (TEXT_PADDING + 8 * 4)) >> 2) << 5)
    words += [0xD4207D00]  # brk #1000: never replace the native shared region when native preparation was omitted
    cleanup = TEXT_PADDING + len(words) * 4
    words += [0xF94903E0, 0xF94907E1, 0xF9400669, 0x8B090000]
    words += [branch(TEXT_PADDING + len(words) * 4, 0x1c484)]
    return struct.pack('<' + 'I' * len(words), *words), cleanup


def patched_bytes(original):
    find_pinned_layout(original)
    spec = importlib.util.spec_from_file_location('platform_patch', Path(__file__).with_name('patch-ios26-platform.py'))
    platform = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(platform)
    payload, cleanup = build_stub()
    validate_padding(original, TEXT_PADDING, TEXT_PADDING, len(payload), b'__TEXT', 5)
    validate_padding(original, CONFIG_VM_OFFSET, CONFIG_FILE_OFFSET, len(CONFIG), b'__DATA_DIRTY', 3)
    data, records = bytearray(original), []

    def patch(offset, before, after, reason):
        require(len(before) == len(after) and data[offset:offset + len(before)] == before, f'Unexpected bytes at {offset:#x}')
        data[offset:offset + len(before)] = after
        records.append(dict(offset=hex(offset), before=before.hex(), after=after.hex(), reason=reason))

    def word(offset, before, after, reason):
        patch(offset, struct.pack('<I', before), struct.pack('<I', after), reason)

    for offset, before, after, reason in platform.PATCHES:
        word(offset, before, after, reason)
    patch(TEXT_PADDING, bytes(len(payload)), payload, 'Signed PC-relative slide/cleanup stubs; no selected base embedded in TEXT')
    patch(CONFIG_FILE_OFFSET, bytes(len(CONFIG)), CONFIG, 'Writable native-preparation configuration, outside every dyld section')
    word(0x1c16c, 0xA9007E68, branch(0x1c16c, TEXT_PADDING), 'Initialize cache base/slide from validated writable configuration')
    word(0x1c1a0, 0xF9400529, 0xAA1F03E9, 'Do not cancel the selected slide in private mapping address calculation')
    word(0x1c1b4, 0xB26107E8, 0xAA0A03E8, 'Use first mapping base; preserve ordinary low-cache compatibility')
    word(0x1c47c, 0xB26107E0, branch(0x1c47c, cleanup), 'Failure cleanup uses selected base and declared cache span')
    word(0x1c480, 0xB26107E1, 0xF94907E1, 'Cleanup cache size rather than original fixed shared-region size')
    allowed = set()
    for record in records:
        start = int(record['offset'], 16)
        allowed.update(range(start, start + len(bytes.fromhex(record['after']))))
    require(len(data) == len(original) and all(a == b or i in allowed for i, (a, b) in enumerate(zip(original, data))), 'Unplanned byte change')
    return bytes(data), records


def patch_file(source, output):
    manifest = output.with_name(output.name + '.slide-patch.json')
    for destination in (output, manifest):
        require(destination.resolve() != source.resolve() and not destination.exists() and not destination.is_symlink(), f'Output already exists: {destination}')
    original = source.read_bytes()
    patched, records = patched_bytes(original)
    report = dict(runtime_build='23A343', source=str(source), source_sha256=SOURCE_SHA256, output=str(output),
                  output_sha256=hashlib.sha256(patched).hexdigest(), config_vm_offset=hex(CONFIG_VM_OFFSET),
                  config_file_offset=hex(CONFIG_FILE_OFFSET), config_size=len(CONFIG), preferred_base=hex(PREFERRED_BASE),
                  selected_base='Native validated sidecar actualBase; no per-base signing', preserved_platform_patches=11,
                  requires_separate_signing=True, discovery=find_pinned_layout(original), patches=records)
    output.parent.mkdir(parents=True, exist_ok=True)
    created = []
    try:
        with output.open('xb') as stream:
            created.append(output)
            stream.write(patched)
        output.chmod(stat.S_IMODE(source.stat().st_mode))
        require(source.read_bytes() == original and output.read_bytes() == patched, 'Input changed or output verification failed')
        with manifest.open('x') as stream:
            created.append(manifest)
            stream.write(json.dumps(report, indent=2) + '\n')
    except Exception:
        for path in reversed(created):
            path.unlink()
        raise
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='Original immutable 23A343 RuntimeRoot/usr/lib/dyld_sim')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--output', type=Path)
    mode.add_argument('--find-only', action='store_true', help='Discover and validate patch sites/padding; print JSON without writing any file')
    args = parser.parse_args()
    try:
        source = args.source.resolve()
        if args.find_only:
            report = dict(runtime_build='23A343', source=str(source), source_sha256=SOURCE_SHA256, find_only=True,
                          discovery=find_pinned_layout(source.read_bytes()))
        else:
            report = patch_file(source, args.output.absolute())
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
