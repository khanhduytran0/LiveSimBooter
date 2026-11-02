#!/usr/bin/env python3
"""Structural checks for the experimental native-support placeholders; no cache build or device access."""

import copy
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location('placeholder_cache', ROOT / 'Tools/setup-ios26-placeholder-cache.py')
PLACEHOLDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PLACEHOLDER)


class PlaceholderValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='lsb-placeholder-tests-')
        directory = Path(cls.temporary.name)
        source = directory / 'placeholder.s'
        binary = directory / 'placeholder.dylib'
        source.write_text('.text\n.p2align 2\n' + ''.join('.globl %s\n%s:\n.long 0\n' % (name, name)
                          for name in ('_test_one', '_test_two', '_test_three')) + '.subsections_via_symbols\n')
        sdk = subprocess.check_output(['xcrun', '--sdk', 'iphonesimulator', '--show-sdk-path'], text=True).strip()
        subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios26.0-simulator', '-isysroot', sdk, '-dynamiclib', '-nostdlib',
                        '-Wl,-add_split_seg_info', '-Wl,-no_deduplicate', '-Wl,-headerpad,0x400',
                        '-Wl,-install_name,/usr/lib/system/libsystem_pthread.dylib',
                        '-Wl,-current_version,1.2.3', '-Wl,-compatibility_version,1.0.0', '-o', str(binary), str(source)], check=True)
        cls.data = binary.read_bytes()
        cls.expected = dict(path='/usr/lib/system/libsystem_pthread.dylib', current_version=0x10203,
                            compatibility_version=0x10000, exports=['_test_one', '_test_three', '_test_two'])
        cls.commands, cls.sections = PLACEHOLDER.image(cls.data)
        PLACEHOLDER.validate_placeholder(cls.data, cls.expected)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def validate(self, data, expected=None):
        return PLACEHOLDER.validate_placeholder(data, self.expected if expected is None else expected)

    def rewrite_exports(self, exports):
        data = bytearray(self.data)
        encoded = PLACEHOLDER.trie.encode_exports(exports)
        command = self.commands.get(0x80000033)
        if command:
            location = command[0] + 8
        else:
            location = PLACEHOLDER.trie.one(self.commands, 0x80000022 if 0x80000022 in self.commands else 0x22) + 40
        struct.pack_into('<II', data, location, len(data), len(encoded))
        data.extend(encoded)
        return data

    def add_command(self, command):
        data = bytearray(self.data)
        count, size = struct.unpack_from('<II', data, 16)
        offset = 32 + size
        self.assertLessEqual(offset + len(command), self.sections[0][4])
        self.assertFalse(any(data[offset:offset + len(command)]))
        data[offset:offset + len(command)] = command
        struct.pack_into('<II', data, 16, count + 1, size + len(command))
        return data

    def test_valid_placeholder_has_no_implementation(self):
        self.assertEqual(self.validate(self.data), dict(path=self.expected['path'], exports=3, implementation_bytes=0, placeholder_bytes=12))
        self.assertEqual(PLACEHOLDER.thin_arm64(self.data), self.data)

    def test_refuses_executable_instruction_in_body(self):
        data = bytearray(self.data)
        struct.pack_into('<I', data, self.sections[0][4], 0xD65F03C0)  # ret
        with self.assertRaisesRegex(ValueError, 'body must contain one zero word'):
            self.validate(data)

    def test_refuses_exports_sharing_one_word(self):
        exports = PLACEHOLDER.exports(self.data, self.commands)
        names = sorted(exports)
        exports[names[1]] = exports[names[0]]
        with self.assertRaisesRegex(ValueError, 'overlap or do not cover'):
            self.validate(self.rewrite_exports(exports))

    def test_refuses_export_outside_placeholder_body(self):
        exports = PLACEHOLDER.exports(self.data, self.commands)
        name = sorted(exports)[0]
        _, cursor = PLACEHOLDER.trie.read_uleb(exports[name], 0)
        address, _ = PLACEHOLDER.trie.read_uleb(exports[name], cursor)
        exports[name] = PLACEHOLDER.trie.uleb(0) + PLACEHOLDER.trie.uleb(address + 4096)
        with self.assertRaisesRegex(ValueError, 'overlap or do not cover'):
            self.validate(self.rewrite_exports(exports))

    def test_refuses_missing_or_extra_exports(self):
        for change in ('missing', 'extra'):
            with self.subTest(change=change):
                exports = PLACEHOLDER.exports(self.data, self.commands)
                if change == 'missing':
                    del exports[sorted(exports)[0]]
                else:
                    exports[b'_unexpected'] = exports[sorted(exports)[0]]
                with self.assertRaisesRegex(ValueError, 'export names changed'):
                    self.validate(self.rewrite_exports(exports))

    def test_refuses_non_regular_export(self):
        exports = PLACEHOLDER.exports(self.data, self.commands)
        name = sorted(exports)[0]
        _, cursor = PLACEHOLDER.trie.read_uleb(exports[name], 0)
        exports[name] = PLACEHOLDER.trie.uleb(1) + exports[name][cursor:]  # thread-local export
        with self.assertRaisesRegex(ValueError, 'Unexpected placeholder export kind'):
            self.validate(self.rewrite_exports(exports))

    def test_refuses_dylib_dependencies(self):
        name = b'/usr/lib/libSystem.B.dylib\0'
        size = (24 + len(name) + 7) & ~7
        for kind in (0xC, 0x80000018, 0x8000001F, 0x80000023, 0x20):
            with self.subTest(command=hex(kind)):
                command = (struct.pack('<6I', kind, size, 24, 0, 0x10000, 0x10000) + name).ljust(size, b'\0')
                with self.assertRaisesRegex(ValueError, 'dependencies or initializers'):
                    self.validate(self.add_command(command))

    def test_refuses_legacy_initializer_commands(self):
        for kind, layout in ((0x11, '<10I'), (0x1A, '<II8Q')):
            with self.subTest(command=hex(kind)):
                size = struct.calcsize(layout)
                command = struct.pack(layout, kind, size, self.sections[0][2], *([0] * 7))
                with self.assertRaisesRegex(ValueError, 'dependencies or initializers'):
                    self.validate(self.add_command(command))

    def test_refuses_undefined_symbols(self):
        data = bytearray(self.data)
        struct.pack_into('<I', data, PLACEHOLDER.trie.one(self.commands, 0xB) + 28, 1)
        with self.assertRaisesRegex(ValueError, 'undefined symbols'):
            self.validate(data)

    def test_refuses_truncated_segment_command(self):
        with self.assertRaisesRegex(ValueError, 'Truncated image segment'):
            self.validate(self.add_command(struct.pack('<II', 0x19, 8)))

    def test_refuses_identity_or_version_mismatch(self):
        for field, value in (('path', '/usr/lib/system/another.dylib'), ('current_version', 0x20000), ('compatibility_version', 0x20000)):
            with self.subTest(field=field):
                expected = copy.deepcopy(self.expected)
                expected[field] = value
                with self.assertRaisesRegex(ValueError, 'identity/version mismatch'):
                    self.validate(self.data, expected)

    def test_refuses_non_simulator_platform(self):
        data = bytearray(self.data)
        struct.pack_into('<I', data, PLACEHOLDER.trie.one(self.commands, 0x32) + 8, 2)
        with self.assertRaisesRegex(ValueError, 'not iOS Simulator'):
            self.validate(data)

    def test_refuses_wrong_thin_architecture_or_file_type(self):
        for offset, value in ((4, 0x1000007), (8, 2), (12, 2)):
            with self.subTest(offset=offset):
                data = bytearray(self.data)
                struct.pack_into('<I', data, offset, value)
                with self.assertRaisesRegex(ValueError, 'Expected plain arm64 dylib'):
                    PLACEHOLDER.thin_arm64(data)
                with self.assertRaisesRegex(ValueError, 'Expected plain arm64 dylib header'):
                    self.validate(data)

    def test_universal_slice_must_be_plain_arm64_and_unambiguous(self):
        def universal(architectures):
            data = bytearray(struct.pack('>II', 0xCAFEBABE, len(architectures)))
            for cpu, subtype in architectures:
                data.extend(struct.pack('>5I', cpu, subtype, 4096, len(self.data), 12))
            return data.ljust(4096, b'\0') + self.data

        self.assertEqual(PLACEHOLDER.thin_arm64(universal([(0x100000C, 0)])), self.data)
        for architectures in ([(0x100000C, 2)], [(0x1000007, 3)], [(0x100000C, 0), (0x100000C, 0)]):
            with self.subTest(architectures=architectures):
                with self.assertRaisesRegex(ValueError, 'exactly one plain arm64 slice'):
                    PLACEHOLDER.thin_arm64(universal(architectures))


