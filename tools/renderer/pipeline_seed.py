#!/usr/bin/env python3
"""S106 PSEED: the merger and the checker of the shipped pipeline seed (`R <hex>` lines + one `H rdr <version> <count>` line in db\\shader_list.txt).
(Fix round: a record's depth slot is the token 0xFFFF, never a literal depth format; the H line is `H rdr <version> <count> <checksum>`; `merge` also writes db\\shaders\\pseed_min.txt, the count `check` holds a list to.)
The seed is hashes, states, vertex layouts and pass formats of OUR renderer's pipelines (no SPIR-V, no game bytes): the rule-1 exception of shader_list.txt (docs\\DECISIONS.md S92).
Record layout = src\\renderer\\pipeline_store.h PutRecord (328 bytes: 56 state, vhash 56, fhash 64, layout 72..214, pass formats 214..224, color_count 224, has_depth 225,
samples 226, pad 227, seen 228, name 232..328). Names are KEPT (a prebuilt program takes its record's name for the whole session). No prepass twins (DrawState::variant 1, byte 14: merge leaves them out, check refuses them; the colour cuts that make
them are off by default). Records with pad > 1 (multiview) are never written: a device without multiview drops them.

  py -I tools\\renderer\\pipeline_seed.py merge --out db\\shaders\\shader_list.txt --catalog CATALOG.txt LIST.bin [LIST.bin ...]   # census store(s) + harvested lists -> the R lines (replaces the old ones)
  py -I tools\\renderer\\pipeline_seed.py check [--list db\\shader_list.txt] --catalog CATALOG.txt    # CATALOG.txt = out\\tools\\pseed_catalog.exe's output (the shaders' own hashes)
  py -I tools\\renderer\\pipeline_seed.py selftest                                                   # the planted faults: each unsafe verdict must be caught
Exit 0 = pass, 1 = a check failed / the merge refused."""
import argparse
import os
import re
import sys

RECORD = 328
HEADER = 40
SEEN = 228
NAME = 232
VARIANT = 14   # DrawState::variant (api.h): 1 = the depth-only prepass twin
VERSION_RE = re.compile(r'kListVersion\s*=\s*(\d+)')
MAX_SEED = 4000   # pipeline_store.h kMaxRecords is 4096: the seed plus the player's own list must not be trimmed silently


def root_default():
    return os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))


def list_version(root):
    with open(os.path.join(root, 'src', 'renderer', 'pipeline_store.h'), encoding='utf-8', errors='replace') as f:
        m = VERSION_RE.search(f.read())
    return int(m.group(1)) if m else 0


def decode_list(path):
    """pipelines.bin -> a list of 328-byte records (the header's count and sizes are checked)."""
    with open(path, 'rb') as f:
        d = f.read()
    if len(d) < HEADER:
        raise ValueError('%s: shorter than its header' % path)
    count = int.from_bytes(d[12:16], 'little')   # ListHeader: magic, version, session, count, payload_bytes (u32 each)
    payload = int.from_bytes(d[16:20], 'little')
    if count * RECORD != payload or len(d) != HEADER + payload:
        raise ValueError('%s: size does not match its record count' % path)
    return [d[HEADER + i * RECORD:HEADER + (i + 1) * RECORD] for i in range(count)]


def key_of(rec):
    """RecordKey = everything but `seen` and the name (pipeline_store.h RecordKey)."""
    return rec[:SEEN] + b'\0' * 4 + b'\0' * (RECORD - NAME)


def vhash(rec):
    return int.from_bytes(rec[56:64], 'little')


def fhash(rec):
    return int.from_bytes(rec[64:72], 'little')


def pad(rec):
    return rec[227]


def rec_variant(rec):
    return rec[VARIANT]


# S106 PSEED fix round (B1): PassData::formats hold VkFormat numbers. The depth slot (index 4, bytes 222..224; has_depth at byte 225) is the device's own depth format
# (VK_FORMAT_D32_SFLOAT_S8_UINT = 130 on the PC), so a seed carries DEPTH_TOKEN there and the loader (src\renderer\pipeline_seed.h ParseSeedLines) swaps in the device's.
DEPTH_SLOT = 222
HAS_DEPTH = 225
DEPTH_TOKEN = 0xFFFF
DEPTH_FORMATS = {124, 125, 126, 127, 128, 129, 130}   # VK_FORMAT_D16_UNORM .. D32_SFLOAT_S8_UINT


