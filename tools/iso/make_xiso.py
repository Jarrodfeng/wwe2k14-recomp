"""Pack a game folder into an Xbox 360 disc image (GDFX / XDVDFS), and check an image against a folder.

Our own tool (Session 26), for testing ISO support with files the user already has. The image it writes is the
user's game data: keep it under game_data\\ (gitignored), never commit or share it.

  py tools\\iso\\make_xiso.py pack game_data\\extracted game_data\\iso\\wwe2k14_test.iso [--offset xgd3]
  py tools\\iso\\make_xiso.py check game_data\\iso\\wwe2k14_test.iso game_data\\extracted [--quick]
  py tools\\iso\\make_xiso.py list  game_data\\iso\\wwe2k14_test.iso

Format (the layout ReXGlue's DiscImageDevice reads, from Xenia; the same as extract-xiso images):
  - 2048-byte sectors; the game partition starts at --offset (0 = a trimmed image; xgd3 = 0x2080000, xgd2 = 0xFD90000,
    where the game partition sits in a full disc image);
  - sector 32 of the partition: "MICROSOFT*XBOX*MEDIA", root table sector (u32), root table size (u32), a FILETIME,
    the magic again at byte 0x7EC;
  - a directory table is a binary tree of entries: left / right links (u16, in 4-byte units from the table start; 0 =
    none), data sector (u32), size (u32), attributes (u8: 0x10 folder), name length (u8), the name, padded to 4 bytes
    with 0xFF; an entry never crosses a sector boundary; entry 0 is the tree's root; names sorted without case.
  All numbers little-endian.
"""
import argparse
import hashlib
import os
import struct
import sys
import time
from pathlib import Path

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
OFFSETS = {'0': 0, 'xgd3': 0x2080000, 'xgd2': 0xFD90000}
SEARCH_OFFSETS = [0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000]  # DiscImageDevice::Verify
ATTR_DIR, ATTR_FILE = 0x10, 0x20


def sectors(n):
    return (n + SECTOR - 1) // SECTOR


class Node:
    def __init__(self, path, name, is_dir):
        self.path, self.name, self.is_dir = path, name, is_dir
        self.children = []  # folders only
        self.size = 0  # files: bytes; folders: table bytes
        self.sector = 0
        self.table = b''


def scan(folder):
    root = Node(folder, '', True)
    stack = [root]
    while stack:
        d = stack.pop()
        for e in sorted(os.scandir(d.path), key=lambda e: e.name.upper()):
            if len(e.name.encode('ascii')) > 255:
                sys.exit(f'name too long: {e.path}')
            n = Node(Path(e.path), e.name, e.is_dir())
            if n.is_dir:
                stack.append(n)
            else:
                n.size = e.stat().st_size
                if n.size >= 1 << 32:
                    sys.exit(f'file over 4 GB (the format stores 32-bit sizes): {e.path}')
            d.children.append(n)
    return root


def balanced(items):
    """Pre-order of a balanced tree over the (sorted) items: (item, left subtree, right subtree)."""
    if not items:
        return None
    mid = len(items) // 2
    return (items[mid], balanced(items[:mid]), balanced(items[mid + 1:]))


def entry_len(n):
    return (14 + len(n.name) + 3) & ~3


