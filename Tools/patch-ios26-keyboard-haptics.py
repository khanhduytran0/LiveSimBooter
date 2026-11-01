#!/usr/bin/env python3
"""Restore the AHAP base URL in 23A343 CoreHaptics' system pattern-library initializer.

The original simulator method parses a system AHAP into a dictionary, losing its source URL.
Native iOS 27 keyboard AHAPs contain relative AudioResources paths; dictionary initialization
returns -4830 and the stock caller dereferences nil. Use the existing public URL initializer,
preserving its NSError, the stock pattern ID, ownership and cleanup. A local nil guard handles
other invalid AHAPs. No engine or resource checks are bypassed, and app dictionary APIs are unchanged.

Only the pinned arm64 simulator cache is supported. Image UUID, sections, original instructions,
class-reference usage and selector stubs are checked before copying. Layout/signature/cache UUID
may differ, including the existing container-metadata patch. Source is immutable; output needs
separate Tools/sign-dyld-cache.py signing. No signing or device actions occur here.
"""

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import stat
import struct
import sys
import uuid


IMAGE = '/System/Library/Frameworks/CoreHaptics.framework/CoreHaptics'
IMAGE_UUID = '2AFD3C06-3A36-35CE-8940-B381C1AFFC0B'
PROFILE = dict(base=0x110000000, span=0x7E194000, image_count=1353)
CHUNK_SIZE = 4 * 1024 * 1024
# Original 23A343 section extents; __TEXT offsets survive the cache builder's relocation.
SECTIONS = {
    ('__TEXT', '__text'): (0xEF0, 0x48BA8, 5),
    ('__TEXT', '__objc_stubs'): (0x6B020, 0x3D40, 5),
    ('__TEXT', '__gcc_except_tab'): (0x4C9B0, 0x87A8, 5),
    ('__DATA_CONST', '__got'): (None, 0x720, 3),
    ('__DATA_CONST', '__objc_selrefs'): (None, 0x1310, 3),
}


def require(value, message):
    if not value:
        raise ValueError(message)


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def branch(site, target, link=False):
    delta = target - site
    require(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), 'Branch target outside ARM64 immediate range')
    return (0x94000000 if link else 0x14000000) | ((delta >> 2) & 0x3FFFFFF)


def cbz(site, target, register):
    delta = target - site
    require(delta % 4 == 0 and -(1 << 20) <= delta < (1 << 20), 'CBZ target outside ARM64 immediate range')
    return 0xB4000000 | (((delta >> 2) & 0x7FFFF) << 5) | register


def adrp(site, target, register):
    pages = (target >> 12) - (site >> 12)
    require(-(1 << 20) <= pages < (1 << 20), 'ADRP target outside ARM64 immediate range')
    immediate = pages & 0x1FFFFF
    return 0x90000000 | ((immediate & 3) << 29) | ((immediate >> 2) << 5) | register


