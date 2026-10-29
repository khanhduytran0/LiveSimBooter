#!/usr/bin/env python3
"""Run the patcher's actual veneer instructions natively, substituting only their external address references."""
import importlib.util
from pathlib import Path
import platform
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('launchd_identity_patch', ROOT / 'Tools/patch-ios26-launchd-identity.py')
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)
EXPECTED_SITES = ((0x236B0, 0xB9401421, 1, 1), (0x2E658, 0xB9401681, 20, 1),
                  (0x2E6B8, 0xB9401688, 20, 8), (0x1BC08, 0xB94017E0, None, 0),
                  (0x1DB98, 0xB9401429, 1, 9), (0x2D130, 0xB9401440, 2, 0),
                  (0x30048, 0xB94016C0, 22, 0), (0x2FFD8, 0xB94016C0, 22, 0),
                  (0x1DAE4, 0xB9401428, 1, 8))
assert patcher.SITES == EXPECTED_SITES, 'Update the independent fixtures when the pinned input build/sites change'


def object_text(path):
    data = path.read_bytes()
    assert struct.unpack_from('<I', data)[0] == 0xFEEDFACF
    cursor = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, cursor)
        if cmd == 0x19:
            for index in range(struct.unpack_from('<I', data, cursor + 64)[0]):
                section = cursor + 72 + index * 80
                if data[section:section + 16].rstrip(b'\0') == b'__text':
                    length = struct.unpack_from('<Q', data, section + 40)[0]
                    offset = struct.unpack_from('<I', data, section + 48)[0]
                    return data[offset:offset + length]
        cursor += size
    raise AssertionError('Missing __text')


def clobber(seed):
    # Honor the native ABI's callee-saved GPRs and low halves of v8-v15; clobber every caller-saved vector component.
    lines = [f'    mov x9, #{seed}']
    for reg in range(32):
        lines.append(f'    mov v{reg}.d[1], x9' if 8 <= reg <= 15 else f'    movi v{reg}.16b, #{seed}')
    lines.extend(('    mov x9, #0x1000000', '    msr fpcr, x9', '    msr fpsr, xzr',
                  '    mov x9, #0x50000000', '    msr nzcv, x9'))
    lines.extend(f'    mov x{reg}, #{0x5000 + reg}' for reg in range(18))
    return lines


