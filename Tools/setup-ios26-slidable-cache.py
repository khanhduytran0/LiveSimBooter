#!/usr/bin/env python3
"""Build the pinned 23A343 full cache with v2 relocation chains at its stock preferred base.

Original inputs and the isolated build pipeline are shared with setup-ios26-cache.py. Only the
size/local-symbol option and simulator slide-info early return are patched; preferred-base
instructions remain stock. This produces an unsigned cache, not a portable instruction rewrite.
Apply patch-ios26-slidable-cache.py, then sign separately before device use. The native loader
currently supports the reviewed six-mapping layout only; different Mac host-support libraries
can produce a layout that needs a new audit. No device or identity-signing operations occur here.
"""

import argparse
import collections
import hashlib
import importlib.util
import json
import mmap
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('lsb_cache_setup', HERE / 'setup-ios26-cache.py')
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)
require = setup.require
BASE = 0x180000000
SPAN = 0x7E1B0000
FIXUPS, TAGGED = 15156132, 40252
PATCHES = (
    (0x12B4, 0x52800028, 0x52802028, 'local build and optimize for size; strip local symbols'),
    (0x5EB14, 0x37000360, 0xD503201F, 'remove simulator slide-info early return; preserve arm64 v2 chains'),
)
PATCHED_BUILDER_SHA256 = '5bdd2e2987c7ecc9fe6a6f08fa8cd9c4e761297119e7563f6eaab9b85782c9a5'
DELTA_MASK = 0x00FFFF0000000000
VALUE_MASK = ((1 << 64) - 1) ^ DELTA_MASK
# Reviewed native profile in ProcessSupport/SlidableCache.h: relative VM position, size, file position, slide-info position/size, flags, max, initial.
MAPPINGS = (
    (0x00000000, 0x5C594000, 0x00000000, 0, 0, 0, 5, 5),
    (0x5C594000, 0x09A84000, 0x5C594000, 0x6E504DB0, 79152, 4, 3, 1),
    (0x68018000, 0x00004000, 0x66018000, 0x6E4FC000, 48, 0x44, 3, 1),
    (0x6801C000, 0x046AC000, 0x6601C000, 0x6E4FC030, 36224, 0, 3, 3),
    (0x6E6C8000, 0x03E34000, 0x6A6C8000, 0, 0, 0x20, 1, 1),
    (0x724FC000, 0x0BCB0000, 0x6E4FC000, 0, 0, 0, 1, 1),
)


def read_aliases(inventory, root=None):
    document = json.loads((HERE / 'setup-ios26-cache-aliases.json').read_text())
    require(document['version'] == 1 and document['runtime_build'] == '23A343', 'Unsupported cache alias inventory')
    cached = {entry['install_name'] for entry in inventory['images']} | set(inventory['automatically_supplied_host_libraries'])
    sources = {entry['install_name']: entry['source'] for entry in inventory['images']}
    aliases, names = document['aliases'], set()
    require(len(aliases) == 63, 'Unexpected pinned cache alias count')
    if root is not None:
        spec = importlib.util.spec_from_file_location('lsb_runtime_paths', HERE / 'setup-ios26-runtime.py')
        runtime = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runtime)
    for alias in aliases:
        name, target = alias['path'], alias['target']
        require(name.startswith('/') and target.startswith('/'), 'Cache aliases must use absolute guest paths')
        setup.relative_path(name[1:])
        setup.relative_path(target[1:])
        require(name not in names and name not in cached and target in cached, 'Cache alias duplicates an image or has an uncached target')
        names.add(name)
        if root is not None:
            source = root / name[1:]
            require(source.is_symlink() and os.readlink(source) == alias['source_link'], 'Original cache alias changed: %s' % name)
            canonical = runtime.canonical_source(root, source)
            # Cryptex file locations can differ from LC_ID_DYLIB; compare against the pinned cache input's actual source.
            expected = runtime.canonical_source(root, root / sources.get(target, target[1:]))
            require(canonical == expected, 'Original cache alias target changed: %s' % name)
    return aliases


