r"""q shader (S111 QGAPS): which constant lanes and samplers a game shader reads, from the SDK's own disassembly of its microcode.

    py tools/q/q.py shader HASH|VARIANT [--effect E] [--reads] [--json] [--all] [--no-cache]

HASH    the 16-hex XXH3 of the microcode (as the fx.json files list it), or a leading part of it (8 hex digits or more).
VARIANT an fx.json variant: `yCh_skin_flex_muscle_fragment` (effect_variant_stage), or `muscle_fragment` / `muscle` with --effect E.
--reads (the default and only mode) prints, per float constant register, the lanes (x y z w) an instruction reads and how many reads;
indexed constants (c[77+a0]) apart; bool constants (b#); each texture sampler with its kind (2D / Cube / 3D), the destination swizzle
and the coordinate register; vertex-fetch slots. A lane is counted by the rule of the instruction: a vector op reads the swizzle
position of every lane its destination mask writes, dp3 / dp4 the first 3 / 4, dp2add 2 + 2 + 1; a scalar-slot op (`+ ...`, the
1- and 2-lane operands) reads every lane it prints.
The microcode comes from the player's own pacs through tools/shaders/harness/check_effect_ucode.py and is disassembled by
out/s100/disasm/ucode_disasm.exe (built by tools/shaders/packets/build_disasm.ps1). The FIRST call for an effect scans its pacs (~20 s);
the answer is cached as a READ LIST ONLY (numbers, no bytes, no listing) in out/harness/shaderreads/<HASH>.json, so the next call is instant.
Errors: `q shader: ...` on stderr, exit 2.
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SCHEMA = 1
PLANT = os.environ.get('WWE_QSHADER_PLANT', '')     # selftest only: dest_mask_off = every lane of every operand counts, so the twins are seen to fail
LANES = 'xyzw'
NO_DEST = ('exec', 'exece', 'cexec', 'cexece', 'alloc', 'cnop', 'label', 'jmp', 'cjmp', 'serialize', 'nop', 'loop', 'endloop', 'setTexLOD')


class ShaderError(Exception):
    pass


# ------------------------------------------------------------------------------------------------ the listing parser (pure)
CONST_RX = re.compile(r'^[-|]*(?:c|c_abs)\[(?P<rel>\d+\+a\d)\]\.(?P<sw>[xyzw]{1,4})$|^[-|]*(?:c|c_abs)\[?(?P<reg>\d+)\]?\.(?P<sw2>[xyzw]{1,4})$')
FETCH_RX = re.compile(r'^(tfetch\w+|vfetch\w*)\s+(.*)$')


def split_operands(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def dest_positions(dest):
    """the positions (0..3) a destination operand writes: `r3.xy__` -> {0, 1}; `r6` / `oPos` -> all four"""
    m = re.match(r'^[A-Za-z_]+(?:\[\d+\])?\d*\.([xyzw_]{1,4})$', dest.strip())
    if not m or len(m.group(1)) != 4:
        return {0, 1, 2, 3}
    pos = {i for i, c in enumerate(m.group(1)) if c != '_'}
    return pos or {0, 1, 2, 3}


def lanes_read(op, k, sw, dest):
    """the lane letters source operand k (0 = first source) of `op` reads, given its swizzle text and the destination"""
    if len(sw) != 4 or PLANT == 'dest_mask_off':      # a scalar-slot operand (1 or 2 lanes): every lane it prints (selftest plant: every operand)
        return set(sw)
    if op == 'dp3':
        return set(sw[:3])
    if op == 'dp4':
        return set(sw[:4])
    if op == 'dp2add':
        return set(sw[:2]) if k < 2 else set(sw[:1])
    return {sw[i] for i in dest_positions(dest)}


def parse_reads(text):
    """the disassembly text -> {'consts': {reg: {lane: n}}, 'indexed': {'77+a0': {lane: n}}, 'bools': [n], 'samplers': [...], 'vfetch': [...]}"""
    consts, indexed, bools, samplers, vfetch = {}, {}, set(), {}, set()
    for raw in text.splitlines():
        ln = re.sub(r'/\*.*?\*/', '', raw).strip()
        if not ln:
            continue
        ln = ln.lstrip('+').strip()
        ln = re.sub(r'^\(!?p\d\)\s*', '', ln)
        if not ln:
            continue
        op = ln.split(None, 1)[0]
        rest = ln[len(op):].strip()
        if op in ('cexec', 'cexece', 'cjmp'):
            for m in re.finditer(r'\bb(\d+)\b', rest):
                bools.add(int(m.group(1)))
            continue
        if op in NO_DEST:
            continue
        f = FETCH_RX.match(ln)
        if f:
            ops = split_operands(f.group(2))
            if op.startswith('tfetch') and len(ops) >= 3:
                m = re.match(r'tf(\d+)$', ops[2])
                if m:
                    s = samplers.setdefault(int(m.group(1)), {'kind': op[len('tfetch'):], 'dest': set(), 'coord': set(), 'n': 0})
                    s['dest'].add(ops[0])
                    s['coord'].add(ops[1])
                    s['n'] += 1
            elif op.startswith('vfetch') and len(ops) >= 3:
                m = re.match(r'vf(\d+)$', ops[2])
                if m:
                    vfetch.add(int(m.group(1)))
            continue
        ops = split_operands(rest)
        if len(ops) < 2:
            continue
        dest, srcs = ops[0], ops[1:]
        for k, s in enumerate(srcs):
            m = CONST_RX.match(s)
            if not m:
                continue
            sw = m.group('sw') or m.group('sw2')
            lanes = lanes_read(op, k, sw, dest)
            if m.group('rel'):
                d = indexed.setdefault(m.group('rel'), {})
            else:
                d = consts.setdefault(int(m.group('reg')), {})
            for lane in lanes:
                d[lane] = d.get(lane, 0) + 1
    return {'consts': {str(r): v for r, v in sorted(consts.items())}, 'indexed': indexed, 'bools': sorted(bools),
            'samplers': [{'slot': k, 'kind': v['kind'], 'dest': sorted(v['dest']), 'coord': sorted(v['coord']), 'n': v['n']} for k, v in sorted(samplers.items())],
            'vfetch': sorted(vfetch)}


# ------------------------------------------------------------------------------------------------ the fx.json names
def load_variants(root):
    """-> [{'effect', 'stage', 'name' (effect_variant_stage), 'variant', 'hash' (int)}] from src/renderer/shaders/fx/*.fx.json"""
    rows = []
    for fn in sorted(glob.glob(os.path.join(root, 'src', 'renderer', 'shaders', 'fx', '*.fx.json'))):
        try:
            with open(fn, encoding='utf-8') as f:
                d = json.load(f)
        except (OSError, ValueError):
            continue
        for stage in ('vertex', 'fragment'):
            for v in d.get(stage, []):
                for h in v.get('hashes', []):
                    try:
                        rows.append({'effect': d['effect'], 'stage': stage, 'variant': v['name'], 'name': '%s_%s_%s' % (d['effect'], v['name'], stage), 'hash': int(h, 16)})
                    except (KeyError, ValueError):
                        pass
    return rows


def resolve(rows, arg, effect=None):
    """HASH (prefix) or VARIANT -> the matching rows (one per effect / stage / variant / hash), duplicates folded"""
    a = arg.strip()
    pool = [r for r in rows if not effect or r['effect'].lower() == effect.lower()]
    if re.fullmatch(r'(?:0x)?[0-9A-Fa-f]{8,16}', a):
        p = a[2:].upper() if a[:2].lower() == '0x' else a.upper()
        hit = [r for r in pool if ('%016X' % r['hash']).startswith(p)]
    else:
        al = a.lower()
        hit = [r for r in pool if r['name'].lower() == al or r['variant'].lower() == al or ('%s_%s' % (r['effect'], r['variant'])).lower() == al]
        if not hit:
            hit = [r for r in pool if al in r['name'].lower()]
    seen, out = set(), []
    for r in hit:
        k = (r['effect'], r['name'], r['hash'])
        if k not in seen:
            seen.add(k)
            out.append(r)
    return out


# ------------------------------------------------------------------------------------------------ cache + disassembly
def cache_dir(root=None):
    env = os.environ.get('WWE_QSHADER_CACHE')
    if env:
        return env
    from qwords import _main_checkout
    return os.path.join(_main_checkout(root or ROOT), 'out', 'harness', 'shaderreads')


def cache_get(h, root=None):
    p = os.path.join(cache_dir(root), '%016X.json' % h)
    try:
        with open(p, encoding='utf-8') as f:
            d = json.load(f)
        return d if d.get('schema') == SCHEMA else None
    except (OSError, ValueError):
        return None


def cache_put(h, d, root=None):
    cd = cache_dir(root)
    os.makedirs(cd, exist_ok=True)
    with open(os.path.join(cd, '%016X.json' % h), 'w', encoding='utf-8') as f:
        json.dump(d, f)


def disassemble_many(effect, hashes, root=None):
    """-> {hash: (kind, listing text)} through the repo's own shader harness (check_effect_ucode scans the pacs, check_effect disassembles)"""
    sys.path.insert(0, os.path.join(root or ROOT, 'tools', 'shaders', 'harness'))
    try:
        import check_effect as ce
    except SystemExit as e:
        raise ShaderError(f'the shader harness cannot start: {e}')
    except ImportError as e:
        raise ShaderError(f'the shader harness cannot start: {e}')
    if not os.path.exists(ce.DISASM):
        raise ShaderError('missing %s: run tools\\shaders\\packets\\build_disasm.ps1' % ce.DISASM)
    try:
        got = ce.uc.get(effect, list(hashes))
    except (OSError, ValueError, KeyError) as e:
        raise ShaderError(f'{effect}: cannot read the pacs ({e!r})')
    out = {}
    for h in hashes:
        if h not in got:
            continue
        kind, code, _lits = got[h]
        try:
            out[h] = (kind, ce.disassemble(code, kind))
        except (SystemExit, RuntimeError) as e:
            raise ShaderError(f'the disassembler failed on {h:016X}: {e}')
    return out


def reads_of(rows, no_cache=False, root=None, disasm=disassemble_many):
    """rows (resolve's) -> [{'row', 'reads'}]; the cache first, one scan per effect for what is missing"""
    res, missing = {}, {}
    for r in rows:
        c = None if no_cache else cache_get(r['hash'], root)
        if c:
            res[r['hash']] = c
        else:
            missing.setdefault(r['effect'], {})[r['hash']] = r
    for effect, hs in missing.items():
        got = disasm(effect, list(hs), root)
        for h, (kind, text) in got.items():
            d = parse_reads(text)
            d.update({'schema': SCHEMA, 'hash': '%016X' % h, 'effect': effect, 'kind': kind})
            cache_put(h, d, root)
            res[h] = d
    out = []
    for r in rows:
        if r['hash'] not in res:
            raise ShaderError(f'{r["name"]}: hash {r["hash"]:016X} is in no pac of effect {r["effect"]} (the fx.json lists it; the player\'s files do not hold it)')
        out.append({'row': r, 'reads': res[r['hash']]})
    return out


def lanes_text(d):
    return ''.join(l for l in LANES if l in d)


def print_reads(item):
    r, d = item['row'], item['reads']
    print(f'== {r["name"]}  {d["hash"]}  ({d.get("kind", "?")}, effect {r["effect"]})')
    if d['consts']:
        print('constants read (c#.lanes  reads):')
        for reg, lanes in d['consts'].items():
            print(f'  c{reg:<4} .{lanes_text(lanes):<4}  ' + ' '.join(f'{l}={lanes[l]}' for l in LANES if l in lanes))
    else:
        print('constants read: none')
    for base, lanes in d['indexed'].items():
        print(f'  c[{base}] .{lanes_text(lanes):<4}  (indexed by the loop / address register)  ' + ' '.join(f'{l}={lanes[l]}' for l in LANES if l in lanes))
    if d['bools']:
        print('bool constants read: ' + ' '.join(f'b{b}' for b in d['bools']))
    if d['samplers']:
        print('samplers:')
        for s in d['samplers']:
            print(f'  tf{s["slot"]:<3} {s["kind"]:<5} x{s["n"]}  dest ' + ', '.join(s['dest']) + '  coord ' + ', '.join(s['coord']))
    if d.get('vfetch'):
        print('vertex fetch slots: ' + ' '.join(f'vf{v}' for v in d['vfetch']))


def main(argv=None, root=None, disasm=disassemble_many):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(prog='q shader', description=__doc__.split('\n\n')[0])
    ap.add_argument('target', help='HASH (16 hex, or a leading part of 8+) or VARIANT')
    ap.add_argument('--reads', action='store_true', help='the constant lanes / samplers the shader reads (the default)')
    ap.add_argument('--effect', default=None)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--all', action='store_true', help='every matching hash (default: 6)')
    ap.add_argument('--no-cache', action='store_true')
    ap.add_argument('--root', default=root)
    a = ap.parse_args(argv)
    root = a.root or ROOT
    try:
        rows = resolve(load_variants(root), a.target, a.effect)
        if not rows:
            raise ShaderError(f'{a.target}: no fx.json shader with that hash / variant name' + (f' in effect {a.effect}' if a.effect else '')
                              + ' (the names: src/renderer/shaders/fx/*.fx.json; `q ours text VARIANT --in src/renderer/shaders/fx`)')
        shown = rows if a.all else rows[:6]
        items = reads_of(shown, a.no_cache, root, disasm)
    except ShaderError as e:
        print(f'q shader: {e}', file=sys.stderr)
        return 2
    if a.json:
        print(json.dumps({'matches': len(rows), 'shaders': [{'name': i['row']['name'], 'effect': i['row']['effect'], **i['reads']} for i in items]}, indent=1))
        return 0
    for i in items:
        print_reads(i)
    if len(rows) > len(shown):
        print(f'(showing {len(shown)} of {len(rows)} matching shaders; all: q shader {a.target} --all' + (f' --effect {a.effect}' if a.effect else '') + ')')
    return 0


if __name__ == '__main__':
    sys.exit(main())
