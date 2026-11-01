#!/usr/bin/env python3
"""Apply the pinned metadata/haptics compatibility patches and wide Objective-C ISA mask to a full slidable 23A343 cache.

The complete image inventory, reviewed mapping profile and exact v2 relocation chains are checked
first. Existing patch validators retain their UUID, instruction, section and selector checks.
The four inspected selector pointers are decoded only after proving membership in a slide chain;
no writable cache data or relocation metadata is changed. The ISA scalar is resolved by export
trie and must match the simulator decoder instructions. A payload-derived UUID distinguishes the
result. The source is immutable. Sign the output separately with sign-dyld-cache.py before use.
"""

import argparse
import ctypes
import errno
import hashlib
import importlib.util
import json
import mmap
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import uuid


HERE = Path(__file__).resolve().parent


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


slide = module('lsb_slidable_setup', 'setup-ios26-slidable-cache.py')
metadata = module('lsb_metadata_patch', 'patch-ios26-container-metadata.py')
haptics = module('lsb_haptics_patch', 'patch-ios26-keyboard-haptics.py')
require = slide.require
OBJC_IMAGE = '/usr/lib/libobjc.A.dylib'
OBJC_UUID = uuid.UUID('99860b81-03a1-3f42-a755-321cc0032934')
MASK_SYMBOL = '_objc_debug_isa_class_mask'
OLD_MASK, NEW_MASK = 0x0000000FFFFFFFF8, 0x007FFFFFFFFFFFF8
MASK_OFFSET = 0x61BB8
CHUNK = 4 * 1024 * 1024


def cache_images(data, layout):
    offset, count = slide.unpack(data, '<II', 448)
    require(count == 1353 and 456 <= offset and offset + count * 32 <= layout['signature_offset'], 'Invalid image inventory table')
    images = {}
    for index in range(count):
        address, _, _, path, _ = slide.unpack(data, '<QQQII', offset + index * 32)
        require(0 < path < layout['signature_offset'], 'Image path outside cache payload')
        end = data.find(b'\0', path, min(path + 4096, layout['signature_offset']))
        require(end > path, 'Unterminated image path')
        name = data[path:end].decode('utf-8')
        require(name.startswith('/') and name not in images, 'Invalid/duplicate image name')
        images[name] = address
    return images


def trie_export(trie, symbol):
    def uleb(position):
        result = shift = 0
        while True:
            require(position < len(trie) and shift < 64, 'Invalid export ULEB128')
            byte = trie[position]
            position += 1
            require(shift != 63 or byte <= 1, 'Overflowing export ULEB128')
            result |= (byte & 0x7F) << shift
            if not byte & 0x80:
                return result, position
            shift += 7
    node, remaining, visited = 0, symbol, set()
    while True:
        require(node not in visited, 'Cyclic export trie')
        visited.add(node)
        terminal_size, terminal = uleb(node)
        children = terminal + terminal_size
        require(children < len(trie), 'Export terminal exceeds trie')
        if not remaining:
            require(terminal_size, 'Missing export terminal')
            flags, position = uleb(terminal)
            require(flags == 0, 'Expected regular non-reexported ISA scalar')
            relative, position = uleb(position)
            require(position == children, 'Unexpected export terminal payload')
            return relative
        position, match = children + 1, None
        for _ in range(trie[children]):
            end = trie.find(b'\0', position)
            require(end >= position, 'Unterminated export edge')
            edge = trie[position:end].decode('ascii')
            child, position = uleb(end + 1)
            require(child < len(trie), 'Export child outside trie')
            if remaining.startswith(edge):
                require(edge and match is None, 'Ambiguous export prefix')
                match = child, remaining[len(edge):]
        require(match is not None, 'Required ISA scalar export missing')
        node, remaining = match