def depth_slot(rec):
    return int.from_bytes(rec[DEPTH_SLOT:DEPTH_SLOT + 2], 'little')


def neutral(rec):
    """The record as a seed carries it: a depth format in the depth slot -> DEPTH_TOKEN; no depth -> slot 0. None = the slot holds something that is no depth format."""
    r = bytearray(rec)
    if rec[HAS_DEPTH]:
        if depth_slot(rec) not in DEPTH_FORMATS and depth_slot(rec) != DEPTH_TOKEN:
            return None
        r[DEPTH_SLOT:DEPTH_SLOT + 2] = DEPTH_TOKEN.to_bytes(2, 'little')
    else:
        r[DEPTH_SLOT:DEPTH_SLOT + 2] = b'\0\0'
    return bytes(r)


def checksum(hexes):
    """FNV-1a 64 over every R line's hex text (lower case) + '\\n': the H rdr line's checksum (pipeline_seed.h SeedChecksum reads the same)."""
    h = 0xcbf29ce484222325
    for hx in hexes:
        for b in (hx.lower() + '\n').encode('ascii'):
            h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def resolvable(rec, catalog):
    return vhash(rec) in catalog and (fhash(rec) == 0 or fhash(rec) in catalog)


def merge_records(lists, catalog=None):
    """Union by key, first come first kept (its name stays), seen reset to 0, multiview classes left out; with a catalog, records whose programs it does not hold are left out
    too (S110 RDRPREP: the catalog holds kVariants AND our own kOwnShaders, so the fixed families rdr_* resolve and are kept; only a record of a program no blob of the current build has is left out). Returns (records, stats)."""
    seen_keys, out = set(), []
    st = {'read': 0, 'duplicate': 0, 'multiview': 0, 'unresolvable': 0, 'bad_depth': 0, 'prepass': 0}
    for recs in lists:
        for r in recs:
            st['read'] += 1
            if pad(r) > 1:
                st['multiview'] += 1
                continue
            if rec_variant(r) != 0:   # S106 PSEEDF2 N9: a depth-only prepass twin is never seeded (its colour cut is off by default)
                st['prepass'] += 1
                continue
            r = neutral(r)   # B1: the depth format becomes the token BEFORE the key, so records from different devices dedupe
            if r is None:
                st['bad_depth'] += 1
                continue
            if catalog is not None and not resolvable(r, catalog):
                st['unresolvable'] += 1
                continue
            k = key_of(r)
            if k in seen_keys:
                st['duplicate'] += 1
                continue
            seen_keys.add(k)
            out.append(r[:SEEN] + b'\0' * 4 + r[NAME:])
    return out, st


def seed_lines(records, version):
    hexes = [r.hex() for r in records]
    return ['H rdr %d %d %016x' % (version, len(records), checksum(hexes))] + ['R ' + h for h in hexes]


def rewrite_list(text, records, version):
    """Drops the old R / `H rdr` lines, appends the new ones at the end (every other line is kept byte for byte)."""
    keep = [ln for ln in text.splitlines() if not (ln.startswith('R ') or ln.startswith('H rdr '))]
    while keep and not keep[-1]:
        keep.pop()
    return '\n'.join(keep + seed_lines(records, version)) + '\n'


