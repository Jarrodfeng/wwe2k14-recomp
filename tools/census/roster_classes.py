r"""Lane 167 (S101): the whole roster as CLASSES the wrestler join branches on, which classes an existing capture already proves, and a representative for the rest.
0 game runs: it reads the player's own pac\ch files (out\tools\roster_dump.exe, tools\census\roster_dump.cpp) and runs the shipped decision core
(out\tools\wrestler_ours_check.exe) over kept captures. Output is ids, counts and class names; it is printed, never committed as game data.

  py -I tools\census\roster_classes.py                      the class table + the reduced live plan (needs out\s101\census\roster_dump.tsv and proven.json)
  py -I tools\census\roster_classes.py --refresh            run roster_dump.exe again (~7 s) before the table
  py -I tools\census\roster_classes.py --scan-proven PATH...  run the checker over captures (a capture folder, a case folder, or a case zip) and add them to proven.json
  py -I tools\census\roster_classes.py --characters         also the per-character table (102 pacs)
  py -I tools\census\roster_classes.py --json OUT           the same as json
  py -I tools\census\roster_classes.py --selftest           synthetic dump + proven: the grouping, proof and cover rules (no game files)

WHICH PROPERTIES THE JOIN BRANCHES ON (read in src\renderer_scene\wrestler_core.h + src\assets\wrestler_geom.h, 2026-11-12):
  1. COUNTS: Resolve() fits an object to roster entries by (vertices, index_count) of every drawn part, part index = the stream's part.  More than one fit = ambiguous, settled by bytes.
  2. LAYOUT: wrestler_geom::MapFor/Classify derive the game's guest vertex from the file's stream strides: weights exist iff S1/8 >= 2, a tangent iff S3 == 12.  The 11-dword fallback needs S3 == 12.
     So the part SHAPE = (weighted?, tangent?).  Effect names are NOT read (no Texture keep in this core); 'trans' (alpha) is kept as a flag only because the 11-dword pieces are all yCh_trans (lane 134).
  3. INDEX: the file's strip must be u16, indexed, one block (groups > 1 is flagged).
  4. PALETTE: the rows at c76 + 3k, k < palette_count (1..20 in the data, raw == clamped, so no clamp class); bodies link to a record skeleton by NODE COUNT then rows >= 0.3;
     pieces link to a body by node NAME (own small table).
  5. KIND: body (entry 0), accessory sets (26, 27), hair / mask pieces (10xxx), alpha pieces (20002), other 2xxxx pieces.
  6. SCALE / fill0: measured per object from the stream (0.1 < s < 10, residual 1e-5); NOT in the file, so not a class.  The proven bodies' scales are in proven.json.
CLASS KEY = (kind, shape set, T = has a trans effect, C = has cloth, M = has a part with more than one index group).
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return Path(env)
    try:
        common = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return HERE


MAIN = main_checkout()
CENSUS = HERE / 'out' / 's101' / 'census'
DUMP = CENSUS / 'roster_dump.tsv'
PROVEN = CENSUS / 'proven.json'

KIND_ORDER = ['BODY', 'SET', 'HAIR', 'ALPHA', 'EXTRA']


def kind_of(entry):
    if entry == 0:
        return 'BODY'
    if entry in (26, 27):
        return 'SET'
    if 10000 <= entry < 20000:
        return 'HAIR'
    if entry == 20002:
        return 'ALPHA'
    return 'EXTRA'


def shape_of(s1, s3):
    """The part shape the Classify branches on: W = weights (S1/8 >= 2), R = rigid; t = tangent (S3 == 12), n = none."""
    return ('W' if s1 // 8 >= 2 else 'R') + ('t' if s3 == 12 else 'n')


def effect_flags(eff):
    lo = eff.lower()
    return ('trans' in lo, 'cloth' in lo)


# ---------------------------------------------------------------- the dump
def parse_dump(lines):
    """-> (entries, parts): entries[(ch, attire, entry)] = {nodes, hash, parts}; parts[(ch, attire, entry)] = [part dicts]."""
    entries, parts = {}, collections.defaultdict(list)
    for ln in lines:
        f = ln.rstrip('\n').split('\t')
        if f[0] == 'E':
            entries[(int(f[1]), int(f[2]), int(f[3]))] = {'nodes': int(f[4]), 'hash': f[5], 'n': int(f[6])}
        elif f[0] == 'P':
            parts[(int(f[1]), int(f[2]), int(f[3]))].append({'i': int(f[4]), 'v': int(f[5]), 'ix': int(f[6]), 'pal': int(f[7]), 'g': int(f[8]), 's': tuple(int(x) for x in f[9:13]),
                                                           'tiles': int(f[13]), 'ok': int(f[14]), 'eff': f[15], 'raw_pal': int(f[17]) if len(f) > 17 else int(f[7])})
    return entries, parts


def features(entries, parts):
    """Per entry: kind, shapes, T, C, M, atoms, and the counts-collision flags."""
    out = {}
    sig_index = collections.defaultdict(set)   # (part index, v, ix) -> {entry keys}
    for k, ps in parts.items():
        for p in ps:
            sig_index[(p['i'], p['v'], p['ix'])].add(k)
    for k, e in entries.items():
        ps = parts.get(k, [])
        shapes = sorted({shape_of(p['s'][1], p['s'][3]) for p in ps})
        t = any(effect_flags(p['eff'])[0] for p in ps)
        c = any(effect_flags(p['eff'])[1] for p in ps)
        m = any(p['g'] > 1 for p in ps)
        bad = any((not p['ok']) or (not p['tiles']) for p in ps)
        atoms = sorted({(p['s'][1] // 8, p['s'][3] == 12, 'cloth' if effect_flags(p['eff'])[1] else ('trans' if effect_flags(p['eff'])[0] else 'solid')) for p in ps})
        # the join's fit: another entry with the same (vertices, indices) at EVERY part index this entry has
        fits = None
        for p in ps:
            s = sig_index[(p['i'], p['v'], p['ix'])]
            fits = set(s) if fits is None else fits & s
        fits = fits or {k}
        same_pac = sum(1 for o in fits if o != k and o[0] == k[0])
        cross = sum(1 for o in fits if o[0] != k[0])
        out[k] = {'kind': kind_of(k[2]), 'shapes': shapes, 'T': t, 'C': c, 'M': m, 'bad': bad, 'atoms': atoms, 'nodes': e['nodes'], 'hash': e['hash'], 'nparts': len(ps),
                  'fit_same_pac': same_pac, 'fit_cross': cross, 'pal_max': max([p['pal'] for p in ps] or [0])}
    return out


def class_key(f):
    return (f['kind'], '+'.join(f['shapes']) or '-', 'T' if f['T'] else '', 'C' if f['C'] else '', 'M' if f['M'] else '')


def class_name(key):
    kind, shapes, t, c, m = key
    return kind + ' ' + shapes + ''.join(' ' + x for x in (t, c, m) if x)


# ---------------------------------------------------------------- proof
def load_proven(path=PROVEN):
    if not Path(path).exists():
        return {'captures': {}}
    return json.loads(Path(path).read_text(encoding='utf-8'))


def proven_entries(proven):
    """-> {(ch, attire, entry): [capture names]} for every capture that passed (replaced > 0, every wrong_ counter 0)."""
    out = collections.defaultdict(list)
    for name, c in proven['captures'].items():
        if not c.get('ok'):
            continue
        for b in c['bodies']:
            out[(b[0], b[1], b[2])].append(name)
    return out


BODY_RE = re.compile(r'^BODY ch(\d+) attire (\d+) entry (\d+) nodes (\d+) scale ([0-9.eE+-]+)', re.M)
RES_RE = re.compile(r'^RESULT .*?replaced=(\d+) .*?wrong_vertex=(\d+) wrong_index=(\d+) wrong_rows=(\d+) wrong_count=(\d+)', re.M)
SEEN_RE = re.compile(r'objects seen (\d+) joined (\d+)')


def parse_checker(text):
    bodies = [[int(a), int(b), int(c), int(d), float(e)] for a, b, c, d, e in BODY_RE.findall(text)]
    m = RES_RE.search(text)
    s = SEEN_RE.search(text)
    replaced, wrongs = (int(m.group(1)), [int(x) for x in m.groups()[1:]]) if m else (0, [1])
    return {'bodies': bodies, 'replaced': replaced, 'wrong': sum(wrongs), 'seen': int(s.group(1)) if s else 0, 'joined': int(s.group(2)) if s else 0,
            'ok': bool(m) and replaced > 0 and sum(wrongs) == 0 and bool(bodies)}


def find_capture(path):
    p = Path(path)
    if p.is_dir():
        for c in (p, p / 'capture', p / 'case' / 'capture'):
            if (c / 'images.bin').exists():
                return c, None
        sys.exit(f'FAIL {p}: no capture folder (images.bin)')
    if p.suffix.lower() != '.zip':
        sys.exit(f'FAIL {p}: neither a folder nor a zip')
    dest = CENSUS / 'cap' / p.stem
    if not (dest / 'capture' / 'images.bin').exists():
        dest.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(p) as z:
            z.extractall(dest)
    return dest / 'capture', dest


def exe(name):
    for c in (HERE / 'out' / 'tools' / name, MAIN / 'out' / 'tools' / name):
        if c.exists():
            return c
    sys.exit(f'FAIL no {name}: build it (tools\\census\\build_roster_dump.ps1 / tools\\wrestler\\build_wrestler_ours_check.ps1)')


def scan_proven(paths, step=4, keep=False):
    game = MAIN / 'game_data' / 'extracted'
    chk = exe('wrestler_ours_check.exe')
    proven = load_proven()
    CENSUS.mkdir(parents=True, exist_ok=True)
    for path in paths:
        cap, tmp = find_capture(path)
        has_scene = (cap / 'scene').is_dir() and any((cap / 'scene').glob('frame_*.rsc'))
        name = Path(path).parent.name if Path(path).suffix.lower() == '.zip' else (Path(path).parent.name if Path(path).name in ('capture', 'case') else Path(path).name)
        if Path(path).name == 'case':
            name = Path(path).parent.name
        if Path(path).name == 'capture' and Path(path).parent.name == 'case':
            name = Path(path).parent.parent.name
        if not has_scene:
            print(f'[scan] {name}: no scene records (the wrestler hook cannot run on it): skipped, not a proof')
            continue
        p = subprocess.run([str(chk), '--capture', str(cap), '--game', str(game), '--step', str(step)], capture_output=True, text=True, errors='replace', timeout=1200)
        r = parse_checker(p.stdout + p.stderr)
        r['path'] = str(path)
        r['exit'] = p.returncode
        r['ok'] = r['ok'] and p.returncode == 0
        proven['captures'][name] = r
        print(f'[scan] {name}: exit {p.returncode} seen {r["seen"]} joined {r["joined"]} replaced {r["replaced"]} wrong {r["wrong"]} bodies {len(r["bodies"])} -> {"PROVEN" if r["ok"] else "NOT A PROOF"}')
        if tmp is not None and not keep:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)
    PROVEN.write_text(json.dumps(proven, indent=1), encoding='utf-8')
    return proven


# ---------------------------------------------------------------- the analysis
def analyse(entries, parts, proven, seatable=None, names=None):
    feats = features(entries, parts)
    pe = proven_entries(proven)
    classes = collections.OrderedDict()
    for k in sorted(feats, key=lambda k: (KIND_ORDER.index(feats[k]['kind']), k)):
        classes.setdefault(class_key(feats[k]), []).append(k)
    rows = []
    for key, members in classes.items():
        chars = sorted({m[0] for m in members})
        proved = [m for m in members if m in pe]
        by_caps = sorted({c for m in proved for c in pe[m]})
        att0 = [m for m in members if m[1] == 0]
        rows.append({'key': key, 'name': class_name(key), 'entries': len(members), 'chars': len(chars), 'attire0': len(att0), 'proven_entries': len(proved),
                     'proven_by': by_caps, 'proven_members': sorted(proved), 'members': members})
    # atoms (part-level shapes x pair count x material family) proven by parts of proven entries
    atom_proven = collections.defaultdict(set)
    for k, f in feats.items():
        if k in pe:
            for a in f['atoms']:
                atom_proven[(f['kind'],) + a].add(k[0])
    for r in rows:
        need = set()
        for m in r['members']:
            for a in feats[m]['atoms']:
                need.add((feats[m]['kind'],) + a)
        r['atoms_missing'] = sorted(a for a in need if a not in atom_proven)
        r['status'] = 'proven' if r['proven_entries'] else ('parts-only' if not r['atoms_missing'] else 'unproven')
    return feats, rows, pe, atom_proven


def small_own_candidates(feats):
    """Lane 206: the non-BODY entries with 12..59 nodes (alpha pieces have 11): candidates for an OWN small skeleton (the descriptor, not the file, says which are)."""
    return sorted(k for k, f in feats.items() if f['kind'] != 'BODY' and 12 <= f['nodes'] < 60)


def skeleton_families(feats, pe):
    """BODY entries grouped by (node count, node-name hash): the join links a body to a record skeleton by the COUNT, then by rows.  -> {(nodes, hash): {'members', 'proven'}}."""
    sk = collections.defaultdict(lambda: {'members': [], 'proven': []})
    for k, f in feats.items():
        if k[2] == 0:
            sk[(f['nodes'], f['hash'])]['members'].append(k)
            if k in pe:
                sk[(f['nodes'], f['hash'])]['proven'].append(k)
    return dict(sk)


def piece_families(feats, pe):
    """Pieces grouped by (kind, node count, hash): a piece links to a body by node NAME through its own small table."""
    pf = collections.defaultdict(lambda: {'members': [], 'proven': []})
    for k, f in feats.items():
        if k[2] != 0:
            pf[(f['kind'], f['nodes'], f['hash'])]['members'].append(k)
            if k in pe:
                pf[(f['kind'], f['nodes'], f['hash'])]['proven'].append(k)
    return dict(pf)


def choose_representatives(rows, feats, pe, seatable, per_run=6):
    """Greedy set cover of the UNPROVEN classes (status != proven) by characters whose attire-0 entries carry them (seatable by the launcher with no attire choice).
    A character brings ALL its attire-0 entries (the game draws the body and its pieces).  Ties go to the character whose attire-0 body skeleton family is still unproven (it costs
    no extra seat), then to the one with more parts, then the lowest id.  Seats left in the run (per_run) go to the biggest unproven skeleton families.
    Returns (picks, uncovered classes, by_char_any, spare) with picks = [(character, [classes], skeleton (nodes, hash) or None, proven?)]."""
    sk = skeleton_families(feats, pe)
    sk_unproven = {key for key, v in sk.items() if not v['proven']}
    todo = {r['key'] for r in rows if r['status'] != 'proven'}
    by_char0 = collections.defaultdict(set)
    by_char_any = collections.defaultdict(set)
    body0 = {}
    parts0 = collections.Counter()
    for k, f in feats.items():
        ck = class_key(f)
        if k[1] == 0:
            parts0[k[0]] += f['nparts']
        if k[1] == 0 and k[2] == 0:
            body0[k[0]] = (f['nodes'], f['hash'])
        if ck in todo:
            by_char_any[k[0]].add((ck, k[1]))
            if k[1] == 0:
                by_char0[k[0]].add(ck)
    picks = []
    left = set(todo)
    used_sk = set()
    while left:
        best, gain, bestscore = None, set(), None
        for c, cl in sorted(by_char0.items()):
            if seatable is not None and c not in seatable:
                continue
            g = cl & left
            if not g:
                continue
            new_sk = c in body0 and body0[c] in sk_unproven and body0[c] not in used_sk
            score = (len(g), 1 if new_sk else 0, parts0[c])
            if bestscore is None or score > bestscore:
                best, gain, bestscore = c, g, score
        if not gain:
            break
        if best in body0:
            used_sk.add(body0[best])
        picks.append((best, sorted(gain), body0.get(best), body0.get(best) in sk_unproven))
        left -= gain
    spare = []
    if len(picks) < per_run:
        fam = sorted(((len(v['members']), key) for key, v in sk.items() if not v['proven'] and key not in used_sk), reverse=True)
        chosen = {p[0] for p in picks}
        for _n, key in fam:
            if len(picks) + len(spare) >= per_run:
                break
            cands = [m[0] for m in sk[key]['members'] if m[1] == 0 and m[0] not in chosen and (seatable is None or m[0] in seatable)]
            if cands:
                c = max(cands, key=lambda c: (parts0[c], -c))
                spare.append((c, [], key, True))
                chosen.add(c)
    return picks, sorted(left), by_char_any, spare


# ---------------------------------------------------------------- output
def active_ids():
    """The roster rows the frozen save marks active with pack code 0 (not DLC-gated): row i is character 100 + i, active byte at +16, pack code at +17, 204-byte rows from payload 0x1C
    (docs\\formats\\chetc_roster_record.md; the same decode lane 158 used: 84 active).  None when the frozen save is not there."""
    import importlib.util
    import struct
    root = MAIN / 'out' / 'bench' / 'userdata_frozen'
    if not root.exists():
        return None
    spec = importlib.util.spec_from_file_location('unlock_all_ro', HERE / 'tools' / 'save' / 'unlock_all.py')
    ua = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ua)
    _data, _word, payload, _hdr = ua.read_payload(ua.find_save(str(root)))
    return {100 + i for i in range(400) if payload[0x1C + 204 * i + 16] == 1 and payload[0x1C + 204 * i + 17] == 0}


def load_names():
    f = HERE / 'db' / 'ids' / 'characters.json'
    if not f.exists():
        return {}, {}
    d = json.loads(f.read_text(encoding='utf-8'))
    return {e['id']: e.get('name', '?') for e in d['entries']}, {e['id']: e.get('kind', '?') for e in d['entries']}


def render(rows, feats, pe, atom_proven, picks, uncovered, by_char_any, spare, names, kinds, characters=False, entries=None):
    L = []
    nent = sum(r['entries'] for r in rows)
    nch = len({k[0] for k in feats})
    L.append(f'# roster classes: {nent} roster entries, {nch} character pacs, {len(rows)} classes (key = kind, shapes, T trans, C cloth, M multi-group)')
    L.append('')
    L.append('| class | entries | chars | attire-0 entries | proven entries | status | proven by (capture) | atoms not proven by any proven entry |')
    L.append('|---|---|---|---|---|---|---|---|')
    for r in rows:
        L.append(f"| {r['name']} | {r['entries']} | {r['chars']} | {r['attire0']} | {r['proven_entries']} | {r['status']} | {', '.join(r['proven_by']) or '-'} | " +
                 (', '.join('/'.join(str(x) for x in a) for a in r['atoms_missing']) or '-') + ' |')
    L.append('')
    L.append('## unproven classes: the members (character attire entry)')
    for r in rows:
        if r['status'] == 'proven':
            continue
        mem = sorted({(m[0], m[1], m[2]) for m in r['members']})
        L.append(f"- {r['name']} ({r['status']}): " + ', '.join(f"ch{c:03d}{'' if names.get(c) is None else ' ' + names[c][:16]} a{a} e{e}" for c, a, e in mem[:10]) + (f' ... (+{len(mem) - 10})' if len(mem) > 10 else ''))
    sk = skeleton_families(feats, pe)
    pf = piece_families(feats, pe)
    L.append('')
    L.append(f"## skeleton families (BODY entries by node count + node-name hash): {len(sk)} families, {sum(1 for v in sk.values() if v['proven'])} with a proven body")
    L.append('| nodes | entries | chars | proven bodies | proven chars |')
    L.append('|---|---|---|---|---|')
    bynodes = collections.defaultdict(lambda: [0, set(), 0, set(), 0])
    for (n, h), v in sk.items():
        x = bynodes[n]
        x[0] += len(v['members']); x[1] |= {m[0] for m in v['members']}; x[2] += len(v['proven']); x[3] |= {m[0] for m in v['proven']}; x[4] += 1
    for n in sorted(bynodes):
        x = bynodes[n]
        L.append(f"| {n} | {x[0]} ({x[4]} famil{'y' if x[4] == 1 else 'ies'}) | {len(x[1])} | {x[2]} | {', '.join('ch%03d' % c for c in sorted(x[3])) or '-'} |")
    L.append('')
    own = small_own_candidates(feats)
    L.append(f"## own-skeleton dimension (lane 206): piece entries with 12..59 nodes, {len(own)} entries in {len({k[0] for k in own})} pacs")
    L.append('A piece with this many nodes may be a hair / cap skeleton of its own (Diesel ch335 a0 e10000: 48 nodes, yCh_Aniso_trans) instead of a borrower of its body\'s rows: the pose hook now keeps a')
    L.append('skeleton under 60 nodes when the game\'s descriptor lists it as a piece (wrestler_hook.h KeepSkeleton). The file cannot tell the two apart (the descriptor does): these are the candidates; the K1 run settles them [U].')
    kinds = collections.Counter(feats[k]['kind'] for k in own)
    L.append('by kind: ' + ', '.join(f'{k} {n}' for k, n in sorted(kinds.items())) + '; pacs: ' + ', '.join(f'ch{c:03d}' for c in sorted({k[0] for k in own})[:60]))
    L.append('')
    L.append(f"## piece node tables (kind + node count + hash): {len(pf)} families, {sum(1 for v in pf.values() if v['proven'])} with a proven piece")
    L.append('')
    L.append('## unproven classes: the representative picks (greedy cover over attire-0 entries; ties: unproven skeleton, then parts)')
    if not picks and not uncovered:
        L.append('every class has a proven entry: no unproven class.')
    for c, cl, skey, sk_new in picks:
        L.append(f"- ch{c:03d} {names.get(c, '?')} ({kinds.get(c, '?')}), attire-0 body {skey[0] if skey else '?'} nodes{' (NEW skeleton family)' if sk_new else ''} covers: " + '; '.join(class_name(k) for k in cl))
    for c, cl, skey, sk_new in spare:
        L.append(f"- spare seat: ch{c:03d} {names.get(c, '?')} ({kinds.get(c, '?')}), attire-0 body {skey[0]} nodes (an unproven skeleton family of {len(sk[skey]['members'])} bodies)")
    if uncovered:
        L.append('- NOT in any attire-0 entry (alternate attire needed): ' + '; '.join(class_name(k) for k in uncovered))
        for k in uncovered:
            where = sorted({(m[0], m[1]) for m in next(r for r in rows if r['key'] == k)['members']})
            L.append(f'    {class_name(k)}: ' + ', '.join(f'ch{c:03d} a{a}' for c, a in where[:14]) + (' ...' if len(where) > 14 else ''))
    # re-find of a known case: lane 134's referee pieces (ch255 e21010 / e21011, the 11-dword draw)
    L.append('')
    for e in (21010, 21011):
        k = (255, 0, e)
        if k in feats:
            f = feats[k]
            L.append(f"RE-FIND referee piece ch255 a0 e{e}: class [{class_name(class_key(f))}] ({sum(1 for x in feats if class_key(feats[x]) == class_key(f))} entries in it), {f['nparts']} part(s), {f['nodes']} nodes; " +
                     (f"PROVEN by {', '.join(pe[k])}" if k in pe else 'not drawn in any kept capture (its sibling in the class is: ' + ', '.join(sorted({n for x in pe if class_key(feats[x]) == class_key(f) for n in pe[x]})) + ')'))
    if characters:
        L.append('')
        L.append('## per character (attires = PACH bodies; bodies/entries by attire; classes; proven attires)')
        per = collections.defaultdict(list)
        for k in feats:
            per[k[0]].append(k)
        for c in sorted(per):
            att = sorted({k[1] for k in per[c]})
            cls = sorted({class_name(class_key(feats[k])) for k in per[c]})
            pa = sorted({k[1] for k in per[c] if k in pe})
            L.append(f"ch{c:03d} {names.get(c, '?'):26} attires {len(att)} entries {len(per[c])} nodes {sorted({feats[k]['nodes'] for k in per[c] if k[2] == 0})} proven attires {pa or '-'} | " + '; '.join(cls))
    return '\n'.join(L)


def selftest():
    # synthetic dump: 3 characters.  A: body W t + hair piece; B: body R t + a trans EXTRA piece (the 'referee's patch'); C: cloth body in attire 1
    def P(ch, at, en, i, v, ix, pal, s, eff, g=1):
        return f'P\t{ch}\t{at}\t{en}\t{i}\t{v}\t{ix}\t{pal}\t{g}\t{s[0]}\t{s[1]}\t{s[2]}\t{s[3]}\t1\t1\t{eff}\t3\t{pal}'
    D = [
        'E\t1\t0\t0\t50\taaaa\t2', P(1, 0, 0, 0, 100, 300, 5, (28, 32, 8, 12), 'yCh_skin'), P(1, 0, 0, 1, 80, 200, 20, (28, 32, 8, 12), 'yCh_gear'),
        'E\t1\t0\t10000\t7\tbbbb\t1', P(1, 0, 10000, 0, 40, 90, 2, (28, 24, 8, 12), 'yCh_Aniso_trans'),
        'E\t2\t0\t0\t60\tcccc\t1', P(2, 0, 0, 0, 111, 333, 4, (28, 8, 8, 12), 'yCh_gear'),
        'E\t2\t0\t21011\t10\tdddd\t1', P(2, 0, 21011, 0, 41, 78, 8, (28, 32, 8, 12), 'yCh_trans'),
        'E\t2\t1\t0\t60\tcccc\t1', P(2, 1, 0, 0, 111, 333, 4, (28, 8, 8, 12), 'yCh_cloth'),
        'E\t3\t0\t21011\t10\tdddd\t1', P(3, 0, 21011, 0, 41, 78, 8, (28, 32, 8, 12), 'yCh_trans'),
    ]
    entries, parts = parse_dump(D)
    feats = features(entries, parts)
    assert len(entries) == 6 and class_key(feats[(1, 0, 0)]) == ('BODY', 'Wt', '', '', ''), class_key(feats[(1, 0, 0)])
    assert class_key(feats[(2, 0, 0)]) == ('BODY', 'Rt', '', '', '')
    assert class_key(feats[(2, 0, 21011)]) == ('EXTRA', 'Wt', 'T', '', '')                       # a trans EXTRA piece is its own class
    assert class_key(feats[(2, 1, 0)]) == ('BODY', 'Rt', '', 'C', '')                              # cloth is its own class
    assert feats[(2, 0, 21011)]['fit_cross'] == 1 and feats[(1, 0, 0)]['fit_cross'] == 0           # the same piece counts in another pac = a cross-character fit
    # proof: only ch1 attire 0 body + the EXTRA piece of ch2 were joined in a capture that passed
    chk = 'BODY ch001 attire 0 entry 0 nodes 50 scale 1.0\nBODY ch002 attire 0 entry 21011 nodes 10 scale 0.9\nRESULT claimed=9 replaced=9 kept x wrong_vertex=0 wrong_index=0 wrong_rows=0 wrong_count=0\nwrestlers: objects seen 2 joined 2'
    r = parse_checker(chk)
    assert r['ok'] and len(r['bodies']) == 2, r
    bad = parse_checker(chk.replace('wrong_rows=0', 'wrong_rows=3'))
    assert not bad['ok']                                                                              # a capture with a wrong draw proves nothing
    proven = {'captures': {'capA': r, 'capBad': bad}}
    feats, rows, pe, ap = analyse(entries, parts, proven)
    st = {x['name']: x['status'] for x in rows}
    assert st['BODY Wt'] == 'proven' and st['EXTRA Wt T'] == 'proven', st
    assert st['BODY Rt'] == 'unproven' and st['BODY Rt C'] == 'unproven', st
    assert st['HAIR Wt T'] == 'unproven'                                                              # hair piece of ch1 was not in the BODY list
    picks, left, _, spare = choose_representatives(rows, feats, pe, None)
    assert {p[0] for p in picks} <= {1, 2, 3} and left and all(k[3] == 'C' for k in left), (picks, left)             # cloth lives only in attire 1: no attire-0 cover
    # parts-only: a class whose every atom is proven by another entry
    D2 = D + ['E\t4\t0\t0\t50\teeee\t1', P(4, 0, 0, 0, 150, 400, 5, (28, 32, 8, 12), 'yCh_skin')]
    e2, p2 = parse_dump(D2)
    f2, rows2, pe2, _ = analyse(e2, p2, proven)
    assert [x['entries'] for x in rows2 if x['name'] == 'BODY Wt'] == [2]                             # same class, one proven member = proven class
    print('roster_classes selftest: ok')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--refresh', action='store_true')
    ap.add_argument('--scan-proven', nargs='+')
    ap.add_argument('--step', type=int, default=4)
    ap.add_argument('--keep-extract', action='store_true')
    ap.add_argument('--characters', action='store_true')
    ap.add_argument('--json')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    CENSUS.mkdir(parents=True, exist_ok=True)
    if a.scan_proven:
        scan_proven(a.scan_proven, a.step, a.keep_extract)
    if a.refresh or not DUMP.exists():
        game = MAIN / 'game_data' / 'extracted'
        p = subprocess.run([str(exe('roster_dump.exe')), '--game', str(game)], capture_output=True, text=True, errors='replace', timeout=600)
        if p.returncode != 0:
            sys.exit('FAIL roster_dump exit %d' % p.returncode)
        DUMP.write_text(p.stdout, encoding='utf-8')
    entries, parts = parse_dump(DUMP.read_text(encoding='utf-8').splitlines())
    names, kinds = load_names()
    proven = load_proven()
    feats, rows, pe, atom_proven = analyse(entries, parts, proven)
    act = active_ids()
    seat = {c for c, k in kinds.items() if k == 'wrestler' and (act is None or c in act)} or None
    picks, uncovered, by_char_any, spare = choose_representatives(rows, feats, pe, seat)
    print(render(rows, feats, pe, atom_proven, picks, uncovered, by_char_any, spare, names, kinds, a.characters, entries))
    print()
    for n, c in sorted(proven['captures'].items()):
        print(f"capture {n}: {'PROVEN' if c['ok'] else 'not a proof'}, objects seen {c['seen']} joined {c['joined']}, replaced {c['replaced']}, wrong {c['wrong']}, bodies {len(c['bodies'])}")
    if a.json:
        Path(a.json).write_text(json.dumps({'classes': [{k: v for k, v in r.items() if k not in ('members', 'proven_members')} | {'key': list(r['key'])} for r in rows],
                                            'picks': picks, 'uncovered': [list(u) for u in uncovered]}, indent=1), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
