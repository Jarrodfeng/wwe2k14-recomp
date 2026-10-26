"""Session 68: mechanical db entries for thunks and tiny functions, decoded from the instructions (no model).

    py tools\\labelling\\mechanical.py [--out FILE] [--list N]

Covers census.py's `thunk` and `trivial` buckets that match a known shape exactly (the rest stay for the labellers):
  b TARGET                          thunk_to_<target db name | sub_x>
  blr                               empty_function
  li r3,K                           return_K
  lwz/lhz/lha/lbz r3,N(r3)          get_u32/u16/s16/u8_field_N        (lfs/lfd f1: get_float/double_field_N)
  stw/sth/stb rS,N(r3)              set_u32/u16/u8_field_N            (stfs/stfd f1: set_float/double_field_N)
  addi r3,r3,N                      field_address_N
  lwz r11,N(r3); lwz r3,M(r11)      get_u32_M_of_pointer_field_N
  lis rX,HI; lwz r3,LO(rX)          get_global_u32_<address>          (address from the q index's lis pair)
  li r3,K; stw r3,N(r3') ...        not decoded (left for the labellers)
Each shape is followed by blr (a leaf that returns) and nothing else. Entries go to out\\ (staging): whether they
enter db\\ is Jarrod's call (CLAUDE.md wants two labels per function; these come from a decoder, not a model).
"""
import argparse
import collections
import json
import os
import re
import sqlite3
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import census  # noqa: E402

BLR = 0x4E800020
LOADS = {32: ('u32', 'r'), 40: ('u16', 'r'), 42: ('s16', 'r'), 34: ('u8', 'r'), 48: ('float', 'f'), 50: ('double', 'f')}
STORES = {36: ('u32', 'r'), 44: ('u16', 'r'), 38: ('u8', 'r'), 52: ('float', 'f'), 54: ('double', 'f')}


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def dform(w):
    return w >> 26, (w >> 21) & 31, (w >> 16) & 31, simm(w)


