#!/usr/bin/env python3
"""Restore the 63 pinned 23A343 cached path aliases without adding duplicate images or changing the mapping layout.

The original simulator builder does not submit filesystem symlinks to its cache builder. Repack only
its dylib path trie using shared identical terminal/subtree nodes and deterministic small-node-first
ordering. Apple dyld's trieWalk follows absolute offsets and detects cycles along the current path;
shared nodes and backward edges are valid (MachOFile.cpp in dyld fd8d0c4d52320ebf64db34f3cb280310d905c5ae).
Every original path/index and every alias/index is independently checked. The encoded trie must fit
its existing allocation; no mapping, image index, prebuilt loader, instruction or fixup is changed.
Source remains immutable. Apply any remaining cache compatibility patches afterward, then identity-sign.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import uuid


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('lsb_compatibility_patch', HERE / 'patch-ios26-slidable-cache.py')
compatibility = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compatibility)
slide = compatibility.slide
require = slide.require


def uleb(value):
    require(isinstance(value, int) and 0 <= value < 1 << 64, 'ULEB value exceeds unsigned 64-bit range')
    output = bytearray()
    while value >= 128:
        output.append((value & 127) | 128)
        value >>= 7
    output.append(value)
    return output


class Node:
    __slots__ = ('value', 'edges', 'offset', 'ordinal')

    def __init__(self):
        self.value, self.edges, self.offset, self.ordinal = None, {}, 0, None


def encode_node(node):
    terminal = b'' if node.value is None else uleb(node.value)
    require(len(node.edges) <= 255, 'Too many trie children')
    output = uleb(len(terminal)) + terminal + bytes((len(node.edges),))
    for edge, child in node.edges.items():
        output += edge.encode('utf-8') + b'\0' + uleb(child.offset)
    return output


def encode_trie(entries):
    require(0 < len(entries) <= 4096, 'Unsupported trie path count')
    root = Node()
    for path, value in sorted(entries.items()):
        require(isinstance(path, str) and path.startswith('/') and '\0' not in path and len(path.encode('utf-8')) < 4096, 'Invalid trie path')
        require(isinstance(value, int) and 0 <= value < 4096, 'Invalid cache image index')
        node = root
        for character in path:
            if character not in node.edges:
                node.edges[character] = Node()
            node = node.edges[character]
        require(node.value is None, 'Duplicate trie path')
        node.value = value

    def compact(node, depth=0):
        require(depth <= 128, 'Excessive branching depth')
        edges = {}
        for edge, child in node.edges.items():
            while child.value is None and len(child.edges) == 1:
                suffix, child = next(iter(child.edges.items()))
                edge += suffix
            compact(child, depth + 1)
            edges[edge] = child
        node.edges = edges
    compact(root)
    interned = {}

    def intern(node):
        node.edges = {edge: intern(child) for edge, child in node.edges.items()}
        key = node.value, tuple((edge, child.ordinal) for edge, child in node.edges.items())
        if key not in interned:
            node.ordinal = len(interned)
            interned[key] = node
        return interned[key]
    root = intern(root)
    nodes, visited = [], set()

    def flatten(node):
        if node.ordinal in visited:
            return
        visited.add(node.ordinal)
        nodes.append(node)
        for child in node.edges.values():
            flatten(child)
    flatten(root)
    # Offsets are all zero here. Stable ties retain sorted-path DFS order, independent of object addresses/dict hashing.
    nodes = nodes[:1] + sorted(nodes[1:], key=lambda node: len(encode_node(node)))
    for _ in range(32):
        changed, cursor = False, 0
        for node in nodes:
            changed |= node.offset != cursor
            node.offset = cursor
            cursor += len(encode_node(node))
        if not changed:
            output = b''.join(encode_node(node) for node in nodes)
            require(len(output) == cursor, 'Inconsistent final trie offsets')
            return output
    raise ValueError('Trie layout failed to converge')


def decode_trie(data):
    def read(position):
        value = shift = 0
        while True:
            require(0 <= position < len(data) and shift < 64, 'Invalid trie ULEB128')
            byte = data[position]
            position += 1
            require(shift != 63 or byte <= 1, 'Overflowing trie ULEB128')
            value |= (byte & 127) << shift
            if byte < 128:
                return value, position
            shift += 7
    found, visits = {}, 0

    def visit(offset, prefix, ancestors):
        nonlocal visits
        visits += 1
        require(visits <= 32768 and len(ancestors) <= 128, 'Excessive trie traversal')
        require(offset not in ancestors, 'Cyclic trie')
        ancestors = ancestors | {offset}
        size, position = read(offset)
        children = position + size
        require(children < len(data), 'Trie terminal exceeds bounds')
        if size:
            index, end = read(position)
            require(end == children and prefix.startswith('/') and prefix not in found and len(found) < 4096, 'Invalid/duplicate trie terminal')
            found[prefix] = index
        position = children + 1
        edge_prefixes = set()
        for _ in range(data[children]):
            end = data.find(b'\0', position)
            require(end >= position, 'Unterminated trie edge')
            edge = data[position:end].decode('utf-8')
            require(edge and edge[0] not in edge_prefixes and len((prefix + edge).encode('utf-8')) < 4096, 'Empty/ambiguous/oversized trie edge')
            edge_prefixes.add(edge[0])
            child, position = read(end + 1)
            require(child < len(data), 'Trie child exceeds bounds')
            visit(child, prefix + edge, ancestors)
    visit(0, '', set())
    return found


def plan_aliases(data, layout):
    aliases = slide.read_aliases(slide.setup.read_inventory())
    canonical = {path: index for index, path in enumerate(compatibility.cache_images(data, layout))}
    address, capacity = slide.unpack(data, '<QQ', 264)
    offset = slide.locate(layout, address, capacity)
    owners = [row for row in layout['mappings'] if row['file_offset'] <= offset and offset + capacity <= row['file_offset'] + row['size']]
    require(len(owners) == 1 and owners[0]['maxprot'] == owners[0]['initprot'] == 1, 'Dylib trie is not wholly read-only metadata')
    require(all(not row['slide_info_size'] or offset + capacity <= row['slide_info_file_offset'] or
                offset >= row['slide_info_file_offset'] + row['slide_info_size'] for row in layout['mappings']), 'Dylib trie overlaps slide metadata')
    original = bytes(data[offset:offset + capacity])
    require(decode_trie(original) == canonical, 'Original trie must contain exactly the canonical image indices')
    entries = dict(canonical)
    for alias in aliases:
        require(alias['path'] not in entries and alias['target'] in canonical, 'Invalid alias/canonical index relationship')
        entries[alias['path']] = canonical[alias['target']]
    encoded = encode_trie(entries)
    require(len(encoded) <= capacity, 'Combined alias trie exceeds its existing allocation; a layout review is required')
    require(decode_trie(encoded) == entries, 'Repacked trie changed lookup semantics')
    for path, index in entries.items():
        require(slide.trie_index(encoded, path) == index, 'Independent path lookup disagrees')
    replacement = encoded + bytes(capacity - len(encoded))
    digest = hashlib.sha256()
    for position in range(0, layout['signature_offset'], compatibility.CHUNK):
        digest.update(data[position:min(position + compatibility.CHUNK, layout['signature_offset'])])
    payload_hash = digest.hexdigest()
    alias_hash = slide.setup.sha(HERE / 'setup-ios26-cache-aliases.json')
    new_uuid = uuid.uuid5(uuid.NAMESPACE_URL, 'LiveSimBooter/23A343/cache-alias-trie1/' + payload_hash + '/' + alias_hash)
    patches = [dict(offset=88, before=bytes(data[88:104]), after=new_uuid.bytes, image=None, purpose='Distinct alias-cache payload UUID'),
               dict(offset=offset, before=original, after=replacement, image=None, purpose='Preserve canonical indices and register 63 original cached aliases')]
    return patches, dict(old_uuid=layout['uuid'], new_uuid=str(new_uuid), source_payload_sha256=payload_hash,
                         alias_inventory_sha256=alias_hash, original_paths=len(canonical), aliases_added=len(aliases), resulting_paths=len(entries),
                         trie_offset=offset, trie_address=address, trie_capacity=capacity, encoded_trie_bytes=len(encoded),
                         encoded_trie_sha256=hashlib.sha256(encoded).hexdigest(), original_lookup_indices_unchanged=True,
                         mappings_and_image_count_unchanged=True, aliases=aliases)


def patch_cache(source, output=None, manifest=None):
    return compatibility.patch_cache(source, output, manifest, planner=plan_aliases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--output', type=Path)
    action.add_argument('--check-only', action='store_true')
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args()
    require(not args.check_only or not args.manifest, '--manifest requires --output')
    print(json.dumps(patch_cache(args.source, args.output, args.manifest), indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, struct.error, UnicodeError) as error:
        sys.exit('patch-ios26-cache-aliases: %s' % error)
