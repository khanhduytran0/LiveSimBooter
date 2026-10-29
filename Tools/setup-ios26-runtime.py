#!/usr/bin/env python3
"""Inventory or stage matching iOS 26.0 (23A343) runtime resources and uncached code.

Default mode is a read-only inventory. --stage signs into a new directory; it never
installs, modifies the source runtime, registers apps, or claims a complete boot image.
Patched dyld_sim/launchd and locally built guest helpers are explicit --add-binary inputs.
"""
import argparse
import collections
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

REPOSITORY = Path(__file__).resolve().parent.parent
DEFAULT_SCOPES = ('Applications', 'System/Applications', 'System/Library', 'System/Cryptexes', 'Library', 'usr', 'bin', 'sbin', 'private/etc', 'etc')
RESERVED = {'/usr/lib/dyld_sim', '/sbin/launchd_sim'}
MACOS_HOST_TOOLS = {'/usr/sbin/iokitsimd', '/sbin/launchd_sim_trampoline', '/usr/lib/system/host/liblaunch_sim.dylib'}
CAPABILITIES = '/Library/Developer/CoreSimulator/DeviceType/capabilities.plist'
APPLICATION_MANIFEST = '/usr/share/LiveSimBooter/applications.json'
TEXT_TO_SPEECH = '/System/Library/PrivateFrameworks/TextToSpeech.framework/TextToSpeech'
SWIFT_SQLITE = '/System/Library/PrivateFrameworks/SwiftSQLite.framework/SwiftSQLite'
MACHO_MAGICS = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xfe\xed\xfa\xce',
                b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca', b'\xca\xfe\xba\xbf', b'\xbf\xba\xfe\xca'}
LOADS = {0xc, 0x80000018, 0x8000001f, 0x80000023, 0x20}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def guest_path(value):
    require(isinstance(value, str) and value.startswith('/') and value != '/', 'Guest path must be absolute and nonempty')
    require(not any(c in value for c in '\0\r\n') and all(part not in ('', '.', '..') for part in value.split('/')[1:]),
            'Guest path must be canonical, without traversal or empty components')
    return value