def decode(words, addr, globals_at):
    """(name, summary, evidence) for a recognised shape, else None. words exclude nothing: the function body."""
    if not words or words[-1] != BLR:
        return None
    body = words[:-1]
    if not body:
        return ('empty_function', 'Returns at once: no loads, stores or calls. Purpose unknown.',
                ['script decode: the body is a single blr'])
    if len(body) == 1:
        op, rt, ra, d = dform(body[0])
        if op == 14 and ra == 0 and rt == 3:
            k = f'minus_{-d}' if d < 0 else str(d)
            return (f'return_{k}', f'Returns the constant {d} in r3; no other effect. Purpose unknown.',
                    [f'script decode: li r3,{d} then blr'])
        if op in LOADS and ra == 3 and rt == (1 if LOADS[op][1] == 'f' else 3) and d >= 0:
            t, _ = LOADS[op]
            reg = 'f1' if LOADS[op][1] == 'f' else 'r3'
            return (f'get_{t}_field_{d}', f'Getter: returns the {t} at offset +{d} of the object in r3 (in {reg}). '
                    'Purpose unknown.', [f'script decode: one {t} load from r3+{d} into {reg}, then blr'])
        if op in STORES and ra == 3 and d >= 0:
            t, kind = STORES[op]
            src = f'f{rt}' if kind == 'f' else f'r{rt}'
            if (kind == 'f' and rt == 1) or (kind == 'r' and rt == 4):
                return (f'set_{t}_field_{d}', f'Setter: stores {src} as the {t} at offset +{d} of the object in r3. '
                        'Purpose unknown.', [f'script decode: one {t} store of {src} to r3+{d}, then blr'])
        if op == 14 and rt == 3 and ra == 3:
            nm = f'minus_{-d}' if d < 0 else str(d)
            return (f'field_address_{nm}', f'Returns the address r3+{d} (a member embedded at +{d}). Purpose unknown.',
                    [f'script decode: addi r3,r3,{d} then blr'])
    if len(body) == 2:
        op1, rt1, ra1, d1 = dform(body[0])
        op2, rt2, ra2, d2 = dform(body[1])
        if op1 == 32 and ra1 == 3 and rt1 != 3 and op2 == 32 and ra2 == rt1 and rt2 == 3 and d1 >= 0 and d2 >= 0:
            return (f'get_u32_{d2}_of_pointer_field_{d1}',
                    f'Loads the pointer at +{d1} of the object in r3 and returns the u32 at +{d2} of what it points '
                    'to. Purpose unknown.', [f'script decode: two chained u32 loads (+{d1}, then +{d2}), then blr'])
        if op1 == 15 and ra1 == 0 and op2 == 32 and ra2 == rt1 and rt2 == 3:
            g = globals_at.get(addr + 4)
            if g is not None:
                return (f'get_global_u32_{g:08x}', f'Returns the u32 global at 0x{g:08X}. Purpose unknown.',
                        [f'script decode: lis pair loads the word at 0x{g:08X} into r3, then blr'])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(census.REPO, 'out', 'harness', 'labelling', 's68', 'mechanical.json'))
    ap.add_argument('--list', type=int, default=0, help='print N sample entries')
    args = ap.parse_args()

    con = sqlite3.connect(census.INDEX)
    funcs = {a: (s, n) for a, s, n in con.execute('select addr, size, insns from funcs where has_body = 1')}
    with open(census.IMAGE, 'rb') as f:
        img = f.read()
    labelled = census.db_addresses() & funcs.keys()
    libs = census.library_ranges()
    names, systems = {}, {}
    for p in __import__('glob').glob(os.path.join(census.REPO, 'db', '*.json')):
        with open(p, encoding='utf-8') as f:
            for e in json.load(f):
                try:
                    a = int(e['address'], 16)
                except (KeyError, ValueError, TypeError):
                    continue
                names[a], systems[a] = e.get('name'), e.get('system', 'other')
    globals_at = {site: v for site, v in con.execute("select site, value from imms where op = 'lis+lwz'")}
    tails = collections.defaultdict(list)
    calls = collections.defaultdict(int)
    for src, dst, kind in con.execute('select src, dst, kind from edges'):
        if kind == 'tail':
            tails[src].append(dst)
        elif kind == 'call':
            calls[src] += 1

    out, shapes, skipped = [], collections.Counter(), collections.Counter()
    for a, (size, n) in sorted(funcs.items()):
        if a in labelled or any(s <= a < e for s, e, _ in libs):
            continue
        words = list(struct.unpack('>%dI' % (size // 4), img[a - census.BASE:a - census.BASE + size]))
        if n == 1 and len(tails.get(a, [])) == 1 and words and words[0] >> 26 == 18 and not words[0] & 1:
            t = tails[a][0]
            tn = names.get(t)
            name = f'thunk_to_{tn}' if tn else f'thunk_to_sub_{t:08x}'
            entry = dict(address=f'0x{a:08X}', name=name,
                         summary=f'One-instruction thunk: branches to sub_{t:08X}' + (f' ({tn})' if tn else '') +
                         ' with the arguments unchanged.',
                         evidence=['script decode: a single unconditional branch, no link',
                                   f'target sub_{t:08X}' + (f' (db: {tn})' if tn else ' (no db entry)')],
                         confidence='high', system=systems.get(t, 'other') if tn else 'other')
            shapes['thunk'] += 1
        elif n <= 4 and calls.get(a, 0) == 0 and not tails.get(a):
            d = decode(words, a, globals_at)
            if not d:
                skipped['trivial, shape not decoded'] += 1
                continue
            name, summary, ev = d
            entry = dict(address=f'0x{a:08X}', name=name, summary=summary,
                         evidence=ev + [f'{n} instructions, no calls (q index)'], confidence='high', system='other')
            shapes[re.sub(r'_(minus_)?[0-9a-f]+(_|$).*', '', name)] += 1
        else:
            continue
        entry['evidence'].append('labels: mechanical decode (script, no model)')
        entry['source'] = 'tools/labelling/mechanical.py (script decode), 2026-10-26'
        out.append(entry)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=1)
        f.write('\n')
    print(f'{len(out):,} mechanical entries -> {args.out}')
    print('shapes:', ', '.join(f'{k} {v:,}' for k, v in shapes.most_common()))
    print('skipped:', ', '.join(f'{k} {v:,}' for k, v in skipped.most_common()))
    for e in out[:args.list]:
        print(' ', e['address'], e['name'])


if __name__ == '__main__':
    sys.exit(main())