def trie_index(trie, path):
    def uleb(position):
        result = shift = 0
        while True:
            require(position < len(trie) and shift < 64, 'Invalid cache path-trie ULEB128')
            byte = trie[position]
            position += 1
            require(shift != 63 or byte <= 1, 'Overflowing cache path-trie ULEB128')
            result |= (byte & 0x7F) << shift
            if not byte & 0x80:
                return result, position
            shift += 7
    node, remaining, visited = 0, path, set()
    while True:
        require(node not in visited, 'Cyclic cache path trie')
        visited.add(node)
        terminal_size, terminal = uleb(node)
        children = terminal + terminal_size
        require(children < len(trie), 'Cache path-trie terminal exceeds bounds')
        if not remaining:
            if not terminal_size:
                return None
            index, end = uleb(terminal)
            require(end == children, 'Invalid cache image-index terminal')
            return index
        position, match = children + 1, None
        for _ in range(trie[children]):
            end = trie.find(b'\0', position)
            require(end >= position, 'Unterminated cache path-trie edge')
            edge = trie[position:end].decode('utf-8')
            child, position = uleb(end + 1)
            require(child < len(trie), 'Cache path-trie child outside bounds')
            if remaining.startswith(edge):
                require(edge and match is None, 'Ambiguous cache path-trie prefix')
                match = child, remaining[len(edge):]
        if match is None:
            return None
        node, remaining = match


def check_aliases(data, layout, aliases):
    address, size = unpack(data, '<QQ', 264)
    offset = locate(layout, address, size)
    trie = bytes(data[offset:offset + size])
    images_offset, image_count = unpack(data, '<II', 448)
    require(456 <= images_offset and images_offset + image_count * 32 <= layout['signature_offset'], 'Invalid cache image table')
    indices = {}
    for index in range(image_count):
        _, _, _, path, _ = unpack(data, '<QQQII', images_offset + index * 32)
        require(0 < path < layout['signature_offset'], 'Image name outside payload')
        end = data.find(b'\0', path, min(path + 4096, layout['signature_offset']))
        require(end > path, 'Unterminated image name')
        name = data[path:end].decode('utf-8')
        require(name not in indices, 'Duplicate image name')
        indices[name] = index
    for alias in aliases:
        target = indices.get(alias['target'])
        require(target is not None and trie_index(trie, alias['target']) == target, 'Canonical alias target is absent from cache trie')
        require(trie_index(trie, alias['path']) == target, 'Missing or incorrect cached alias: %s' % alias['path'])
    return len(aliases)


def unpack(data, fmt, offset):
    require(0 <= offset <= len(data) - struct.calcsize(fmt), 'Cache field outside file at %#x' % offset)
    return struct.unpack_from(fmt, data, offset)


def locate(layout, address, length=1):
    require(length > 0, 'Empty address range')
    matches = [row for row in layout['mappings'] if row['address'] <= address and address + length <= row['address'] + row['size']]
    require(len(matches) == 1, 'Address does not fit one cache mapping')
    return matches[0]['file_offset'] + address - matches[0]['address']


