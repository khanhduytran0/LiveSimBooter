#!/usr/bin/env python3
"""Prepare native argument-stack placement before signing, without enlarging Mach-O segments."""

import argparse
import os
from pathlib import Path
import stat
import struct
import tempfile

ADDRESS = 0x10B000000
SIZE = 0x65000000
NAME = b"__LSB_STACK"
SECTION = b"__reserve"
PLACEHOLDER_SIZE = 0x4000
STACK_SIZE = 0x800000
ARGUMENT_STACK_SIZE = 0x90000000


def prepare(data, enable=False, preserve_arguments=False):
    if enable and preserve_arguments:
        raise ValueError("the legacy wide segment and argument-stack placement are mutually exclusive")
    if len(data) < 32:
        raise ValueError("truncated Mach-O header")
    magic, cpu, _, kind, count, command_bytes, flags, _ = struct.unpack_from("<8I", data)
    if (magic, cpu, kind) != (0xFEEDFACF, 0x100000C, 2) or not flags & 0x200000:
        raise ValueError("expected a thin PIE arm64 executable")
    end = 32 + command_bytes
    if end > len(data) or count > command_bytes // 8:
        raise ValueError("invalid load-command bounds")
    segments, entry_commands, symbols = [], [], None
    cursor = 32
    for _ in range(count):
        if cursor + 8 > end:
            raise ValueError("truncated load command")
        command, size = struct.unpack_from("<II", data, cursor)
        if size < 8 or size % 8 or cursor + size > end:
            raise ValueError("invalid load-command size")
        if command == 0x19:
            if size < 72:
                raise ValueError("truncated segment")
            segments.append((cursor, struct.unpack_from("<II16sQQQQiiII", data, cursor)))
        elif command == 0x80000028:
            if size != 24:
                raise ValueError("invalid LC_MAIN")
            entry_commands.append((cursor, *struct.unpack_from("<QQ", data, cursor + 8)))
        elif command == 5:
            raise ValueError("LC_UNIXTHREAD must not replace normal LC_MAIN startup")
        elif command == 2:
            if size != 24 or symbols is not None:
                raise ValueError("invalid symbol table")
            symbols = struct.unpack_from("<IIII", data, cursor + 8)
        cursor += size
    if cursor != end or len(entry_commands) != 1 or entry_commands[0][2] not in (STACK_SIZE, ARGUMENT_STACK_SIZE):
        raise ValueError("expected exactly one LC_MAIN with a recognized native stack size")
    matching = [(offset, segment) for offset, segment in segments if segment[2].rstrip(b"\0") == NAME]
    if len(matching) != 1:
        raise ValueError("expected exactly one __LSB_STACK placeholder")
    offset, segment = matching[0]
    _, command_size, _, address, size, file_offset, file_size, maximum, initial, sections, segment_flags = segment
    if (command_size, file_offset, file_size, maximum, initial, sections, segment_flags) != (152, 0, 0, 0, 0, 1, 0):
        raise ValueError("stack exclusion must have one section, no file bytes and no memory permissions")
    section = struct.unpack_from("<16s16sQQ8I", data, offset + 72)
    if (section[0].rstrip(b"\0"), section[1].rstrip(b"\0"), section[2], section[3], *section[4:]) != (
            SECTION, NAME, address, size, 0, 14, 0, 0, 1, 0, 0, 0):
        raise ValueError("stack exclusion section is not the exact empty aligned zero-fill placeholder")
    if (address, size) != (ADDRESS, SIZE) and (size != PLACEHOLDER_SIZE or address & (PLACEHOLDER_SIZE - 1)):
        raise ValueError("unexpected existing stack-exclusion layout")
    linkedit = [other for _, other in segments if other[2].rstrip(b"\0") == b"__LINKEDIT"]
    if len(linkedit) != 1 or linkedit[0][3] < PLACEHOLDER_SIZE:
        raise ValueError("expected one LINKEDIT after the linked placeholder")
    placeholder_address = linkedit[0][3] - PLACEHOLDER_SIZE
    if placeholder_address & (PLACEHOLDER_SIZE - 1):
        raise ValueError("unaligned linked stack placeholder")
    if size == PLACEHOLDER_SIZE and address != placeholder_address:
        raise ValueError("stack placeholder must immediately precede LINKEDIT")
    target_address, target_size = (ADDRESS, SIZE) if enable else (placeholder_address, PLACEHOLDER_SIZE)
    for other_offset, other in segments:
        if other_offset == offset:
            continue
        start, span = other[3:5]
        if span and (start + span > (1 << 64) - 1 or start < target_address + target_size and target_address < start + span):
            raise ValueError("stack exclusion would overlap a real executable segment")
    patched = bytearray(data)
    # XNU's fixed initial stack overlaps PAGEZERO/image at this size, so create_unix_stack chooses another address and
    # updates p->user_stack. Unlike the old wide segment, this does not move native dyld or its permanent LINKEDIT pages.
    # Only virtual address space is reserved; the loader advertises the upper 8 MiB to the simulator with a new guard.
    # https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/kern_exec.c (create_unix_stack)
    struct.pack_into("<Q", patched, entry_commands[0][0] + 16, ARGUMENT_STACK_SIZE if preserve_arguments else STACK_SIZE)
    struct.pack_into("<QQ", patched, offset + 24, target_address, target_size)
    struct.pack_into("<QQ", patched, offset + 104, target_address, target_size)
    # The private no-dead-strip marker has no runtime references, but keep debugger symbol metadata truthful.
    if symbols is not None:
        symbol_offset, symbol_count, string_offset, string_size = symbols
        if symbol_offset + symbol_count * 16 > len(data) or string_offset + string_size > len(data):
            raise ValueError("symbol table extends beyond executable")
        for index in range(symbol_count):
            at = symbol_offset + index * 16
            string_index, symbol_type, _, _, value = struct.unpack_from("<IBBHQ", data, at)
            if string_index >= string_size:
                raise ValueError("invalid symbol string offset")
            name_end = data.find(b"\0", string_offset + string_index, string_offset + string_size)
            if name_end < 0:
                raise ValueError("unterminated symbol name")
            if data[string_offset + string_index:name_end] == b"_LSBStackReservation":
                if symbol_type & 0x0E != 0x0E or value != address:
                    raise ValueError("invalid stack-exclusion symbol")
                struct.pack_into("<Q", patched, at + 8, target_address)
    return bytes(patched)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--enable", action="store_true", help="enable the experimental wide reservation (known iPhone cache-mapping failure)")
    parser.add_argument("--preserve-arguments", action="store_true", help="place native argv outside the simulator cache using LC_MAIN stack size")
    args = parser.parse_args()
    original = args.executable.read_bytes()
    try:
        patched = prepare(original, enable=args.enable, preserve_arguments=args.preserve_arguments)
    except ValueError as error:
        parser.error(str(error))
    if patched != original:
        mode = stat.S_IMODE(args.executable.stat().st_mode)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=args.executable.parent, prefix=".native-stack-", delete=False) as output:
                temporary = Path(output.name)
                output.write(patched)
                os.fchmod(output.fileno(), mode)
            os.replace(temporary, args.executable)
            temporary = None
        finally:
            if temporary is not None:
                temporary.unlink()
    if args.preserve_arguments:
        print(f"Native argument stack: {ARGUMENT_STACK_SIZE:#x} demand-zero reservation, unchanged segment extents; sign after this step")
    elif args.enable:
        print(f"EXPERIMENTAL native stack exclusion: {ADDRESS:#x}-{ADDRESS + SIZE:#x}; sign after this step")
    else:
        print("Native stack experiment disabled: linked 16 KiB placeholder only; sign after this step")


if __name__ == "__main__":
    main()