def load64(target, base, destination):
    offset = target & 4095
    require(offset % 8 == 0, 'Unaligned 64-bit class/selector reference')
    return 0xF9400000 | ((offset // 8) << 10) | (base << 5) | destination


def direct_branch_target(site, instruction):
    if instruction & 0x7C000000 == 0x14000000:
        bits, immediate = 26, instruction & 0x3FFFFFF
    elif instruction & 0xFF000010 == 0x54000000 or instruction & 0x7E000000 == 0x34000000:
        bits, immediate = 19, (instruction >> 5) & 0x7FFFF
    elif instruction & 0x7E000000 == 0x36000000:
        bits, immediate = 14, (instruction >> 5) & 0x3FFF
    else:
        return None
    signed = (immediate ^ (1 << (bits - 1))) - (1 << (bits - 1))
    return site + signed * 4


def cache_layout(data, profile):
    require(len(data) >= 456 and data[:16] == b'dyld_v1   arm64\0', 'Expected a complete arm64 cache')
    mapping_offset, mapping_count = struct.unpack_from('<II', data, 16)
    signature, signature_size = struct.unpack_from('<QQ', data, 40)
    platform, flags = struct.unpack_from('<II', data, 216)
    base, span = struct.unpack_from('<QQ', data, 224)
    images_offset, image_count = struct.unpack_from('<II', data, 448)
    require(base == profile['base'] and span == profile['span'], 'Unexpected simulator cache reservation')
    require(platform == 7 and flags & 0x200, 'Expected iOS simulator cache platform')
    require(struct.unpack_from('<I', data, 396)[0] == 0, 'Subcaches are unsupported')
    require(signature > 0 and signature % 16384 == 0 and signature_size > 0 and signature + signature_size == len(data),
            'Invalid signature extent')
    require(mapping_offset >= 456 and 0 < mapping_count <= 64 and mapping_offset + 32 * mapping_count <= signature,
            'Invalid mapping table')
    require(image_count == profile['image_count'] and images_offset >= mapping_offset + mapping_count * 32 and
            images_offset + image_count * 32 <= signature, 'Invalid image table')
    mappings = []
    for index in range(mapping_count):
        address, size, offset, maximum, initial = struct.unpack_from('<QQQII', data, mapping_offset + index * 32)
        require(size > 0 and base <= address < address + size <= base + span and offset + size <= signature,
                'Mapping exceeds cache reservation/payload')
        require(maximum & ~7 == 0 and initial & ~maximum == 0, 'Invalid mapping protections')
        for prior in mappings:
            require(address + size <= prior[0] or address >= prior[0] + prior[1], 'Overlapping VM mappings')
            require(offset + size <= prior[2] or offset >= prior[2] + prior[1], 'Overlapping file mappings')
        mappings.append((address, size, offset, maximum, initial))
    require(mappings[0][0] == base and mappings[0][2] == 0, 'Unexpected first mapping')

    def locate(address, size=1, protection=None):
        matches = [item for item in mappings if item[0] <= address and address + size <= item[0] + item[1]]
        require(size > 0 and len(matches) == 1, 'Address does not fit one cache mapping')
        region = matches[0]
        if protection is not None:
            allowed_initial = (1, 3) if protection == 3 else (protection,)
            require(region[3] == protection and region[4] in allowed_initial, 'Unexpected cache mapping protections')
        return region[2] + address - region[0]

    paths, image_base = set(), None
    for index in range(image_count):
        address, _, _, path_offset, _ = struct.unpack_from('<QQQII', data, images_offset + index * 32)
        require(0 < path_offset < signature, 'Image path outside payload')
        end = data.find(b'\0', path_offset, min(path_offset + 4096, signature))
        require(end > path_offset, 'Unterminated image path')
        path = data[path_offset:end].decode('utf-8')
        require(path.startswith('/') and path not in paths, 'Invalid or duplicate image path')
        paths.add(path)
        if path == IMAGE:
            image_base = address
    require(image_base is not None, 'Missing CoreHaptics image')
    return locate, image_base, signature


def image_sections(data, locate, base):
    start = locate(base, 32, 5)
    magic, cpu, subtype, kind, count, length, _, _ = struct.unpack_from('<8I', data, start)
    require(magic == 0xFEEDFACF and cpu == 0x100000C and subtype & 0xFFFFFF == 0 and kind == 6, 'Unexpected CoreHaptics Mach-O')
    require(0 < count <= 2048 and count * 8 <= length <= 1024 * 1024, 'Invalid image command count/size')
    locate(base, 32 + length, 5)
    cursor, end, found, found_uuid = start + 32, start + 32 + length, {}, None
    for _ in range(count):
        require(cursor + 8 <= end, 'Truncated image command')
        command, size = struct.unpack_from('<II', data, cursor)
        require(size >= 8 and size % 8 == 0 and cursor + size <= end, 'Invalid image command')
        if command == 0x1B:
            require(size == 24 and found_uuid is None, 'Invalid or duplicate image UUID')
            found_uuid = str(uuid.UUID(bytes=data[cursor + 8:cursor + 24])).upper()
        elif command == 0x19:
            require(size >= 72, 'Truncated segment command')
            owner = data[cursor + 8:cursor + 24].rstrip(b'\0').decode('ascii')
            address, vm_size, offset, file_size, maximum, initial, sections, _ = struct.unpack_from('<QQQQIIII', data, cursor + 24)
            require(sections <= 4096 and size == 72 + sections * 80, 'Invalid segment section count')
            for index in range(sections):
                position = cursor + 72 + index * 80
                name = data[position:position + 16].rstrip(b'\0').decode('ascii')
                section_owner = data[position + 16:position + 32].rstrip(b'\0').decode('ascii')
                key = (section_owner, name)
                if key not in SECTIONS:
                    continue
                require(key not in found and section_owner == owner, 'Duplicate or mismatched pinned section')
                section_address, section_size, direct_offset = struct.unpack_from('<QQI', data, position + 32)
                section_flags = struct.unpack_from('<I', data, position + 64)[0]
                relative, expected_size, protection = SECTIONS[key]
                require(section_size == expected_size and (relative is None or section_address == base + relative), 'Unexpected pinned section extent')
                require(address <= section_address and section_address + section_size <= address + vm_size,
                        'Section outside owning segment')
                require(maximum == protection and initial == protection, 'Unexpected pinned segment protections')
                physical = locate(section_address, section_size, protection)
                require(physical == direct_offset and offset <= physical and physical + section_size <= offset + file_size and
                        physical - offset == section_address - address, 'Section file/VM coordinates disagree')
                require(section_flags & 0xFF == (6 if name == '__got' else 0),
                        'Unexpected pinned section type')
                found[key] = dict(address=section_address, size=section_size, offset=physical)
        cursor += size
    require(cursor == end and found_uuid == IMAGE_UUID, 'Unsupported CoreHaptics UUID or command layout')
    require(len(found) == len(SECTIONS), 'Missing pinned CoreHaptics sections')
    return found


def inspect_cache(data, profile=PROFILE, pointer_reader=None):
    locate, base, signature = cache_layout(data, profile)
    sections = image_sections(data, locate, base)
    got = sections[('__DATA_CONST', '__got')]['address']
    selectors = sections[('__DATA_CONST', '__objc_selrefs')]['address']

    def expect(relative, expected):
        offset = locate(base + relative, len(expected), 5)
        require(data[offset:offset + len(expected)] == expected, 'Original instruction mismatch at CoreHaptics+%#x' % relative)

    def selector(relative, slot_relative, name):
        stub = base + relative
        slot = selectors + slot_relative
        expect(relative, words(adrp(stub, slot, 1), load64(slot, 1, 1)))
        expect(relative + 16, words(0xD61F0200, 0xD4200020, 0xD4200020, 0xD4200020))
        # Slidable-cache callers supply a reader that proves membership in a validated relocation chain before decoding.
        offset = locate(slot, 8, 3)
        pointer = pointer_reader(offset) if pointer_reader is not None else struct.unpack_from('<Q', data, offset)[0]
        offset = locate(pointer, len(name) + 1)
        require(data[offset:offset + len(name) + 1] == name.encode() + b'\0', 'Unexpected Objective-C selector target')

    # Selector slot offsets from the pinned original __objc_selrefs; names are validated in the cache's shared selector pool.
    selector(0x6B040, 0x28, 'JSONObjectWithData:options:error:')
    selector(0x6C3E0, 0x510, 'fileURLWithPath:isDirectory:')
    selector(0x6CA00, 0x698, 'initWithContentsOfURL:error:')
    selector(0x6CA40, 0x6A8, 'initWithDictionary:error:')
    # Independent stock call verifies NSURL's class-reference slot and factory ABI (NSString, isDirectory=NO).
    expect(0x194E0, words(adrp(base + 0x194E0, got + 0x6C0, 8), load64(got + 0x6C0, 8, 0), 0xAA1303E2, 0x52800003,
                          branch(base + 0x194F0, base + 0x6C3E0, True)))
    # Keep the existing retain/result slots, error branches, NSError pointer and storeStrong cleanup call intact.
    expect(0x2D194, words(0xF90003FF, 0x910003E4, 0xAA1A03E2, 0xD2800003))
    expect(0x2D1A8, words(0x94007356, 0xAA0003FB, 0xF94003E8, 0x940073E3, 0xAA0003F9, 0xF9400388,
                          0xB5000100, 0xB40000FB))
    expect(0x2D1CC, words(0xF9400108, 0x940073DC, 0xAA0003FC, 0xB5000660, 0x14000052))
    expect(0x2D324, words(adrp(base + 0x2D324, got + 0x48, 8), load64(got + 0x48, 8, 0), 0x940072E6,
                          0xAA1B03E2, 0xAA1503E3, 0x9400FDC2, 0xAA0003FC, 0x91006000, 0xAA1703E1,
                          0x94007387, 0x94007335, 0x94007331, 0x9400732A))
    text = sections[('__TEXT', '__text')]
    incoming = []
    for relative in range(0, text['size'], 4):
        instruction = struct.unpack_from('<I', data, text['offset'] + relative)[0]
        site = text['address'] + relative
        target = direct_branch_target(site, instruction)
        if target is not None and base + 0x2D298 <= target < base + 0x2D2A8:
            incoming.append((site - base, target - base))
    require(incoming == [(0x2D1C8, 0x2D298), (0x2D1D8, 0x2D2A4)], 'Unexpected branch into reused success-log instructions')

    changes = [
        (0x2D18C, words(adrp(base + 0x2D18C, got + 0x168, 8), load64(got + 0x168, 8, 0)),
         words(adrp(base + 0x2D18C, got + 0x6C0, 8), load64(got + 0x6C0, 8, 0)), 'Use existing NSURL class reference'),
        (0x2D19C, words(0xAA1A03E2), words(0xAA1803E2), 'Pass retained AHAP pathname x24 instead of data x26'),
        (0x2D1A4, words(0x9400F7A7), words(branch(base + 0x2D1A4, base + 0x6C3E0, True)), 'Create file URL; existing x3 isDirectory=NO'),
        (0x2D1C8, words(0xB4000688), words(branch(base + 0x2D1C8, base + 0x2D324)), 'Skip serialized-dictionary success logging'),
        (0x2D298, words(adrp(base + 0x2D298, got + 0x4A8, 28), load64(got + 0x4A8, 28, 28), 0x9400739C, 0xAA1C03E0),
         words(cbz(base + 0x2D298, base + 0x2D34C, 0), 0x91006000, 0xAA1703E1, branch(base + 0x2D2A4, base + 0x2D348)),
         'Guard nil before original pattern-ID store; otherwise return to original storeStrong call site'),
        (0x2D338, words(0x9400FDC2), words(branch(base + 0x2D338, base + 0x6CA00, True)), 'Use public URL initializer with original NSError pointer'),
        (0x2D340, words(0x91006000), words(branch(base + 0x2D340, base + 0x2D298)), 'Route through local nil guard'),
    ]
    result = []
    for relative, before, after, purpose in changes:
        require(len(before) == len(after), 'Patch changes instruction extent')
        expect(relative, before)
        result.append(dict(image_relative=relative, address=base + relative, file_offset=locate(base + relative, len(before), 5),
                           before=before.hex(), after=after.hex(), purpose=purpose))
    return dict(image=IMAGE, image_uuid=IMAGE_UUID, image_address=base, sites=result,
                preserved_exception_table_sha256=hashlib.sha256(data[sections[('__TEXT', '__gcc_except_tab')]['offset']:
                    sections[('__TEXT', '__gcc_except_tab')]['offset'] + sections[('__TEXT', '__gcc_except_tab')]['size']]).hexdigest(),
                signature_offset=signature)


def identity(metadata):
    return metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns, metadata.st_ctime_ns


def patch_cache(source, output=None, manifest=None, profile=PROFILE):
    source = Path(source).absolute()
    before = source.lstat()
    require(stat.S_ISREG(before.st_mode), 'Source must be a regular file, not a symlink')
    require(source.parent.resolve() == source.parent, 'Source parent must be canonical')
    if output is not None:
        output = Path(output).absolute()
        manifest = Path(manifest).absolute() if manifest else Path(str(output) + '.patch-manifest.json')
        require(output != source and manifest not in (source, output), 'Source/output/manifest must be distinct')
        for destination in (output, manifest):
            require(destination.parent.resolve() == destination.parent and destination.parent.is_dir(), 'Output parent must be existing and canonical')
            require(not os.path.lexists(destination), 'Output or manifest already exists')
    created = []
    try:
        descriptor = os.open(source, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, 'rb') as stream:
            require(identity(os.fstat(stream.fileno())) == identity(before), 'Source changed before opening')
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                details = inspect_cache(data, profile)
            source_hash = hashlib.sha256()
            while block := stream.read(CHUNK_SIZE):
                source_hash.update(block)
            require(identity(os.fstat(stream.fileno())) == identity(before), 'Source changed during validation')
            report = dict(version=1, runtime='iOS Simulator 26.0 build 23A343', source=str(source), source_bytes=before.st_size,
                          source_sha256=source_hash.hexdigest(), requires_separate_signing=True, **details)
            if output is None:
                return dict(report, validation_only=True)
            source_copy_hash, patched_hash = hashlib.sha256(), hashlib.sha256()
            position = 0
            stream.seek(0)
            with output.open('xb') as destination:
                created.append((output, os.fstat(destination.fileno())))
                while block := stream.read(CHUNK_SIZE):
                    source_copy_hash.update(block)
                    changed = bytearray(block)
                    for site in details['sites']:
                        offset, replacement = site['file_offset'], bytes.fromhex(site['after'])
                        begin, end = max(position, offset), min(position + len(block), offset + len(replacement))
                        if begin < end:
                            changed[begin - position:end - position] = replacement[begin - offset:end - offset]
                    destination.write(changed)
                    patched_hash.update(changed)
                    position += len(block)
                require(position == before.st_size and source_copy_hash.digest() == source_hash.digest(), 'Source changed during copy')
                require(identity(os.fstat(stream.fileno())) == identity(before), 'Source metadata changed during copy')
                os.fchmod(destination.fileno(), stat.S_IMODE(before.st_mode) & 0o777)
                destination.flush()
                os.fsync(destination.fileno())
            require(identity(source.lstat()) == identity(before), 'Source path changed during patching')
            with output.open('rb') as verification:
                verified = hashlib.sha256()
                while block := verification.read(CHUNK_SIZE):
                    verified.update(block)
            require(verified.digest() == patched_hash.digest(), 'Output verification failed')
            report.update(output=str(output), output_sha256=patched_hash.hexdigest(), output_bytes=position, signature_invalidated=True,
                          changed_bytes=sum(sum(a != b for a, b in zip(bytes.fromhex(s['before']), bytes.fromhex(s['after']))) for s in details['sites']))
            with manifest.open('x') as record:
                created.append((manifest, os.fstat(record.fileno())))
                json.dump(report, record, indent=2)
                record.write('\n')
            return report
    except Exception:
        for path, metadata in reversed(created):
            try:
                current = path.lstat()
                if (current.st_dev, current.st_ino) == (metadata.st_dev, metadata.st_ino):
                    path.unlink()
            except FileNotFoundError:
                pass
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--output', type=Path)
    action.add_argument('--check-only', action='store_true')
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args()
    if args.check_only and args.manifest:
        parser.error('--manifest requires --output')
    try:
        report = patch_cache(args.source, args.output, args.manifest)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, 'error: %s\n' % error)
    print(json.dumps(report, indent=2))
    if args.output:
        print('Signature invalidated: sign the new cache separately before deployment.', file=sys.stderr)


if __name__ == '__main__':
    main()