def fixture_assembly():
    lines = ['.text', '.p2align 2', '.globl _LSBTestRunVeneer', '_LSBTestRunVeneer:', '    sub sp, sp, #0x120']
    for reg in range(19, 31, 2):
        lines.append(f'    stp x{reg}, x{reg + 1}, [sp, #{0x20 + (reg - 19) * 8}]')
    for reg in range(8, 16, 2):
        lines.append(f'    stp q{reg}, q{reg + 1}, [sp, #{0x80 + (reg - 8) * 16}]')
    lines.extend(('    mrs x9, fpcr', '    mrs x10, fpsr', '    stp x9, x10, [sp, #0x100]',
                  '    adrp x30, _LSBTestFixture@PAGE', '    add x30, x30, _LSBTestFixture@PAGEOFF',
                  '    str x18, [x30, #144]', '    mov x9, sp', '    str x9, [x30, #0x600]',
                  '    add x9, x30, #0x688', '    ldp q0, q1, [x9]', '    stp q0, q1, [sp]'))
    for index in range(len(EXPECTED_SITES) - 1):
        lines.extend((f'    cmp w0, #{index}', f'    b.eq .Lcase{index}'))
    lines.append(f'    b .Lcase{len(EXPECTED_SITES) - 1}')
    for index, (_, _, token, _) in enumerate(EXPECTED_SITES):
        lines.append(f'.Lcase{index}:')
        lines.append('    mov x9, sp' if token is None else '    add x9, x30, #0x688')
        lines.append('    str x9, [x30, #0x640]')
        if token is not None:
            lines.append(f'    str x9, [x30, #{token * 8}]')
        for name, offset in (('nzcv', 0x610), ('fpcr', 0x620), ('fpsr', 0x630)):
            lines.extend((f'    ldr x9, [x30, #{offset}]', f'    msr {name}, x9'))
        for reg in range(0, 32, 2):
            lines.append(f'    ldp q{reg}, q{reg + 1}, [x30, #{0x200 + reg * 16}]')
        for reg in range(0, 30, 2):
            if reg == 18:  # Darwin reserves x18; preserve its actual value instead of manufacturing one.
                lines.append('    ldr x19, [x30, #152]')
            else:
                lines.append(f'    ldp x{reg}, x{reg + 1}, [x30, #{reg * 8}]')
        lines.extend(('    ldr x30, [x30, #240]', f'    b _LSBTestVeneer{index}'))
    for index in range(len(EXPECTED_SITES)):
        lines.extend((f'.globl _LSBTestResume{index}', f'_LSBTestResume{index}:', '    stp x16, x17, [sp, #-16]!',
                      f'    mov x17, #{index}', '    b .Lrecord'))
    lines.extend(('.Lrecord:', '    adrp x16, _LSBTestFixture@PAGE', '    add x16, x16, _LSBTestFixture@PAGEOFF',
                  '    str x17, [x16, #0x680]'))
    for reg in range(0, 30, 2):
        if reg == 16:
            continue
        lines.append(f'    stp x{reg}, x{reg + 1}, [x16, #{0x100 + reg * 8}]')
    lines.extend(('    str x30, [x16, #0x1f0]', '    ldp x0, x1, [sp]', '    stp x0, x1, [x16, #0x180]',
                  '    add x0, sp, #16', '    str x0, [x16, #0x608]', '    mrs x0, nzcv', '    str x0, [x16, #0x618]',
                  '    mrs x0, fpcr', '    str x0, [x16, #0x628]', '    mrs x0, fpsr', '    str x0, [x16, #0x638]',
                  '    add x17, x16, #0x400'))
    for reg in range(0, 32, 2):
        lines.append(f'    stp q{reg}, q{reg + 1}, [x17, #{reg * 16}]')
    lines.extend(('    ldp q0, q1, [sp, #16]', '    add x17, x16, #0x6a8', '    stp q0, q1, [x17]', '    add sp, sp, #16',
                  '    ldp x9, x10, [sp, #0x100]', '    msr fpcr, x9', '    msr fpsr, x10'))
    for reg in range(8, 16, 2):
        lines.append(f'    ldp q{reg}, q{reg + 1}, [sp, #{0x80 + (reg - 8) * 16}]')
    for reg in range(19, 31, 2):
        lines.append(f'    ldp x{reg}, x{reg + 1}, [sp, #{0x20 + (reg - 19) * 8}]')
    lines.extend(('    add sp, sp, #0x120', '    ret', '.globl _LSBTestFakeDlsym', '_LSBTestFakeDlsym:',
                  '    adrp x8, _LSBTestFixture@PAGE', '    add x8, x8, _LSBTestFixture@PAGEOFF',
                  '    str x0, [x8, #0x650]', '    str x1, [x8, #0x658]',
                  '    ldr x9, [x8, #0x660]', '    add x9, x9, #1', '    str x9, [x8, #0x660]'))
    lines.extend(clobber(0x5a))
    lines.extend(('    adrp x0, _LSBTestFixture@PAGE', '    add x0, x0, _LSBTestFixture@PAGEOFF', '    ldr x0, [x0, #0x678]',
                  '    cbz x0, .LdlsymReturn', '    adrp x0, _LSBTestFakeResolver@PAGE',
                  '    add x0, x0, _LSBTestFakeResolver@PAGEOFF', '.LdlsymReturn:', '    ret', '_LSBTestFakeResolver:',
                  '    adrp x8, _LSBTestFixture@PAGE', '    add x8, x8, _LSBTestFixture@PAGEOFF',
                  '    str x0, [x8, #0x648]', '    ldr x9, [x8, #0x668]', '    add x9, x9, #1', '    str x9, [x8, #0x668]'))
    lines.extend(clobber(0xa5))
    lines.extend(('    adrp x0, _LSBTestFixture@PAGE', '    add x0, x0, _LSBTestFixture@PAGEOFF',
                  '    ldr x0, [x0, #0x670]', '    movk x0, #0xfedc, lsl #48', '    ret'))
    return '\n'.join(lines) + '\n'


def main():
    assert platform.system() == 'Darwin' and platform.machine() == 'arm64', 'Native arm64 macOS is required'
    with tempfile.TemporaryDirectory(prefix='lsb-launchd-veneer-') as temporary:
        directory = Path(temporary)
        clang = ['xcrun', '--sdk', 'macosx', 'clang', '-arch', 'arm64']
        code, expected = [], []
        for index in range(len(EXPECTED_SITES)):
            machine, source = patcher.build_veneer(index, external_labels=True)
            code.append(source)
            expected.append(machine)
        code.extend(('.globl _LSBTestSymbolName\n_LSBTestSymbolName:\n', '    .asciz "LSBLaunchdResolveAuditPID"\n'))
        (directory / 'veneers.S').write_text('\n'.join(code))
        subprocess.run(clang + ['-c', str(directory / 'veneers.S'), '-o', str(directory / 'veneers.o')], check=True)
        actual = object_text(directory / 'veneers.o')
        cursor = 0
        for index, machine in enumerate(expected):
            _, source = patcher.build_veneer(index, external_labels=True)
            instructions = [line.strip() for line in source.splitlines() if line.startswith('    ')]
            relocations = ('adr x1,', 'adrp x16,', 'ldr x16, [x16,', 'b _LSBTestResume')
            assert len(instructions) * 4 == len(machine)
            assert sum(line.startswith(relocations) for line in instructions) == 4
            for offset, instruction in enumerate(instructions):
                if not instruction.startswith(relocations):
                    assert actual[cursor + offset * 4:cursor + offset * 4 + 4] == machine[offset * 4:offset * 4 + 4], (index, instruction)
            cursor += len(machine)
        (directory / 'fixture.S').write_text(fixture_assembly())
        executable = directory / 'LaunchdIdentityVeneerTests'
        subprocess.run(clang + ['-std=c11', '-Wall', '-Wextra', '-Werror', str(ROOT / 'Tests/LaunchdIdentityVeneerTests.c'),
                               str(directory / 'fixture.S'), str(directory / 'veneers.o'), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