def contained(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def read_manifest(path):
    document = json.loads(path.read_text())
    require(document.get('version') == 1 and document.get('runtime_build') == '23A343', 'Expected 23A343 cache input manifest v1')
    names = [guest_path(image['install_name']) for image in document['images']]
    names += [guest_path(name) for name in document['automatically_supplied_host_libraries']]
    require(len(set(names)) == len(names) == document['expected_image_count'], 'Duplicate or inconsistent cache image inventory')
    return set(names)


def check_runtime(root):
    info = plistlib.loads((root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    require(info.get('ProductBuildVersion') == '23A343' and info.get('ProductVersion') == '26.0', 'Expected original iOS 26.0 build 23A343')


def macho(path, hash_sections=False):
    with path.open('rb') as stream:
        header = stream.read(32)
        if header[:4] not in MACHO_MAGICS:
            return None
        require(len(header) == 32 and header[:4] == b'\xcf\xfa\xed\xfe', 'Only thin little-endian arm64 Mach-O is supported')
        _, cpu, subtype, kind, count, size, _, _ = struct.unpack('<8I', header)
        require(cpu == 0x100000c and kind in (2, 6, 7, 8), 'Unsupported Mach-O architecture or file type')
        file_size = path.stat().st_size
        require(size <= 8 * 1024 * 1024 and 32 + size <= file_size and count <= size // 8, 'Invalid Mach-O load command span')
        commands = stream.read(size)
        result = {'cpu': cpu, 'subtype': subtype, 'filetype': kind, 'build_versions': [], 'platforms': [], 'legacy_platforms': [], 'dependencies': [],
                  'rpaths': [], 'text_sections': {}, 'signature': None}
        offset = 0
        for _ in range(count):
            require(offset + 8 <= size, 'Truncated load command')
            command, length = struct.unpack_from('<II', commands, offset)
            require(length >= 8 and length % 8 == 0 and offset + length <= size, 'Invalid load command size')
            if command in LOADS or command in (0xd, 0x8000001c):
                require(length >= 12, 'Truncated load command string')
                start = struct.unpack_from('<I', commands, offset + 8)[0]
                require(12 <= start < length and b'\0' in commands[offset + start:offset + length], 'Invalid load command string')
                name = commands[offset + start:offset + length].split(b'\0', 1)[0].decode('utf-8')
                if command in LOADS:
                    result['dependencies'].append({'path': name, 'weak': command == 0x80000018})
                elif command == 0xd:
                    result['install_name'] = name
                else:
                    result['rpaths'].append(name)
            elif command == 0x32:
                require(length >= 24, 'Truncated build version')
                result['build_versions'].append(commands[offset:offset + length].hex())
                result['platforms'].append(struct.unpack_from('<I', commands, offset + 8)[0])
            elif command == 0x1d:
                require(length == 16 and result['signature'] is None, 'Invalid or duplicate code signature command')
                result['signature'] = struct.unpack_from('<II', commands, offset + 8)
            elif command in (0x24, 0x25, 0x2f, 0x30):
                require(length == 16, 'Invalid legacy version command')
                result['legacy_platforms'].append({0x24: 1, 0x25: 2, 0x2f: 3, 0x30: 4}[command])
                result['build_versions'].append(commands[offset:offset + length].hex())
            elif command == 0x19:
                require(length >= 72, 'Truncated segment command')
                sections = struct.unpack_from('<I', commands, offset + 64)[0]
                require(72 + sections * 80 == length, 'Invalid section table')
                for index in range(sections):
                    entry = offset + 72 + index * 80
                    section, segment, _, section_size, file_offset = struct.unpack_from('<16s16sQQI', commands, entry)
                    flags = struct.unpack_from('<I', commands, entry + 64)[0]
                    if segment.rstrip(b'\0') != b'__TEXT' or flags & 0xff in (1, 12, 18):
                        continue
                    require(file_offset <= file_size and section_size <= file_size - file_offset, 'Section exceeds file')
                    name = section.rstrip(b'\0').decode('ascii')
                    require(name not in result['text_sections'], 'Duplicate __TEXT section')
                    if hash_sections:
                        stream.seek(file_offset)
                        digest = hashlib.sha256()
                        remaining = section_size
                        while remaining:
                            chunk = stream.read(min(remaining, 1024 * 1024))
                            require(chunk, 'Truncated section')
                            digest.update(chunk)
                            remaining -= len(chunk)
                        result['text_sections'][name] = {'size': section_size, 'sha256': digest.hexdigest()}
                    else:
                        result['text_sections'][name] = {'size': section_size}
            offset += length
        require(offset == size, 'Load command count mismatch')
        return result


def excluded(name, cached):
    parts = PurePosixPath(name).parts
    return (name in cached or name in RESERVED or name in MACOS_HOST_TOOLS or '_CodeSignature' in parts or
            PurePosixPath(name).name.startswith(('dyld_shared_cache', 'dyld_sim_shared_cache')))


def canonical_source(root, path, strict=True):
    pending, resolved, links = list(path.relative_to(root).parts), [], 0
    while pending:
        part = pending.pop(0)
        if part in ('', '.'):
            continue
        if part == '..':
            require(resolved, f'Source symlink escapes RuntimeRoot: {path}')
            resolved.pop()
            continue
        candidate = root.joinpath(*resolved, part)
        if candidate.is_symlink():
            links += 1
            require(links <= 40, f'Source symlink cycle: {path}')
            target = os.readlink(candidate)
            if target.startswith('/'):
                resolved = []
            pending = list(PurePosixPath(target).parts[1:] if target.startswith('/') else PurePosixPath(target).parts) + pending
        else:
            require(not strict or candidate.exists(), f'Missing source symlink target: {path}')
            resolved.append(part)
    return root.joinpath(*resolved)


def compatible_guest(info):
    return info['subtype'] == 0 and (info['platforms'] in ([2], [7]) or not info['platforms'] and info['legacy_platforms'] == [2])


def parse_additions(values, cached):
    additions = {}
    for value in values:
        name, separator, source = value.partition('=')
        require(separator and source, '--add-binary requires /guest/path=/local/file')
        name = guest_path(name)
        require(name not in additions and name not in cached and name not in (CAPABILITIES, APPLICATION_MANIFEST) and
                (not excluded(name, set()) or name in RESERVED),
                'Duplicate, cached or reserved non-code override')
        path = Path(source).resolve(strict=True)
        require(path.is_file(), 'Additional binary must be a regular file')
        info = macho(path)
        require(info is not None and compatible_guest(info), 'Additional binary must be plain arm64 iOS/iOS Simulator Mach-O, not macOS or arm64e')
        additions[name] = (path, info)
    return additions


def inventory(root, cached, scopes, additions):
    entries, skipped, errors = {}, collections.Counter(), []
    # The 23A343 cache builder zeros TextToSpeech's weak SwiftSQLite slots when that provider is absent.
    # Stage the original dylib so dyld binds those slots at load time; keep the original cache set for dependency coverage.
    loose_overrides = {TEXT_TO_SPEECH} if TEXT_TO_SPEECH in cached and SWIFT_SQLITE not in cached else set()
    omitted_cached = cached - loose_overrides
    def inspect(path):
        name = '/' + path.relative_to(root).as_posix()
        if excluded(name, omitted_cached):
            skipped['cached_code' if name in omitted_cached else 'managed_loader' if name in RESERVED else
                    'macos_host_tool' if name in MACOS_HOST_TOOLS else 'stock_signature_or_cache'] += 1
            return
        if name in additions:
            return
        try:
            if path.is_symlink():
                target = canonical_source(root, path, strict=False)
                target_name = '/' + target.relative_to(root).as_posix()
                if excluded(target_name, omitted_cached):
                    skipped['cached_or_managed_alias'] += 1
                    return
                # Relative links remain valid after moving the stage, including originally absolute guest paths.
                entries[name] = {'kind': 'symlink', 'target': os.path.relpath(target, path.parent), 'size': 0, 'missing_source_target': not target.exists()}
            elif path.is_file():
                info = macho(path)
                if info and info.get('install_name') in omitted_cached:
                    skipped['cached_code_alias'] += 1
                    return
                if name in loose_overrides:
                    require(info and info['filetype'] == 6 and info.get('install_name') == name, 'Expected original TextToSpeech dylib')
                entries[name] = {'kind': 'code' if info else 'resource', 'size': path.stat().st_size}
                if info:
                    require(compatible_guest(info), 'Stock image is not plain arm64 iOS/iOS Simulator code')
                    entries[name]['macho'] = info
                    if info.get('install_name') in loose_overrides:
                        require(info['filetype'] == 6, 'Expected original TextToSpeech dylib')
                        entries[name]['loose_cache_override'] = info['install_name']
            elif not path.is_dir():
                raise ValueError('Unsupported non-regular source entry')
        except (OSError, ValueError, UnicodeError, struct.error) as error:
            entries.pop(name, None)
            errors.append({'path': name, 'error': str(error)})
    for scope in scopes:
        name = guest_path('/' + scope)
        path = root / name[1:]
        require(path.exists() or path.is_symlink(), f'Missing scope: {scope}')
        if path.is_symlink() or not path.is_dir():
            inspect(path)
            continue
        for folder, directories, files in os.walk(path, followlinks=False, onerror=lambda error: errors.append({'path': error.filename, 'error': str(error)})):
            directory = Path(folder)
            for child in directories[:]:
                candidate = directory / child
                if child == '_CodeSignature' or child == 'com.apple.dyld':
                    directories.remove(child)
                    skipped['stock_signature_or_cache_directory'] += 1
                elif candidate.is_symlink():
                    directories.remove(child)
                    inspect(candidate)
            for child in files:
                inspect(directory / child)
    for name, (path, info) in additions.items():
        entries[name] = {'kind': 'code', 'size': path.stat().st_size, 'macho': info, 'override_source': str(path)}
    missing = []
    for name, entry in entries.items():
        if entry['kind'] != 'code':
            continue
        for dependency in entry['macho']['dependencies']:
            target = dependency['path']
            if dependency['weak']:
                continue
            if target.startswith('@loader_path/'):
                target = os.path.normpath(str(PurePosixPath(name).parent / target[len('@loader_path/'):]))
            if target.startswith('@rpath/'):
                candidates = []
                for rpath in entry['macho']['rpaths']:
                    base = rpath.replace('@loader_path', str(PurePosixPath(name).parent), 1)
                    if base.startswith('/'):
                        candidates.append(os.path.normpath(base + '/' + target[len('@rpath/'):]))
                target = next((candidate for candidate in candidates if candidate in cached or candidate in entries), target)
            present = target in cached or target in entries
            if not present and target.startswith('/'):
                try:
                    canonical = '/' + canonical_source(root, root / target[1:]).relative_to(root).as_posix()
                    present = canonical in cached or canonical in entries
                except (ValueError, OSError):
                    pass
            if not present:
                missing.append({'origin': name, 'dependency': target})
    return entries, dict(skipped), errors, missing


def app_inventory(root, entries):
    apps = []
    locations = ('Applications', 'System/Applications', 'System/Library/CoreServices')
    present = set()
    for name in entries:
        for location in locations:
            prefix = '/' + location + '/'
            if name.startswith(prefix):
                bundle = name[len(prefix):].split('/', 1)[0]
                if bundle.endswith('.app'):
                    present.add(prefix + bundle)
                break
    for location in locations:
        parent = root / location
        if not parent.is_dir():
            continue
        for app in sorted(parent.glob('*.app')):
            prefix = '/' + app.relative_to(root).as_posix()
            if prefix not in present:
                continue
            item = {'path': prefix, 'registerable': False}
            try:
                info_path = app / 'Info.plist'
                require(not info_path.is_symlink() and info_path.stat().st_size <= 1024 * 1024, 'Invalid app metadata')
                info = plistlib.loads(info_path.read_bytes())
                require(isinstance(info, dict), 'Invalid app metadata')
                executable = info.get('CFBundleExecutable')
                item['identifier'] = info.get('CFBundleIdentifier')
                if isinstance(executable, str) and executable and '/' not in executable and '\0' not in executable and executable not in ('.', '..'):
                    item['executable'] = prefix + '/' + executable
                    main = entries.get(item['executable'], {})
                    item['registerable'] = (isinstance(item['identifier'], str) and bool(item['identifier']) and '\0' not in item['identifier'] and
                                            main.get('kind') == 'code' and main['macho']['filetype'] == 2)
            except (OSError, ValueError, plistlib.InvalidFileException):
                pass
            apps.append(item)
    return apps


def verify_signed(path):
    info = macho(path, True)
    require(info and info['signature'], 'Signed image lacks a signature')
    start, capacity = info['signature']
    with path.open('rb') as stream:
        stream.seek(start)
        data = stream.read(capacity)
    require(len(data) == capacity and capacity >= 12, 'Truncated signature')
    magic, length, count = struct.unpack_from('>III', data)
    require(magic == 0xfade0cc0 and 12 + count * 8 <= length <= capacity, 'Invalid signature SuperBlob')
    kinds = [struct.unpack_from('>I', data, 12 + index * 8)[0] for index in range(count)]
    require(5 not in kinds and 7 not in kinds, 'Native signature unexpectedly contains entitlements')
    return info


def stable_metadata(info):
    return {key: info[key] for key in ('cpu', 'subtype', 'filetype', 'build_versions', 'text_sections')}


def sign_binary(source, output, identity, identifier, helper, signer):
    original, source_hash = macho(source, True), sha(source)
    output.parent.mkdir(parents=True, exist_ok=True)
    if original['filetype'] == 2:
        record = signer.sign_executable(SimpleNamespace(source=source, output=output, identity=identity, identifier=identifier, cms_helper=helper))
    else:
        with tempfile.TemporaryDirectory(prefix='.image-', dir=output.parent) as temporary:
            image = Path(temporary) / 'image'
            shutil.copyfile(source, image)
            image.chmod(stat.S_IMODE(source.stat().st_mode) | 0o600)
            subprocess.run(['/usr/bin/codesign', '--force', '--sign', identity, '--identifier', identifier,
                            '--digest-algorithm=sha256', '--pagesize', '16384', '--timestamp=none', str(image)], check=True)
            subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(image)], check=True)
            require(stable_metadata(verify_signed(image)) == stable_metadata(original), 'Original Mach-O sections/platform/type changed')
            os.link(image, output)
        record = {'source_sha256': source_hash, 'sha256': sha(output), 'filetype': original['filetype'], 'entitlements': None}
    require(stable_metadata(verify_signed(output)) == stable_metadata(original), 'Original Mach-O sections/platform/type changed')
    require(sha(source) == source_hash, 'Source changed during signing')
    record['original_text_sections'] = original['text_sections']
    record['original_macho_metadata_preserved'] = True
    return record


def stage(options, root, entries, report):
    output = options.output_dir.absolute()
    require(not os.path.lexists(output), 'Output must be a new directory; existing stages are never overwritten')
    require(not contained(output.resolve(), root) and not contained(root, output.resolve()), 'Output and source trees must be separate')
    require(re.fullmatch(r'[0-9a-fA-F]{40}', options.identity or ''), 'Signing identity must be an exact 40-character certificate SHA-1')
    require(options.identifier and not any(c in options.identifier for c in '\0\r\n'), 'A signing identifier is required')
    require(not report['errors'], 'Preflight errors prevent staging; inspect dry-run JSON')
    require(not report['missing_strong_dependencies'] or getattr(options, 'allow_missing_dependencies', False),
            'Strong-dependency gaps prevent staging; narrow scopes or explicitly accept an incomplete runtime with --allow-missing-dependencies')
    supplied_helper = getattr(options, 'cms_helper', None)
    if supplied_helper is not None:
        supplied_helper = supplied_helper.resolve(strict=True)
        require(supplied_helper.is_file() and os.access(supplied_helper, os.X_OK), 'CMS helper must be an existing executable file')
    output.mkdir(parents=True, exist_ok=False)
    runtime = output / 'RuntimeRoot'
    runtime.mkdir()
    tools = output / 'tools'
    tools.mkdir()
    helper = supplied_helper or tools / 'sign-code-directory'
    if supplied_helper is None:
        subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-framework', 'Foundation', '-framework', 'Security',
                        str(REPOSITORY / 'Tools/sign-code-directory.m'), '-o', str(helper)], check=True)
    spec = importlib.util.spec_from_file_location('setup_signer', REPOSITORY / 'Tools/sign-sim-executable.py')
    signer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(signer)
    records = output / 'signing-records'
    records.mkdir()
    result, links = [], []
    # Copy data before signing; symlinks come last so no output parent can redirect writes.
    for name, entry in sorted(entries.items(), key=lambda item: (item[1]['kind'] == 'symlink', item[0])):
        target = runtime / name[1:]
        target.parent.mkdir(parents=True, exist_ok=True)
        require(not os.path.lexists(target), f'Duplicate stage destination: {name}')
        source = Path(entry['override_source']) if 'override_source' in entry else root / name[1:]
        record = {'path': name, 'kind': entry['kind']}
        if entry['kind'] == 'symlink':
            links.append((entry['target'], target))
            record['target'] = entry['target']
        elif entry['kind'] == 'resource':
            with source.open('rb') as incoming, target.open('xb') as outgoing:
                shutil.copyfileobj(incoming, outgoing)
            target.chmod(stat.S_IMODE(source.stat().st_mode))
            record['size'], record['sha256'] = target.stat().st_size, sha(target)
        else:
            signing = sign_binary(source, target, options.identity, options.identifier, helper, signer)
            key = hashlib.sha256(name.encode()).hexdigest() + '.json'
            (records / key).write_text(json.dumps(signing, indent=2) + '\n')
            record.update(size=target.stat().st_size, sha256=signing['sha256'], signing_record='signing-records/' + key)
        result.append(record)
    applications = [item['path'] for item in report['applications'] if item['registerable']]
    if applications:
        registration = runtime / APPLICATION_MANIFEST[1:]
        registration.parent.mkdir(parents=True, exist_ok=True)
        with registration.open('x') as stream:
            json.dump({'version': 1, 'applications': applications}, stream, indent=2)
            stream.write('\n')
        result.append({'path': APPLICATION_MANIFEST, 'kind': 'generated-registration-manifest',
                       'size': registration.stat().st_size, 'sha256': sha(registration)})
    for link, path in links:
        os.symlink(link, path)
    report.update(mode='staged', signing_identity=options.identity, signing_identifier=options.identifier, entries=result,
                  native_signature_entitlements=None, entitlement_source='Original Mach-O __TEXT,__ents_der sections, preserved byte-for-byte')
    (output / 'stage-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    return {'output': str(output), 'manifest': str(output / 'stage-manifest.json'), 'files': len(result), 'complete_boot_image': False,
            'loose_cache_overrides': report.get('loose_cache_overrides', [])}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-root', type=Path, required=True)
    parser.add_argument('--cache-manifest', type=Path, default=REPOSITORY / 'Tools/setup-ios26-cache-inputs.json')
    parser.add_argument('--scope', action='append', help='Repeat for specific RuntimeRoot-relative files/directories; default covers stock apps/resources/tools')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--dry-run', action='store_true', help='Read-only JSON inventory (the default)')
    mode.add_argument('--stage', action='store_true', help='Copy and sign only after a complete successful preflight')
    parser.add_argument('--output-dir', type=Path, help='New output directory, required with --stage')
    parser.add_argument('--identity', help='Exact signing certificate SHA-1, required with --stage')
    parser.add_argument('--identifier', help='Signing bundle identifier, required with --stage')
    parser.add_argument('--cms-helper', type=Path, help='Reuse an existing executable sign-code-directory helper already approved by the keychain')
    parser.add_argument('--add-binary', action='append', default=[], metavar='/GUEST/PATH=/LOCAL/FILE',
                        help='Unsigned/patched guest image or built helper to sign; may replace a stock image, but never a cached image')
    parser.add_argument('--capabilities', type=Path, help='Matching stock device-type capabilities.plist; copied to the path expected by LiveProcess')
    parser.add_argument('--allow-missing-dependencies', action='store_true', help='Explicitly stage incomplete stock tools/apps; missing strong paths remain in manifest')
    options = parser.parse_args(argv)
    require(not options.stage or options.output_dir is not None, '--stage requires --output-dir')
    root = options.runtime_root.resolve(strict=True)
    check_runtime(root)
    cached = read_manifest(options.cache_manifest)
    additions = parse_additions(options.add_binary, cached)
    entries, skipped, errors, missing = inventory(root, cached, options.scope or DEFAULT_SCOPES, additions)
    if options.capabilities:
        capabilities = options.capabilities.resolve(strict=True)
        require(isinstance(plistlib.loads(capabilities.read_bytes()), dict), 'Capabilities must be a plist dictionary')
        entries[CAPABILITIES] = {'kind': 'resource', 'size': capabilities.stat().st_size, 'override_source': str(capabilities)}
    counts = collections.Counter(entry['kind'] for entry in entries.values())
    report = {'version': 1, 'mode': 'inventory', 'runtime_build': '23A343', 'source_root': str(root),
              'cache_manifest': str(options.cache_manifest.resolve()), 'scopes': list(options.scope or DEFAULT_SCOPES),
              'counts': dict(counts), 'source_bytes': sum(entry['size'] for entry in entries.values()), 'skipped': skipped,
              'loose_cache_overrides': sorted({entry['loose_cache_override'] for entry in entries.values() if 'loose_cache_override' in entry}),
              'required_loader_overrides_missing': sorted(RESERVED - additions.keys()), 'errors': errors, 'missing_strong_dependencies': missing,
              'missing_stock_symlink_targets': [name for name, entry in entries.items() if entry.get('missing_source_target')],
              'applications': app_inventory(root, entries), 'complete_boot_image': False,
              'notes': ['No device publication or app registration.', 'Cache and patched loaders require the documented separate workflow.',
                        'Mutable data is not copied.', 'Original simulator entitlement sections stay in Mach-O; no entitlement sidecars are used.']}
    print(json.dumps(stage(options, root, entries, report) if options.stage else report, indent=2))
    return 0 if not errors and (not missing or options.allow_missing_dependencies) else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, struct.error, subprocess.CalledProcessError) as error:
        sys.exit(f'setup-ios26-runtime: {error}')