class TLVValidationTests(unittest.TestCase):
    def setUp(self):
        self.data = bytearray(512)
        self.commands = {0x19: [32]}
        self.layout = {'mappings': [{'address': 0x1000, 'size': 256, 'file_offset': 256}]}
        struct.pack_into('<I', self.data, 32 + 64, 1)
        struct.pack_into('<QQ', self.data, 32 + 72 + 32, 0x1000, 48)
        struct.pack_into('<I', self.data, 32 + 72 + 64, 0x13)

    def test_collects_dynamic_tlv_slots(self):
        self.assertEqual(PLACEHOLDER.tlv_slots(self.data, self.commands, self.layout), [256, 280])

    def test_refuses_fixed_pthread_key(self):
        struct.pack_into('<Q', self.data, 288, 125)
        with self.assertRaisesRegex(ValueError, 'fixed pthread key'):
            PLACEHOLDER.tlv_slots(self.data, self.commands, self.layout)

    def test_refuses_incomplete_descriptor(self):
        struct.pack_into('<Q', self.data, 32 + 72 + 40, 47)
        with self.assertRaisesRegex(ValueError, 'Invalid TLV descriptor section'):
            PLACEHOLDER.tlv_slots(self.data, self.commands, self.layout)


if __name__ == '__main__':
    unittest.main()