def layout_table(d):
    """Places d's entries (pre-order, root first) and returns {id(node): byte offset}; no entry crosses a sector."""
    pos, offs = 0, {}

    def place(t):
        nonlocal pos
        if t is None:
            return
        n, left, right = t
        size = entry_len(n)
        if pos // SECTOR != (pos + size - 1) // SECTOR:
            pos = (pos // SECTOR + 1) * SECTOR
        offs[id(n)] = pos
        pos += size
        place(left)
        place(right)

    tree = balanced(d.children)
    place(tree)
    return tree, offs, pos


def build_table(d):
    tree, offs, used = layout_table(d)
    buf = bytearray(b'\xff' * (sectors(used) * SECTOR))

    def write(t):
        if t is None:
            return
        n, left, right = t
        lo = offs[id(left[0])] // 4 if left else 0
        ro = offs[id(right[0])] // 4 if right else 0
        if lo > 0xFFFF or ro > 0xFFFF:
            sys.exit(f'folder too large for 16-bit links: {d.path}')
        name = n.name.encode('ascii')
        rec = struct.pack('<HHIIBB', lo, ro, n.sector, n.size, ATTR_DIR if n.is_dir else ATTR_FILE, len(name)) + name
        o = offs[id(n)]
        buf[o:o + len(rec)] = rec
        write(left)
        write(right)

    write(tree)
    return bytes(buf)


def pack(folder, out, offset, extras=(), empty_ff=False):
    t0 = time.time()
    root = scan(Path(folder))
    for spec in extras:  # NAME=FILE: one more file at the top of the image (tests, e.g. a stray default.xexp)
        name, src = spec.split('=', 1)
        n = Node(Path(src), name, False)
        n.size = n.path.stat().st_size
        root.children = sorted([c for c in root.children if c.name.upper() != name.upper()] + [n],
                               key=lambda c: c.name.upper())
    dirs, files, q = [], [], [root]
    while q:  # breadth first: tables first, then file data
        d = q.pop(0)
        dirs.append(d)
        for c in d.children:
            (q if c.is_dir else files).append(c)
    # Table sizes depend only on names, so sectors can be handed out before the tables are written.
    next_sector = 33
    for d in dirs:
        used = layout_table(d)[2]
        # An empty folder: length 0 (default), or with --empty-ff one sector of 0xFF padding (another encoding the
        # readers must accept, Session 26 review).
        d.size = sectors(used) * SECTOR if d.children else (SECTOR if empty_ff else 0)
        d.sector = next_sector if d.size else 0
        next_sector += sectors(d.size)
    for f in files:
        f.sector = next_sector if f.size else 0
        next_sector += sectors(f.size)
    for d in dirs:
        d.table = build_table(d) if d.children else bytes([0xFF]) * d.size
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    total = offset + next_sector * SECTOR
    with open(out, 'wb') as w:
        w.truncate(total)
        vd = bytearray(SECTOR)
        vd[0:20] = MAGIC
        struct.pack_into('<IIQ', vd, 20, root.sector, root.size, 0)
        vd[0x7EC:0x7EC + 20] = MAGIC
        w.seek(offset + 32 * SECTOR)
        w.write(vd)
        for d in dirs:
            if d.table:
                w.seek(offset + d.sector * SECTOR)
                w.write(d.table)
        done = 0
        for i, f in enumerate(files, 1):
            if not f.size:
                continue
            w.seek(offset + f.sector * SECTOR)
            with open(f.path, 'rb') as r:
                while chunk := r.read(16 << 20):
                    w.write(chunk)
            done += f.size
            if i % 50 == 0:
                print(f'  {i}/{len(files)} files, {done / 1e9:.2f} GB', flush=True)
    print(f'packed {len(files)} files in {len(dirs)} folders: {out} ({total:,} bytes, game partition at {offset:#x}), '
          f'{time.time() - t0:.0f} s')


def read_image(path):
    """Yields (relative path, is_dir, data offset, size) for every entry, like DiscImageDevice (any tree shape)."""
    f = open(path, 'rb')
    size = os.path.getsize(path)
    for base in SEARCH_OFFSETS:
        f.seek(base + 32 * SECTOR)
        head = f.read(28)
        if head[:20] == MAGIC:
            break
    else:
        sys.exit(f'{path}: no "{MAGIC.decode()}" at any known game partition offset')
    root_sector, root_size = struct.unpack_from('<II', head, 20)
    out = []

    def walk(table, ordinal, prefix):
        if ordinal == 0 and table[:4] == bytes([0xFF]) * 4:
            return  # an empty folder stored as padding
        p = ordinal * 4
        lo, ro, sector, length, attr, nlen = struct.unpack_from('<HHIIBB', table, p)
        name = table[p + 14:p + 14 + nlen].decode('ascii')
        if lo:
            walk(table, lo, prefix)
        rel = prefix + name
        if attr & ATTR_DIR:
            out.append((rel, True, 0, 0))
            if length:
                f.seek(base + sector * SECTOR)
                walk(f.read(length), 0, rel + '/')
        else:
            out.append((rel, False, base + sector * SECTOR, length))
        if ro:
            walk(table, ro, prefix)

    f.seek(base + root_sector * SECTOR)
    walk(f.read(root_size), 0, '')
    f.close()
    return base, size, out


def check(image, folder, quick):
    t0 = time.time()
    base, size, entries = read_image(image)
    folder = Path(folder)
    want = {str(p.relative_to(folder)).replace('\\', '/'): p for p in folder.rglob('*')}
    got = {e[0]: e for e in entries}
    bad = 0
    for rel in sorted(set(want) ^ set(got)):
        print(f'  only in {"folder" if rel in want else "image"}: {rel}')
        bad += 1
    with open(image, 'rb') as f:
        for rel, (_, is_dir, off, length) in sorted(got.items()):
            p = want.get(rel)
            if p is None or is_dir:
                continue
            if p.stat().st_size != length:
                print(f'  size differs: {rel} {length} vs {p.stat().st_size}')
                bad += 1
                continue
            if off + length > size:
                print(f'  past the end of the image: {rel}')
                bad += 1
                continue
            h1, h2 = hashlib.sha1(), hashlib.sha1()
            f.seek(off)
            todo = min(length, 1 << 20) if quick else length
            with open(p, 'rb') as r:
                while todo:
                    n = min(todo, 16 << 20)
                    h1.update(f.read(n))
                    h2.update(r.read(n))
                    todo -= n
            if h1.digest() != h2.digest():
                print(f'  content differs: {rel}')
                bad += 1
    files = sum(1 for e in entries if not e[1])
    print(f'{"PASS" if not bad else "FAIL"}: {files} files, {len(entries) - files} folders, partition at {base:#x}, '
          f'{"first MB of each file" if quick else "every byte"} compared, {bad} problem(s), {time.time() - t0:.0f} s')
    return 0 if not bad else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('pack')
    a.add_argument('folder')
    a.add_argument('image')
    a.add_argument('--offset', default='0', help='0 (trimmed), xgd3, xgd2 or a number')
    a.add_argument('--extra', action='append', default=[], metavar='NAME=FILE',
                   help='add FILE at the top of the image as NAME (tests)')
    a.add_argument('--empty-ff', action='store_true', help='store empty folders as a sector of 0xFF (tests)')
    b = sub.add_parser('check')
    b.add_argument('image')
    b.add_argument('folder')
    b.add_argument('--quick', action='store_true', help='compare only the first MB of each file')
    c = sub.add_parser('list')
    c.add_argument('image')
    args = ap.parse_args()
    if args.cmd == 'pack':
        offset = OFFSETS.get(args.offset)
        pack(args.folder, args.image, int(args.offset, 0) if offset is None else offset, args.extra, args.empty_ff)
    elif args.cmd == 'check':
        sys.exit(check(args.image, args.folder, args.quick))
    else:
        base, size, entries = read_image(args.image)
        for rel, is_dir, off, length in entries:
            print(f'{"<dir>" if is_dir else length:>12}  {rel}')
        print(f'{len(entries)} entries, partition at {base:#x}, image {size:,} bytes')


if __name__ == '__main__':
    main()
