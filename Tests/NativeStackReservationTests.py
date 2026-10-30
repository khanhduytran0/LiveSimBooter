#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("native_stack", ROOT / "Tools/prepare-native-stack.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def rejects(data, **options):
    try:
        tool.prepare(data, **options)
    except ValueError:
        return
    raise AssertionError("accepted malformed native stack image")


def run_probe(executable, environment):
    result = subprocess.run([str(executable), "argument-canary"], env=environment, capture_output=True, text=True, timeout=15)
    if result.stdout:
        print(result.stdout.strip(), flush=True)
    if result.stderr:
        print(result.stderr.strip(), file=sys.stderr, flush=True)
    result.check_returncode()


with tempfile.TemporaryDirectory(prefix="LiveSimBooter-native-stack-") as temporary:
    executable = Path(temporary) / "native-stack-probe"
    subprocess.run(["xcrun", "--sdk", "macosx", "clang", "-arch", "arm64", "-Wall", "-Wextra", "-Werror",
                    "-Wl,-dead_strip,-segprot,__LSB_STACK,---,---,-stack_size,0x800000",
                    str(ROOT / "Tests/NativeStackProbe.c"), str(ROOT / "LiveProcess/ProcessDisplayName.c"),
                    str(ROOT / "LiveProcess/StackReservation.s"), "-o", str(executable)], check=True)
    original = executable.read_bytes()
    cursor = 32
    guard = entry = None
    chained = False
    for _ in range(struct.unpack_from("<I", original, 16)[0]):
        command, size = struct.unpack_from("<II", original, cursor)
        if command == 0x19 and original[cursor + 8:cursor + 24].rstrip(b"\0") == tool.NAME:
            guard = cursor
        if command == 0x80000028:
            entry = cursor
        chained |= command == 0x80000034
        cursor += size
    assert guard is not None and entry is not None and chained, "test must retain modern LC_MAIN and chained fixups"
    assert tool.prepare(original) == original, "wide reservation must be disabled by default"
    patched = tool.prepare(original, enable=True)
    assert tool.prepare(patched) == original, "turning the experiment off must restore an already expanded incremental build"
    assert tool.prepare(patched, enable=True) == patched and len(patched) == len(original)
    assert patched[entry:entry + 24] == original[entry:entry + 24]
    assert struct.unpack_from("<QQ", patched, guard + 24) == (tool.ADDRESS, tool.SIZE)
    arguments = tool.prepare(original, preserve_arguments=True)
    assert arguments[:entry + 16] == original[:entry + 16] and arguments[entry + 24:] == original[entry + 24:], \
        "argument preservation must change only LC_MAIN.stacksize, leaving entry, segment extents and fixups byte-identical"
    assert struct.unpack_from("<Q", arguments, entry + 16)[0] == tool.ARGUMENT_STACK_SIZE
    assert tool.prepare(arguments, preserve_arguments=True) == arguments
    assert tool.prepare(arguments) == original and tool.prepare(arguments, enable=True) == patched
    assert tool.prepare(patched, preserve_arguments=True) == arguments, "wide-guard incremental builds must restore the small placeholder"
    rejects(original, enable=True, preserve_arguments=True)
    for offset, value in ((4, 7), (16, 0xFFFFFFFF), (guard + 4, 72), (guard + 48, 1), (guard + 56, 1),
                          (guard + 60, 1), (guard + 64, 2), (guard + 72 + 56, 1), (guard + 72 + 64, 0), (entry + 16, 0)):
        malformed = bytearray(original)
        struct.pack_into("<I", malformed, offset, value)
        rejects(malformed)
    rejects(original[:31])
    rejects(original[:128])
    for stack_size in (0x1000000, 0x90000001, 0xFFFFFFFFFFFFFFFF):
        malformed = bytearray(original)
        struct.pack_into("<Q", malformed, entry + 16, stack_size)
        rejects(malformed, preserve_arguments=True)
    # XNU arm64 combines 0/32/64 MiB section entropy with less than16 MiB page entropy.
    for slide in range(0, 0x5000000, 0x4000):
        start, end = tool.ADDRESS + slide, tool.ADDRESS + slide + tool.SIZE
        assert start <= 0x110000000 < end and end <= 0x175000000
        assert start <= 0x16FE00000 - slide < end
        assert start - (0x100000000 + slide) == 176 * 1024 * 1024
    executable.write_bytes(patched)
    subprocess.run(["codesign", "--force", "--sign", "-", str(executable)], check=True, capture_output=True)
    validation = subprocess.run(["xcrun", "dyld_info", "-validate_only", str(executable)], check=True, capture_output=True, text=True)
    assert not validation.stderr and len(validation.stdout.splitlines()) == 1, validation.stdout + validation.stderr
    assert tool.prepare(executable.read_bytes(), enable=True) == executable.read_bytes(), "incremental build must not invalidate a prepared signature"
    environment = dict(os.environ, LSB_STACK_TEST_ENV="environment-canary")
    for _ in range(5):
        run_probe(executable, environment)
    arguments_executable = Path(temporary) / "native-argument-stack-probe"
    subprocess.run(["xcrun", "--sdk", "macosx", "clang", "-arch", "arm64", "-Wall", "-Wextra", "-Werror",
                    "-Wl,-dead_strip,-segprot,__LSB_STACK,---,---,-stack_size,0x800000",
                    str(ROOT / "Tests/NativeArgumentStackProbe.c"), str(ROOT / "LiveProcess/ProcessDisplayName.c"),
                    str(ROOT / "LiveProcess/StackReservation.s"), "-o", str(arguments_executable)], check=True)
    arguments_executable.write_bytes(tool.prepare(arguments_executable.read_bytes(), preserve_arguments=True))
    subprocess.run(["codesign", "--force", "--sign", "-", str(arguments_executable)], check=True, capture_output=True)
    validation = subprocess.run(["xcrun", "dyld_info", "-validate_only", str(arguments_executable)],
                                check=True, capture_output=True, text=True)
    assert not validation.stderr and len(validation.stdout.splitlines()) == 1, validation.stdout + validation.stderr
    signed = arguments_executable.read_bytes()
    assert tool.prepare(signed, preserve_arguments=True) == signed, "repeated preparation must preserve the signature"
    for _ in range(5):
        run_probe(arguments_executable, environment)
print("Native stack reservation: validation, ASLR bounds, chained fixups and real kernel argv passed")
