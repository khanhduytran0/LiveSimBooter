#!/usr/bin/env python3
"""Build the pinned iOS 26.0/23A343 graphical cache at 0x110000000; no identity signing or device operations."""

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys
import time


HERE = Path(__file__).resolve().parent
BASE = 0x110000000
SHARED_REGION = 0x180000000
PATCHES = (
    (0x5EA5C, 0xB26107E8, 0xD2C00028, 'mov x8, #0x100000000'),
    (0x5EA60, 0xD2C00029, 0xB2640108, 'orr x8, x8, #0x10000000'),
    (0x5EA64, 0x9A881129, 0xD2C00029, 'mov x9, #0x100000000; retain builder layout capacity'),
    (0x012B4, 0x52800028, 0x52802028, 'mov w8, #0x101; local build and optimize for size'),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def relative_path(text):
    path = PurePosixPath(text)
    require(text and not path.is_absolute() and all(part not in ('', '.', '..') for part in text.split('/')), 'Invalid relative input path')
    return path


def is_within(path, directory):
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False


def read_inventory():
    manifest = json.loads((HERE / 'setup-ios26-cache-inputs.json').read_text())
    require(manifest['version'] == 1 and manifest['runtime_build'] == '23A343' and manifest['cache_base'] == hex(BASE), 'Unsupported inventory')
    names = set()
    for record in manifest['images']:
        relative_path(record['source'])
        require(record['install_name'].startswith('/'), 'Install names must be absolute')
        relative_path(record['install_name'][1:])
        require(record['install_name'] not in names, 'Duplicate install name')
        names.add(record['install_name'])
    hosts = manifest['automatically_supplied_host_libraries']
    require(len(hosts) == 3 and not names.intersection(hosts), 'Invalid automatic host inputs')
    require(len(names) + len(hosts) == manifest['expected_image_count'] == 1353, 'Unexpected image inventory count')
    return manifest


def original_inputs(root, inventory):
    require(root.name == 'RuntimeRoot', 'Source must be the original simulator RuntimeRoot')
    version = root / 'System/Library/CoreServices/SystemVersion.plist'
    if version.is_file():
        import plistlib
        info = plistlib.loads(version.read_bytes())
        require(info.get('ProductBuildVersion') == '23A343' and info.get('ProductVersion') == '26.0', 'Requires iOS 26.0 build 23A343')
    builder = root.parent / 'update_dyld_sim_shared_cache'
    require(sha(builder) == inventory['builder_sha256'], 'Original 23A343 builder hash mismatch')
    require(sha(root / 'usr/lib/dyld_sim') == inventory['dyld_sim_sha256'], 'Original 23A343 dyld_sim hash mismatch')
    total = 0
    for record in inventory['images']:
        source = (root / record['source']).resolve(strict=True)
        require(is_within(source, root), f'Input escapes source RuntimeRoot: {record["source"]}')
        require(source.is_file() and source.stat().st_size == record['size'] and sha(source) == record['sha256'],
                f'Original 23A343 image mismatch: {record["source"]}')
        total += record['size']
    # The stock simulator builder opens these files directly on the Mac, outside -root.
    # They are deliberately recorded rather than mislabeled as fixed runtime inputs.
    hosts = []
    for name in inventory['automatically_supplied_host_libraries']:
        source = Path(name)
        require(source.is_file(), f'Mac host-support dylib missing: {name}')
        hosts.append(dict(path=name, size=source.stat().st_size, sha256=sha(source)))
    return dict(runtime_build='23A343', runtime_version='26.0', source_root=str(root), input_count=len(inventory['images']),
                input_bytes=total, builder=str(builder), builder_sha256=inventory['builder_sha256'], host_libraries=hosts)


def patch_builder(data, inventory, *, patches=PATCHES, expected_sha256=None):
    require(hashlib.sha256(data).hexdigest() == inventory['builder_sha256'], 'Builder hash mismatch')
    patched = bytearray(data)
    for offset, before, after, _ in patches:
        require(struct.unpack_from('<I', data, offset)[0] == before, f'Builder instruction mismatch at {offset:#x}')
        struct.pack_into('<I', patched, offset, after)
    expected = expected_sha256 if expected_sha256 is not None else inventory['patched_builder_sha256_before_signing']
    require(hashlib.sha256(patched).hexdigest() == expected, 'Patched builder hash mismatch')
    return patched


def check_cache(path, inventory, *, expected_base=BASE, shared_region_limit=SHARED_REGION):
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        def unpack(fmt, offset):
            require(0 <= offset <= len(data) - struct.calcsize(fmt), f'Cache field outside file at {offset:#x}')
            return struct.unpack_from(fmt, data, offset)

        def string(offset):
            require(0 <= offset < len(data), 'Cache string outside file')
            end = data.find(b'\0', offset, min(len(data), offset + 4096))
            require(end >= offset, 'Unterminated cache image path')
            return data[offset:end].decode('utf-8')

        require(data[:16] == b'dyld_v1   arm64\0', 'Expected arm64 cache')
        base, span = unpack('<QQ', 224)
        require(base == expected_base and 0 < span < (1 << 32), 'Unexpected cache base/span')
        require(unpack('<I', 216)[0] == 7 and unpack('<I', 396)[0] == 0, 'Expected a single-file iOS simulator cache')
        offset, count = unpack('<II', 16)
        require(0 < count <= 64, 'Invalid cache mapping count')
        mappings = []
        for index in range(count):
            start, size, file_offset, maximum, initial = unpack('<QQQII', offset + index * 32)
            require(size and base <= start < start + size <= base + span, 'Mapping outside cache reservation')
            require(file_offset + size <= len(data) and initial & ~maximum == 0 and maximum & ~7 == 0, 'Invalid mapping extent/protection')
            if shared_region_limit is not None:
                require(not (maximum & 6) or start + size <= shared_region_limit, 'Executable/writable cache mapping crosses native shared region')
            require(all(start + size <= other['start'] or start >= other['end'] for other in mappings), 'Overlapping cache mappings')
            mappings.append(dict(start=start, end=start + size, size=size, file_offset=file_offset, maximum=maximum, initial=initial))
        require(any(region['initial'] & 4 for region in mappings), 'No executable mapping')

        def file_offset(address):
            for region in mappings:
                if region['start'] <= address < region['end']:
                    return region['file_offset'] + address - region['start']
            raise ValueError(f'Cached image address outside mappings: {address:#x}')

        image_offset, count = unpack('<II', 448)
        require(count == inventory['expected_image_count'], 'Unexpected cache image count')
        names = set()
        local_symbols = exports = function_starts = 0
        for index in range(count):
            address, _, _, name_offset, _ = unpack('<QQQII', image_offset + index * 32)
            name = string(name_offset)
            require(name not in names, 'Duplicate cached image')
            names.add(name)
            position = file_offset(address)
            header = unpack('<8I', position)
            require(header[0] == 0xFEEDFACF and header[1] == 0x100000C and header[3] == 6, f'Invalid cached arm64 dylib: {name}')
            end = position + 32 + header[5]
            require(end <= len(data), 'Cached load commands exceed file')
            position += 32
            for _ in range(header[4]):
                command, size = unpack('<II', position)
                require(size >= 8 and position + size <= end, 'Invalid cached load-command bounds')
                if command == 0xB:
                    require(size >= 80, 'Truncated dynamic symbol table')
                    local_symbols += unpack('<I', position + 12)[0]
                elif command == 0x80000033:
                    require(size >= 16, 'Truncated exports command')
                    exports += unpack('<I', position + 12)[0] != 0
                elif command in (0x22, 0x80000022):
                    require(size >= 48, 'Truncated dyld-info command')
                    exports += unpack('<I', position + 44)[0] != 0
                elif command == 0x26:
                    require(size >= 16, 'Truncated function-starts command')
                    function_starts += unpack('<I', position + 12)[0] != 0
                position += size
            require(position == end, 'Cached command count does not match size')
        expected = {item['install_name'] for item in inventory['images']} | set(inventory['automatically_supplied_host_libraries'])
        require(names == expected, f'Cache image inventory mismatch: missing={sorted(expected - names)}, extra={sorted(names - expected)}')
        require(local_symbols == 0 and exports == count and function_starts == count, 'Required stripped-cache metadata does not match')
        return dict(cache=str(path), runtime_build='23A343', base=hex(base), reservation_end=hex(base + span),
                    bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), images=count, local_symbols=local_symbols,
                    export_tries=exports, function_starts=function_starts, mappings=mappings,
                    all_code_and_writable_mappings_below=hex(shared_region_limit) if shared_region_limit is not None else None,
                    signature_validation='not_performed',
                    note='Offline layout validation only. Signature acceptance and execution require separate device validation.')


