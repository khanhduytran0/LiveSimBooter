#!/usr/bin/env python3
"""No signing key, runtime download, device connection or SDK is needed for these fixtures."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

REPOSITORY = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runtime = load('runtime_setup', REPOSITORY / 'Tools/setup-ios26-runtime.py')
signer = load('runtime_signer', REPOSITORY / 'Tools/sign-sim-executable.py')


def make_macho(path, kind=2, dependencies=(), platform=7, install_name=None):
    build = struct.pack('<6I', 0x32, 24, platform, 0x1a0000, 0x1a0000, 0)
    commands = [build]
    if install_name:
        value = install_name.encode() + b'\0'
        length = (24 + len(value) + 7) & ~7
        commands.append(struct.pack('<6I', 0xd, length, 24, 0, 0, 0) + value + bytes(length - 24 - len(value)))
    for name, weak in dependencies:
        value = name.encode() + b'\0'
        length = (24 + len(value) + 7) & ~7
        commands.append(struct.pack('<6I', 0x80000018 if weak else 0xc, length, 24, 0, 0, 0) + value + bytes(length - 24 - len(value)))
    payload = b'original-text\0' + b'\x70\x05\x02\x01\x01\xb0\x00'
    command_size = sum(map(len, commands)) + 232
    offset = 32 + command_size
    segment = struct.pack('<II16sQQQQIIII', 0x19, 232, b'__TEXT', 0x100000000, offset + len(payload), 0, offset + len(payload), 5, 5, 2, 0)
    for name, start, size in ((b'__text', 0, 14), (b'__ents_der', 14, len(payload) - 14)):
        segment += struct.pack('<16s16sQQIIIIIIII', name, b'__TEXT', 0x100000000 + offset + start, size, offset + start, 0, 0, 0, 0, 0, 0, 0)
    commands.append(segment)
    data = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, kind, len(commands), command_size, 0, 0) + b''.join(commands) + payload
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(0o755)
    return data


class RuntimeSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='LiveSimBooter-runtime-tests-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / 'RuntimeRoot'
        self.root.mkdir()
        version = self.root / 'System/Library/CoreServices/SystemVersion.plist'
        version.parent.mkdir(parents=True)
        version.write_bytes(plistlib.dumps({'ProductVersion': '26.0', 'ProductBuildVersion': '23A343'}))
        self.cached = {'/usr/lib/cached.dylib'}
        self.manifest = self.base / 'cache.json'
        self.manifest.write_text(json.dumps({'version': 1, 'runtime_build': '23A343', 'images': [{'install_name': '/usr/lib/cached.dylib'}],
                                             'automatically_supplied_host_libraries': [], 'expected_image_count': 1}))

    def app(self, location='Applications/Example.app', executable='Example'):
        directory = self.root / location
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'example.app', 'CFBundleExecutable': executable,
                                                              'CFBundlePackageType': 'APPL'}))
        make_macho(directory / executable, dependencies=[('/usr/lib/cached.dylib', False)])
        return directory

    def test_inventory_is_read_only_and_omits_cached_images_cache_and_old_seals(self):
        app = self.app()
        (app / 'Assets.car').write_bytes(b'resource')
        (app / '_CodeSignature').mkdir()
        (app / '_CodeSignature/CodeResources').write_bytes(b'old-seal')
        make_macho(self.root / 'usr/lib/cached.dylib', 6)
        make_macho(self.root / 'usr/lib/dyld_sim', 7)
        cache = self.root / 'System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64'
        cache.parent.mkdir(parents=True)
        cache.write_bytes(b'stock DSC')
        before = {str(path.relative_to(self.root)): path.read_bytes() for path in self.root.rglob('*') if path.is_file()}
        entries, skipped, errors, missing = runtime.inventory(self.root, self.cached, ['Applications', 'usr', 'System'], {})
        self.assertFalse(errors or missing)
        self.assertIn('/Applications/Example.app/Example', entries)
        self.assertNotIn('/usr/lib/cached.dylib', entries)
        self.assertNotIn('/usr/lib/dyld_sim', entries)
        self.assertEqual(skipped['cached_code'], 1)
        self.assertNotIn('/Applications/Example.app/_CodeSignature/CodeResources', entries)
        self.assertEqual(before, {str(path.relative_to(self.root)): path.read_bytes() for path in self.root.rglob('*') if path.is_file()})
        apps = runtime.app_inventory(self.root, entries)
        self.assertEqual([(item['path'], item['registerable']) for item in apps], [('/Applications/Example.app', True)])

    def test_contained_relative_and_absolute_guest_links_and_escape(self):
        target = self.root / 'private/etc'
        target.mkdir(parents=True)
        (target / 'config').write_bytes(b'data')
        os.symlink('private/etc', self.root / 'etc')
        os.symlink('/private/etc/config', target / 'absolute')
        self.assertEqual(runtime.canonical_source(self.root, self.root / 'etc/config'), target / 'config')
        self.assertEqual(runtime.canonical_source(self.root, target / 'absolute'), target / 'config')
        os.symlink('../../../outside', target / 'escape')
        with self.assertRaisesRegex(ValueError, 'escapes'):
            runtime.canonical_source(self.root, target / 'escape')
        os.symlink('loop', target / 'loop')
        with self.assertRaisesRegex(ValueError, 'cycle'):
            runtime.canonical_source(self.root, target / 'loop')
        entries, _, errors, _ = runtime.inventory(self.root, self.cached, ['etc'], {})
        self.assertFalse(errors)
        self.assertEqual(entries['/etc']['target'], 'private/etc')

    def test_invalid_macho_and_platform_are_reported_not_copied_as_resources(self):
        binary = self.root / 'bin/test'
        original = make_macho(binary, platform=1)
        entries, _, errors, _ = runtime.inventory(self.root, self.cached, ['bin'], {})
        self.assertEqual(len(errors), 1)
        self.assertIn('not plain arm64 iOS/iOS Simulator', errors[0]['error'])
        binary.write_bytes(b'\xca\xfe\xba\xbe' + bytes(28))
        _, _, errors, _ = runtime.inventory(self.root, self.cached, ['bin'], {})
        self.assertEqual(len(errors), 1)
        binary.write_bytes(original[:32])
        with self.assertRaisesRegex(ValueError, 'span'):
            runtime.macho(binary)

    def test_exact_overrides_can_supply_patched_loaders_but_not_cached_code(self):
        helper = self.base / 'patched-launchd'
        make_macho(helper)
        additions = runtime.parse_additions(['/sbin/launchd_sim=' + str(helper)], self.cached)
        self.assertEqual(additions['/sbin/launchd_sim'][0], helper)
        for value in ('/usr/lib/cached.dylib=', '/../escape=', '//double=', '/System/dyld_shared_cache_arm64='):
            with self.assertRaises(ValueError):
                runtime.parse_additions([value + str(helper)], self.cached)
        with self.assertRaises(ValueError):
            runtime.parse_additions(['/bin/test=' + str(helper)] * 2, self.cached)
        for platform in (2, 7):
            make_macho(helper, platform=platform)
            self.assertIn('/bin/test', runtime.parse_additions(['/bin/test=' + str(helper)], self.cached))
        make_macho(helper, platform=1)
        with self.assertRaisesRegex(ValueError, 'not macOS'):
            runtime.parse_additions(['/bin/test=' + str(helper)], self.cached)
        data = bytearray(make_macho(helper))
        struct.pack_into('<I', data, 8, 2)
        helper.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'arm64e'):
            runtime.parse_additions(['/bin/test=' + str(helper)], self.cached)

    def text_to_speech_fixture(self):
        self.cached.add(runtime.TEXT_TO_SPEECH)
        for name in (runtime.TEXT_TO_SPEECH, runtime.SWIFT_SQLITE, '/usr/lib/cached.dylib'):
            dependencies = [(runtime.SWIFT_SQLITE, True)] if name == runtime.TEXT_TO_SPEECH else []
            make_macho(self.root / name[1:], 6, dependencies=dependencies, install_name=name)

    def run_report(self, *arguments):
        self.manifest.write_text(json.dumps({'version': 1, 'runtime_build': '23A343',
                                            'images': [{'install_name': name} for name in sorted(self.cached)],
                                            'automatically_supplied_host_libraries': [], 'expected_image_count': len(self.cached)}))
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            result = runtime.main(['--runtime-root', str(self.root), '--cache-manifest', str(self.manifest), *arguments])
        self.assertEqual(result, 0)
        return json.loads(stream.getvalue())

    def test_text_to_speech_stock_override_keeps_original_cache_and_addition_policy(self):
        self.text_to_speech_fixture()
        entries, skipped, errors, missing = runtime.inventory(self.root, self.cached, ['System/Library', 'usr'], {})
        self.assertFalse(errors or missing)
        self.assertEqual(entries[runtime.TEXT_TO_SPEECH]['kind'], 'code')
        self.assertEqual(entries[runtime.TEXT_TO_SPEECH]['loose_cache_override'], runtime.TEXT_TO_SPEECH)
        self.assertNotIn('override_source', entries[runtime.TEXT_TO_SPEECH])
        self.assertIn(runtime.SWIFT_SQLITE, entries)
        self.assertNotIn('/usr/lib/cached.dylib', entries)
        self.assertEqual(skipped['cached_code'], 1)
        self.assertIn(runtime.TEXT_TO_SPEECH, self.cached)
        replacement = self.base / 'replacement'
        make_macho(replacement, 6, install_name=runtime.TEXT_TO_SPEECH)
        with self.assertRaisesRegex(ValueError, 'cached'):
            runtime.parse_additions([runtime.TEXT_TO_SPEECH + '=' + str(replacement)], self.cached)

    def test_text_to_speech_report_follows_scope_and_provider_cache_membership(self):
        self.text_to_speech_fixture()
        self.app()
        self.assertEqual(self.run_report('--scope', 'System/Library')['loose_cache_overrides'], [runtime.TEXT_TO_SPEECH])
        self.assertEqual(self.run_report('--scope', 'Applications')['loose_cache_overrides'], [])
        self.cached.add(runtime.SWIFT_SQLITE)
        self.assertEqual(self.run_report('--scope', 'System/Library')['loose_cache_overrides'], [])
        entries, _, errors, missing = runtime.inventory(self.root, self.cached, ['System/Library'], {})
        self.assertFalse(errors or missing)
        self.assertNotIn(runtime.TEXT_TO_SPEECH, entries)
        self.assertNotIn(runtime.SWIFT_SQLITE, entries)
        self.cached.remove(runtime.TEXT_TO_SPEECH)
        self.assertEqual(self.run_report('--scope', 'System/Library')['loose_cache_overrides'], [])
        entries, _, _, _ = runtime.inventory(self.root, self.cached, ['System/Library'], {})
        self.assertIn(runtime.TEXT_TO_SPEECH, entries)  # Ordinary uncached code, not a cache override.

    def test_text_to_speech_alias_requires_included_code_to_report_override(self):
        self.text_to_speech_fixture()
        original = self.root / runtime.TEXT_TO_SPEECH[1:]
        real = original.parent / 'Versions/A/TextToSpeech'
        real.parent.mkdir(parents=True)
        original.rename(real)
        original.symlink_to('Versions/A/TextToSpeech')
        unrelated = original.parent / 'cached-alias'
        make_macho(unrelated, 6, install_name='/usr/lib/cached.dylib')
        entries, skipped, errors, missing = runtime.inventory(self.root, self.cached, ['System/Library'], {})
        self.assertFalse(errors or missing)
        self.assertEqual(entries[runtime.TEXT_TO_SPEECH]['kind'], 'symlink')
        real_name = '/' + real.relative_to(self.root).as_posix()
        self.assertEqual(entries[real_name]['loose_cache_override'], runtime.TEXT_TO_SPEECH)
        self.assertNotIn('/' + unrelated.relative_to(self.root).as_posix(), entries)
        self.assertEqual(skipped['cached_code_alias'], 1)
        self.assertEqual(self.run_report('--scope', 'System/Library')['loose_cache_overrides'], [runtime.TEXT_TO_SPEECH])
        self.assertEqual(self.run_report('--scope', runtime.TEXT_TO_SPEECH[1:])['loose_cache_overrides'], [])

    def test_text_to_speech_override_is_signed_from_stock_through_normal_stage_path(self):
        self.text_to_speech_fixture()
        original = self.root / runtime.TEXT_TO_SPEECH[1:]
        before = original.read_bytes()
        output = self.base / 'signed-stage'
        calls = []
        def fake_sign(source, target, identity, identifier, helper, unused_signer):
            calls.append((source, target, identity, identifier))
            target.write_bytes(source.read_bytes())
            return {'sha256': runtime.sha(target)}
        with mock.patch.object(runtime.subprocess, 'run'), mock.patch.object(runtime, 'sign_binary', side_effect=fake_sign):
            result = self.run_report('--scope', 'System/Library', '--scope', 'usr', '--stage', '--output-dir', str(output),
                                     '--identity', '1' * 40, '--identifier', 'example.stage')
        self.assertEqual(result['loose_cache_overrides'], [runtime.TEXT_TO_SPEECH])
        saved = json.loads((output / 'stage-manifest.json').read_text())
        self.assertEqual(saved['loose_cache_overrides'], [runtime.TEXT_TO_SPEECH])
        staged = output / 'RuntimeRoot' / runtime.TEXT_TO_SPEECH[1:]
        self.assertIn((original, staged, '1' * 40, 'example.stage'), calls)
        self.assertEqual(len(calls), 2)  # TextToSpeech and its uncached SwiftSQLite provider.
        self.assertEqual(staged.read_bytes(), before)
        self.assertEqual(original.read_bytes(), before)
        self.assertFalse((output / 'RuntimeRoot/usr/lib/cached.dylib').exists())

    def test_text_to_speech_override_rejects_non_dylib_stock_input(self):
        self.text_to_speech_fixture()
        original = self.root / runtime.TEXT_TO_SPEECH[1:]
        for data in (b'not Mach-O', make_macho(original, 2, install_name=runtime.TEXT_TO_SPEECH)):
            original.write_bytes(data)
            entries, _, errors, _ = runtime.inventory(self.root, self.cached, [runtime.TEXT_TO_SPEECH[1:]], {})
            self.assertNotIn(runtime.TEXT_TO_SPEECH, entries)
            self.assertEqual(errors, [{'path': runtime.TEXT_TO_SPEECH, 'error': 'Expected original TextToSpeech dylib'}])

    def test_strong_dependency_gap_and_weak_dependency_policy(self):
        binary = self.root / 'bin/test'
        make_macho(binary, dependencies=[('/usr/lib/absent.dylib', False), ('/usr/lib/optional.dylib', True)])
        _, _, errors, missing = runtime.inventory(self.root, self.cached, ['bin'], {})
        self.assertFalse(errors)
        self.assertEqual(missing, [{'origin': '/bin/test', 'dependency': '/usr/lib/absent.dylib'}])

    def test_preserved_text_metadata_includes_simulator_entitlements(self):
        path = self.base / 'code'
        make_macho(path)
        original = runtime.macho(path, True)
        self.assertIn('__ents_der', original['text_sections'])
        value = bytearray(path.read_bytes())
        value[-1] ^= 1
        path.write_bytes(value)
        self.assertNotEqual(runtime.stable_metadata(original), runtime.stable_metadata(runtime.macho(path, True)))

    def test_cryptex_framework_alias_dependency_resolution(self):
        code = self.root / 'System/Cryptexes/OS/System/Library/Frameworks/Example.framework/Example'
        make_macho(code, 6)
        alias = self.root / 'System/Library/Frameworks/Example.framework'
        alias.parent.mkdir(parents=True)
        os.symlink('../../../System/Cryptexes/OS/System/Library/Frameworks/Example.framework', alias)
        make_macho(self.root / 'bin/tool', dependencies=[('/System/Library/Frameworks/Example.framework/Example', False)])
        entries, _, errors, missing = runtime.inventory(self.root, set(), ['System', 'bin'], {})
        self.assertFalse(errors or missing)
        self.assertEqual(entries['/System/Library/Frameworks/Example.framework']['kind'], 'symlink')
        self.assertIn('/System/Cryptexes/OS/System/Library/Frameworks/Example.framework/Example', entries)

    def test_dry_run_never_compiles_signs_or_creates_output(self):
        self.app()
        output = self.base / 'new-output'
        with mock.patch.object(runtime.subprocess, 'run', side_effect=AssertionError('No subprocess in dry-run')):
            stream = io.StringIO()
            with contextlib.redirect_stdout(stream):
                code = runtime.main(['--runtime-root', str(self.root), '--cache-manifest', str(self.manifest), '--scope', 'Applications',
                                     '--dry-run', '--output-dir', str(output)])
        self.assertEqual(code, 0)
        self.assertFalse(output.exists())
        report = json.loads(stream.getvalue())
        self.assertFalse(report['complete_boot_image'])
        self.assertEqual(report['required_loader_overrides_missing'], ['/sbin/launchd_sim', '/usr/lib/dyld_sim'])

    def test_stage_copies_data_in_new_tree_and_refuses_existing_output(self):
        data = self.root / 'Library/resource'
        data.parent.mkdir()
        data.write_bytes(b'payload')
        entries = {'/Library/resource': {'kind': 'resource', 'size': 7}}
        report = {'errors': [], 'missing_strong_dependencies': [], 'applications': []}
        options = SimpleNamespace(output_dir=self.base / 'stage', identity='1' * 40, identifier='example.stage')
        with mock.patch.object(runtime.subprocess, 'run') as command:
            runtime.stage(options, self.root, entries, report)
        self.assertEqual(command.call_count, 1)  # One stable CMS helper for the whole batch, no per-image rebuild.
        self.assertEqual((options.output_dir / 'RuntimeRoot/Library/resource').read_bytes(), b'payload')
        self.assertEqual(data.read_bytes(), b'payload')
        with self.assertRaisesRegex(ValueError, 'new directory'):
            runtime.stage(options, self.root, entries, report)
        options.output_dir = self.root / 'inside'
        with self.assertRaisesRegex(ValueError, 'separate'):
            runtime.stage(options, self.root, entries, report)
        self.assertFalse(options.output_dir.exists())

    def test_build_and_manifest_pins(self):
        runtime.check_runtime(self.root)
        self.assertEqual(runtime.read_manifest(self.manifest), self.cached)
        self.manifest.write_text(json.dumps({'version': 1, 'runtime_build': '24A434'}))
        with self.assertRaises(ValueError):
            runtime.read_manifest(self.manifest)
        (self.root / 'System/Library/CoreServices/SystemVersion.plist').write_bytes(plistlib.dumps({'ProductVersion': '27.0'}))
        with self.assertRaises(ValueError):
            runtime.check_runtime(self.root)

    def test_incomplete_stage_requires_explicit_opt_in_and_records_limitations(self):
        options = SimpleNamespace(output_dir=self.base / 'stage', identity='1' * 40, identifier='example.stage')
        report = {'errors': [], 'missing_strong_dependencies': [{'origin': '/bin/tool', 'dependency': '/absent'}], 'applications': []}
        with self.assertRaisesRegex(ValueError, 'Strong-dependency'):
            runtime.stage(options, self.root, {}, report)
        self.assertFalse(options.output_dir.exists())
        options.allow_missing_dependencies = True
        with mock.patch.object(runtime.subprocess, 'run'):
            runtime.stage(options, self.root, {}, report)
        saved = json.loads((options.output_dir / 'stage-manifest.json').read_text())
        self.assertEqual(saved['missing_strong_dependencies'], report['missing_strong_dependencies'])

    def test_generated_registration_manifest_excludes_placeholders(self):
        self.app()
        placeholder = self.root / 'Applications/Placeholder.app'
        placeholder.mkdir()
        (placeholder / 'asset').write_bytes(b'data')
        entries, _, errors, missing = runtime.inventory(self.root, self.cached, ['Applications'], {})
        report = {'errors': errors, 'missing_strong_dependencies': missing, 'applications': runtime.app_inventory(self.root, entries)}
        options = SimpleNamespace(output_dir=self.base / 'stage', identity='1' * 40, identifier='example.stage')
        def fake_sign(source, target, *args):
            target.write_bytes(source.read_bytes())
            return {'sha256': runtime.sha(target)}
        with mock.patch.object(runtime.subprocess, 'run'), mock.patch.object(runtime, 'sign_binary', side_effect=fake_sign):
            runtime.stage(options, self.root, entries, report)
        manifest = json.loads((options.output_dir / 'RuntimeRoot' / runtime.APPLICATION_MANIFEST[1:]).read_text())
        self.assertEqual(manifest, {'version': 1, 'applications': ['/Applications/Example.app']})

    def test_cms_helper_reuse_avoids_recompile_and_rejects_nonexecutable(self):
        helper = self.base / 'sign-code-directory'
        helper.write_bytes(b'fixture')
        helper.chmod(0o755)
        with mock.patch.object(signer, 'run') as command:
            self.assertEqual(signer.cms_helper(SimpleNamespace(cms_helper=helper), self.base), helper)
            command.assert_not_called()
            self.assertEqual(signer.cms_helper(SimpleNamespace(), self.base), helper)
            command.assert_called_once()
        helper.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'executable'):
            signer.cms_helper(SimpleNamespace(cms_helper=helper), self.base)

    def test_stage_reuses_explicit_cms_helper_and_validates_before_creating_output(self):
        helper = self.base / 'approved-cms-helper'
        helper.write_bytes(b'fixture')
        helper.chmod(0o755)
        source = self.root / 'bin/test'
        make_macho(source)
        entries = {'/bin/test': {'kind': 'code', 'size': source.stat().st_size}}
        report = {'errors': [], 'missing_strong_dependencies': [], 'applications': []}
        options = SimpleNamespace(output_dir=self.base / 'stage', identity='1' * 40, identifier='example.stage', cms_helper=helper)
        def fake_sign(original, output, identity, identifier, actual_helper, unused_signer):
            self.assertEqual(actual_helper, helper)
            output.write_bytes(original.read_bytes())
            return {'sha256': runtime.sha(output)}
        with mock.patch.object(runtime.subprocess, 'run') as command, mock.patch.object(runtime, 'sign_binary', side_effect=fake_sign):
            runtime.stage(options, self.root, entries, report)
        command.assert_not_called()
        self.assertEqual(helper.read_bytes(), b'fixture')
        options.output_dir = self.base / 'invalid-stage'
        helper.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'executable'):
            runtime.stage(options, self.root, {}, report)
        self.assertFalse(options.output_dir.exists())
        options.cms_helper = self.base
        with self.assertRaisesRegex(ValueError, 'executable'):
            runtime.stage(options, self.root, {}, report)
        self.assertFalse(options.output_dir.exists())
        options.cms_helper = self.base / 'missing-helper'
        with self.assertRaises(FileNotFoundError):
            runtime.stage(options, self.root, {}, report)
        self.assertFalse(options.output_dir.exists())


if __name__ == '__main__':
    unittest.main()
