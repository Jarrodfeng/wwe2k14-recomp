"""Session 32: pack a folder into an Xbox 360 STFS content package, to test DLC package support without a real one.

    py tools\\dlc\\make_test_stfs.py FOLDER OUT_FILE [--magic LIVE] [--writable] [--title 545408B2] [--type 2]
        [--name "Display name"]

The layout follows ReXGlue's StfsContainerDevice (thirdparty\\rexglue-sdk-src\\src\\filesystem\\devices\\
stfs_container_device.cpp: BlockToOffsetSTFS, BlockToHashBlockNumberSTFS) and the structures in stfs_xbox.h, so
ReXGlue's reader (the game's mount) is the independent check of what this writes. Read-only format by default (one
hash table per level, as marketplace packages); --writable writes the two-table format (both copies, active index 0).
Files are stored in consecutive blocks with their level-0 hash entries chained (next block, 0xFFFFFF at the end) and
SHA-1s filled in. No signature (nothing checks it). A package made from game files is game content: keep it under
out\\ (gitignored), never commit or share it.
"""

import argparse
import hashlib
import os
import struct
import sys

BLOCK = 0x1000
L0, L1 = 170, 28900
END = 0xFFFFFF
HEADER_SIZE = 0x971A


class Layout:
    def __init__(self, bpht):
        self.bpht = bpht
        self.step0 = L0 + bpht
        self.step1 = L1 + (L0 + 1) * bpht
        self.base = (HEADER_SIZE + 0xFFF) & ~0xFFF

    def block_offset(self, b):  # BlockToOffsetSTFS
        base, block = L0, b
        for _ in range(3):
            block += ((b + base) // base) * self.bpht
            if b < base:
                break
            base *= L0
        return self.base + (block << 12)

    def hash_block(self, b, level):  # BlockToHashBlockNumberSTFS
        if level == 0:
            if b < L0:
                return 0
            block = (b // L0) * self.step0 + ((b // L1) + 1) * self.bpht
            return block if b < L1 else block + self.bpht
        if level == 1:
            return self.step0 if b < L1 else (b // L1) * self.step1 + self.bpht
        return self.step1


def collect(folder):
    """Entries in directory-table order: each directory's entries, then its subdirectories' (parents first)."""
    entries = []  # (name, is_dir, parent_index, host_path)

    def walk(host_dir, parent_index):
        names = sorted(os.listdir(host_dir), key=str.lower)
        subdirs = []
        for n in names:
            p = os.path.join(host_dir, n)
            if len(n.encode('ascii', 'replace')) > 40:
                raise SystemExit('name longer than 40 characters: %s' % p)
            if os.path.isdir(p):
                entries.append((n, True, parent_index, p))
                subdirs.append((p, len(entries) - 1))
            else:
                entries.append((n, False, parent_index, p))
        for p, idx in subdirs:
            walk(p, idx)

    walk(folder, 0xFFFF)
    return entries


def fat_datetime():
    return 0x5B53, 0x6000  # 2025-10-19 12:00 (any valid FAT date / time)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder')
    ap.add_argument('out')
    ap.add_argument('--magic', default='LIVE', choices=['LIVE', 'PIRS', 'CON '])
    ap.add_argument('--writable', action='store_true')
    ap.add_argument('--active-secondary', action='store_true',
                    help='with --writable: every hash table only in its second copy, the root and every level-1 / '
                         'level-2 record marking the second copy active (as a console leaves a rewritten package); '
                         'the first copies stay zero, so a reader that picks them sees broken chains')
    ap.add_argument('--title', default='545408B2')
    ap.add_argument('--type', default='2')
    ap.add_argument('--name', default='Test package')
    a = ap.parse_args()

    if a.active_secondary and not a.writable:
        raise SystemExit('--active-secondary needs --writable (read-only packages have one copy of each table)')
    entries = collect(a.folder)
    lay = Layout(2 if a.writable else 1)
    sec = BLOCK if a.active_secondary else 0
    ft_count = max(1, (len(entries) + 63) // 64)
    # Blocks: the file table first, then each file's data, consecutively.
    next_block = ft_count
    starts, lengths = {}, {}
    for i, (name, is_dir, parent, path) in enumerate(entries):
        if not is_dir:
            size = os.path.getsize(path)
            lengths[i] = size
            starts[i] = next_block if size else 0
            next_block += (size + BLOCK - 1) // BLOCK
    total = next_block

    # Every block's chain successor.
    chain = {}
    for b in range(ft_count):
        chain[b] = b + 1 if b + 1 < ft_count else END
    for i, start in starts.items():
        n = (lengths[i] + BLOCK - 1) // BLOCK
        for k in range(n):
            chain[start + k] = start + k + 1 if k + 1 < n else END

    # The file's size: past every data block and every hash table the reader may touch (both copies if writable).
    last = 0
    for b in range(total):
        last = max(last, lay.block_offset(b) + BLOCK)
        last = max(last, lay.base + (lay.hash_block(b, 0) << 12) + BLOCK * lay.bpht)
        if total > L0:
            last = max(last, lay.base + (lay.hash_block(b, 1) << 12) + BLOCK * lay.bpht)
        if total > L1:
            last = max(last, lay.base + (lay.hash_block(b, 2) << 12) + BLOCK * lay.bpht)

    with open(a.out, 'wb') as out:
        out.truncate(last)

        def put(off, data):
            out.seek(off)
            out.write(data)

        def hash_entry(b, data):
            off = lay.base + (lay.hash_block(b, 0) << 12) + sec + (b % L0) * 0x18
            put(off, hashlib.sha1(data.ljust(BLOCK, b'\0')).digest() + struct.pack('>I', (2 << 30) | chain[b]))
            if a.active_secondary:  # the level-1 / level-2 records covering b: "the second copy is active"
                if total > L0:
                    put(lay.base + (lay.hash_block(b, 1) << 12) + sec + ((b // L0) % L0) * 0x18 + 0x14,
                        struct.pack('>I', 0x40000000))
                if total > L1:
                    put(lay.base + (lay.hash_block(b, 2) << 12) + sec + ((b // L1) % L0) * 0x18 + 0x14,
                        struct.pack('>I', 0x40000000))

        # File table.
        table = bytearray(ft_count * BLOCK)
        d, t = fat_datetime()
        for i, (name, is_dir, parent, path) in enumerate(entries):
            e = bytearray(0x40)
            nb = name.encode('ascii', 'replace')
            e[0:len(nb)] = nb
            n_blocks = 0 if is_dir else (lengths[i] + BLOCK - 1) // BLOCK
            e[0x28] = len(nb) | (0x80 if is_dir else 0x40)  # directory, or contiguous
            e[0x29:0x2C] = struct.pack('<I', n_blocks)[:3]
            e[0x2C:0x2F] = struct.pack('<I', n_blocks)[:3]
            e[0x2F:0x32] = struct.pack('<I', 0 if is_dir else starts[i])[:3]
            e[0x32:0x34] = struct.pack('>H', parent)
            e[0x34:0x38] = struct.pack('>I', 0 if is_dir else lengths[i])
            e[0x38:0x40] = struct.pack('>HHHH', d, t, d, t)
            table[i * 0x40:(i + 1) * 0x40] = e
        for b in range(ft_count):
            blk = bytes(table[b * BLOCK:(b + 1) * BLOCK])
            put(lay.block_offset(b), blk)
            hash_entry(b, blk)
        # Data.
        for i, start in starts.items():
            with open(entries[i][3], 'rb') as f:
                k = 0
                while True:
                    blk = f.read(BLOCK)
                    if not blk:
                        break
                    put(lay.block_offset(start + k), blk)
                    hash_entry(start + k, blk)
                    k += 1

        # Header (stfs_xbox.h offsets).
        h = bytearray(HEADER_SIZE)
        h[0:4] = a.magic.encode('ascii')
        struct.pack_into('>I', h, 0x340, HEADER_SIZE)
        struct.pack_into('>I', h, 0x344, int(a.type, 16))
        struct.pack_into('>I', h, 0x348, 2)  # metadata version
        struct.pack_into('>Q', h, 0x34C, last)  # content size
        struct.pack_into('>I', h, 0x360, int(a.title, 16))
        vd = 0x379
        h[vd] = 0x24
        h[vd + 2] = (0x00 if a.writable else 0x01) | (0x02 if a.active_secondary else 0)  # read_only, root_active
        struct.pack_into('<H', h, vd + 3, ft_count)
        h[vd + 5:vd + 8] = struct.pack('<I', 0)[:3]  # file table at block 0
        struct.pack_into('>I', h, vd + 0x1C, total)
        struct.pack_into('>I', h, vd + 0x20, 0)
        struct.pack_into('>I', h, 0x39D, 0)  # data file count (STFS)
        struct.pack_into('>I', h, 0x3A9, 0)  # volume type STFS
        name16 = a.name.encode('utf-16-be')[:254]
        h[0x411:0x411 + len(name16)] = name16
        put(0, bytes(h))

    print('%s: %d entries, %d blocks (%s format), %.1f MB' % (a.out, len(entries), total,
                                                             'writable' if a.writable else 'read-only', last / 2**20))
    return 0


if __name__ == '__main__':
    sys.exit(main())
