#!/usr/bin/env python3
"""Experimental 23A343 cache with placeholders for the three native support libraries.

Keeps all 1,350 pinned guest images, including libsystem_sim_*. The three Mac support
implementations are replaced by inert export records. This cache CANNOT boot until
dyld can override these records with native implementations. No signing for a device,
deployment, or change to the working cache profile is performed here.
"""

import argparse
import collections
import importlib.util
import json
import mmap
from pathlib import Path
import struct
import subprocess
import sys


HERE = Path(__file__).resolve().parent


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


slide = module('lsb_placeholder_slide', 'setup-ios26-slidable-cache.py')
setup = slide.setup
trie = module('lsb_placeholder_trie', 'prepare-native-metal.py')
require = setup.require
SUPPORT = tuple('/usr/lib/system/libsystem_%s.dylib' % name for name in ('kernel', 'platform', 'pthread'))
# Exact original 23A343 builder, not offsets inferred from newer source. loadMRMFiles
# 0x18d4 branches past the hardcoded Mac-input loop to skipDirs initialization at 0x1994.
# optimizeTLVs starts at 0x89b74. Newer Apple's B&I path skips this pass for simulator
# placeholders; this older binary lacks that guard, so return before touching pthread globals.
# See docs/native-libsystem.md for source provenance and disassembly evidence.
PATCHES = slide.PATCHES + (
    (0x18D4, 0xF000113B, 0x14000030, 'skip automatic Mac host-support inputs; use staged placeholders'),
    (0x89B74, 0x6DB923E9, 0xD65F03C0, 'skip static TLV optimization; native pthread key range is unknown'),
)
PATCHED_BUILDER_SHA256 = '8cc052c9414c9690952d25c2c8b78ba6498cf163cb113f692e180ec6d47d2d3c'


def thin_arm64(data):
    if data[:4] in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        wide = data[:4] == b'\xca\xfe\xba\xbf'
        count, = slide.unpack(data, '>I', 4)
        require(0 < count <= 32, 'Invalid universal architecture count')
        candidates = []
        for index in range(count):
            cpu, subtype, offset, length, *_ = slide.unpack(data, '>IIQQII' if wide else '>IIIII', 8 + index * (32 if wide else 20))
            require(offset + length <= len(data), 'Universal slice exceeds file')
            if cpu == 0x100000C and subtype == 0:
                candidates.append(data[offset:offset + length])
        require(len(candidates) == 1, 'Expected exactly one plain arm64 slice')
        data = candidates[0]
    require(slide.unpack(data, '<4I', 0) == (0xFEEDFACF, 0x100000C, 0, 6), 'Expected plain arm64 dylib')
    return data


def image(data, header=0):
    magic, cpu, subtype, kind, count, size = slide.unpack(data, '<6I', header)
    require((magic, cpu, subtype, kind) == (0xFEEDFACF, 0x100000C, 0, 6), 'Expected plain arm64 dylib header')
    end = header + 32 + size
    require(end <= len(data), 'Truncated image commands')
    commands, sections = {}, []
    cursor = header + 32
    for _ in range(count):
        command, length = slide.unpack(data, '<II', cursor)
        require(length >= 8 and length % 8 == 0 and cursor + length <= end, 'Invalid image command')
        commands.setdefault(command, []).append(cursor)
        if command == 0x19:
            require(length >= 72, 'Truncated image segment')
            nsects, = slide.unpack(data, '<I', cursor + 64)
            require(72 + 80 * nsects <= length, 'Truncated image sections')
            for index in range(nsects):
                entry = cursor + 72 + 80 * index
                section, segment, address, length_bytes, offset = slide.unpack(data, '<16s16sQQI', entry)
                sections.append((segment.rstrip(b'\0'), section.rstrip(b'\0'), address, length_bytes, offset))
        cursor += length
    require(cursor == end, 'Image command count mismatch')
    return commands, sections


def exports(data, commands):
    if 0x80000033 in commands:
        offset, size = slide.unpack(data, '<II', trie.one(commands, 0x80000033) + 8)
    else:
        kind = 0x80000022 if 0x80000022 in commands else 0x22
        offset, size = slide.unpack(data, '<II', trie.one(commands, kind) + 40)
    require(size and offset + size <= len(data), 'Invalid export trie extent')
    return trie.decode_exports(bytes(data[offset:offset + size]))


def identity(data, commands):
    command = trie.one(commands, 0xD)
    size, name_offset, _, current, compatible = slide.unpack(data, '<5I', command + 4)
    require(24 <= name_offset < size, 'Invalid install-name offset')
    name = bytes(data[command + name_offset:command + size]).split(b'\0', 1)[0].decode()
    return name, current, compatible


