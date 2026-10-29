#!/usr/bin/env python3
"""Patch only 23A343 arm64 launchd_sim's nine audit-PID owner reads.

Creates a new executable and manifest without signing or modifying the source. The output MUST be
signed again: its old signature is retained only as an allocation/template for codesign --force.
The injected shim must export LSBLaunchdResolveAuditPID; a missing export resolves to PID -1.
Kernel audit tokens remain untouched. This is not a general launchd or PID-namespace patch.
Earlier patch revisions are deliberately rejected: rebuild from the original or unpatched re-signed binary.
"""

import argparse
import hashlib
import json
import stat
import struct
from pathlib import Path


UUID = bytes.fromhex('c3ec5c458c853d11b59c62c4248f5ef9')
UUID_TEXT = 'C3EC5C45-8C85-3D11-B59C-62C4248F5EF9'
BASE = 0x100000000
ORIGINAL_SIGNATURE = 0x67B70
PAYLOAD_HASH = 'dfcc9522dd657eeb42c6e4697514f6332c613bd0e66969a80e762e15e0ee2524'
FIXUPS_OFFSET, FIXUPS_SIZE = 0x60000, 10416
FIXUPS_COMMAND = 0xA48
SIGNATURE_COMMAND = 0xBE0
LINKEDIT_COMMAND = 0xA00
CHAIN_TAIL, CHAIN_TAIL_WORD = 0x59070, 0x5D218
GOT_SLOT = 0x5BFF8
# Original __TEXT ends at 0x54000; its last section (__unwind_info) ends at 0x51b8c.
# The pinned payload hash and zero-padding check cover this entire nine-veneer allocation.
CAVE_BEGIN, CAVE_END, VENEER_STRIDE = 0x51C00, 0x52F00, 0x200
SYMBOL = b'LSBLaunchdResolveAuditPID\0'
FRAME_SIZE = 0x320
# Offset, exact original LDR, token register (None means original SP), destination W register.
SITES = ((0x236B0, 0xB9401421, 1, 1), (0x2E658, 0xB9401681, 20, 1),
         (0x2E6B8, 0xB9401688, 20, 8), (0x1BC08, 0xB94017E0, None, 0),
         (0x1DB98, 0xB9401429, 1, 9),  # Routine 718: XPC service dictionary owner comparison.
         (0x2D130, 0xB9401440, 2, 0),  # Requestor domain: choose the managed job's domain before its existing fallback.
         (0x30048, 0xB94016C0, 22, 0),  # Routine 403: event publisher fetch subscriptions; preserve its stream checks.
         (0x2FFD8, 0xB94016C0, 22, 0),  # Routine 406: event publisher arm poll; preserve its stream checks.
         (0x1DAE4, 0xB9401428, 1, 8))  # Routine 716: extension alive check-in; preserve owner comparison and startup state.