def inspect_slide_layout(data, *, expected_mappings=MAPPINGS, expected_span=SPAN):
    require(len(data) >= 456 and data[:16] == b'dyld_v1   arm64\0', 'Expected complete arm64 cache header')
    base, span, max_slide = unpack(data, '<QQQ', 224)
    require(base == BASE and span == expected_span and max_slide == 0x20000000, 'Unreviewed preferred cache base/span/maxSlide')
    platform, flags = unpack(data, '<II', 216)
    require(platform == 7 and flags & 0x200 and unpack(data, '<I', 396)[0] == 0, 'Expected single-file simulator cache')
    cache_uuid = uuid.UUID(bytes=bytes(data[88:104]))
    require(cache_uuid.int != 0, 'Invalid cache UUID')
    signature, signature_size = unpack(data, '<QQ', 40)
    require(signature > 0 and not signature % 16384 and signature_size > 0 and signature + signature_size == len(data), 'Invalid signature extent')
    legacy_offset, legacy_count = unpack(data, '<II', 16)
    slide_offset, slide_count = unpack(data, '<II', 312)
    require(legacy_count == slide_count == len(expected_mappings), 'Unreviewed mapping count')
    require(456 <= legacy_offset and legacy_offset + legacy_count * 32 <= signature and
            456 <= slide_offset and slide_offset + slide_count * 56 <= signature, 'Invalid mapping-table bounds')
    mappings = []
    for index, expected in enumerate(expected_mappings):
        address, size, offset, info_offset, info_size, map_flags, maximum, initial = unpack(data, '<6QII', slide_offset + index * 56)
        require((address - base, size, offset, info_offset, info_size, map_flags, maximum, initial) == expected,
                'Unreviewed mapping %d; audit Mac host-support inputs and native layout profile' % index)
        require(unpack(data, '<QQQII', legacy_offset + index * 32) == (address, size, offset, maximum, initial), 'Mapping tables disagree')
        require(size and base <= address < address + size <= base + span and offset + size <= signature and
                (address | size | offset) & 0x3FFF == 0 and initial & ~maximum == 0 and maximum & ~7 == 0, 'Invalid mapping extent/protection')
        require(all(address + size <= row['address'] or address >= row['address'] + row['size'] for row in mappings), 'Overlapping VM mappings')
        require(all(offset + size <= row['file_offset'] or offset >= row['file_offset'] + row['size'] for row in mappings), 'Overlapping file mappings')
        row = dict(index=index, address=address, size=size, file_offset=offset, slide_info_file_offset=info_offset,
                   slide_info_size=info_size, flags=map_flags, maxprot=maximum, initprot=initial)
        if info_size:
            require(maximum == 3 and info_size >= 40 and info_offset + info_size <= signature, 'Invalid writable mapping slide metadata')
            version, page_size, starts, count, extras, extra_count, mask, value_add = unpack(data, '<6I2Q', info_offset)
            require(version == 2 and page_size == 4096 and count * page_size == size, 'Unsupported slide version/page coverage')
            require(starts >= 40 and not starts % 2 and starts + count * 2 <= info_size and extras == extra_count == 0,
                    'Unsupported page-start/extras table')
            require(mask == DELTA_MASK and value_add == 0, 'Unsupported slide pointer encoding')
            row.update(page_size=page_size, starts_offset=starts, starts_count=count, delta_mask=mask, value_add=value_add)
        else:
            require(not info_offset and not maximum & 2, 'Writable mapping is missing relocation metadata')
        mappings.append(row)
    require(mappings[0]['address'] == base and mappings[0]['file_offset'] == 0, 'Cache header mapping mismatch')
    for row in mappings:
        if row['slide_info_size']:
            matches = [owner for owner in mappings if owner['file_offset'] <= row['slide_info_file_offset'] and
                       row['slide_info_file_offset'] + row['slide_info_size'] <= owner['file_offset'] + owner['size']]
            require(len(matches) == 1 and matches[0]['maxprot'] == 1, 'Slide metadata must be in one read-only mapping')
    return dict(uuid=str(cache_uuid), base=base, span=span, max_slide=max_slide, signature_offset=signature,
                signature_size=signature_size, mapping_count=len(mappings), mappings=mappings)


def page_fixups(data, layout, row, page):
    require(0 <= page < row['starts_count'], 'Slide page outside mapping')
    start, = unpack(data, '<H', row['slide_info_file_offset'] + row['starts_offset'] + page * 2)
    if start == 0x4000:
        return
    require(not start & 0xC000, 'Unsupported extra or reserved page-start entry')
    within = start * 4
    while True:
        require(within % 4 == 0 and within + 8 <= row['page_size'], 'Slide pointer crosses page boundary')
        slot = row['file_offset'] + page * row['page_size'] + within
        raw, = unpack(data, '<Q', slot)
        value = raw & VALUE_MASK
        target = value & 0x00FFFFFFFFFFFFFF
        require(not value or layout['base'] <= target < layout['base'] + layout['span'], 'Slide pointer target is outside cache')
        yield slot, value
        delta = (raw & DELTA_MASK) >> 38
        if not delta:
            break
        require(delta >= 8, 'Overlapping slide pointer slots')
        within += delta


def iter_fixups(data, layout):
    last = -8
    for row in layout['mappings']:
        if not row['slide_info_size']:
            continue
        for page in range(row['starts_count']):
            for slot, value in page_fixups(data, layout, row, page):
                require(slot >= last + 8, 'Unsorted/overlapping slide pointer slots')
                last = slot
                yield row['index'], slot, value


