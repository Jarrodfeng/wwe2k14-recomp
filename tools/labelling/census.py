"""Session 68: how much of the image can be named without any model (zero-token passes), and what is left.

    py tools\\labelling\\census.py [--json OUT]

Reads only derived data (the q index, image.bin, the library map, coverage, db\\*.json) and prints counts; it names
nothing and writes nothing unless --json is given (then out\\ only: it holds addresses).

Buckets, each function counted once, first match wins:
  labelled      has a db entry already
  library       inside a library-map block (Havok, CRT, Lua, Bink, audio, XDK, png / zlib)
  thunk         one instruction, a plain branch to another function
  trivial       at most 4 instructions, no call (returns a constant / field, stores a field, empty)
  clone         same instructions as a labelled function (calls resolved to their targets, data addresses masked)
  rtti          a slot of an RTTI vtable (class name known), or builds an RTTI vtable address (ctor / dtor)
  strings       references at least one string
  rest          none of the above
Plus, across buckets: how many ran in the coverage build, and the clone clusters among unlabelled functions (label
one, the rest follow).
"""
import argparse
import collections
import glob
import hashlib
import json
import os
import sqlite3
import struct
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
INDEX = os.path.join(REPO, 'out', 'harness', 'index', 'index.db')
IMAGE = os.path.join(REPO, 'out', 'harness', 'index', 'image.bin')
LIBMAP = os.path.join(REPO, 'out', 'harness', 'labelling', 'library_map.json')
COVERAGE = os.path.join(REPO, 'out', 'harness', 'coverage', 'coverage.json')
BASE = 0x82000000


def db_addresses():
    out = set()
    for p in glob.glob(os.path.join(REPO, 'db', '*.json')):
        with open(p, encoding='utf-8') as f:
            d = json.load(f)
        for e in d if isinstance(d, list) else d.get('functions', []):
            try:
                out.add(int(e['address'], 16))
            except (KeyError, ValueError, TypeError):
                pass
    return out


def library_ranges():
    with open(LIBMAP, encoding='utf-8') as f:
        d = json.load(f)
    return [(int(b['start'], 16), int(b['end'], 16), b['lib']) for b in d['blocks']]