def build(root, output, inventory, inputs, *, base=BASE, patches=PATCHES, patched_builder_sha256=None, validator=check_cache):
    require(not os.path.lexists(output), 'Output already exists; choose a new output directory')
    require(not is_within(output, root), 'Output must not be inside the original RuntimeRoot')
    output.parent.mkdir(parents=True, exist_ok=True)
    require(shutil.disk_usage(output.parent).free >= inputs['input_bytes'] + 5 * 1024 ** 3, 'Insufficient space for isolated inputs and cache')
    patched = patch_builder(Path(inputs['builder']).read_bytes(), inventory, patches=patches, expected_sha256=patched_builder_sha256)
    output.mkdir()
    stage = output / 'root'
    for record in inventory['images']:
        destination = stage / record['install_name'][1:]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / record['source'], destination)
        require(sha(destination) == record['sha256'], f'Staged source changed during copy: {record["source"]}')
    loader = stage / 'usr/lib/dyld_sim'
    loader.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / 'usr/lib/dyld_sim', loader)
    require(sha(loader) == inventory['dyld_sim_sha256'], 'Staged dyld_sim changed during copy')
    builder = output / 'update_dyld_sim_shared_cache'
    builder.write_bytes(patched)
    builder.chmod(0o755)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', builder], check=True)
    command = [str(builder), '-root', str(stage), '-cache_dir', str(output / 'unsigned'), '-iOS', '-arch', 'arm64',
               '-force', '-verbose', '-time-passes', '-stats']
    manifest = dict(inputs, base=hex(base), expected_images=sorted(
        {item['install_name'] for item in inventory['images']} | set(inventory['automatically_supplied_host_libraries'])),
        inventory_sha256=sha(HERE / 'setup-ios26-cache-inputs.json'), command=command,
        patched_builder_sha256_before_signing=hashlib.sha256(patched).hexdigest(),
        patches=[dict(offset=hex(o), before=hex(b), after=hex(a), instruction=s) for o, b, a, s in patches],
        cache_identity_signed=False, device_modified=False)
    (output / 'stage-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    started = time.monotonic()
    with (output / 'build.log').open('xb') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    (output / 'build-result.json').write_text(json.dumps(dict(exit_code=result.returncode, seconds=time.monotonic() - started), indent=2) + '\n')
    require(result.returncode == 0, f'Cache build failed; inspect {output / "build.log"}')
    for host in inputs['host_libraries']:
        require(sha(Path(host['path'])) == host['sha256'], f'Host library changed during build: {host["path"]}')
    report = validator(output / 'unsigned/dyld_sim_shared_cache_arm64', inventory)
    (output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-root', type=Path, help='Original iOS 26.0 build 23A343 RuntimeRoot')
    parser.add_argument('--output-dir', type=Path, help='New isolated output directory; never overwritten')
    parser.add_argument('--validate-only', action='store_true', help='Check original inputs and Mac host-support files; do not build')
    parser.add_argument('--check-cache', type=Path, help='Validate an existing unsigned or signed cache without rebuilding')
    args = parser.parse_args()
    inventory = read_inventory()
    if args.check_cache:
        require(not args.runtime_root and not args.output_dir and not args.validate_only, '--check-cache cannot be combined with build options')
        report = check_cache(args.check_cache.resolve(strict=True), inventory)
    else:
        require(args.runtime_root and (args.validate_only or args.output_dir), 'Provide --runtime-root and --output-dir, or --validate-only')
        require(not args.validate_only or not args.output_dir, '--validate-only does not create output')
        root = args.runtime_root.resolve(strict=True)
        report = original_inputs(root, inventory)
        if not args.validate_only:
            report = build(root, args.output_dir.resolve(), inventory, report)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, struct.error, subprocess.CalledProcessError) as error:
        sys.exit(f'setup-ios26-cache: {error}')