def pointer_reader(data, layout):
    def read(slot):
        matches = [row for row in layout['mappings'] if row['file_offset'] <= slot and slot + 8 <= row['file_offset'] + row['size']]
        require(len(matches) == 1 and matches[0]['slide_info_size'], 'Pointer is outside a writable slide mapping')
        row = matches[0]
        page = (slot - row['file_offset']) // row['page_size']
        for candidate, value in page_fixups(data, layout, row, page):
            if candidate == slot:
                return value
        raise ValueError('Pointer is not a member of a validated slide chain')
    return read


def check_cache(path, inventory, *, aliases=()):
    report = setup.check_cache(path, inventory, expected_base=BASE, shared_region_limit=None)
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        layout = inspect_slide_layout(data)
        counts = [collections.Counter(fixups=0, tagged=0, null=0) for _ in layout['mappings']]
        digest = hashlib.sha256()
        for index, slot, value in iter_fixups(data, layout):
            counts[index]['fixups'] += 1
            counts[index]['tagged'] += bool(value >> 56)
            counts[index]['null'] += not value
            digest.update(struct.pack('<QQ', slot, value))
        totals = sum(counts, collections.Counter())
        require(totals['fixups'] == FIXUPS and totals['tagged'] == TAGGED and totals['null'] == 0,
                'Unreviewed relocation counts; audit Mac host-support inputs and native slide profile')
        for row, count in zip(layout['mappings'], counts):
            row.update(count)
        alias_count = check_aliases(data, layout, aliases) if aliases else 0
    report.update(slide_layout=layout, exact_fixups=dict(totals), fixup_inventory_sha256=digest.hexdigest(),
                  cache_aliases_checked=alias_count,
                  note='Reviewed full-cache layout with exact v2 relocation chains; apply compatibility patches and identity-sign separately. '
                       'No device execution or signature acceptance is implied. Mac host inputs are recorded by the build manifest.')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-root', type=Path, help='Original iOS Simulator 26.0 build 23A343 RuntimeRoot')
    parser.add_argument('--output-dir', type=Path, help='New isolated output directory; never overwritten')
    parser.add_argument('--validate-only', action='store_true', help='Validate original inputs without building')
    parser.add_argument('--check-cache', type=Path, help='Validate an existing full slidable cache without patching/signing')
    parser.add_argument('--include-cache-aliases', action='store_true', help='Restore 63 original cached aliases in the path trie; require coverage with --check-cache')
    args = parser.parse_args()
    inventory = setup.read_inventory()
    if args.check_cache:
        require(not args.runtime_root and not args.output_dir and not args.validate_only, '--check-cache cannot be combined with build options')
        aliases = read_aliases(inventory) if args.include_cache_aliases else ()
        report = check_cache(args.check_cache.resolve(strict=True), inventory, aliases=aliases)
    else:
        require(args.runtime_root and (args.validate_only or args.output_dir), 'Provide --runtime-root and --output-dir, or --validate-only')
        require(not args.validate_only or not args.output_dir, '--validate-only does not create output')
        root = args.runtime_root.resolve(strict=True)
        report = setup.original_inputs(root, inventory)
        aliases = read_aliases(inventory, root) if args.include_cache_aliases else ()
        if aliases:
            report['cache_aliases'] = aliases
            report['cache_alias_inventory_sha256'] = setup.sha(HERE / 'setup-ios26-cache-aliases.json')
            report['cache_alias_registration'] = 'Post-build path-trie repair; the original simulator driver does not submit symlinks'
        if not args.validate_only:
            output = args.output_dir.resolve()
            report = setup.build(root, output, inventory, report, base=BASE, patches=PATCHES,
                                 patched_builder_sha256=PATCHED_BUILDER_SHA256, validator=check_cache)
            if aliases:
                spec = importlib.util.spec_from_file_location('lsb_cache_alias_patch', HERE / 'patch-ios26-cache-aliases.py')
                alias_patch = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(alias_patch)
                (output / 'aliases').mkdir()
                destination = output / 'aliases/dyld_sim_shared_cache_arm64'
                alias_patch.patch_cache(output / 'unsigned/dyld_sim_shared_cache_arm64', destination)
                report = check_cache(destination, inventory, aliases=aliases)
                (output / 'alias-validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, struct.error, subprocess.CalledProcessError) as error:
        sys.exit('setup-ios26-slidable-cache: %s' % error)