def validate_placeholder(data, expected, header=0):
    commands, sections = image(data, header)
    require(identity(data, commands) == (expected['path'], expected['current_version'], expected['compatibility_version']),
            'Placeholder identity/version mismatch')
    require(slide.unpack(data, '<I', trie.one(commands, 0x32) + 8)[0] == 7, 'Placeholder is not iOS Simulator')
    require(not any(kind in commands for kind in (0xC, 0x80000018, 0x8000001F, 0x80000023, 0x20, 0x11, 0x1A)),
            'Placeholder must not have dependencies or initializers')
    require(slide.unpack(data, '<I', trie.one(commands, 0xB) + 28)[0] == 0, 'Placeholder has undefined symbols')
    require(len(sections) == 1 and sections[0][:2] == (b'__TEXT', b'__text'), 'Placeholder contains non-placeholder sections')
    _, _, address, size, offset = sections[0]
    names = exports(data, commands)
    require(sorted(name.decode() for name in names) == expected['exports'], 'Placeholder export names changed')
    require(size == len(names) * 4 and offset + size <= len(data) and not any(data[offset:offset + size]),
            'Placeholder body must contain one zero word per export')
    # Cached export offsets are relative to this Mach-O header's VM address, not its file offset.
    text = [item for item in commands[0x19] if bytes(data[item + 8:item + 24]).rstrip(b'\0') == b'__TEXT']
    require(len(text) == 1, 'Expected one TEXT segment')
    text_address, = slide.unpack(data, '<Q', text[0] + 24)
    addresses = []
    for terminal in names.values():
        flags, cursor = trie.read_uleb(terminal, 0)
        value, cursor = trie.read_uleb(terminal, cursor)
        require(flags == 0 and cursor == len(terminal), 'Unexpected placeholder export kind')
        addresses.append(text_address + value)
    require(set(addresses) == set(range(address, address + size, 4)), 'Placeholder exports overlap or do not cover their words')
    return dict(path=expected['path'], exports=len(names), implementation_bytes=0, placeholder_bytes=size)


def version(value):
    return '%d.%d.%d' % (value >> 16, (value >> 8) & 255, value & 255)


def tlv_slots(data, commands, layout):
    slots = []
    for segment in commands.get(0x19, []):
        count, = slide.unpack(data, '<I', segment + 64)
        for index in range(count):
            section = segment + 72 + index * 80
            flags, = slide.unpack(data, '<I', section + 64)
            if flags & 255 != 0x13:  # S_THREAD_LOCAL_VARIABLES: three pointer-sized words per descriptor.
                continue
            address, size = slide.unpack(data, '<QQ', section + 32)
            require(size and size % 24 == 0, 'Invalid TLV descriptor section')
            offset = slide.locate(layout, address, size)
            for slot in range(offset, offset + size, 24):
                require(slide.unpack(data, '<Q', slot + 8)[0] == 0, 'TLV descriptor uses a fixed pthread key')
                slots.append(slot)
    return slots