def masked_words(img, addr, size, mask_data):
    """Instruction words with relative branches made position independent.

    b / bl (opcode 18): the displacement becomes the absolute target, so two copies calling the same function
    match and copies calling different functions do not. bc (16) stays relative (inside the function).
    mask_data: low 16 bits of lis / addi / ori / D-form loads and stores whose base is not r1 (stack) are zeroed
    when the instruction is a lis, or an addi / ori / load / store right after one; a coarse "same code, other
    global" match.
    """
    words = struct.unpack('>%dI' % (size // 4), img[addr - BASE:addr - BASE + size])
    out = []
    lis_regs = set()
    for i, w in enumerate(words):
        op = w >> 26
        pc = addr + 4 * i
        if op == 18:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            tgt = (li if w & 2 else pc + li) & 0xFFFFFFFF
            out.append((w & 0xFC000003, tgt))
            continue
        if mask_data:
            rt = (w >> 21) & 31
            ra = (w >> 16) & 31
            if op == 15 and ra == 0:      # lis
                lis_regs.add(rt)
                out.append((w & 0xFFFF0000, 0))
                continue
            if op in (14, 24) or 32 <= op <= 55:
                src = ra if op != 24 else rt
                if src in lis_regs:
                    out.append((w & 0xFFFF0000, 0))
                    continue
        out.append((w, 0))
    return out


def fingerprint(img, addr, size, mask_data):
    h = hashlib.blake2b(digest_size=16)
    for w, t in masked_words(img, addr, size, mask_data):
        h.update(struct.pack('>II', w, t))
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', help='write the buckets (addresses only) here (out\\ only)')
    args = ap.parse_args()

    con = sqlite3.connect(INDEX)
    funcs = {a: (s, n) for a, s, n in con.execute('select addr, size, insns from funcs where has_body = 1')}
    with open(IMAGE, 'rb') as f:
        img = f.read()
    labelled = db_addresses() & funcs.keys()
    libs = library_ranges()
    with open(COVERAGE, encoding='utf-8') as f:
        ran = {int(k, 16) for k in json.load(f)['functions']}

    calls = collections.defaultdict(set)
    for src, dst, kind in con.execute('select src, dst, kind from edges'):
        calls[src].add((dst, kind))
    rtti_slot = {a for (a,) in con.execute('select distinct func from rtti_slot')}
    vtables = {a for (a,) in con.execute('select vt from rtti_vtable')}
    builds_vt = {f for f, v in con.execute("select func, value from imms where op = 'lis+addi'") if v in vtables}
    strfuncs = {a for (a,) in con.execute('select distinct func from strrefs')}

    def lib_of(a):
        for s, e, lib in libs:
            if s <= a < e:
                return lib
        return None

    # clone fingerprints: exact (calls resolved) and data-masked
    fp_exact, fp_mask = {}, {}
    for a, (size, n) in funcs.items():
        if size < 8 or size % 4:
            continue
        fp_exact[a] = fingerprint(img, a, size, False)
        fp_mask[a] = fingerprint(img, a, size, True)
    labelled_fp = {fp_exact[a] for a in labelled if a in fp_exact} | {fp_mask[a] for a in labelled if a in fp_mask}

    bucket = {}
    lib_counts = collections.Counter()
    for a, (size, n) in funcs.items():
        if a in labelled:
            bucket[a] = 'labelled'
            continue
        lib = lib_of(a)
        if lib:
            bucket[a] = 'library'
            lib_counts[lib] += 1
            continue
        c = calls.get(a, set())
        if n == 1 and len(c) == 1 and next(iter(c))[1] == 'tail':
            bucket[a] = 'thunk'
            continue
        if n <= 4 and not any(k == 'call' for _, k in c):
            bucket[a] = 'trivial'
            continue
        if fp_exact.get(a) in labelled_fp or fp_mask.get(a) in labelled_fp:
            bucket[a] = 'clone'
            continue
        if a in rtti_slot or a in builds_vt:
            bucket[a] = 'rtti'
            continue
        if a in strfuncs:
            bucket[a] = 'strings'
            continue
        bucket[a] = 'rest'

    order = ['labelled', 'library', 'thunk', 'trivial', 'clone', 'rtti', 'strings', 'rest']
    total = len(funcs)
    insns_total = sum(n for _, n in funcs.values())
    print(f'functions with a body: {total:,} ({insns_total:,} instructions); ran in coverage: {len(ran & funcs.keys()):,}')
    print(f'{"bucket":10} {"funcs":>7} {"share":>6} {"ran":>6} {"instructions":>13}')
    for b in order:
        mem = [a for a, v in bucket.items() if v == b]
        ins = sum(funcs[a][1] for a in mem)
        print(f'{b:10} {len(mem):7,} {100 * len(mem) / total:5.1f}% {len(set(mem) & ran):6,} {ins:13,}')
    print('library by lib:', ', '.join(f'{k} {v:,}' for k, v in lib_counts.most_common()))

    # clone clusters among the unlabelled, non-library functions: label one, the rest follow
    pool = [a for a, v in bucket.items() if v in ('rtti', 'strings', 'rest') and a in fp_mask]
    groups = collections.defaultdict(list)
    for a in pool:
        groups[fp_mask[a]].append(a)
    multi = [g for g in groups.values() if len(g) > 1]
    saved = sum(len(g) - 1 for g in multi)
    print(f'clone clusters among the {len(pool):,} unlabelled game functions: {len(multi):,} clusters holding '
          f'{sum(len(g) for g in multi):,} functions; labelling one per cluster saves {saved:,}')
    left = [a for a in pool if a not in {x for g in multi for x in g[1:]}]
    big = sorted(left, key=lambda a: -funcs[a][1])
    sizes = [funcs[a][1] for a in left]
    sizes.sort()
    q = lambda p: sizes[int(p * (len(sizes) - 1))] if sizes else 0
    print(f'left for a model after all of that: {len(left):,} functions, {sum(sizes):,} instructions '
          f'(median {q(0.5)}, p90 {q(0.9)}, max {q(1.0)}); of them ran in coverage: {len(set(left) & ran):,}')

    if args.json:
        out = {'buckets': {b: sorted(f'0x{a:08X}' for a, v in bucket.items() if v == b) for b in order},
               'clone_clusters': [sorted(f'0x{a:08X}' for a in g) for g in multi],
               'left': sorted(f'0x{a:08X}' for a in left)}
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(out, f, indent=1)
        print('wrote', args.json)


if __name__ == '__main__':
    sys.exit(main())
