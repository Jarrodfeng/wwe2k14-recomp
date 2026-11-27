#!/usr/bin/env python3
# entry: main
"""S111 PSEEDSTALE: moves the shipped pipeline seed's stale shader hashes onto the current ones (0 game runs, no GPU, no game data).
A seed record (db\\shaders\\shader_list.txt R line, pipeline_store.h PutRecord) names its program by the hashes of OUR compiled SPIR-V; every other field (the draw state, the
vertex layout, the pass formats, the name) comes from the draw (renderer StoreNoteBuilt), and every program shares one pipeline layout. So when a shader's WORDS change and its
interface does not, the record a new harvest would write is the old one with the new hash: this tool writes exactly that, instead of a harvest that needs game runs.
  py -I tools\\renderer\\pseed_rekey.py [--list db\\shaders\\shader_list.txt] [--base REV] [--dry]
  py -I tools\\renderer\\pseed_rekey.py --catalog NEW.txt --old-catalog OLD.txt [--list FILE] [--dry]     (catalogs already built: pseed_catalog.exe output)
  py -I tools\\renderer\\pseed_rekey.py --selftest
The OLD catalog is the shaders the seed was made from: the tree at --base (default: the last commit that changed the list's `H rdr` line), its src\\renderer read with
`git archive` (nothing in the repo changes) and compiled with THIS tree's pseed_catalog.cpp, so both catalogs carry the interface column. A stale hash is moved only when its
OLD blob (name, stage, place among that name's blobs: a kVariants alpha twin shares its variant's name) is ONE blob of the NEW catalog with the same interface; any other
stale hash is left and named (that record needs a harvest: pipeline_census.py + pipeline_seed.py merge). The H rdr line is rebuilt (count, checksum); every other line of
the list is kept byte for byte, its line ends too. Exit 0 = the list now passes pseed_stale_check (no stale record, no problem); 1 = a record is left stale or the list has
another problem; 2 = a catalog could not be built."""
import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pipeline_seed as P   # noqa: E402
import pseed_stale_check as S   # noqa: E402

VHASH, FHASH = 56, 64   # pipeline_store.h PutRecord: the vertex / fragment hash (u64 little endian each)


def parse_catalog(text):
    """pseed_catalog lines -> (ids, iface): ids[hash] = {(name, stage, n)}, n = the blob's place among the catalog's blobs of that name and stage (0 = a variant's spirv,
    1 = its alpha twin); iface[hash] = {the interface column} ({None} for a three-column catalog, which this tool never moves a hash by)."""
    ids, iface, count = {}, {}, {}
    for ln in text.splitlines():
        w = ln.split()
        if len(w) < 3 or not re.fullmatch(r'[0-9a-f]{16}', w[0]):
            continue
        h, ident = int(w[0], 16), (w[1], w[-1])
        n = count.get(ident, 0)
        count[ident] = n + 1
        ids.setdefault(h, set()).add(ident + (n,))
        iface.setdefault(h, set()).add(w[2] if len(w) >= 4 else None)
    return ids, iface


def rekey_records(records, old_text, new_text):
    """(records, moved, left, merged): each record hash the NEW catalog lacks becomes the NEW hash of the same blob when that blob is one NEW hash with the same interface;
    moved = [(index, name, stage word, old hash, new hash)], left = [(index, name, stage word, hash, why)] (kept as they were), merged = records dropped because the move made
    them the key of an earlier record."""
    old_ids, old_if = parse_catalog(old_text)
    new_ids, new_if = parse_catalog(new_text)
    by_ident = {}
    for h, idents in new_ids.items():
        for ident in idents:
            by_ident.setdefault(ident, set()).add(h)
    out, keys, moved, left, merged = [], set(), [], [], 0
    for i, r in enumerate(records):
        b = bytearray(r)
        name = r[P.NAME:].split(b'\0')[0].decode('latin1')
        for what, off in (('vertex', VHASH), ('fragment', FHASH)):
            h = int.from_bytes(r[off:off + 8], 'little')
            if (h == 0 and what == 'fragment') or h in new_ids:
                continue
            cands = set()
            for ident in old_ids.get(h, ()):
                cands |= by_ident.get(ident, set())
            nh = next(iter(cands)) if len(cands) == 1 else None
            if h not in old_ids:
                why = 'no blob of the old catalog has this hash (pass the tree the seed was made from: --base REV)'
            elif nh is None:
                why = '%d current blobs carry its name and stage' % len(cands)
            elif None in old_if[h] or None in new_if[nh] or old_if[h] != new_if[nh]:
                why = 'its interface changed (or a catalog has no interface column): a harvest writes this record'
            else:
                b[off:off + 8] = nh.to_bytes(8, 'little')
                moved.append((i, name, what, h, nh))
                continue
            left.append((i, name, what, h, why))
        k = P.key_of(bytes(b))
        if k in keys:
            merged += 1
            continue
        keys.add(k)
        out.append(bytes(b))
    return out, moved, left, merged


