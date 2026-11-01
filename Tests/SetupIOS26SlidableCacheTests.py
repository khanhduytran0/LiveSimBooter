#!/usr/bin/env python3
"""Validate slidable-cache build/patch boundaries with synthetic bytes; no Apple inputs or signing required."""

import hashlib
import importlib.util
from pathlib import Path
import re
import struct
import tempfile
import unittest
import uuid
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'Tools' / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


BUILD = load('slidable_build_test', 'setup-ios26-slidable-cache.py')
PATCH = load('slidable_patch_test', 'patch-ios26-slidable-cache.py')


class SlidableCacheTests(unittest.TestCase):
    def setUp(self):
        self.data = bytearray(0x14080)
        self.data[:16] = b'dyld_v1   arm64\0'
        self.data[88:104] = uuid.UUID('12345678-1234-5678-1234-567812345678').bytes
        struct.pack_into('<II', self.data, 16, 512, 3)
        struct.pack_into('<QQ', self.data, 40, 0x14000, 0x80)
        struct.pack_into('<II', self.data, 216, 7, 0x200)
        struct.pack_into('<QQQ', self.data, 224, BUILD.BASE, 0x14000, 0x20000000)
        struct.pack_into('<II', self.data, 312, 640, 3)
        self.profile = ((0, 0x4000, 0, 0, 0, 0, 5, 5),
                        (0x4000, 0x4000, 0x4000, 0xC000, 48, 0, 3, 1),
                        (0x8000, 0xC000, 0x8000, 0, 0, 0, 1, 1))
        for index, row in enumerate(self.profile):
            address, size, fileoff, slideoff, slidesize, flags, maximum, initial = row
            struct.pack_into('<QQQII', self.data, 512 + index * 32, BUILD.BASE + address, size, fileoff, maximum, initial)
            struct.pack_into('<6QII', self.data, 640 + index * 56, BUILD.BASE + address, size, fileoff, slideoff, slidesize, flags, maximum, initial)
        struct.pack_into('<6I2Q', self.data, 0xC000, 2, 4096, 40, 4, 0, 0, BUILD.DELTA_MASK, 0)
        struct.pack_into('<4H', self.data, 0xC028, 0, 0x4000, 0x4000, 0x4000)
        struct.pack_into('<QQ', self.data, 0x4000, BUILD.BASE + 0x1000 | (8 << 38), BUILD.BASE + 0x1100 | (0x80 << 56))

    def layout(self):
        return BUILD.inspect_slide_layout(self.data, expected_mappings=self.profile, expected_span=0x14000)

    def test_exact_chain_preserves_tag_and_excludes_pointer_lookalikes(self):
        struct.pack_into('<Q', self.data, 0x4010, BUILD.BASE + 0x1200)
        layout = self.layout()
        expected = [(1, 0x4000, BUILD.BASE + 0x1000), (1, 0x4008, BUILD.BASE + 0x1100 | (0x80 << 56))]
        self.assertEqual(list(BUILD.iter_fixups(self.data, layout)), expected)
        read = BUILD.pointer_reader(self.data, layout)
        self.assertEqual(read(0x4008), expected[1][2])
        with self.assertRaisesRegex(ValueError, 'not a member'):
            read(0x4010)
        with self.assertRaisesRegex(ValueError, 'outside a writable'):
            read(0x1000)

    def test_zero_pointer_and_empty_pages(self):
        struct.pack_into('<Q', self.data, 0x4008, 0)
        self.assertEqual(list(BUILD.iter_fixups(self.data, self.layout()))[-1], (1, 0x4008, 0))
        struct.pack_into('<H', self.data, 0xC028, 0x4000)
        self.assertEqual(list(BUILD.iter_fixups(self.data, self.layout())), [])

    def test_invalid_encoding_and_targets(self):
        original = bytes(self.data)
        for offset, fmt, value, message in ((0xC000, '<I', 3, 'slide version'), (0xC004, '<I', 16384, 'page coverage'),
                                          (0xC008, '<I', 41, 'page-start'), (0xC014, '<I', 1, 'extras'),
                                          (0xC018, '<Q', 0xFFFF000000000000, 'encoding'),
                                          (0xC020, '<Q', 1, 'encoding')):
            with self.subTest(offset=offset):
                self.data[:] = original
                struct.pack_into(fmt, self.data, offset, value)
                with self.assertRaisesRegex(ValueError, message):
                    self.layout()
        for value, message in ((4 << 38 | BUILD.BASE, 'Overlapping'), (BUILD.BASE - 8, 'outside cache')):
            self.data[:] = original
            struct.pack_into('<Q', self.data, 0x4000, value)
            with self.assertRaisesRegex(ValueError, message):
                list(BUILD.iter_fixups(self.data, self.layout()))

    def test_page_boundaries_and_extra_starts(self):
        for start, message in ((0x8000, 'extra'), (0x3FF, 'crosses page')):
            with self.subTest(start=start):
                struct.pack_into('<H', self.data, 0xC028, start)
                with self.assertRaisesRegex(ValueError, message):
                    list(BUILD.iter_fixups(self.data, self.layout()))

    def test_profile_and_header_mismatches(self):
        original = bytes(self.data)
        for offset, fmt, value, message in ((216, '<I', 2, 'simulator'), (220, '<I', 0, 'simulator'),
                                          (224, '<Q', 0x110000000, 'base/span'), (232, '<Q', 0x18000, 'base/span'),
                                          (240, '<Q', 0, 'maxSlide'), (396, '<I', 1, 'simulator'),
                                          (48, '<Q', 0x40, 'signature'), (316, '<I', 2, 'mapping count'),
                                          (512 + 24, '<I', 3, 'tables disagree'), (640 + 48, '<I', 7, 'Unreviewed mapping')):
            with self.subTest(offset=offset):
                self.data[:] = original
                struct.pack_into(fmt, self.data, offset, value)
                with self.assertRaisesRegex(ValueError, message):
                    self.layout()
        self.data[:] = original
        self.data[88:104] = bytes(16)
        with self.assertRaisesRegex(ValueError, 'UUID'):
            self.layout()

    def test_only_slide_and_size_builder_instructions_change(self):
        original = bytearray(0x60000)
        for offset, before, _, _ in BUILD.PATCHES:
            struct.pack_into('<I', original, offset, before)
        stock_layout = ((0x5EA5C, 0xB26107E8), (0x5EA60, 0xD2C00029), (0x5EA64, 0x9A881129))
        for offset, word in stock_layout:
            struct.pack_into('<I', original, offset, word)
        expected = bytearray(original)
        for offset, _, after, _ in BUILD.PATCHES:
            struct.pack_into('<I', expected, offset, after)
        inventory = dict(builder_sha256=hashlib.sha256(original).hexdigest())
        actual = BUILD.setup.patch_builder(original, inventory, patches=BUILD.PATCHES, expected_sha256=hashlib.sha256(expected).hexdigest())
        self.assertEqual(actual, expected)
        for offset, word in stock_layout:
            self.assertEqual(struct.unpack_from('<I', actual, offset)[0], word)
        struct.pack_into('<I', original, BUILD.PATCHES[0][0], 0)
        inventory['builder_sha256'] = hashlib.sha256(original).hexdigest()
        with self.assertRaisesRegex(ValueError, 'instruction mismatch'):
            BUILD.setup.patch_builder(original, inventory, patches=BUILD.PATCHES, expected_sha256=hashlib.sha256(expected).hexdigest())

    def test_export_trie_requires_exact_regular_symbol_and_bounded_uleb(self):
        symbol = PATCH.MASK_SYMBOL
        prefix = b'\0\1' + symbol.encode() + b'\0'
        trie = prefix + bytes([len(prefix) + 1]) + b'\2\0\10\0'
        self.assertEqual(PATCH.trie_export(trie, symbol), 8)
        with self.assertRaisesRegex(ValueError, 'missing'):
            PATCH.trie_export(trie, '_other')
        with self.assertRaisesRegex(ValueError, 'Cyclic'):
            PATCH.trie_export(prefix + b'\0', symbol)
        with self.assertRaisesRegex(ValueError, 'ULEB128'):
            PATCH.trie_export(b'\xff' * 11, symbol)
        invalid = bytearray(trie)
        invalid[-3] = 8
        with self.assertRaisesRegex(ValueError, 'regular'):
            PATCH.trie_export(bytes(invalid), symbol)

    def test_output_collisions_fail_before_cache_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary).resolve()
            source, output = folder / 'source', folder / 'output'
            source.write_bytes(b'not a cache')
            output.write_bytes(b'keep')
            with self.assertRaisesRegex(ValueError, 'existing output'):
                PATCH.patch_cache(source, output)
            self.assertEqual(output.read_bytes(), b'keep')
            with self.assertRaisesRegex(ValueError, 'in-place'):
                PATCH.patch_cache(source, source)

    def test_native_and_setup_profile_agree(self):
        header = (ROOT / 'ProcessSupport/SlidableCache.h').read_text()
        table = header.split('LSBSlidableCacheProfile[6] = {', 1)[1].split('};', 1)[0]
        rows = []
        for line in re.findall(r'\{([^{}]+)\}', table):
            fields = [int(value.strip(), 0) for value in line.split(',')]
            fields[0] -= BUILD.BASE
            rows.append(tuple(fields))
        self.assertEqual(tuple(rows), BUILD.MAPPINGS)
        for name, expected in (('FIXUPS', BUILD.FIXUPS), ('TAGGED', BUILD.TAGGED), ('SPAN', BUILD.SPAN)):
            value = re.search(r'#define LSB_SLIDABLE_CACHE_' + name + r' UINT64_C\(([^)]+)\)', header)
            self.assertIsNotNone(value)
            self.assertEqual(int(value[1], 0), expected)

    def test_failed_copy_removes_only_its_partial_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary).resolve()
            source, output = folder / 'source', folder / 'output'
            source.write_bytes(b'input')

            def fail_after_write(stream, destination, size):
                destination.write(b'partial')
                raise OSError('injected copy failure')

            with source.open('rb') as stream, mock.patch.object(PATCH.ctypes, 'CDLL', return_value=object()), \
                    mock.patch.object(PATCH.shutil, 'copyfileobj', side_effect=fail_after_write):
                with self.assertRaisesRegex(OSError, 'injected copy failure'):
                    PATCH.clone_or_copy(stream, output, 5)
            self.assertFalse(output.exists())
            self.assertEqual(source.read_bytes(), b'input')


if __name__ == '__main__':
    unittest.main()