def isa_patch(data, layout, images):
    require(OBJC_IMAGE in images, 'Missing pinned libobjc')
    base = images[OBJC_IMAGE]
    start = slide.locate(layout, base, 32)
    header = slide.unpack(data, '<8I', start)
    require(header[:4] == (0xFEEDFACF, 0x100000C, 0, 6) and 0 < header[4] <= 2048 and header[4] * 8 <= header[5] <= 1024 * 1024,
            'Invalid libobjc Mach-O header')
    slide.locate(layout, base, 32 + header[5])
    cursor, end = start + 32, start + 32 + header[5]
    image_uuid = export = None
    sections = []
    for _ in range(header[4]):
        command, size = slide.unpack(data, '<II', cursor)
        require(size >= 8 and size % 8 == 0 and cursor + size <= end, 'Invalid libobjc command extent')
        if command == 0x1B:
            require(size == 24 and image_uuid is None, 'Invalid/duplicate libobjc UUID')
            image_uuid = uuid.UUID(bytes=bytes(data[cursor + 8:cursor + 24]))
        elif command == 0x80000033:
            require(size == 16 and export is None, 'Invalid/duplicate exports command')
            export = slide.unpack(data, '<II', cursor + 8)
        elif command == 0x19:
            require(size >= 72, 'Truncated libobjc segment')
            segment = data[cursor + 8:cursor + 24].rstrip(b'\0').decode('ascii')
            count, = slide.unpack(data, '<I', cursor + 64)
            require(count <= 4096 and size == 72 + count * 80, 'Invalid libobjc section table')
            for index in range(count):
                position = cursor + 72 + index * 80
                name = data[position:position + 16].rstrip(b'\0').decode('ascii')
                owner = data[position + 16:position + 32].rstrip(b'\0').decode('ascii')
                require(owner == segment, 'Section belongs to wrong segment')
                address, length = slide.unpack(data, '<QQ', position + 32)
                sections.append((segment, name, address, length))
        cursor += size
    require(cursor == end and image_uuid == OBJC_UUID and export is not None, 'Unsupported libobjc image or exports')
    offset, length = export
    require(length > 0 and offset + length <= layout['signature_offset'], 'Export trie exceeds signed payload')
    address = base + trie_export(bytes(data[offset:offset + length]), MASK_SYMBOL)
    require(any(segment == '__TEXT' and name == '__const' and begin <= address and address + 8 <= begin + size
                for segment, name, begin, size in sections), 'ISA scalar is not in libobjc __TEXT,__const')
    mask_offset = slide.locate(layout, address, 8)
    require(mask_offset == MASK_OFFSET, 'ISA scalar moved outside the reviewed native profile')
    require(slide.unpack(data, '<Q', mask_offset)[0] == OLD_MASK, 'Unexpected/already-patched ISA scalar')
    # Pinned objc_msgSend and object_getClass actually decode 0x007ffffffffffff8; only the exported debug scalar is narrow.
    for relative, instruction in ((0x2C0C, 0x927DCDD0), (0x53F8, 0x927DCD08)):
        require(slide.unpack(data, '<I', slide.locate(layout, base + relative, 4))[0] == instruction, 'Unexpected libobjc ISA decoder instruction')
    return dict(offset=mask_offset, before=struct.pack('<Q', OLD_MASK), after=struct.pack('<Q', NEW_MASK), image=OBJC_IMAGE,
                purpose='Match exported ISA class mask to actual simulator decoder', symbol=MASK_SYMBOL, address=address)


def plan_patches(data, layout):
    profile = dict(base=layout['base'], span=layout['span'], image_count=1353)
    metadata_report = metadata.inspect_cache(data, dict(metadata.PROFILE, **profile))
    patches = [dict(offset=int(site['file_offset'], 16), before=metadata.ORIGINAL, after=metadata.REPLACEMENT,
                    purpose='Retain container metadata filename compatibility', image=site['image']) for site in metadata_report['sites']]
    # This callback validates chain membership without changing the mmap, even temporarily.
    haptics_report = haptics.inspect_cache(data, profile, pointer_reader=slide.pointer_reader(data, layout))
    for site in haptics_report['sites']:
        patches.append(dict(offset=site['file_offset'], before=bytes.fromhex(site['before']), after=bytes.fromhex(site['after']),
                            purpose=site['purpose'], image=haptics.IMAGE))
    isa = isa_patch(data, layout, cache_images(data, layout))
    patches.append(isa)
    digest = hashlib.sha256()
    for position in range(0, layout['signature_offset'], CHUNK):
        digest.update(data[position:min(position + CHUNK, layout['signature_offset'])])
    payload_hash = digest.hexdigest()
    new_uuid = uuid.uuid5(uuid.NAMESPACE_URL, 'LiveSimBooter/23A343/slidable-metadata1-ahap1-isa1/' + payload_hash)
    patches.append(dict(offset=88, before=bytes(data[88:104]), after=new_uuid.bytes, image=None,
                        purpose='Distinct UUID derived from source payload and compatibility patch version'))
    patches.sort(key=lambda patch: patch['offset'])
    end = 0
    for patch in patches:
        offset, before, after = patch['offset'], patch['before'], patch['after']
        require(len(before) == len(after) and end <= offset and offset + len(before) <= layout['signature_offset'], 'Overlapping/invalid patch extent')
        require(data[offset:offset + len(before)] == before, 'Original patch bytes changed')
        owners = [row for row in layout['mappings'] if row['file_offset'] <= offset and offset + len(before) <= row['file_offset'] + row['size']]
        require(len(owners) == 1 and owners[0]['maxprot'] in (1, 5) and not owners[0]['slide_info_size'], 'Patch would modify writable slide data')
        # Slide metadata is in read-only LINKEDIT; exclude those spans explicitly as well.
        require(all(not row['slide_info_size'] or offset + len(before) <= row['slide_info_file_offset'] or
                    offset >= row['slide_info_file_offset'] + row['slide_info_size'] for row in layout['mappings']), 'Patch overlaps slide metadata')
        end = offset + len(before)
    return patches, dict(source_payload_sha256=payload_hash, old_uuid=layout['uuid'], new_uuid=str(new_uuid), metadata=metadata_report,
                         haptics=haptics_report, objc_mask={key: value for key, value in isa.items() if key not in ('before', 'after')})