def seed_records(text):
    """The R lines' records in list order (a line that is no record is left to check_text to name)."""
    out = []
    for ln in text.split('\n'):
        ln = ln.rstrip(' \r')
        if ln.startswith('R '):
            hx = ln[2:].lstrip(' ')
            if len(hx) == P.RECORD * 2 and re.fullmatch(r'[0-9a-fA-F]+', hx):
                out.append(bytes.fromhex(hx))
    return out


def rekey_text(text, old_text, new_text, version):
    """(new list text, moved, left, merged): the list with its R / H rdr lines rewritten (pipeline_seed.rewrite_list), line ends kept."""
    nl = '\r\n' if '\r\n' in text else '\n'
    recs, moved, left, merged = rekey_records(seed_records(text), old_text, new_text)
    if not moved and not merged:
        return text, moved, left, merged
    return P.rewrite_list(text.replace('\r\n', '\n'), recs, version).replace('\n', nl), moved, left, merged


def default_base(root):
    p = subprocess.run(['git', '-C', root, 'log', '-1', '--format=%H', '-G', '^H rdr ', '--', 'db/shaders/shader_list.txt'], capture_output=True, text=True, timeout=120)
    return p.stdout.strip() if p.returncode == 0 else ''


def base_catalog(root, rev, out_dir):
    """The catalog of the tree at REV: its src\\renderer (git archive: read only) compiled with THIS tree's pseed_catalog.cpp. Returns the path or None."""
    tree = os.path.join(out_dir, 'base_tree')
    p = subprocess.run(['git', '-C', root, 'archive', '--format=tar', rev, 'src/renderer'], capture_output=True, timeout=600)
    if p.returncode != 0:
        print('git archive %s failed: %s' % (rev, p.stderr.decode('utf-8', 'replace')[-500:]))
        return None
    with tarfile.open(fileobj=io.BytesIO(p.stdout)) as t:
        t.extractall(tree, filter='data')
    os.makedirs(os.path.join(tree, 'tools', 'renderer'), exist_ok=True)
    shutil.copy(os.path.join(root, 'tools', 'renderer', 'pseed_catalog.cpp'), os.path.join(tree, 'tools', 'renderer', 'pseed_catalog.cpp'))
    return S.build_catalog(tree, os.path.join(out_dir, 'base'))


def read(path):
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--list', default='')
    ap.add_argument('--base', default='', help='the commit whose shaders the seed was made from (default: the last commit that changed the H rdr line)')
    ap.add_argument('--catalog', default='', help='the current catalog (default: built from this tree)')
    ap.add_argument('--old-catalog', default='', help='the seed\'s catalog (default: built from --base)')
    ap.add_argument('--dry', action='store_true', help='print what would move, write nothing')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    tmp = tempfile.mkdtemp(prefix='pseed_rekey_')
    try:
        return rekey_cli(a, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)   # the two catalogs, their exes and the base tree's src\renderer


def rekey_cli(a, tmp):
    root = os.path.abspath(a.root)
    lst = a.list or os.path.join(root, 'db', 'shaders', 'shader_list.txt')
    new_cat = a.catalog or S.build_catalog(root, os.path.join(tmp, 'now'))
    base = a.base or ('' if a.old_catalog else default_base(root))
    old_cat = a.old_catalog or (base_catalog(root, base, tmp) if base else None)
    if not new_cat or not old_cat:
        print('pseed_rekey: no %s catalog (base %s)' % ('current' if not new_cat else 'old', base or 'none'))
        return 2
    text = read(lst)
    version = P.list_version(root)
    new_text, moved, left, merged = rekey_text(text, read(old_cat), read(new_cat), version)
    for i, name, what, h, nh in moved:
        print('moved: record %d (%s) %s hash %016x -> %016x' % (i + 1, name, what, h, nh))
    for i, name, what, h, why in left:
        print('LEFT: record %d (%s) %s hash %016x: %s' % (i + 1, name, what, h, why))
    if merged:
        print('merged: %d records became the key of an earlier record and were dropped' % merged)
    if not a.dry and new_text != text:
        with open(lst, 'w', encoding='utf-8', newline='') as f:
            f.write(new_text)
    stages = {}
    catalog = P.load_catalog(new_cat, stages)
    minimum = P.read_min(os.path.join(os.path.dirname(os.path.abspath(lst)), 'pseed_min.txt'))
    bad, stale = S.verdict(new_text, catalog, version, stages, minimum)
    for p in bad[:40]:
        print('FAIL ' + p)
    print('pseed_rekey: %s (base %s; %d moved, %d left, %d merged; %s)' % ('ok' if not bad and not stale else 'FAILED', base or a.old_catalog, len(moved), len(left), merged,
                                                                       'dry: nothing written' if a.dry else ('written to ' + lst if new_text != text else 'the list was already current')))
    return 1 if bad or stale else 0


