r"""Self-test of `q codediff --field levels` (S111 LEVELSDIFF): `py -I tools/q/qlevels_selftest.py`   (0 game runs)

A. Planted families: invented game call events (the shape qlevels.events_of makes from a decomp) and an invented src tree (a dict
   reader), through the production path in its order: ours_facts -> upload_rule -> lists_of -> resolve_inherited -> run. Each verdict:
   OK (a constant both sides, per name both sides, the whole chain both sides, a rule of ours citing the builder of a non-constant
   count, a key >= 100 pairing an uncited directory), DIFF (another constant), GAME-ONLY (the file's chain; a 0 arm of a rule; a cube
   list the upload rule refuses; an unpaired list), UNSETTLED (an inherited count), OURS-ONLY; the count in force through a restore,
   a per-name lookup in a loop and a caller one level up; keys narrowing a cited pair.
   Planted twins (WWE_QLEVELS_PLANT), each seen to FAIL its check: no_cube, no_keys, no_inherit, no_unpaired, no_paths, no_table.
B. Real re-finds (our code at the parent refs of this week's fixes, the game's decomps; SKIP without the refs): each family flagged
   (DIFF / GAME-ONLY) at its parent and OK at the tip: MIPLEVELS 5c1ebed73^1 (the blood list sub_824ECAD8), PROPLEVELS f4e993cab^1
   (the weapon set sub_8258E270), CUBELEVELS cf40b67a3^1 (cubes refused: the prop cube list sub_8272D710), CHCOMMONCUBE 1ba795d87^1
   (sub_8271CB50), STAGELEVELS 3c54efabc^1 (sub_82577300).
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qlevels as L   # noqa: E402

FAILS, MISSED = [], []
REFINDS = [   # (name, fix merge, game fn, list address)
    ('MIPLEVELS', '5c1ebed73', 'sub_824ECAD8', '0x824ECF60'),
    ('PROPLEVELS', 'f4e993cab', 'sub_8258E270', '0x8258E5DC'),
    ('CUBELEVELS', 'cf40b67a3', 'sub_8272D710', '0x8272DBB8'),
    ('CHCOMMONCUBE', '1ba795d87', 'sub_8271CB50', '0x8271CD3C'),
    ('STAGELEVELS', '3c54efabc', 'sub_82577300', '0x82577500'),
]


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, caught, detail=''):
    if not caught:
        MISSED.append(name)
        print(f'NOT CAUGHT planted twin: {name} {detail}')
    else:
        print(f'ok   planted twin caught: {name}')


# ------------------------------------------------------------------ A: the planted tree
RELEVEL_OLD = '\n'.join([
    'inline bool Relevel(const assets::Texture& t, assets::Texture& out) {',
    '  if (t.game_levels == 0 || t.faces != 1 || t.width == 0 || t.format == assets::TexFormat::kUnknown) return false;',
    '  return true;',
    '}'])
RELEVEL_NEW = '\n'.join([
    'inline bool Relevel(const assets::Texture& t, assets::Texture& out) {',
    '  const bool cube = t.faces == 6 && t.width == t.height;',
    '  if (t.game_levels == 0 || (t.faces != 1 && !cube) || t.width == 0 || t.format == assets::TexFormat::kUnknown) return false;',
    '  return true;',
    '}'])
SYN = '\n'.join([
    '// syn loaders (the selftest tree)',                                                          # 1
    '#pragma once',                                                                                # 2
    'inline constexpr uint32_t kSynLevels = 3;   // sub_82000100 +0x10, sub_82000300 +0x10',        # 3
    'inline constexpr uint32_t kFive = 5;',                                                        # 4
    'inline constexpr uint32_t kWhole = 32;',                                                      # 5
    'inline constexpr uint32_t kKey0 = 0;',                                                        # 6
    '// the key-0 count of sub_82000C00 +0x1EC',                                                   # 7
    'inline constexpr uint32_t ListRule(int type) { return type == 3 ? 7u : 4u; }',                 # 8
    '// the old character rule: sub_82000E00 builds at 3',                                         # 9
    'inline uint32_t OldLevels(bool c, uint32_t n) { return c ? n : 0u; }',                         # 10
    '// sub_82000100 builds key 20 at 3',                                                          # 11
    'inline bool AddOne(TexSet& tex, Bytes&& d, uint32_t key) {',                                  # 12
    '  if (key == 20) return tex.AddDirLevels(std::move(d), kSynLevels);',                         # 13
    '  return false;',                                                                             # 14
    '}',                                                                                           # 15
    '// sub_82000200 appends this list',                                                           # 16
    'inline bool AddTwo(TexSet& tex, Bytes&& d) { return tex.AddDir(std::move(d)); }',             # 17
    '// sub_82000300 appends the cube list',                                                       # 18
    'inline bool AddCube(TexSet& tex, Bytes&& d) { return tex.AddDirLevels(std::move(d), kSynLevels); }',   # 19
    '// sub_82000400 builds per name, then appends at 3',                                          # 20
    'inline bool AddNamed(TexSet& tex, Bytes&& d, const LevelsOf& f) { return tex.AddDirNamed(std::move(d), f); }',   # 21
    '// sub_82000400 appends its second list at 3',                                                # 22
    'inline bool AddFlat(TexSet& tex, Bytes&& d) { return tex.AddDirLevels(std::move(d), 3u); }',   # 23
    '// sub_82000500 appends at the caller\'s count',                                              # 24
    'inline bool AddInh(TexSet& tex, Bytes&& d) { return tex.AddDir(std::move(d)); }',             # 25
    '// sub_82000A00 appends key 30',                                                              # 26
    'inline bool AddKeyed(TexSet& tex, Bytes&& d, uint32_t key) {',                                # 27
    '  if (key == 40) return tex.AddDir(std::move(d));',                                           # 28
    '  return false;',                                                                             # 29
    '}',                                                                                           # 30
    'inline bool AddBig(TexSet& tex, const Bytes& buf, const pac::Pach& h) {',                     # 31
    '  const Bytes d = pac::Get(buf, h, 32010);',                                                  # 32
    '  return tex.AddDirLevels(Bytes(d), kSynLevels);',                                            # 33
    '}',                                                                                           # 34
    '// sub_82000C00 builds the set: key 0 by the holder, key 10 the whole chain',                 # 35
    'inline int AddSet(TexSet& tex, int type) {',                                                  # 36
    '  int added = 0;',                                                                            # 37
    '  const auto add = [&](uint32_t key, uint32_t levels) {',                                     # 38
    '    if (tex.AddDirLevels(Bytes(), levels)) ++added;',                                         # 39
    '  };',                                                                                        # 40
    '  add(kKey0, ListRule(type));',                                                               # 41
    '  add(10, kWhole);',                                                                          # 42
    '  return added;',                                                                             # 43
    '}',                                                                                           # 44
    '// sub_82000E00 builds the old character list',                                               # 45
    'inline bool AddOld(TexSet& tex, Bytes&& d, bool c, uint32_t n) { return tex.AddDirLevels(std::move(d), OldLevels(c, n)); }',   # 46
    '// sub_82000F00 builds at 3; ours says 5',                                                    # 47
    'inline bool AddFive(TexSet& tex, Bytes&& d) { return tex.AddDirLevels(std::move(d), kFive); }',   # 48
    'inline bool AddLone(TexSet& tex, Bytes&& d) { return tex.AddDirLevels(std::move(d), 6u); }',   # 49
    ''])


def ev(off, kind, path=(), tgt=0, regs=None, val=None):
    e = {'off': off, 'path': tuple(path), 'kind': kind, 'tgt': tgt, 'regs': regs or {}, 'line': ''}
    if kind == 'set':
        e['tgt'], e['val'] = L.SETTER, val
    elif kind == 'get':
        e['tgt'] = L.GETTER
    elif kind == 'find':
        e['tgt'] = 0x82C351E8
    elif kind == 'pername':
        e['tgt'] = 0x827FDB78
    return e


def app(off, kind=0, path=(), tgt=0x82BA9D68):
    return ev(off, 'append', path, tgt, {L.APPENDS[tgt]: str(kind)})


def game_events():
    """{fn: events}: one family per case"""
    return {
        0x82000100: [ev(0x10, 'set', val=3), ev(0x14, 'find', regs={'r4': '20'}), app(0x20)],
        0x82000200: [ev(0x10, 'set', val=4), app(0x20)],
        0x82000300: [ev(0x10, 'set', val=3), app(0x20, kind=1)],
        0x82000400: [ev(0x08, 'get'), ev(0x10, 'set', val=3), ev(0x14, 'get'),
                     ev(0x20, 'pername', path=(1, 2)), app(0x30, path=(1,), tgt=0x82BA9EF8),
                     ev(0x34, 'set', path=(1,), val=('ret', 0x14)), app(0x40, kind=1), ev(0x44, 'set', val=('ret', 0x08))],
        0x82000500: [app(0x20)],
        0x82000700: [app(0x20)],
        0x82000800: [ev(0x10, 'set', val=4), ev(0x18, 'call', tgt=0x82000700)],
        0x82000900: [ev(0x10, 'set', val=2), app(0x20)],
        0x82000A00: [ev(0x10, 'set', val=3), ev(0x14, 'find', regs={'r4': '30'}), app(0x20)],
        0x82000B00: [ev(0x10, 'set', val=3), ev(0x14, 'find', regs={'r4': '32010'}), app(0x20)],
        0x82000C00: [ev(0x10, 'set', val=('reg', '[in_r3+176]')), app(0x20), ev(0x24, 'set', val=0), app(0x30, kind=1)],
        0x82000E00: [ev(0x10, 'set', val=3), app(0x20)],
        0x82000F00: [ev(0x10, 'set', val=3), app(0x20)],
        0x82000D00: [ev(0x08, 'get'), ev(0x10, 'set', val=5), app(0x18), ev(0x1C, 'set', val=('ret', 0x08)), app(0x20)],
    }


def planted(relevel=RELEVEL_NEW):
    tree = L.DictTree({'src/assets/syn_levels.h': SYN, 'src/renderer_scene/game_levels.h': relevel})
    facts, problems = L.ours_facts(tree)
    rule = L.upload_rule(tree)
    evs = game_events()
    lists = []
    for f in sorted(evs):
        if any(e['kind'] == 'append' for e in evs[f]):
            lists += L.lists_of(f, evs[f])
    callers = {0x82000700: [0x82000800]}
    L.resolve_inherited(lists, evs, lambda f: callers.get(f, []))
    rows = L.run(lists, facts, rule)
    return rows, lists, facts, problems, rule


def by(rows, game, at=None):
    out = [r for r in rows if r['game'] == game and (at is None or (r['list'] or {}).get('at') == at)]
    return out[0] if out else None


def vals(lists, game, at):
    li = next((x for x in lists if x['fn'] == game and x['at'] == at), None)
    return li['vals'] if li else None


def part_a():
    rows, lists, facts, problems, rule = planted()
    check('A0 no problem reading the planted tree', not problems, str(problems))
    check('A0 the cube-aware Relevel refuses no kind', rule['present'] and not rule['refused'], rule['why'])
    r = by(rows, 'sub_82000100')
    check('A1 a constant both sides: OK', r and r['verdict'] == 'OK' and r['fn'] == 'AddOne', r and (r['verdict'], r['fn'], r['why']))
    r = by(rows, 'sub_82000F00')
    check('A2 another constant (game 3, ours kFive 5): DIFF', r and r['verdict'] == 'DIFF', r and (r['verdict'], r['why']))
    r = by(rows, 'sub_82000200')
    check('A3 the game counts, ours AddDir (the file\'s chain): GAME-ONLY', r and r['verdict'] == 'GAME-ONLY' and r['fn'] == 'AddTwo', r and (r['verdict'], r['fn']))
    r = by(rows, 'sub_82000300')
    check('A4 a cube list at 3, ours 3, the rule takes cubes: OK', r and r['verdict'] == 'OK', r and (r['verdict'], r['why']))
    check('A5 per-name lookup in the loop: the element build holds 3 and per-name', vals(lists, 'sub_82000400', '0x82000430') == {3, L.TABLE},
          str(vals(lists, 'sub_82000400', '0x82000430')))
    check('A5 after the loop\'s restore the list append holds 3 only', vals(lists, 'sub_82000400', '0x82000440') == {3},
          str(vals(lists, 'sub_82000400', '0x82000440')))
    r = by(rows, 'sub_82000400', '0x82000430')
    check('A5 per name both sides: OK', r and r['verdict'] == 'OK' and r['fn'] == 'AddNamed', r and (r['verdict'], r['fn'], r['why']))
    r = by(rows, 'sub_82000400', '0x82000440')
    check('A5 the flat append beside it: OK by the flat directory', r and r['verdict'] == 'OK' and r['fn'] == 'AddFlat', r and (r['verdict'], r['fn']))
    r = by(rows, 'sub_82000500')
    check('A6 no set, no caller read: UNSETTLED (inherited)', r and r['verdict'] == 'UNSETTLED' and 'inherited' in r['why'], r and (r['verdict'], r['why']))
    check('A6 an inherited count taken from the caller one level up', vals(lists, 'sub_82000700', '0x82000720') == {4}, str(vals(lists, 'sub_82000700', '0x82000720')))
    r = by(rows, 'sub_82000900')
    check('A7 an unpaired counted list: GAME-ONLY, no ours', r and r['verdict'] == 'GAME-ONLY' and r['fn'] == '-', r and (r['verdict'], r['fn']))
    r = by(rows, 'sub_82000A00')
    check('A8 a cited directory whose keys share none with the list does not pair', r and r['fn'] == '-' and r['verdict'] == 'GAME-ONLY', r and (r['verdict'], r['fn']))
    r = by(rows, 'sub_82000B00')
    check('A9 an uncited directory paired by its key 32010: OK', r and r['verdict'] == 'OK' and r['fn'] == 'AddBig' and r['pair'] == ['key 32010'],
          r and (r['verdict'], r['fn'], r['pair']))
    r = by(rows, 'sub_82000C00', '0x82000C20')
    check('A10 a lambda\'s count: the rule ListRule citing the builder vs the holder\'s register: OK', r and r['verdict'] == 'OK' and '41' in r['ours'][0]['at'],
          r and (r['verdict'], r['ours'], r['why']))
    r = by(rows, 'sub_82000C00', '0x82000C30')
    check('A11 the whole chain both sides (game 0, ours kWhole 32): OK', r and r['verdict'] == 'OK', r and (r['verdict'], r['ours'], r['why']))
    r = by(rows, 'sub_82000E00')
    check('A12 a rule with a 0 arm against the game\'s 3: GAME-ONLY', r and r['verdict'] == 'GAME-ONLY' and 'arm' in r['why'], r and (r['verdict'], r['why']))
    check('A13 a set, an append, a restore of the save: the second append is inherited',
          vals(lists, 'sub_82000D00', '0x82000D18') == {5} and vals(lists, 'sub_82000D00', '0x82000D20') == {L.INHERIT},
          f"{vals(lists, 'sub_82000D00', '0x82000D18')} {vals(lists, 'sub_82000D00', '0x82000D20')}")
    lone = [r for r in rows if r['verdict'] == 'OURS-ONLY' and r['fn'] == 'AddLone']
    check('A14 a counted directory pairing nothing: OURS-ONLY', len(lone) == 1, str([(r['fn'], r['verdict']) for r in rows if r['verdict'] == 'OURS-ONLY']))
    # the old upload rule: a cube is refused whatever its count
    rows_old, _l, _f, _p, rule_old = planted(RELEVEL_OLD)
    check('A15 the old Relevel (faces != 1) refuses the cube kind', 1 in rule_old['refused'] and 0 not in rule_old['refused'], rule_old['why'])
    r = by(rows_old, 'sub_82000300')
    check('A15 a cube list ours counts but the upload refuses: GAME-ONLY', r and r['verdict'] == 'GAME-ONLY' and 'refuses' in r['why'], r and (r['verdict'], r['why']))
    rows_none = L.run(*planted_parts(None))
    check('A16 no upload rule in the tree: every counted list GAME-ONLY', by(rows_none, 'sub_82000100')['verdict'] == 'GAME-ONLY')


def planted_parts(relevel):
    files = {'src/assets/syn_levels.h': SYN}
    if relevel:
        files['src/renderer_scene/game_levels.h'] = relevel
    tree = L.DictTree(files)
    facts, _p = L.ours_facts(tree)
    evs = game_events()
    lists = []
    for f in sorted(evs):
        if any(e['kind'] == 'append' for e in evs[f]):
            lists += L.lists_of(f, evs[f])
    L.resolve_inherited(lists, evs, lambda f: [])
    return lists, facts, L.upload_rule(tree)


def with_plant(name, fn):
    old = L.PLANT
    L.PLANT = name
    try:
        return fn()
    finally:
        L.PLANT = old


def part_twins():
    r = with_plant('no_cube', lambda: by(planted(RELEVEL_OLD)[0], 'sub_82000300'))
    twin('no_cube (the upload rule\'s cube refusal ignored)', r and r['verdict'] != 'GAME-ONLY', r and r['verdict'])
    r = with_plant('no_keys', lambda: by(planted()[0], 'sub_82000A00'))
    twin('no_keys (keys never narrow a pair)', r and r['fn'] != '-', r and r['fn'])
    r = with_plant('no_inherit', lambda: by(planted()[0], 'sub_82000500'))
    twin('no_inherit (an inherited count read as set)', r and r['verdict'] != 'UNSETTLED', r and r['verdict'])
    r = with_plant('no_unpaired', lambda: by(planted()[0], 'sub_82000900'))
    twin('no_unpaired (an unpaired list not reported)', r is None, r and r['verdict'])
    v = with_plant('no_paths', lambda: vals(planted()[1], 'sub_82000400', '0x82000440'))
    twin('no_paths (every set adds)', v != {3}, str(v))
    v = with_plant('no_table', lambda: vals(planted()[1], 'sub_82000400', '0x82000430'))
    twin('no_table (the per-name lookup not read)', L.TABLE not in (v or set()), str(v))


# ------------------------------------------------------------------ B: the real re-finds
def have(ref):
    return subprocess.run(['git', 'cat-file', '-e', ref + '^{commit}'], cwd=L.REPO, capture_output=True).returncode == 0


def part_b():
    try:
        db = L.GameDb()
    except Exception as e:  # noqa: BLE001 - no q db: the real half is skipped and said
        print(f'SKIP B: the q db is not readable ({type(e).__name__}: {e})')
        return
    tip, problems, _info = L.collect(None, db)
    check('B0 the working tree reads with no problem', not problems, str(problems[:2]))
    bad = [r for r in tip if r['verdict'] in ('DIFF', 'GAME-ONLY')]
    for name, fix, game, at in REFINDS:
        if not have(fix):
            print(f'SKIP B {name}: {fix} is not in this repository')
            continue
        rows, p, _i = L.collect(fix + '^1', db)
        r = by(rows, game, at)
        check(f'B {name} at {fix}^1: {game} list {at} flagged', r and r['verdict'] in ('DIFF', 'GAME-ONLY'), r and (r['verdict'], r['fn'], r['why']))
        t = by(tip, game, at)
        check(f'B {name} at the tip: {game} list {at} OK', t and t['verdict'] == 'OK', t and (t['verdict'], t['fn'], t['why']))
    cube = by(L.collect('cf40b67a3^1', db)[0], 'sub_8272D710', '0x8272DBB8') if have('cf40b67a3') else None
    if cube:
        check('B CUBELEVELS: the reason is the upload rule refusing a cube', 'refuses a cube' in cube['why'], cube['why'])
    check('B the tip has no levels DIFF / GAME-ONLY', not bad, '; '.join(f"{r['verdict']} {r['fn']} vs {r['game']} {(r['list'] or {}).get('at')}" for r in bad[:4]))


def main():
    part_a()
    part_twins()
    if '--planted-only' not in sys.argv:
        part_b()
    if MISSED:
        print(f'qlevels selftest: {len(MISSED)} planted twin(s) NOT caught: {", ".join(MISSED)}')
        return 2
    if FAILS:
        print(f'qlevels selftest: {len(FAILS)} check(s) failed')
        return 1
    print('qlevels selftest: all checks pass, every planted twin caught')
    return 0


if __name__ == '__main__':
    sys.exit(main())