def load_catalog(path, stages=None):
    """The catalog's hashes. With `stages` (a dict), also fills stages[hash] = set of 'V' / 'F' / '?' the blob(s) of that hash have (pseed_catalog's third column)."""
    hashes = set()
    with open(path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            w = ln.split()
            if w and re.fullmatch(r'[0-9a-f]{16}', w[0]):
                hashes.add(int(w[0], 16))
                if stages is not None and len(w) >= 3:
                    stages.setdefault(int(w[0], 16), set()).add(w[-1])
    return hashes


def seed_min_path(root):
    return os.path.join(root, 'db', 'shaders', 'pseed_min.txt')


def read_min(path):
    """The committed least number of R lines the shipped list must hold. `merge` writes it; a list regenerated without its seed fails against it. None = no readable file:
    check_text then refuses any list that holds R lines (deleting the file must not switch the minimum off; S106 PSEEDF2 N5)."""
    try:
        with open(path, encoding='utf-8') as f:
            return int(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def check_text(text, catalog, version, stages=None, minimum=0):
    """Returns a list of problems (empty = the seed is good). `stages` = load_catalog's stage map (vertex hashes must be V blobs, fragment hashes F blobs); `minimum` = the committed count."""
    bad = []
    recs, keys, header, hexes, nheaders = [], set(), None, [], 0
    for no, ln in enumerate(text.split('\n'), 1):   # the C++ loader splits on \n only and trims only spaces and CR (pipeline_seed.h ParseSeedLines)
        ln = ln.rstrip(' \r')
        if ln.startswith('H rdr '):
            if header is not None or nheaders:
                bad.append('line %d: more than one H rdr line (the loader reads the first)' % no)
            nheaders += 1
            w = ln.split()
            if len(w) != 5 or not (w[2].isdigit() and w[3].isdigit() and re.fullmatch(r'[0-9a-fA-F]{16}', w[4])):
                bad.append('line %d: malformed H rdr line (want: H rdr <version> <count> <16-digit checksum>)' % no)
            else:
                header = (int(w[2]), int(w[3]), int(w[4], 16))
            continue
        if not ln.startswith('R '):
            continue
        hx = ln[2:].lstrip(' ')
        if len(hx) != RECORD * 2 or not re.fullmatch(r'[0-9a-fA-F]+', hx):
            bad.append('line %d: not a %d-byte hex record' % (no, RECORD))
            continue
        hexes.append(hx)
        r = bytes.fromhex(hx)
        want_slot = DEPTH_TOKEN if r[HAS_DEPTH] else 0
        if SPIRV_MAGIC_LE in bytes.fromhex(hx) or SPIRV_MAGIC_BE in bytes.fromhex(hx):
            bad.append('line %d: the record holds the SPIR-V magic 03022307 (shader code in the shipped list: CLAUDE.md rule 1)' % no)
        if depth_slot(r) != want_slot:
            bad.append('line %d: depth slot holds %d, a seed carries %s there (a literal depth format is device-specific: 129 / 130)' % (no, depth_slot(r), 'the token 0xFFFF' if want_slot else '0'))
        if rec_variant(r) != 0:
            bad.append('line %d: a depth-only prepass twin (variant %d) is never shipped' % (no, rec_variant(r)))
        if pad(r) > 1:
            bad.append('line %d: a multiview class (pad %d) is never shipped' % (no, pad(r)))
        if r[226] == 0 or r[226] & (r[226] - 1):
            bad.append('line %d: samples %d is not a power of two' % (no, r[226]))
        k = key_of(r)
        if k in keys:
            bad.append('line %d: duplicate of an earlier record (same key)' % no)
        keys.add(k)
        if int.from_bytes(r[SEEN:SEEN + 4], 'little') != 0:
            bad.append('line %d: seen is not 0' % no)
        for what, h, stage in (('vertex', vhash(r), 'V'), ('fragment', fhash(r), 'F')):
            if h == 0 and what == 'fragment':
                continue
            if h not in catalog:
                bad.append('line %d: %s hash %016x is not in the shader catalog (a stale seed)' % (no, what, h))
            elif stages is not None and h in stages and stage not in stages[h]:
                bad.append('line %d: %s hash %016x is a %s blob in the catalog (a swapped or mis-merged record)' % (no, what, h, '/'.join(sorted(stages[h]))))
        recs.append(r)
    if header is None:
        if recs:
            bad.append('R lines but no H rdr <version> <count> <checksum> line')
    else:
        if header[0] != version:
            bad.append('H rdr version %d, this build writes lists of version %d' % (header[0], version))
        if header[1] != len(recs):
            bad.append('H rdr count %d but %d R lines' % (header[1], len(recs)))
        if header[2] != checksum(hexes):
            bad.append('H rdr checksum does not match the R lines (edited or merged by hand: run `pipeline_seed.py merge` again)')
    if minimum is None:
        if recs:
            bad.append('%d R lines but no readable db/shaders/pseed_min.txt (the committed minimum a seed is held to: restore it, or run `pipeline_seed.py merge`)' % len(recs))
    elif len(recs) < minimum:
        bad.append('%d R lines, the committed minimum is %d (db/shaders/pseed_min.txt: a regenerated list lost its seed?)' % (len(recs), minimum))
    if len(recs) > MAX_SEED:
        bad.append('%d R lines: more than %d (the list holds 4096 and StoreSave would trim the oldest silently)' % (len(recs), MAX_SEED))
    return bad


SPIRV_MAGIC_LE = bytes.fromhex('03022307')   # the SPIR-V module magic as stored by a little-endian word dump (and its byte-swapped twin): never in a shipped record
SPIRV_MAGIC_BE = bytes.fromhex('07230203')


def synth(vh, fh, pad_=0, seen=0, name=b'x', samples=1, depth=DEPTH_TOKEN, variant=0):
    r = bytearray(RECORD)
    r[VARIANT] = variant
    r[56:64] = vh.to_bytes(8, 'little')
    r[64:72] = fh.to_bytes(8, 'little')
    r[224:228] = bytes([1, 1, samples, pad_])
    r[DEPTH_SLOT:DEPTH_SLOT + 2] = depth.to_bytes(2, 'little')
    r[SEEN:SEEN + 4] = seen.to_bytes(4, 'little')
    r[NAME:NAME + len(name)] = name
    return bytes(r)


def selftest():
    """The unsafe direction of each verdict: a stale hash, a flipped byte, a duplicate, a multiview record, a wrong count, a literal depth format, an in-range edit, a vertex hash that is a
    fragment blob, a lost seed."""
    cat = {0xAAAA, 0xBBBB, 0xCCCC}
    stages = {0xAAAA: {'V'}, 0xBBBB: {'F'}, 0xCCCC: {'V'}}
    a, b = synth(0xAAAA, 0xBBBB, name=b'yBG_a'), synth(0xCCCC, 0, name=b'rdr_ui')
    good, st = merge_records([[a, b, a, synth(0xAAAA, 0xBBBB, pad_=2)], [synth(0xAAAA, 0xBBBB, seen=7, name=b'other')]])
    fails = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            fails.append(what)

    def probs(t, version=2, **kw):
        return check_text(t, cat, version, **kw)

    need(len(good) == 2 and st['duplicate'] == 2 and st['multiview'] == 1, 'merge: duplicates (even with another seen / name) and multiview classes are left out, the first name kept')
    only, st2 = merge_records([[a, synth(0x9999, 0xBBBB, name=b'rdr_ui')]], cat)
    need(len(only) == 1 and st2['unresolvable'] == 1, 'merge: a record whose program the catalog lacks is left out')
    need(good[0][NAME:NAME + 5] == b'yBG_a' and good[0][SEEN:SEEN + 4] == b'\0' * 4, 'merge: names kept, seen reset')
    # B1: the depth format never ships literally; the PC's D32S8 (130) and a D24 device's (129) records are one record
    lit, st3 = merge_records([[synth(0xAAAA, 0xBBBB, depth=130)], [synth(0xAAAA, 0xBBBB, depth=129)], [synth(0xCCCC, 0, depth=77)]])
    need(len(lit) == 1 and depth_slot(lit[0]) == DEPTH_TOKEN and st3['duplicate'] == 1 and st3['bad_depth'] == 1, 'merge: a literal depth format (129 / 130) becomes the token, device records dedupe, a non-depth format in the slot is left out')
    text = rewrite_list('S 1 2\nR deadbeef\nH rdr 1 1\nN fbo 00\n', good, 2)
    need('R deadbeef' not in text and 'S 1 2' in text and 'N fbo 00' in text and text.count('H rdr ') == 1, 'rewrite: old R / H rdr lines replaced, every other line kept')
    need(probs(text, stages=stages) == [], 'check: the good seed passes (twin 0)')
    flipped = text.replace('R ' + good[0].hex(), 'R ' + good[0].hex()[:112] + '0' + good[0].hex()[113:], 1)   # a flipped digit inside the vertex hash
    need(any('not in the shader catalog' in p for p in probs(flipped)), 'twin 1: a flipped hash digit is caught as stale')
    need(any('not in the shader catalog' in p for p in check_text(text, {0xAAAA}, 2)), 'twin 2: a catalog that lost a shader is caught')
    dup = text + 'R ' + good[0].hex() + '\n'
    need(any('duplicate' in p for p in probs(dup)), 'twin 3: a duplicate line is caught')
    mv = text + 'R ' + synth(0xAAAA, 0xBBBB, pad_=3).hex() + '\n'
    need(any('multiview' in p for p in probs(mv)), 'twin 4: a multiview record is caught')
    need(any('count' in p for p in probs(text.replace('H rdr 2 2 ', 'H rdr 2 5 '))), 'twin 5: a wrong count in the H line is caught')
    need(any('version' in p for p in probs(text, 3)), 'twin 6: a version the build does not read is caught')
    need(any('not a' in p for p in probs(text + 'R 00\n')), 'twin 7: a short line is caught')
    need(any('no H rdr' in p for p in probs('R ' + good[0].hex() + '\n')), 'twin 8: R lines with no H rdr line are caught')
    # B1 twin: the builder's old seed (the PC's literal depth format 130 in the depth slot, everything else right) must FAIL
    litrec = synth(0xAAAA, 0xBBBB, depth=130, name=b'yBG_a')
    old = '\n'.join(seed_lines([litrec], 2)) + '\n'
    need(any('depth slot' in p for p in probs(old)), 'twin 9: a seed holding the literal D32S8 (130) in the depth slot is caught')
    # F1 twin: an in-range edit (a flipped digit of the name, still decodes, still in the catalog) passes every other check: only the checksum can see it
    hx = good[0].hex()
    edited = text.replace('R ' + hx, 'R ' + hx[:160] + ('1' if hx[160] == '0' else '0') + hx[161:], 1)   # a digit inside the vertex layout: still a valid record, still in the catalog
    need(edited != text and any('checksum' in p for p in probs(edited)), 'twin 10: an in-range one-digit edit is caught by the H rdr checksum')
    # F8 twin: a record whose vertex hash is a fragment blob
    swapped = '\n'.join(seed_lines([synth(0xBBBB, 0xAAAA, name=b'yBG_a')], 2)) + '\n'
    need(probs(swapped) == [] and any('blob' in p for p in probs(swapped, stages=stages)), 'twin 11: a vertex hash that is a fragment blob (and the reverse) is caught with the stage map')
    # F7 twin: the seed is lost from a regenerated list while the committed minimum says it shipped
    need(any('minimum' in p for p in probs('S 1 2\n', minimum=2)), 'twin 12: a list that lost its seed fails against the committed minimum')
    need(probs(text, minimum=2) == [], 'check: a seed at the minimum passes')
    need(probs('S 1 2\n') == [], 'check: a list with no seed at all passes while no minimum is committed')
    big = rewrite_list('', [synth(0xAAAA, 0xBBBB, seen=0, name=b'n%d' % i) for i in range(1)], 2)
    need(probs(big) == [], 'check: a one-record seed passes')
    # S106 PSEEDF2 N5 / N7 / N9: the fix round's twins
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        need(read_min(os.path.join(d, 'pseed_min.txt')) is None, 'N5: read_min says None (not 0) for a missing pseed_min.txt')
    need(any('minimum' in p for p in probs(text, minimum=None)), 'N5 twin: a list with R lines and NO pseed_min.txt fails (deleting the file must not switch the check off)')
    need(probs('S 1 2\n', minimum=None) == [], 'N5: a list with no seed and no pseed_min.txt passes')
    hdr = [ln for ln in text.split('\n') if ln.startswith('H rdr ')][0]
    need(any('more than one' in p for p in probs(text + hdr + '\n')), 'N7 twin: a second H rdr line is refused (C++ reads the first, Python used to read the last)')
    need(any('not a' in p for p in probs(text.replace('R ' + good[0].hex(), 'R ' + good[0].hex() + '\t', 1))), 'N7 twin: a tab after an R line fails here as it fails in the C++ loader')
    need(probs(text.replace('R ' + good[0].hex(), 'R ' + good[0].hex() + ' \r', 1)) == [], 'N7: trailing spaces and a CR are trimmed (as the C++ loader trims them)')
    pre, stp = merge_records([[synth(0xAAAA, 0xBBBB, variant=1, name=b'prepass')]])
    need(pre == [] and stp['prepass'] == 1, 'N9: merge leaves out a depth-only prepass twin (variant 1)')
    need(any('prepass' in p for p in probs('\n'.join(seed_lines([synth(0xAAAA, 0xBBBB, variant=1)], 2)) + '\n')), 'N9 twin: a list holding a prepass twin is refused by check')
    # SHADERSEED twin: shader code in the shipped list (rule 1): a record whose name field holds the SPIR-V magic (either byte order) is refused
    for magic in (SPIRV_MAGIC_LE, SPIRV_MAGIC_BE):
        need(any('SPIR-V magic' in p for p in probs('\n'.join(seed_lines([synth(0xAAAA, 0xBBBB, name=b'x' + magic)], 2)) + '\n')), 'SHADERSEED twin: an R line holding the SPIR-V magic %s is refused' % magic.hex())
    print('pipeline_seed selftest: ' + ('FAILED: ' + '; '.join(fails) if fails else 'every check passed'))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('merge')
    m.add_argument('--out', required=True)
    m.add_argument('--catalog', required=True, help='pseed_catalog.exe output: only records its shaders can build are written')
    m.add_argument('lists', nargs='+')
    c = sub.add_parser('check')
    c.add_argument('--list', default=os.path.join(root_default(), 'db', 'shaders', 'shader_list.txt'))
    c.add_argument('--catalog', required=True)
    sub.add_parser('selftest')
    a = ap.parse_args()
    root = root_default()
    if a.cmd == 'selftest':
        return selftest()
    version = list_version(root)
    if a.cmd == 'merge':
        recs, st = merge_records([decode_list(p) for p in a.lists], load_catalog(a.catalog))
        if len(recs) > MAX_SEED:
            print('FAIL: %d records, more than %d' % (len(recs), MAX_SEED))
            return 1
        with open(a.out, encoding='utf-8', newline='') as f:
            text = f.read()
        nl = '\r\n' if '\r\n' in text else '\n'
        new = rewrite_list(text, recs, version)
        with open(a.out, 'w', encoding='utf-8', newline='') as f:
            f.write(new.replace('\n', nl))
        mp = os.path.join(os.path.dirname(os.path.abspath(a.out)), 'pseed_min.txt')   # F7: the committed count a regenerated list is checked against (lower it by hand, on purpose)
        with open(mp, 'w', newline='\n') as f:
            f.write('%d\n' % len(recs))
        print('pipeline_seed: %d records written to %s (%d read, %d duplicates, %d multiview, %d unresolvable, %d bad depth slot left out); minimum %d -> %s' % (
            len(recs), a.out, st['read'], st['duplicate'], st['multiview'], st['unresolvable'], st['bad_depth'], len(recs), mp))
        return 0
    with open(a.list, encoding='utf-8', errors='replace') as f:
        text = f.read()
    stages = {}
    bad = check_text(text, load_catalog(a.catalog, stages), version, stages, read_min(os.path.join(os.path.dirname(os.path.abspath(a.list)), 'pseed_min.txt')))
    for p in bad[:40]:
        print('FAIL ' + p)
    n = sum(1 for ln in text.splitlines() if ln.startswith('R '))
    print('pipeline_seed check: %s (%d R lines, %d problems)' % ('FAILED' if bad else 'ok', n, len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
