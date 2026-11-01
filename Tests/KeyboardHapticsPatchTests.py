#!/usr/bin/env python3
"""Exercise the pinned haptics patch with synthetic bytes, without runtime inputs or signing."""

import hashlib
import importlib.util
import json
import os
import struct
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('keyboard_haptics_patch', ROOT / 'Tools/patch-ios26-keyboard-haptics.py')
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


def code(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def b(site, target, link=False):
    return (0x94000000 if link else 0x14000000) | (((target - site) // 4) & 0x3FFFFFF)


def page(site, target, register):
    immediate = ((target >> 12) - (site >> 12)) & 0x1FFFFF
    return 0x90000000 | ((immediate & 3) << 29) | ((immediate >> 2) << 5) | register


def load(target, base, destination):
    return 0xF9400000 | (((target & 4095) // 8) << 10) | (base << 5) | destination


def branch_target(site, instruction, bits=26, shift=0):
    immediate = (instruction >> shift) & ((1 << bits) - 1)
    if immediate & (1 << (bits - 1)):
        immediate -= 1 << bits
    return site + immediate * 4


class KeyboardHapticsPatchTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.source = self.directory / 'source-cache'
        self.output = self.directory / 'patched-cache'
        self.manifest = Path(str(self.output) + '.patch-manifest.json')
        self.profile = dict(PATCH.PROFILE, image_count=1)
        self.base = self.profile['base']
        self.header = 0x4000
        self.image = self.base + self.header
        self.got, self.selectors = self.base + 0x84000, self.base + 0x85000
        self.data = bytearray(0xB0080)
        self.data[:16] = b'dyld_v1   arm64\0'
        struct.pack_into('<II', self.data, 16, 0x228, 3)
        struct.pack_into('<QQ', self.data, 40, 0xB0000, 0x80)
        struct.pack_into('<II', self.data, 216, 7, 0x200)
        struct.pack_into('<QQ', self.data, 224, self.base, self.profile['span'])
        struct.pack_into('<II', self.data, 448, 0x500, 1)
        for index, (offset, size, maximum, initial) in enumerate(((0, 0x80000, 5, 5), (0x80000, 0x20000, 3, 1), (0xA0000, 0x10000, 1, 1))):
            struct.pack_into('<QQQII', self.data, 0x228 + index * 32, self.base + offset, size, offset, maximum, initial)
        struct.pack_into('<QQQII', self.data, 0x500, self.image, 0, 0, 0x1000, 0)
        path = PATCH.IMAGE.encode() + b'\0'
        self.data[0x1000:0x1000 + len(path)] = path
        struct.pack_into('<8I', self.data, self.header, 0xFEEDFACF, 0x100000C, 0, 6, 3, 24 + 312 + 232, 0, 0)
        struct.pack_into('<II16s', self.data, self.header + 32, 0x1B, 24, uuid.UUID(PATCH.IMAGE_UUID).bytes)
        self.section_records = {}
        self.text_segment = self.header + 56
        self.make_segment(self.text_segment, '__TEXT', self.header, 0x70000, 5,
                          [('__text', self.header + 0xEF0, 0x48BA8, 0),
                           ('__objc_stubs', self.header + 0x6B020, 0x3D40, 0),
                           ('__gcc_except_tab', self.header + 0x4C9B0, 0x87A8, 0)])
        self.data_segment = self.text_segment + 312
        self.make_segment(self.data_segment, '__DATA_CONST', 0x84000, 0x4000, 3,
                          [('__got', 0x84000, 0x720, 6), ('__objc_selrefs', 0x85000, 0x1310, 0)])
        self.selector_strings = {}
        for index, (stub, slot, name) in enumerate(((0x6B040, 0x28, 'JSONObjectWithData:options:error:'),
                                                   (0x6C3E0, 0x510, 'fileURLWithPath:isDirectory:'),
                                                   (0x6CA00, 0x698, 'initWithContentsOfURL:error:'),
                                                   (0x6CA40, 0x6A8, 'initWithDictionary:error:'))):
            target = self.selectors + slot
            self.put(stub, page(self.image + stub, target, 1), load(target, 1, 1))
            self.put(stub + 16, 0xD61F0200, 0xD4200020, 0xD4200020, 0xD4200020)
            string_offset = 0xA0000 + index * 0x100
            struct.pack_into('<Q', self.data, 0x85000 + slot, self.base + string_offset)
            encoded = name.encode() + b'\0'
            self.data[string_offset:string_offset + len(encoded)] = encoded
            self.selector_strings[stub] = string_offset
        self.put(0x194E0, page(self.image + 0x194E0, self.got + 0x6C0, 8), load(self.got + 0x6C0, 8, 0),
                 0xAA1303E2, 0x52800003, b(0x194F0, 0x6C3E0, True))
        self.put(0x2D18C, page(self.image + 0x2D18C, self.got + 0x168, 8), load(self.got + 0x168, 8, 0))
        self.put(0x2D194, 0xF90003FF, 0x910003E4, 0xAA1A03E2, 0xD2800003, 0x9400F7A7)
        self.put(0x2D1A8, 0x94007356, 0xAA0003FB, 0xF94003E8, 0x940073E3, 0xAA0003F9, 0xF9400388,
                 0xB5000100, 0xB40000FB, 0xB4000688, 0xF9400108, 0x940073DC, 0xAA0003FC, 0xB5000660, 0x14000052)
        self.put(0x2D298, page(self.image + 0x2D298, self.got + 0x4A8, 28), load(self.got + 0x4A8, 28, 28), 0x9400739C, 0xAA1C03E0)
        self.put(0x2D324, page(self.image + 0x2D324, self.got + 0x48, 8), load(self.got + 0x48, 8, 0), 0x940072E6,
                 0xAA1B03E2, 0xAA1503E3, 0x9400FDC2, 0xAA0003FC, 0x91006000, 0xAA1703E1,
                 0x94007387, 0x94007335, 0x94007331, 0x9400732A)
        exception = self.header + 0x4C9B0
        self.data[exception:exception + 0x87A8] = bytes((index % 251 for index in range(0x87A8)))
        self.expected_changes = {
            0x2D18C: code(page(self.image + 0x2D18C, self.got + 0x6C0, 8), load(self.got + 0x6C0, 8, 0)),
            0x2D19C: code(0xAA1803E2),
            0x2D1A4: code(b(0x2D1A4, 0x6C3E0, True)),
            0x2D1C8: code(b(0x2D1C8, 0x2D324)),
            0x2D298: code(0xB40005A0, 0x91006000, 0xAA1703E1, b(0x2D2A4, 0x2D348)),
            0x2D338: code(b(0x2D338, 0x6CA00, True)),
            0x2D340: code(b(0x2D340, 0x2D298)),
        }
        self.write_source()

    def make_segment(self, cursor, owner, offset, size, protection, sections):
        struct.pack_into('<II16sQQQQIIII', self.data, cursor, 0x19, 72 + 80 * len(sections), owner.encode(),
                         self.base + offset, size, offset, size, protection, protection, len(sections), 0)
        for index, (name, section_offset, section_size, flags) in enumerate(sections):
            position = cursor + 72 + index * 80
            struct.pack_into('<16s16sQQ8I', self.data, position, name.encode(), owner.encode(), self.base + section_offset,
                             section_size, section_offset, 0, 0, 0, flags, 0, 0, 0)
            self.section_records[name] = position

    def put(self, relative, *instructions):
        value = code(*instructions)
        offset = self.header + relative
        self.data[offset:offset + len(value)] = value

    def write_source(self):
        self.source.write_bytes(self.data)

    def patch(self):
        return PATCH.patch_cache(self.source, self.output, profile=self.profile)

    def assert_rejected(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.patch()
        self.assertFalse(os.path.lexists(self.output))
        self.assertFalse(os.path.lexists(self.manifest))

    def expected_output(self):
        expected = bytearray(self.data)
        for relative, replacement in self.expected_changes.items():
            offset = self.header + relative
            expected[offset:offset + len(replacement)] = replacement
        return expected

    def test_slidable_pointer_reader_preserves_encoded_cache_bytes(self):
        slots = [0x85000 + relative for relative in (0x28, 0x510, 0x698, 0x6A8)]
        decoded = {offset: struct.unpack_from('<Q', self.data, offset)[0] for offset in slots}
        for offset in slots:
            struct.pack_into('<Q', self.data, offset, decoded[offset] | (8 << 38))
        before, inspected = bytes(self.data), []

        def read(offset):
            inspected.append(offset)
            return decoded[offset]

        report = PATCH.inspect_cache(bytes(self.data), self.profile, pointer_reader=read)
        self.assertEqual(inspected, slots)
        self.assertEqual(bytes(self.data), before)
        self.assertEqual(len(report['sites']), 7)
        decoded[slots[0]] = self.base + 0xA0100  # Different valid selector text must still be rejected.
        with self.assertRaisesRegex(ValueError, 'selector target'):
            PATCH.inspect_cache(bytes(self.data), self.profile, pointer_reader=read)

    def test_exact_patch_preserves_source_other_bytes_and_exception_table(self):
        before = bytes(self.data)
        report = self.patch()
        expected = self.expected_output()
        self.assertEqual(self.output.read_bytes(), expected)
        self.assertEqual(self.source.read_bytes(), before)
        self.assertEqual(report, json.loads(self.manifest.read_text()))
        self.assertEqual(report['source_sha256'], hashlib.sha256(before).hexdigest())
        self.assertEqual(report['output_sha256'], hashlib.sha256(expected).hexdigest())
        self.assertEqual(report['changed_bytes'], sum(a != b for a, b in zip(before, expected)))
        self.assertEqual({site['image_relative'] for site in report['sites']}, set(self.expected_changes))
        exception = self.header + 0x4C9B0
        self.assertEqual(report['preserved_exception_table_sha256'], hashlib.sha256(before[exception:exception + 0x87A8]).hexdigest())
        self.assertTrue(report['signature_invalidated'] and report['requires_separate_signing'])
        self.assertEqual(self.output.stat().st_mode & 0o777, self.source.stat().st_mode & 0o777)

    def test_guard_routes_nil_to_cleanup_and_non_nil_to_original_store_call(self):
        self.patch()
        output = self.output.read_bytes()

        def instruction(relative):
            return struct.unpack_from('<I', output, self.header + relative)[0]

        self.assertEqual(branch_target(0x2D1C8, instruction(0x2D1C8)), 0x2D324)
        self.assertEqual(branch_target(0x2D338, instruction(0x2D338)), 0x6CA00)
        self.assertEqual(branch_target(0x2D340, instruction(0x2D340)), 0x2D298)
        self.assertEqual(instruction(0x2D298) & 0xFF00001F, 0xB4000000)  # CBZ x0, cleanup.
        self.assertEqual(branch_target(0x2D298, instruction(0x2D298), 19, 5), 0x2D34C)
        self.assertEqual(instruction(0x2D29C), 0x91006000)  # ADD x0, x0, #0x18 only on non-nil fallthrough.
        self.assertEqual(instruction(0x2D2A0), 0xAA1703E1)  # MOV x1, x23 preserves the pattern ID.
        self.assertEqual(branch_target(0x2D2A4, instruction(0x2D2A4)), 0x2D348)
        for relative in (0x2D330, 0x2D334, 0x2D33C, 0x2D348, 0x2D34C):
            self.assertEqual(instruction(relative), struct.unpack_from('<I', self.data, self.header + relative)[0])

    def test_streaming_patch_crosses_instruction_boundary(self):
        with mock.patch.object(PATCH, 'CHUNK_SIZE', self.header + 0x2D18F):
            self.patch()
        self.assertEqual(self.output.read_bytes(), self.expected_output())

    def test_check_only_writes_nothing_and_default_profile_rejects_fixture(self):
        report = PATCH.patch_cache(self.source, profile=self.profile)
        self.assertTrue(report['validation_only'])
        self.assertEqual([item.name for item in self.directory.iterdir()], ['source-cache'])
        with self.assertRaisesRegex(ValueError, 'image table'):
            PATCH.patch_cache(self.source, self.output)
        self.assertFalse(self.output.exists())

    def test_all_original_instruction_groups_reject_mutation(self):
        original = bytes(self.data)
        relatives = list(self.expected_changes) + [0x194E0, 0x2D194, 0x2D1A8, 0x2D1CC, 0x2D324, 0x2D348, 0x6C3F0]
        for relative in relatives:
            with self.subTest(relative=hex(relative)):
                self.data[:] = original
                self.data[self.header + relative] ^= 1
                self.write_source()
                self.assert_rejected('Original instruction mismatch|Unexpected branch')

    def test_partial_and_repeated_patch_rejected(self):
        original = bytes(self.data)
        for relative, replacement in self.expected_changes.items():
            with self.subTest(relative=hex(relative)):
                self.data[:] = original
                offset = self.header + relative
                self.data[offset:offset + len(replacement)] = replacement
                self.write_source()
                self.assert_rejected('Original instruction mismatch|Unexpected branch')
        self.data[:] = original
        self.data[:] = self.expected_output()
        self.write_source()
        self.assert_rejected('Original instruction mismatch|Unexpected branch')

    def test_unknown_incoming_branches_to_guard_island_rejected(self):
        original = bytes(self.data)
        for target in (0x2D298, 0x2D29C, 0x2D2A0, 0x2D2A4):
            for instruction in (b(0x30000, target), b(0x30000, target, True),
                                0x54000000 | ((((target - 0x30000) // 4) & 0x7FFFF) << 5),
                                0xB5000000 | ((((target - 0x30000) // 4) & 0x7FFFF) << 5),
                                0x36000000 | ((((target - 0x30000) // 4) & 0x3FFF) << 5)):
                with self.subTest(target=hex(target), instruction=hex(instruction)):
                    self.data[:] = original
                    self.put(0x30000, instruction)
                    self.write_source()
                    self.assert_rejected('Unexpected branch')

    def test_selector_targets_and_original_stubs_are_validated(self):
        original = bytes(self.data)
        for stub, offset in self.selector_strings.items():
            for position in (offset, offset + len(self.data[offset:].split(b'\0', 1)[0]), self.header + stub):
                with self.subTest(stub=hex(stub), position=hex(position)):
                    self.data[:] = original
                    self.data[position] ^= 1
                    self.write_source()
                    self.assert_rejected('selector target|Original instruction mismatch')
        self.data[:] = original
        struct.pack_into('<Q', self.data, 0x85028, self.base + 0xB0000)
        self.write_source()
        self.assert_rejected('cache mapping')

    def test_cache_identity_and_tables(self):
        original = bytes(self.data)
        changes = [(self.header + 40, '<16s', (bytes(16),), 'UUID'),
                   (224, '<Q', (0x120000000,), 'reservation'), (232, '<Q', (0x1000,), 'reservation'),
                   (216, '<I', (2,), 'simulator'), (220, '<I', (0,), 'simulator'), (396, '<I', (1,), 'Subcaches'),
                   (452, '<I', (2,), 'image table'), (448, '<I', (0xAFFF8,), 'image table'),
                   (40, '<Q', (0xA0000,), 'signature extent'), (48, '<Q', (1,), 'signature extent'),
                   (16, '<I', (0xAFFE0,), 'mapping table'), (20, '<I', (65,), 'mapping table'),
                   (self.header + 20, '<I', (567,), 'image command')]
        for offset, format_string, values, message in changes:
            with self.subTest(offset=hex(offset), values=values):
                self.data[:] = original
                struct.pack_into(format_string, self.data, offset, *values)
                self.write_source()
                self.assert_rejected(message)

    def test_mapping_permissions_and_nonoverlap(self):
        original = bytes(self.data)
        changes = [(0x228 + 24, '<II', (7, 5), 'mapping protections'),
                   (0x228 + 24, '<II', (5, 1), 'mapping protections'),
                   (0x228 + 32 + 24, '<II', (1, 1), 'mapping protections'),
                   (0x228 + 32 + 24, '<II', (3, 7), 'Invalid mapping protections'),
                   (0x228 + 32, '<Q', (self.base + 0x70000,), 'Overlapping VM'),
                   (0x228 + 32 + 16, '<Q', (0x70000,), 'Overlapping file'),
                   (0x228 + 8, '<Q', (0x10000,), 'cache mapping'),
                   (0x228 + 8, '<Q', (0xC0000,), 'reservation/payload')]
        for offset, format_string, values, message in changes:
            with self.subTest(offset=hex(offset), values=values):
                self.data[:] = original
                struct.pack_into(format_string, self.data, offset, *values)
                self.write_source()
                self.assert_rejected(message)

    def test_pinned_section_extent_coordinates_type_and_duplicates(self):
        original = bytes(self.data)
        text, got = self.section_records['__text'], self.section_records['__got']
        changes = [(text + 32, '<Q', (self.image + 0xEF4,), 'section extent'),
                   (text + 40, '<Q', (0x48BA4,), 'section extent'),
                   (text + 48, '<I', (self.header + 0xEF4,), 'coordinates disagree'),
                   (text + 64, '<I', (6,), 'section type'), (got + 64, '<I', (0,), 'section type'),
                   (self.text_segment + 32, '<Q', (0x1000,), 'outside owning segment'),
                   (self.text_segment + 48, '<Q', (0x1000,), 'coordinates disagree'),
                   (self.text_segment + 60, '<I', (1,), 'segment protections')]
        for offset, format_string, values, message in changes:
            with self.subTest(offset=hex(offset), values=values):
                self.data[:] = original
                struct.pack_into(format_string, self.data, offset, *values)
                self.write_source()
                self.assert_rejected(message)
        self.data[:] = original
        duplicate = self.section_records['__objc_stubs']
        self.data[duplicate:duplicate + 80] = self.data[text:text + 80]
        self.write_source()
        self.assert_rejected('Duplicate')

    def test_duplicate_uuid_or_image_and_missing_image(self):
        original = bytes(self.data)
        struct.pack_into('<II16s', self.data, self.text_segment, 0x1B, 24, uuid.UUID(PATCH.IMAGE_UUID).bytes)
        self.write_source()
        self.assert_rejected('duplicate image UUID')
        self.data[:] = original
        self.data[0x520:0x540] = self.data[0x500:0x520]
        struct.pack_into('<I', self.data, 452, 2)
        self.profile['image_count'] = 2
        self.write_source()
        self.assert_rejected('duplicate image path')
        self.profile['image_count'] = 1
        self.data[:] = original
        self.data[0x1000:0x100D] = b'/wrong/image\0'
        self.write_source()
        self.assert_rejected('Missing CoreHaptics')

    def test_source_symlink_and_noncanonical_parent_rejected(self):
        link = self.directory / 'source-link'
        link.symlink_to(self.source)
        with self.assertRaisesRegex(ValueError, 'regular file'):
            PATCH.patch_cache(link, self.output, profile=self.profile)
        directory_link = self.directory / 'directory-link'
        directory_link.symlink_to(self.directory, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'canonical'):
            PATCH.patch_cache(directory_link / self.source.name, self.output, profile=self.profile)
        self.assertFalse(self.output.exists())

    def test_existing_destinations_and_aliases_are_preserved(self):
        with self.assertRaisesRegex(ValueError, 'distinct'):
            PATCH.patch_cache(self.source, self.source, profile=self.profile)
        with self.assertRaisesRegex(ValueError, 'distinct'):
            PATCH.patch_cache(self.source, self.output, manifest=self.source, profile=self.profile)
        self.output.write_bytes(b'existing output')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.patch()
        self.assertEqual(self.output.read_bytes(), b'existing output')
        self.output.unlink()
        self.manifest.write_text('existing manifest')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.patch()
        self.assertEqual(self.manifest.read_text(), 'existing manifest')
        self.manifest.unlink()
        self.output.symlink_to(self.directory / 'missing')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.patch()
        self.output.unlink()
        os.link(self.source, self.output)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.patch()
        self.assertEqual(self.source.read_bytes(), self.data)

    def test_failed_output_flush_cleans_only_new_output(self):
        with mock.patch.object(PATCH.os, 'fsync', side_effect=OSError('synthetic flush failure')):
            with self.assertRaisesRegex(OSError, 'synthetic flush failure'):
                self.patch()
        self.assertFalse(self.output.exists())
        self.assertFalse(self.manifest.exists())
        self.assertEqual(self.source.read_bytes(), self.data)

    def test_failed_copy_does_not_remove_a_replaced_output_inode(self):
        retained = self.directory / 'retained-incomplete-output'

        def replace_output(_descriptor):
            self.output.rename(retained)
            self.output.write_bytes(b'concurrently created replacement')
            raise OSError('synthetic replacement failure')

        with mock.patch.object(PATCH.os, 'fsync', side_effect=replace_output):
            with self.assertRaisesRegex(OSError, 'synthetic replacement failure'):
                self.patch()
        self.assertEqual(self.output.read_bytes(), b'concurrently created replacement')
        self.assertEqual(retained.read_bytes(), self.expected_output())
        self.assertFalse(self.manifest.exists())
        self.assertEqual(self.source.read_bytes(), self.data)


if __name__ == '__main__':
    unittest.main()
