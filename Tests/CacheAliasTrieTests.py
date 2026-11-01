#!/usr/bin/env python3
"""Cache aliases preserve canonical image indices and reject malformed or ambiguous path metadata."""

import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('alias_patch_test', ROOT / 'Tools/patch-ios26-cache-aliases.py')
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class CacheAliasTrieTests(unittest.TestCase):
    def test_shared_terminals_and_backward_offsets_preserve_all_names(self):
        paths = {'/System/Library/Frameworks/Network.framework/Network': 123, '/usr/lib/swift/libswiftNetwork.dylib': 123,
                 '/usr/lib/libnetwork.dylib': 123, '/usr/lib/A.dylib': 0, '/usr/lib/B.dylib': 1352,
                 '/usr/lib/prefix': 8, '/usr/lib/prefix/more': 9}
        encoded = PATCH.encode_trie(paths)
        self.assertEqual(PATCH.decode_trie(encoded), paths)
        self.assertEqual(PATCH.encode_trie(dict(reversed(list(paths.items())))), encoded)
        for name, index in paths.items():
            self.assertEqual(PATCH.slide.trie_index(encoded, name), index)
        for name in ('/', '/usr/lib/A', '/usr/lib/A.dylib/more', '/usr/lib/prefix/missing', '/absent'):
            self.assertIsNone(PATCH.slide.trie_index(encoded, name))

    def test_large_index_set_and_aliases_remain_deterministic(self):
        entries = {'/System/Library/Frameworks/F%04d.framework/F%04d' % (i, i): i for i in range(1353)}
        canonical = dict(entries)
        for index in range(63):
            entries['/usr/lib/swift/libswiftF%04d.dylib' % index] = index
        encoded = PATCH.encode_trie(entries)
        decoded = PATCH.decode_trie(encoded)
        self.assertEqual(decoded, entries)
        self.assertTrue(all(decoded[name] == index for name, index in canonical.items()))
        self.assertEqual(PATCH.encode_trie(dict(reversed(list(entries.items())))), encoded)

    def test_cycles_bounds_and_ambiguous_edges_are_rejected(self):
        malformed = [(b'\0\1/\0\0', 'Cyclic'), (b'\xff' * 11, 'ULEB'), (b'\x7f\0', 'terminal'),
                     (b'\0\1/no-terminator', 'Unterminated'), (b'\0\1/\0\x7f', 'child'), (b'\0\1/\0\x80', 'ULEB'),
                     (b'\0\2/a\0\x0c/b\0\x0f\0\0\1\0\0\1\1\0', 'ambiguous')]
        for data, message in malformed:
            with self.subTest(data=data), self.assertRaisesRegex(ValueError, message):
                PATCH.decode_trie(data)
        for path in ('relative', '/embedded\0nul', '/' + 'x' * 4096):
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'path'):
                PATCH.encode_trie({path: 1})
        with self.assertRaisesRegex(ValueError, 'index'):
            PATCH.encode_trie({'/valid': 4096})

    def test_pinned_alias_inventory_is_confined_and_targets_real_cache_images(self):
        inventory = PATCH.slide.setup.read_inventory()
        aliases = PATCH.slide.read_aliases(inventory)
        self.assertEqual(len(aliases), 63)
        network = next(x for x in aliases if x['path'] == '/usr/lib/swift/libswiftNetwork.dylib')
        self.assertEqual(network['target'], '/System/Library/Frameworks/Network.framework/Network')
        swift = [row for row in aliases if Path(row['path']).name.startswith('libswift')]
        self.assertEqual(len(swift), 12)
        self.assertEqual(sum(row['target'] == '/System/Library/Frameworks/WebKit.framework/WebKit' for row in swift), 2)
        self.assertFalse(any('/Users/' in value for row in aliases for value in row.values()))

    def test_cryptex_alias_validation_uses_pinned_source_not_install_name_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            inventory = PATCH.slide.setup.read_inventory()
            sources = {row['install_name']: row['source'] for row in inventory['images']}
            aliases = PATCH.slide.read_aliases(inventory)
            alias_paths = {row['path'] for row in aliases}
            for target in {row['target'] for row in aliases}:
                source = root / (target[1:] if '/' + sources[target] in alias_paths else sources[target])
                source.parent.mkdir(parents=True, exist_ok=True)
                source.touch()
                installed = root / target[1:]
                if installed != source:
                    installed.parent.mkdir(parents=True, exist_ok=True)
                    installed.symlink_to(os.path.relpath(source, installed.parent))
            (root / 'System/Library/Frameworks/IOKit.framework/Versions/Current').symlink_to('A')
            for row in aliases:
                alias = root / row['path'][1:]
                alias.parent.mkdir(parents=True, exist_ok=True)
                alias.symlink_to(row['source_link'])
            self.assertEqual(PATCH.slide.read_aliases(inventory, root), aliases)
            webkit = root / 'System/Library/Frameworks/WebKit.framework/WebKit'
            webkit.unlink()
            webkit.touch()
            with self.assertRaisesRegex(ValueError, 'Original cache alias target changed'):
                PATCH.slide.read_aliases(inventory, root)

    def test_original_alias_link_must_match_pinned_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            inventory = PATCH.slide.setup.read_inventory()
            first = PATCH.slide.read_aliases(inventory)[0]
            alias = root / first['path'][1:]
            alias.parent.mkdir(parents=True)
            alias.symlink_to('unexpected-target')
            with self.assertRaisesRegex(ValueError, 'Original cache alias changed'):
                PATCH.slide.read_aliases(inventory, root)

    def test_coverage_checks_canonical_index_not_just_alias_presence(self):
        target, alias = '/System/Library/Frameworks/Network.framework/Network', '/usr/lib/swift/libswiftNetwork.dylib'
        data = bytearray(0x4000)
        struct.pack_into('<II', data, 448, 512, 1)
        struct.pack_into('<QQQII', data, 512, 0x180001000, 0, 0, 1024, 0)
        data[1024:1024 + len(target) + 1] = target.encode() + b'\0'
        layout = dict(signature_offset=len(data), mappings=[dict(address=0x180000000, size=len(data), file_offset=0)])
        aliases = [dict(path=alias, target=target)]
        for value, accepted in ((0, True), (1, False), (None, False)):
            paths = {target: 0}
            if value is not None:
                paths[alias] = value
            trie = PATCH.encode_trie(paths)
            struct.pack_into('<QQ', data, 264, 0x180002000, len(trie))
            data[0x2000:0x2000 + len(trie)] = trie
            if accepted:
                self.assertEqual(PATCH.slide.check_aliases(data, layout, aliases), 1)
            else:
                with self.assertRaisesRegex(ValueError, 'Missing or incorrect'):
                    PATCH.slide.check_aliases(data, layout, aliases)

    def planner_fixture(self, already_aliased=False):
        inventory = PATCH.slide.setup.read_inventory()
        paths = [entry['install_name'] for entry in inventory['images']] + inventory['automatically_supplied_host_libraries']
        canonical = {path: index for index, path in enumerate(paths)}
        entries = dict(canonical)
        if already_aliased:
            for alias in PATCH.slide.read_aliases(inventory):
                entries[alias['path']] = canonical[alias['target']]
        trie = PATCH.encode_trie(entries)
        names = b''.join(path.encode() + b'\0' for path in paths)
        trie_offset = (0x10000 + len(names) + 0x3FFF) & ~0x3FFF
        signature = (trie_offset + len(trie) + 0x3FFF) & ~0x3FFF
        data = bytearray(signature + 0x80)
        data[0x10000:0x10000 + len(names)] = names
        struct.pack_into('<II', data, 448, 0x1000, len(paths))
        name_offset = 0x10000
        for index, path in enumerate(paths):
            struct.pack_into('<QQQII', data, 0x1000 + index * 32, 0x180000000, 0, 0, name_offset, 0)
            name_offset += len(path.encode()) + 1
        data[trie_offset:trie_offset + len(trie)] = trie
        struct.pack_into('<QQ', data, 264, 0x180000000 + trie_offset, len(trie))
        layout = dict(uuid='12345678-1234-5678-1234-567812345678', signature_offset=signature,
                      mappings=[dict(address=0x180000000, size=signature, file_offset=0, maxprot=1, initprot=1, slide_info_size=0)])
        return data, layout

    def test_capacity_overflow_keeps_original_bytes(self):
        data, layout = self.planner_fixture()
        before = bytes(data)
        with self.assertRaisesRegex(ValueError, 'exceeds its existing allocation'):
            PATCH.plan_aliases(data, layout)
        self.assertEqual(bytes(data), before)

    def test_already_aliased_cache_is_rejected_without_mutation(self):
        data, layout = self.planner_fixture(already_aliased=True)
        before = bytes(data)
        with self.assertRaisesRegex(ValueError, 'exactly the canonical'):
            PATCH.plan_aliases(data, layout)
        self.assertEqual(bytes(data), before)


if __name__ == '__main__':
    unittest.main()