def prepare_stage(stage, output):
    directory = output / 'placeholders'
    directory.mkdir()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphonesimulator', '--show-sdk-path'], text=True).strip()
    compiler = subprocess.check_output(['xcrun', 'clang', '--version'], text=True).strip()
    records = []
    for name in SUPPORT:
        source = Path(name)
        data = thin_arm64(source.read_bytes())
        commands, _ = image(data)
        install_name, current, compatible = identity(data, commands)
        require(install_name == name, 'Mac support install name changed')
        names = sorted(symbol.decode('ascii') for symbol in exports(data, commands))
        require(names and all(symbol.startswith('_') and all(c.isalnum() or c in '_.$' for c in symbol) for symbol in names),
                'Unsupported export name in assembly input')
        record = dict(path=name, export_source_sha256=setup.sha(source), current_version=current, compatibility_version=compatible, exports=names)
        assembly = directory / (source.stem + '.s')
        # Like Apple's synthesizeHostDylib: data names also get inert function atoms.
        # Distinct entries are essential: cache patch tables identify exports by implementation offset.
        assembly.write_text('.text\n.p2align 2\n' + ''.join('.globl "%s"\n"%s":\n.long 0\n' % (symbol, symbol) for symbol in names) +
                            '.subsections_via_symbols\n')
        destination = stage / name[1:]
        require(not destination.exists(), 'Refusing to replace a staged guest image')
        destination.parent.mkdir(parents=True, exist_ok=True)
        command = ['xcrun', 'clang', '-target', 'arm64-apple-ios26.0-simulator', '-isysroot', sdk, '-dynamiclib', '-nostdlib',
                   '-Wl,-add_split_seg_info', '-Wl,-no_deduplicate', '-Wl,-install_name,' + name,
                   '-Wl,-current_version,' + version(current), '-Wl,-compatibility_version,' + version(compatible),
                   '-o', str(destination), str(assembly)]
        subprocess.run(command, check=True)
        contents = destination.read_bytes()
        require(len(contents) >= 8192, 'Placeholder is too small for the builder root scanner')
        placeholder_commands, _ = image(contents)
        split, split_size = slide.unpack(contents, '<II', trie.one(placeholder_commands, 0x1E) + 8)
        require(split_size and contents[split] == 0x7F, 'Placeholder needs v2 split-segment metadata')
        validate_placeholder(contents, record)
        record.update(sha256=setup.sha(destination), bytes=len(contents), command=command)
        records.append(record)
    manifest = dict(compiler=compiler, records=records, executable=False, requires_native_override=True)
    (output / 'placeholder-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return dict(support_placeholders=manifest, mac_support_implementations_included=False, static_tlv_optimization=False)


def check_cache(path, inventory):
    report = setup.check_cache(path, inventory, expected_base=slide.BASE, shared_region_limit=None)
    manifest = json.loads((path.parent.parent / 'placeholder-manifest.json').read_text())
    expected = {item['path']: item for item in manifest['records']}
    require(set(expected) == set(SUPPORT), 'Unexpected placeholder manifest')
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        # This is a new experimental profile: inspect its structural validity without changing
        # the exact accepted production layout in SlidableCache.h or the ordinary cache validator.
        base, span = slide.unpack(data, '<QQ', 224)
        offset, count = slide.unpack(data, '<II', 312)
        require(0 < count <= 64, 'Invalid experimental mapping count')
        rows = [slide.unpack(data, '<6QII', offset + index * 56) for index in range(count)]
        layout = slide.inspect_slide_layout(data, expected_span=span, expected_mappings=tuple((r[0] - base,) + r[1:] for r in rows))
        image_offset, count = slide.unpack(data, '<II', 448)
        placeholders, tlvs, tlv_images, bootstrap = [], set(), 0, None
        for index in range(count):
            address, _, _, name_offset, _ = slide.unpack(data, '<QQQII', image_offset + index * 32)
            end = data.find(b'\0', name_offset, name_offset + 4096)
            require(end > name_offset, 'Invalid cache image name')
            name = data[name_offset:end].decode()
            header = slide.locate(layout, address)
            commands, _ = image(data, header)
            if name in expected:
                placeholders.append(validate_placeholder(data, expected[name], header))
            slots = tlv_slots(data, commands, layout)
            tlvs.update(slots)
            tlv_images += bool(slots)
            if name == '/usr/lib/system/libdyld.dylib':
                terminal = exports(data, commands)[b'__tlv_bootstrap']
                flags, cursor = trie.read_uleb(terminal, 0)
                value, cursor = trie.read_uleb(terminal, cursor)
                require(flags == 0 and cursor == len(terminal), 'Unexpected TLV bootstrap export kind')
                bootstrap = address + value
        require(len(placeholders) == 3, 'Missing cached placeholders')
        require(bootstrap is not None and tlvs, 'Missing libdyld bootstrap or TLV descriptors')
        pending_tlvs = set(tlvs)
        counts = collections.Counter()
        for _, slot, value in slide.iter_fixups(data, layout):
            counts['fixups'] += 1
            counts['tagged'] += bool(value >> 56)
            counts['null'] += not value
            if slot in pending_tlvs:
                require(value == bootstrap, 'TLV descriptor bypasses dynamic initialization')
                pending_tlvs.remove(slot)
        require(counts['fixups'] and not counts['null'], 'Missing or null slide fixups')
        require(not pending_tlvs, 'TLV descriptor is missing a slide fixup')
    report.update(slide_layout=layout, exact_fixups=dict(counts), placeholders=placeholders,
                  unoptimized_tlv_descriptors=len(tlvs), images_with_tlvs=tlv_images,
                  mac_support_implementations_included=False, static_tlv_optimization=False, deployable=False,
                  note='Offline experimental cache only. Native-image override and initialization ownership remain unimplemented; do not deploy.')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-root', required=True, type=Path, help='Original iOS Simulator 26.0/23A343 RuntimeRoot')
    parser.add_argument('--output-dir', required=True, type=Path, help='New output directory, never overwritten')
    args = parser.parse_args()
    inventory = setup.read_inventory()
    root = args.runtime_root.resolve(strict=True)
    inputs = setup.original_inputs(root, inventory)
    report = setup.build(root, args.output_dir.resolve(), inventory, inputs, base=slide.BASE, patches=PATCHES,
                         patched_builder_sha256=PATCHED_BUILDER_SHA256, validator=check_cache, prepare_stage=prepare_stage)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, struct.error, subprocess.CalledProcessError) as error:
        sys.exit('setup-ios26-placeholder-cache: %s' % error)