def selftest():
    fails = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            fails.append(what)

    def cat(rows):
        return '\n'.join('%016x %s %s %s' % r if len(r) == 4 else '%016x %s %s' % r for r in rows) + '\n'

    old = cat([(0xAAAA, 'rdr_flash_vert', '00000000000000f1', 'V'), (0xBBBB, 'rdr_flash_frag', '00000000000000f2', 'F'),
               (0xCCCC, 'yX_vertex', '00000000000000f3', 'V'), (0xDDDD, 'yX_vertex', '00000000000000f3', 'V'), (0x7777, 'rdr_ui_vert', '00000000000000f4', 'V')])
    new = cat([(0xEEEE, 'rdr_flash_vert', '00000000000000f1', 'V'), (0xBBBB, 'rdr_flash_frag', '00000000000000f2', 'F'),
               (0x1111, 'yX_vertex', '00000000000000f3', 'V'), (0x2222, 'yX_vertex', '00000000000000f3', 'V'), (0x8888, 'rdr_ui_vert', '00000000000000f9', 'V')])
    flash, twin, ui, cur = (P.synth(0xAAAA, 0xBBBB, name=b'flash: crowd'), P.synth(0xDDDD, 0, name=b'yX alpha'), P.synth(0x7777, 0, name=b'rdr_ui'),
                            P.synth(0x1111, 0, name=b'yX'))
    recs, moved, left, merged = rekey_records([flash, twin, ui, cur], old, new)
    need(P.vhash(recs[0]) == 0xEEEE and P.fhash(recs[0]) == 0xBBBB and recs[0][P.NAME:P.NAME + 5] == b'flash', 'a stale own vertex hash with the same interface moves to the current blob of that name; the rest of the record is kept')
    need(P.vhash(recs[1]) == 0x2222, 'an alpha twin (the second blob of a variant name) moves to the second blob, not the first')
    need(P.vhash(recs[2]) == 0x7777 and any(x[1] == 'rdr_ui' and 'interface' in x[4] for x in left), 'twin: a changed interface is never moved (named in left)')
    need(recs[3] == cur and len(moved) == 2, 'a current record is untouched')
    _, _, left2, _ = rekey_records([P.synth(0x9999, 0xBBBB)], old, new)
    need(len(left2) == 1 and 'old catalog' in left2[0][4], 'twin: a hash no old blob has is left and named')
    three = '\n'.join(' '.join(ln.split()[:2] + ln.split()[3:]) for ln in old.splitlines()) + '\n'
    _, mv3, left3, _ = rekey_records([flash], three, new)
    need(not mv3 and len(left3) == 1, 'twin: a catalog without the interface column moves nothing')
    same_words_old = old + cat([(0x5555, 'p_vert', '00000000000000f5', 'V'), (0x5555, 'q_vert', '00000000000000f5', 'V')])
    same_words_new = new + cat([(0x6666, 'p_vert', '00000000000000f5', 'V'), (0x6767, 'q_vert', '00000000000000f5', 'V')])
    _, mv4, left4, _ = rekey_records([P.synth(0x5555, 0)], same_words_old, same_words_new)
    need(not mv4 and len(left4) == 1 and '2 current blobs' in left4[0][4], 'twin: an old hash two blobs shared, now two hashes, is never guessed (left and named)')
    dup, mv5, _, merged5 = rekey_records([flash, P.synth(0xEEEE, 0xBBBB, name=b'flash: crowd')], old, new)
    need(len(dup) == 1 and merged5 == 1, 'a record the move makes the key of an earlier one is dropped (merged)')
    lst = '\r\n'.join(['S 1 2', 'N fbo 00'] + P.seed_lines([flash, cur], 2)) + '\r\n'
    catset = {0xEEEE, 0xBBBB, 0x1111, 0x2222, 0x8888}
    need(any('not in the shader catalog' in p for p in P.check_text(lst, catset, 2)), 'the list before the rekey fails pipeline_seed check (the planted stale seed)')
    out, mv6, left6, _ = rekey_text(lst, old, new, 2)
    need(P.check_text(out, catset, 2) == [] and len(mv6) == 1 and not left6, 'after the rekey the list passes pipeline_seed check (count and checksum rebuilt)')
    need(out.startswith('S 1 2\r\nN fbo 00\r\n') and '\n' not in out.replace('\r\n', ''), 'every other line kept, the CRLF line ends kept')
    same, mv7, _, _ = rekey_text(out, old, new, 2)
    need(same == out and not mv7, 'a current list is left byte for byte')
    print('pseed_rekey selftest: ' + ('FAILED: ' + '; '.join(fails) if fails else 'every check passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
