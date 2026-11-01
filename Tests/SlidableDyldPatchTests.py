#!/usr/bin/env python3
"""Validate the pinned loader patch without requiring Apple runtime binaries or executable anonymous memory."""
import importlib.util
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('cache_slide_patch', ROOT / 'Tools/patch-ios26-cache-slide.py')
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)

# Independent assembly of the stub. Labels exercise branch displacement and the actual linked virtual offsets;
# the ADRP operand is the verified distance between the code page (0x4d000) and writable configuration page (0x7c000).
ASSEMBLY = '''.text
.org 0x1c170
prepare_continue:
.org 0x1c484
cleanup_continue:
.org 0x4d540
movz x9,#0
movk x9,#0x8000,lsl #16
movk x9,#1,lsl #32
movk x9,#0,lsl #48
cmp x8,x9
b.ne ordinary
adrp x9,#0x2f000
ldr x9,[x9,#0x220]
cbz x9,missing
sub x9,x9,x8
add x8,x8,x9
stp x8,x9,[x19]
b prepare_continue
ordinary:
stp x8,xzr,[x19]
b prepare_continue
missing:
brk #1000
ldr x0,[sp,#0x1200]
ldr x1,[sp,#0x1208]
ldr x9,[x19,#8]
add x0,x0,x9
b cleanup_continue
'''


def text_section(data):
    offset = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, length = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            for index in range(struct.unpack_from('<I', data, offset + 64)[0]):
                section = struct.unpack_from('<16s16sQQIIIIIIII', data, offset + 72 + index * 80)
                if section[0].rstrip(b'\0') == b'__text':
                    return data[section[4]:section[4] + section[3]]
        offset += length
    raise AssertionError('Assembler output has no text section')


def discovery_fixture():
    def segment(name, address, fileoff, size, protection, sections):
        result = struct.pack('<II16sQQQQiiII', 0x19, 72 + len(sections) * 80, name, address, size, fileoff, size,
                             protection, protection, len(sections), 0)
        for section_name, start, length, offset, flags in sections:
            result += struct.pack('<16s16sQQIIIIIIII', section_name, name, start, length, offset, 2, 0, 0, flags, 0, 0, 0)
        return result

    text = segment(b'__TEXT', 0, 0, 0x7000, 5,
                   [(b'__text', 0x1000, 0x3000, 0x1000, 0x80000400), (b'__const', 0x4000, 0x31, 0x4000, 0)])
    data = segment(b'__DATA_DIRTY', 0xa000, 0x7000, 0x2000, 3, [(b'__data', 0xa000, 0x154, 0x7000, 0)])
    fixture = bytearray(0x9000)
    fixture[:32] = struct.pack('<IIIIIIII', 0xFEEDFACF, 0x0100000C, 0, 7, 2, len(text) + len(data), 0, 0)
    fixture[32:32 + len(text) + len(data)] = text + data
    positions = (0x1100, 0x1800, 0x2100)
    for position, (_, pattern, _) in zip(positions, PATCH.CONTEXTS):
        fixture[position:position + len(pattern)] = pattern
    return fixture, positions


class SlidableDyldPatchTests(unittest.TestCase):
    def test_stub_matches_independent_arm64_assembler(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / 'stub.s', Path(directory) / 'stub.o'
            source.write_text(ASSEMBLY)
            subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64', '-c', str(source), '-o', str(output)], check=True)
            expected, cleanup = PATCH.build_stub()
            self.assertEqual(text_section(output.read_bytes())[PATCH.TEXT_PADDING:], expected)
            self.assertEqual(cleanup, 0x4d580)

    def test_config_abi_and_no_selected_base_in_payload(self):
        magic, version, size, preferred, actual, span, uuid = struct.unpack('<16sIIQQQ16s', PATCH.CONFIG)
        self.assertEqual((magic, version, size, preferred), (b'LSBDyldSlideV1\0\0', 1, 64, 0x180000000))
        self.assertEqual((actual, span, uuid), (0, 0, bytes(16)))
        self.assertEqual(PATCH.CONFIG_VM_OFFSET - PATCH.CONFIG_FILE_OFFSET, 0x20000)

    def test_unknown_binary_and_overflowing_instruction_fail_closed(self):
        with self.assertRaises(ValueError):
            PATCH.patched_bytes(bytes(0x84000))
        for target in (1, 1 << 28, -(1 << 28)):
            with self.assertRaises(ValueError):
                PATCH.branch(0, target)
        with self.assertRaises(ValueError):
            PATCH.adrp(9, 0, 1 << 34)

    def test_finder_discovers_moved_contexts_and_parsed_padding_without_applying_them(self):
        fixture, positions = discovery_fixture()
        discovery = PATCH.discover_layout(fixture)
        for position, (_, _, members) in zip(positions, PATCH.CONTEXTS):
            for name, relative in members:
                self.assertEqual(int(discovery['sites'][name]['file_offset'], 16), position + relative)
        self.assertEqual(discovery['text_padding']['file_offset'], '0x4040')
        self.assertEqual(discovery['data_configuration']['file_offset'], '0x7200')
        self.assertEqual(discovery['data_configuration']['vm_address'], '0xa200')
        with self.assertRaisesRegex(ValueError, 'differs from the reviewed'):
            PATCH.validate_reviewed_layout(discovery)

    def test_finder_rejects_duplicate_or_missing_instruction_contexts(self):
        for index, (label, pattern, _) in enumerate(PATCH.CONTEXTS):
            fixture, positions = discovery_fixture()
            fixture[0x3000:0x3000 + len(pattern)] = pattern
            with self.assertRaisesRegex(ValueError, f'Ambiguous instruction context: {label}'):
                PATCH.discover_layout(fixture)
            fixture, positions = discovery_fixture()
            fixture[positions[index]] ^= 1
            with self.assertRaisesRegex(ValueError, f'Missing instruction context: {label}'):
                PATCH.discover_layout(fixture)

    def test_finder_rejects_nonzero_padding_and_duplicate_segment(self):
        for position in (0x4040, 0x7200):
            fixture, _ = discovery_fixture()
            fixture[position] = 1
            with self.assertRaisesRegex(ValueError, 'padding is not zero'):
                PATCH.discover_layout(fixture)
        fixture, _ = discovery_fixture()
        second_segment = 32 + 72 + 2 * 80
        fixture[second_segment + 8:second_segment + 24] = b'__TEXT'.ljust(16, b'\0')
        with self.assertRaisesRegex(ValueError, 'Ambiguous duplicate segment'):
            PATCH.discover_layout(fixture)


if __name__ == '__main__':
    unittest.main()