SYMBOL_OFFSET = CAVE_BEGIN + len(SITES) * VENEER_STRIDE


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def branch(pc, target):
    delta = target - pc
    require(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), 'B target outside signed imm26 range')
    return 0x14000000 | ((delta // 4) & 0x3FFFFFF)


def adr(register, pc, target, page=False):
    delta = ((target >> 12) - (pc >> 12)) if page else target - pc
    require(-(1 << 20) <= delta < (1 << 20), 'ADR/ADRP target outside signed imm21 range')
    return (0x90000000 if page else 0x10000000) | ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5) | register


def pair(first, second, offset, load=False, vector=False):
    scale = 16 if vector else 8
    require(offset % scale == 0 and 0 <= offset // scale < 64, 'Pair offset outside positive signed imm7 range')
    opcode = (0xAD000000 if vector else 0xA9000000) | (0x400000 if load else 0)
    return opcode | ((offset // scale) << 15) | (second << 10) | (31 << 5) | first


def stack_word(register, offset, load=False):
    require(offset % 8 == 0 and 0 <= offset // 8 < 4096, 'Stack offset outside scaled imm12 range')
    return (0xF9400000 if load else 0xF9000000) | ((offset // 8) << 10) | (31 << 5) | register


def build_veneer(index, external_labels=False):
    """Return the exact machine code and matching assembly; external labels support the native ABI harness.

    The harness supplies _LSBTestDlsymPointer, _LSBTestSymbolName and _LSBTestResume{index}. It replaces
    only the address relocations. Every save, restore, call, flag operation and local branch is shared.
    """
    site, _, token_register, destination = SITES[index]
    start = CAVE_BEGIN + index * VENEER_STRIDE
    words, assembly, labels, pending = [], [], {}, []
    if external_labels:
        assembly.extend(('.text', '.p2align 2', f'.globl _LSBTestVeneer{index}', f'_LSBTestVeneer{index}:'))

    def emit(word, text):
        words.append(word)
        assembly.append('    ' + text)

    def label(name):
        labels[name] = start + 4 * len(words)
        assembly.append(f'.L{name}{index}:')

    def to_label(name, zero=False):
        pending.append((len(words), name, zero))
        emit(0, f'{"cbz x0," if zero else "b"} .L{name}{index}')

    emit(0xD10003FF | (FRAME_SIZE << 10), f'sub sp, sp, #{FRAME_SIZE}')
    for register in range(0, 32, 2):
        second = 'xzr' if register == 30 else f'x{register + 1}'
        emit(pair(register, register + 1, register * 8), f'stp x{register}, {second}, [sp, #{register * 8}]')
    for name, opcode, offset in (('nzcv', 0xD53B4210, 0x100), ('fpcr', 0xD53B4410, 0x108), ('fpsr', 0xD53B4430, 0x110)):
        emit(opcode, f'mrs x16, {name}')
        emit(stack_word(16, offset), f'str x16, [sp, #{offset}]')
    for register in range(0, 32, 2):
        offset = 0x120 + register * 16
        emit(pair(register, register + 1, offset, vector=True), f'stp q{register}, q{register + 1}, [sp, #{offset}]')
    if token_register is None:
        emit(0x910003F3 | (FRAME_SIZE << 10), f'add x19, sp, #{FRAME_SIZE}')
    else:
        emit(stack_word(19, token_register * 8, load=True), f'ldr x19, [sp, #{token_register * 8}]')
    emit(0x92800020, 'mov x0, #-2')  # Darwin RTLD_DEFAULT, not an image-specific handle.
    pc = start + len(words) * 4
    emit(adr(1, pc, SYMBOL_OFFSET), 'adr x1, _LSBTestSymbolName' if external_labels else f'adr x1, . + {SYMBOL_OFFSET - pc}')
    pc = start + len(words) * 4
    instruction = adr(16, pc, GOT_SLOT, page=True)
    emit(instruction, 'adrp x16, _LSBTestDlsymPointer@PAGE' if external_labels else f'.inst {instruction:#x} // adrp x16, dlsym GOT page')
    instruction = 0xF9400000 | (((GOT_SLOT & 0xFFF) // 8) << 10) | (16 << 5) | 16
    emit(instruction, 'ldr x16, [x16, _LSBTestDlsymPointer@PAGEOFF]' if external_labels else f'ldr x16, [x16, #{GOT_SLOT & 0xFFF}]')
    emit(0xD63F0200, 'blr x16')
    to_label('missing', zero=True)
    emit(0xAA0003F0, 'mov x16, x0')
    emit(0xAA1303E0, 'mov x0, x19')
    emit(0xD63F0200, 'blr x16')
    to_label('result')
    label('missing')
    emit(0x12800000, 'mov w0, #-1')  # An absent shim must never accept the old native/proxy owner.
    label('result')
    emit(0x2A0003E0, 'mov w0, w0')  # Match LDR W's zero extension even if an ABI return has undefined upper bits.
    emit(stack_word(0, destination * 8), f'str x0, [sp, #{destination * 8}]')
    for register in range(0, 32, 2):
        offset = 0x120 + register * 16
        emit(pair(register, register + 1, offset, load=True, vector=True), f'ldp q{register}, q{register + 1}, [sp, #{offset}]')
    for name, opcode, offset in (('fpcr', 0xD51B4410, 0x108), ('fpsr', 0xD51B4430, 0x110), ('nzcv', 0xD51B4210, 0x100)):
        emit(stack_word(16, offset, load=True), f'ldr x16, [sp, #{offset}]')
        emit(opcode, f'msr {name}, x16')
    for register in range(0, 32, 2):
        second = 'xzr' if register == 30 else f'x{register + 1}'
        emit(pair(register, register + 1, register * 8, load=True), f'ldp x{register}, {second}, [sp, #{register * 8}]')
    emit(0x910003FF | (FRAME_SIZE << 10), f'add sp, sp, #{FRAME_SIZE}')
    pc = start + len(words) * 4
    emit(branch(pc, site + 4), f'b _LSBTestResume{index}' if external_labels else f'b . + {site + 4 - pc}')
    for word_index, name, zero in pending:
        pc, target = start + word_index * 4, labels[name]
        words[word_index] = (0xB4000000 | (((target - pc) // 4) << 5)) if zero else branch(pc, target)
    result = struct.pack('<' + 'I' * len(words), *words)
    require(len(result) <= VENEER_STRIDE, 'Veneer exceeds reserved slot')
    return result, '\n'.join(assembly) + '\n'


def cave_bytes():
    require(SYMBOL_OFFSET + len(SYMBOL) <= CAVE_END, 'Veneers and resolver name exceed verified padding')
    result = bytearray(CAVE_END - CAVE_BEGIN)
    for index in range(len(SITES)):
        code, _ = build_veneer(index)
        begin = index * VENEER_STRIDE
        result[begin:begin + len(code)] = code
    result[SYMBOL_OFFSET - CAVE_BEGIN:SYMBOL_OFFSET - CAVE_BEGIN + len(SYMBOL)] = SYMBOL
    return bytes(result)


def relocated_fixups(original):
    """Keep all existing imports/starts intact and append one libSystem _dlsym chained import."""
    blob = original[FIXUPS_OFFSET:FIXUPS_OFFSET + FIXUPS_SIZE]
    require(len(blob) == FIXUPS_SIZE and struct.unpack_from('<7I', blob) == (0, 32, 112, 1856, 436, 1, 0),
            'Unexpected chained-fixup header')
    strings = blob[1856:]
    require(b'_dlsym\0' not in strings, 'Unexpected existing _dlsym import')
    # DYLD_CHAINED_IMPORT: lib_ordinal:8, weak:1, name_offset:23. libSystem is LC_LOAD_DYLIB ordinal 1.
    imported = 1 | (len(strings) << 9)
    result = bytearray(blob[:1856] + struct.pack('<I', imported) + strings + b'_dlsym\0')
    struct.pack_into('<I', result, 12, 1860)
    struct.pack_into('<I', result, 16, 437)
    result.extend(bytes(align(len(result), 8) - len(result)))
    return bytes(result)


def normalized_payload(data):
    result = bytearray(data[:ORIGINAL_SIGNATURE])
    # codesign may change only these sizing/allocation fields while leaving the pinned payload intact.
    for begin, size in ((0xA20, 8), (0xA30, 8), (0xBE8, 8)):
        result[begin:begin + size] = bytes(size)
    return result


def inspect_header(data):
    require(len(data) >= ORIGINAL_SIGNATURE, 'Truncated launchd_sim')
    require(struct.unpack_from('<4I', data) == (0xFEEDFACF, 0x100000C, 0, 2), 'Expected thin arm64 MH_EXECUTE')
    require(struct.unpack_from('<2I', data, 0xAF0) == (0x1B, 24) and data[0xAF8:0xB08] == UUID, 'Unsupported launchd UUID')
    for offset, command, size in ((LINKEDIT_COMMAND, 0x19, 72), (FIXUPS_COMMAND, 0x80000034, 16),
                                  (SIGNATURE_COMMAND, 0x1D, 16)):
        require(struct.unpack_from('<2I', data, offset) == (command, size), f'Unexpected load command at {offset:#x}')
    signature, capacity = struct.unpack_from('<2I', data, SIGNATURE_COMMAND + 8)
    require(signature >= ORIGINAL_SIGNATURE and capacity >= 12 and signature + capacity == len(data), 'Invalid signature allocation')
    file_offset, file_size = struct.unpack_from('<2Q', data, LINKEDIT_COMMAND + 40)
    vm_size = struct.unpack_from('<Q', data, LINKEDIT_COMMAND + 32)[0]
    require(file_offset == 0x60000 and file_size == len(data) - file_offset and vm_size == align(file_size, 0x4000),
            'Invalid __LINKEDIT extent')
    return signature, capacity


def is_patched(data):
    signature, _ = inspect_header(data)
    instructions = tuple(struct.unpack_from('<I', data, offset)[0] for offset, _, _, _ in SITES)
    original_instructions = tuple(before for _, before, _, _ in SITES)
    patched_instructions = tuple(branch(offset, CAVE_BEGIN + index * VENEER_STRIDE) for index, (offset, _, _, _) in enumerate(SITES))
    if instructions == original_instructions:
        require(signature == ORIGINAL_SIGNATURE, 'Unexpected unpatched signature offset')
        require(digest(normalized_payload(data)) == PAYLOAD_HASH, 'Unpatched payload does not match pinned 23A343 bytes')
        return False
    require(instructions == patched_instructions, 'Partial patch or mismatched owner-read instructions')
    require(data[CAVE_BEGIN:CAVE_END] == cave_bytes(), 'Patched veneer bytes differ')
    expected_tail = CHAIN_TAIL_WORD | (((GOT_SLOT - CHAIN_TAIL) // 4) << 51)
    require(struct.unpack_from('<Q', data, CHAIN_TAIL)[0] == expected_tail, 'Patched chained-fixup tail differs')
    require(struct.unpack_from('<Q', data, GOT_SLOT)[0] == (1 << 63) | 436, 'Patched _dlsym bind pointer differs')
    metadata = relocated_fixups(data)
    require(struct.unpack_from('<2I', data, FIXUPS_COMMAND + 8) == (ORIGINAL_SIGNATURE, len(metadata)), 'Patched fixup command differs')
    expected_signature = align(ORIGINAL_SIGNATURE + len(metadata), 16)
    require(signature == expected_signature and data[ORIGINAL_SIGNATURE:ORIGINAL_SIGNATURE + len(metadata)] == metadata,
            'Patched fixup metadata differs')
    require(not any(data[ORIGINAL_SIGNATURE + len(metadata):signature]), 'Nonzero appended metadata padding')
    restored = normalized_payload(data)
    for offset, before, _, _ in SITES:
        struct.pack_into('<I', restored, offset, before)
    restored[CAVE_BEGIN:CAVE_END] = bytes(CAVE_END - CAVE_BEGIN)
    struct.pack_into('<Q', restored, CHAIN_TAIL, CHAIN_TAIL_WORD)
    struct.pack_into('<Q', restored, GOT_SLOT, 0)
    struct.pack_into('<2I', restored, FIXUPS_COMMAND + 8, FIXUPS_OFFSET, FIXUPS_SIZE)
    require(digest(restored) == PAYLOAD_HASH, 'Patched payload contains unrelated or unsupported changes')
    return True


def patched_bytes(original):
    if is_patched(original):
        return original
    require(not any(original[CAVE_BEGIN:CAVE_END]) and not any(original[GOT_SLOT:GOT_SLOT + 8]), 'Patch padding is occupied')
    require(struct.unpack_from('<Q', original, CHAIN_TAIL)[0] == CHAIN_TAIL_WORD, 'Unexpected existing chain tail')
    metadata = relocated_fixups(original)
    signature = align(ORIGINAL_SIGNATURE + len(metadata), 16)
    result = bytearray(original[:ORIGINAL_SIGNATURE])
    result.extend(metadata)
    result.extend(bytes(signature - len(result)))
    result.extend(original[ORIGINAL_SIGNATURE:])
    for index, (offset, _, _, _) in enumerate(SITES):
        struct.pack_into('<I', result, offset, branch(offset, CAVE_BEGIN + index * VENEER_STRIDE))
    result[CAVE_BEGIN:CAVE_END] = cave_bytes()
    struct.pack_into('<Q', result, CHAIN_TAIL, CHAIN_TAIL_WORD | (((GOT_SLOT - CHAIN_TAIL) // 4) << 51))
    struct.pack_into('<Q', result, GOT_SLOT, (1 << 63) | 436)  # DYLD_CHAINED_PTR_64_OFFSET bind, ordinal 436, next=0.
    struct.pack_into('<2I', result, FIXUPS_COMMAND + 8, ORIGINAL_SIGNATURE, len(metadata))
    struct.pack_into('<I', result, SIGNATURE_COMMAND + 8, signature)
    struct.pack_into('<Q', result, LINKEDIT_COMMAND + 48, len(result) - 0x60000)
    struct.pack_into('<Q', result, LINKEDIT_COMMAND + 32, align(len(result) - 0x60000, 0x4000))
    require(is_patched(result), 'Internal patched-output verification failed')
    return bytes(result)


def patch_file(source, output):
    require(source.is_file(), f'Input is not a regular file: {source}')
    manifest = output.with_name(output.name + '.identity-patch.json')
    for destination in (output, manifest):
        require(destination.resolve() != source.resolve(), f'Output would overwrite input: {destination}')
        require(not destination.exists() and not destination.is_symlink(), f'Refusing to overwrite: {destination}')
    original = source.read_bytes()
    already = is_patched(original)
    patched = patched_bytes(original)
    signature, capacity = inspect_header(patched)
    report = dict(runtime_build='23A343', uuid=UUID_TEXT, source=str(source), source_sha256=digest(original),
                  output=str(output), output_sha256=digest(patched), already_patched=already, requires_separate_signing=True,
                  symbol=SYMBOL[:-1].decode(), lookup='dlsym(RTLD_DEFAULT, symbol)', missing_symbol_pid=-1,
                  original_audit_tokens_unchanged=True, frame_size=FRAME_SIZE,
                  preserved='all x0-x30 except destination W register; q0-q31; SP/LR; NZCV/FPCR/FPSR',
                  got_offset=hex(GOT_SLOT), chained_import_ordinal=436, chained_import_library='/usr/lib/libSystem.B.dylib',
                  fixups_offset=hex(ORIGINAL_SIGNATURE), fixups_size=len(relocated_fixups(original)),
                  signature_offset=signature, signature_capacity=capacity,
                  diagnostic_limit='The added GOT slot is in segment padding; dyld_info -fixups may trap while naming its absent section. '
                                   'dyld_info -validate_only and -imports support this layout.',
                  sites=[dict(offset=hex(offset), before=hex(before), after=hex(branch(offset, CAVE_BEGIN + i * VENEER_STRIDE)),
                              veneer_offset=hex(CAVE_BEGIN + i * VENEER_STRIDE), destination=f'w{destination}',
                              token='original sp' if token is None else f'x{token}')
                         for i, (offset, before, token, destination) in enumerate(SITES)])
    output.parent.mkdir(parents=True, exist_ok=True)
    created = []
    try:
        with output.open('xb') as stream:
            created.append(output)
            stream.write(patched)
        output.chmod(stat.S_IMODE(source.stat().st_mode))
        require(output.read_bytes() == patched, 'Output verification failed')
        require(source.read_bytes() == original, 'Input changed during patching')
        with manifest.open('x') as stream:
            created.append(manifest)
            stream.write(json.dumps(report, indent=2) + '\n')
    except Exception:
        for destination in reversed(created):
            destination.unlink()
        raise
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        report = patch_file(args.source.resolve(), args.output.absolute())
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
