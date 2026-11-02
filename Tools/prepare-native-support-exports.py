#!/usr/bin/env python3
"""Reserve private, non-executable native-support export tries in the experimental 23A343 cache before signing."""

import argparse
import hashlib
import importlib.util
import json
import mmap
from pathlib import Path
import shutil
import struct
import uuid


SPEC = importlib.util.spec_from_file_location('placeholder', Path(__file__).with_name('setup-ios26-placeholder-cache.py'))
P = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(P)
require = P.require
SIZES = (0x10000, 0x4000, 0x4000)


def reserve(source, destination):
    require(source != destination and not destination.exists(), 'Output must be a new file')
    with source.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        require(data[:16] == b'dyld_v1   arm64\0', 'Expected arm64 cache')
        base, span = struct.unpack_from('<QQ', data, 224)
        require((base, span) == (0x180000000, 0x7E134000), 'Unreviewed placeholder cache layout')
        signature, signature_size = struct.unpack_from('<QQ', data, 40)
        legacy, count = struct.unpack_from('<II', data, 16)
        slide, slide_count = struct.unpack_from('<II', data, 312)
        require(count == slide_count == 6 and signature + signature_size == len(data), 'Invalid mapping/signature bounds')
        maps = [struct.unpack_from('<QQQII', data, legacy + i * 32) for i in range(count)]
        last = maps[-1]
        require(last[3:] == (1, 1) and last[0] + last[1] + 0x4000 == base + span and last[2] + last[1] == signature,
                'Expected terminal read-only LINKEDIT mapping')
        require(struct.unpack_from('<QQ', data, 496) == (span - 0x4000, 0x4000), 'Unreviewed dynamic-data reservation')
        image_table, image_count = struct.unpack_from('<II', data, 448)
        require(image_count == 1353, 'Unexpected placeholder image count')
        records = {}
        for index in range(image_count):
            address, _, _, name_offset, _ = struct.unpack_from('<QQQII', data, image_table + index * 32)
            name = data[name_offset:data.find(b'\0', name_offset)].decode()
            if name not in P.SUPPORT:
                continue
            row = next(m for m in maps if m[0] <= address < m[0] + m[1])
            header = row[2] + address - row[0]
            commands, _ = P.image(data, header)
            command = P.trie.one(commands, 0x80000033)
            linkedit = [item for item in commands[0x19] if data[item + 8:item + 24].rstrip(b'\0') == b'__LINKEDIT']
            require(len(linkedit) == 1 and struct.unpack_from('<QQQQ', data, linkedit[0] + 24) == (last[0], last[1], last[2], last[1]),
                    'Unreviewed placeholder LINKEDIT segment')
            offset, size = struct.unpack_from('<II', data, command + 8)
            records[name] = dict(command=command, linkedit=linkedit[0], original_offset=offset, original_size=size,
                                 contents=bytes(data[offset:offset + size]))
        require(set(records) == set(P.SUPPORT), 'Missing support export tries')
        old_signature = bytes(data[signature:])
    extra = sum(SIZES)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    with destination.open('r+b') as stream:
        stream.truncate(signature + extra + signature_size)
        with mmap.mmap(stream.fileno(), 0) as data:
            data[signature:signature + extra] = bytes(extra)
            data[signature + extra:] = old_signature
            struct.pack_into('<Q', data, 40, signature + extra)
            struct.pack_into('<Q', data, 232, span + extra)
            struct.pack_into('<Q', data, 496, span - 0x4000 + extra)
            struct.pack_into('<Q', data, legacy + 5 * 32 + 8, last[1] + extra)
            struct.pack_into('<Q', data, slide + 5 * 56 + 8, last[1] + extra)
            cursor = signature
            for name, capacity in zip(P.SUPPORT, SIZES):
                record = records[name]
                require(record['original_size'] < capacity, 'Export reserve too small')
                data[cursor:cursor + record['original_size']] = record.pop('contents')
                struct.pack_into('<II', data, record['command'] + 8, cursor, capacity)
                struct.pack_into('<Q', data, record['linkedit'] + 32, last[1] + extra)
                struct.pack_into('<Q', data, record['linkedit'] + 48, last[1] + extra)
                record.update(offset=cursor, capacity=capacity, address=last[0] + cursor - last[2])
                cursor += capacity
            # Deterministic new identity, distinguishing this code/header layout from the unsigned precursor.
            data[88:104] = bytes(16)
            digest = hashlib.sha256()
            for offset in range(0, signature + extra, 1024 * 1024):
                digest.update(data[offset:min(offset + 1024 * 1024, signature + extra)])
            identity = uuid.UUID(bytes=digest.digest()[:16], version=5)
            data[88:104] = identity.bytes
            data.flush()
    report = dict(source=str(source), output=str(destination), uuid=str(identity), span=hex(span + extra),
                  signature_offset=hex(signature + extra), reserves=records, signature_valid=False,
                  requires_native_support_bridge=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    print(json.dumps(reserve(args.source.resolve(), args.destination.resolve()), indent=2))