def clone_or_copy(stream, output, source_size):
    libc = ctypes.CDLL(None, use_errno=True)
    clone = getattr(libc, 'fclonefileat', None)
    if clone is not None:
        clone.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        clone.restype = ctypes.c_int
        if clone(stream.fileno(), -2, os.fsencode(output), 0) == 0:  # Darwin AT_FDCWD; destination is absolute.
            return True
        error = ctypes.get_errno()
        if error not in (errno.ENOTSUP, errno.EXDEV, errno.ENOSYS, errno.EINVAL):
            raise OSError(error, 'fclonefileat failed', str(output))
    require(shutil.disk_usage(output.parent).free >= source_size + 1024 ** 3, 'Insufficient space for cache copy after CoW clone unavailable')
    stream.seek(0)
    with output.open('xb') as destination:
        original = os.fstat(destination.fileno())
        try:
            shutil.copyfileobj(stream, destination, CHUNK)
        except Exception:
            current = output.lstat()
            if (current.st_dev, current.st_ino) == (original.st_dev, original.st_ino):
                output.unlink()
            raise
    return False


def patch_cache(source, output=None, manifest=None, *, planner=plan_patches):
    source = Path(source).absolute()
    output = Path(output).absolute() if output else None
    manifest = Path(manifest).absolute() if manifest else Path(str(output) + '.patch-manifest.json') if output else None
    before = metadata.validate_paths(source, output, manifest)
    require(source.parent.resolve() == source.parent, 'Source parent must be canonical')
    if output:
        require(output.parent.resolve() == output.parent and manifest.parent.resolve() == manifest.parent, 'Output parents must be canonical')
    inventory = slide.setup.read_inventory()
    validation = slide.check_cache(source, inventory)
    created = []
    try:
        descriptor = os.open(source, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, 'rb') as stream:
            require(metadata.file_identity(os.fstat(stream.fileno())) == metadata.file_identity(before), 'Source changed before opening')
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                layout = slide.inspect_slide_layout(data)
                require(hashlib.sha256(data).hexdigest() == validation['sha256'], 'Source changed after cache validation')
                patches, details = planner(data, layout)
            report = dict(version=1, runtime_build='23A343', source=str(source), source_sha256=validation['sha256'], bytes=before.st_size,
                          preferred_base=hex(layout['base']), span=hex(layout['span']), image_count=1353, mapping_count=layout['mapping_count'],
                          exact_fixups=validation['exact_fixups'], fixup_inventory_sha256=validation['fixup_inventory_sha256'],
                          requires_separate_signing=True, fixup_tables_and_slots_unchanged=True, **details)
            report['patches'] = [dict(patch, before=patch['before'].hex(), after=patch['after'].hex()) for patch in patches]
            if output is None:
                return dict(report, validation_only=True)
            cloned = clone_or_copy(stream, output, before.st_size)
            created.append((output, output.lstat()))
            with output.open('r+b') as destination:
                for patch in patches:
                    destination.seek(patch['offset'])
                    require(destination.read(len(patch['before'])) == patch['before'], 'Cloned/copied patch bytes do not match source')
                    destination.seek(patch['offset'])
                    destination.write(patch['after'])
                os.fchmod(destination.fileno(), stat.S_IMODE(before.st_mode) & 0o777)
                destination.flush()
                os.fsync(destination.fileno())
            stream.seek(0)
            source_hash, output_hash, position = hashlib.sha256(), hashlib.sha256(), 0
            with output.open('rb') as destination:
                while block := stream.read(CHUNK):
                    source_hash.update(block)
                    expected = bytearray(block)
                    for patch in patches:
                        begin, end = max(position, patch['offset']), min(position + len(block), patch['offset'] + len(patch['after']))
                        if begin < end:
                            expected[begin - position:end - position] = patch['after'][begin - patch['offset']:end - patch['offset']]
                    actual = destination.read(len(block))
                    require(actual == expected, 'Unexpected output change at %#x' % position)
                    output_hash.update(actual)
                    position += len(block)
                require(not destination.read(1) and position == before.st_size and source_hash.hexdigest() == validation['sha256'],
                        'Source changed during patching')
            require(metadata.file_identity(os.fstat(stream.fileno())) == metadata.file_identity(before) and
                    metadata.file_identity(source.lstat()) == metadata.file_identity(before), 'Source identity changed during patching')
            report.update(output=str(output), output_sha256=output_hash.hexdigest(), cloned=cloned, every_other_byte_identical=True,
                          changed_payload_bytes=sum(sum(a != b for a, b in zip(p['before'], p['after'])) for p in patches))
            with manifest.open('x') as record:
                created.append((manifest, os.fstat(record.fileno())))
                json.dump(report, record, indent=2)
                record.write('\n')
            return report
    except Exception:
        for path, original in reversed(created):
            try:
                current = path.lstat()
                if (current.st_dev, current.st_ino) == (original.st_dev, original.st_ino):
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
    require(not args.check_only or not args.manifest, '--manifest requires --output')
    print(json.dumps(patch_cache(args.source, args.output, args.manifest), indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, struct.error) as error:
        sys.exit('patch-ios26-slidable-cache: %s' % error)
