r"""Self-test of `q fieldsdiff` (S111 FIELDSDIFF): `py -I tools/q/qfieldsdiff_selftest.py`   (0 game runs)

A. The game side (qfieldsdiff.game_reads) on an invented decomp body in the placement loader's shape: a whole-buffer memcpy (the
   destination an alias of in_r4), a 4-byte memcpy into a stack local, a 2-byte local copy, the count's bytes swapped through two more
   locals, an extsh gate and a byte gate; plus a direct field read used as a call argument. Expected: in_r4+0 u8 gate, in_r4+2 s16 gate
   (byte-reversed), in_r4+1 u8 copy, in_r3+16 u32 arg, and no read for the shuffle's intermediate byte loads.
B. The ours side (qfieldsdiff.ours_reads) on invented C++: a signed reader in int16_t(...), a byte subscript, a reader with a guest
   base + a constexpr offset, Be32(b + 0x18), ReadWord(&f, obj + k, v), a cast load, a global word K, a pointer from a global word
   ([0x83930050]+80), a subscript store (not a read), a reader's own definition (not a call), and a loop index `i` that some src
   file declares constexpr (the tree-wide index answers constant-shaped names only: planted twin `any name`).
C. The join (bind + judge) and the whole path (collect over a fixture tree, an invented game side and invented q pairs rows):
   a record row binds a function with no read at all (every game read GAME-ONLY READ); an overlap of 2 binds and shows the missed
   field; an overlap of 1 binds nothing; two game bases that tie bind nothing; WIDTH (signed vs unsigned); OURS-ONLY READ; the rank
   (GAME-ONLY gate rows first).
   Planted twins, each seen to FAIL its check: no copy-through (the gate reads vanish), no sign (the count turns WIDTH), bind any (an
   overlap of 0 / 1 binds).
D. Real re-find (our code at frozen refs, the game index; SKIP without them): PLACECOUNT0. At fd832a342^1 our
   assets::arena_props::PlacementIds never reads block 1's byte 0 / signed count that sub_8258F698 gates on (GAME-ONLY READ +0 / +2,
   gate); at fd832a342 both are OK.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import collections
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfieldsdiff as F   # noqa: E402

FAILS, CAUGHT = [], []
PARENT, FIX = 'fd832a342^1', 'fd832a342'


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, caught, detail=''):
    if not caught:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name} {detail}')
    else:
        print(f'ok   planted twin caught: {name}')


def planted(p, fn, *a):
    old, F.PLANT = F.PLANT, p
    try:
        return fn(*a)
    finally:
        F.PLANT = old


MEMCPY = {'82C2F7B8'}
GAME_LINES = [
    '@8258F71C  [listing :1]',
    '  r3@8258F71C = one of: @8258F704 ret#70, @8258F710 ret#80',
    '  +90  call#90 sub_82C2F7B8(r3=r3@8258F71C, r4=in_r4, r5=in_r5)',
    'if (r3@8258F71C != 0) {   // +BC, skips to @8258FDF8',
    '    +CC  call#CC sub_82C2F7B8(r3=sp+88, r4=r3@8258F71C, r5=4)',
    '    +D8  local[sp+86] = 0  (u16)',
    '    +E8  call#E8 sub_82C2F7B8(r3=sp+86, r4=sp+90, r5=2)',
    '    +EC  r11 = u8 local[sp+87]',
    '    +F0  r10 = u8 local[sp+86]',
    '    +F4  r9 = u8 local[sp+88]',
    '    +F8  r8 = u8 local[sp+89]',
    '    +FC  local[sp+80] = L87@+EC  (u8)',
    '    +100  local[sp+81] = L86@+F0  (u8)',
    '    +104  r10 = u16 local[sp+80]',
    '    +110  [in_r3+6] = L88@+F4  (u8)',
    '    +114  [in_r3+7] = L89@+F8  (u8)',
    '    +118  [in_r3+4] = L80@+104  (u16)',
    '  if (extsh((L80@+104 & 65535)) > 0) {   // +124, skips to @8258FDEC',
    '    if ((L88@+F4 & 255) == 1) {   // +130, skips to @8258FDEC',
    '        +140  r4 = u32 [in_r3+16]',
    '        +150  call#150 sub_82252758(r3=[in_r3+16], r4=8)',
    '    }',
    '  }',
    '}',
]
GAME_SUMMARY = {'reads': [{'addr': 'in_r3+16', 'w': 'u32', 'n': 1}]}


def pick(rows, base, off, **kw):
    return [r for r in rows if r['base'] == base and r['off'] == off and all(r.get(k) == v for k, v in kw.items())]


def part_a():
    g = F.game_reads(GAME_LINES, GAME_SUMMARY, MEMCPY)
    check('A byte 0 of the file (in_r4 through the whole-buffer copy) is a u8 gate read', pick(g, 'in_r4', 0, n=1, k='u', use='gate'), str(g))
    c = pick(g, 'in_r4', 2, n=2)
    check('A the count (bytes 3, 2 through two locals) is an s16 gate read, byte-reversed', c and c[0]['k'] == 's' and c[0]['use'] == 'gate'
          and 'byte-reversed' in c[0]['via'], str(c))
    check('A byte 1 is a u8 read stored on (copy)', pick(g, 'in_r4', 1, n=1, use='copy'), str(g))
    check('A the shuffle\'s intermediate byte loads are not reads (only moved into a local)', not pick(g, 'in_r4', 3) and not pick(g, 'in_r4', 2, n=1), str(g))
    check('A a direct field read passed to a call is an arg read', pick(g, 'in_r3', 16, n=4, use='arg'), str(g))
    g0 = planted('no_copy_through', F.game_reads, GAME_LINES, GAME_SUMMARY, MEMCPY)
    twin('A no copy-through: the gate reads vanish', not pick(g0, 'in_r4', 0) and not pick(g0, 'in_r4', 2))
    g1 = planted('no_sign', F.game_reads, GAME_LINES, GAME_SUMMARY, MEMCPY)
    twin('A no sign: the count reads unsigned', pick(g1, 'in_r4', 2, n=2, k='u'))


OURS_CPP = '\n'.join([
    'inline uint32_t Rd32(const uint8_t* base, uint32_t a) { return 0; }',      # 1  a definition (its own body is not this function)
    'constexpr uint32_t kOffCount = 0x7C;',                                     # 2
    'bool Syn::Parse(const std::vector<uint8_t>& b, uint8_t* base, const uint8_t* h, uint32_t mgr, uint32_t obj) {',   # 3
    '  if (int16_t(Rd16(b, 2)) <= 0 || b[0] != 0x01) return false;',          # 4
    '  const uint32_t n = Rd32(base, mgr + kOffCount);',                        # 5
    '  const uint32_t t = Be32(h + 0x18);',                                     # 6
    '  uint32_t v = 0; ReadWord(&ReadGuestNoNote, obj + 8, v); ReadWord(read, mgr, v);',   # 7
    '  const float s = *(const float*)(h + 12);',                               # 8
    '  const uint32_t w = Rd32(base, 0x83763038);',                             # 9
    '  const uint32_t p = Rd32(base, Rd32(base, 0x83930050) + 80);',            # 10
    '  b2[3] = 1;',                                                             # 11 (b2 is no byte buffer here; and a store)
    '  h2[4] = 2;',                                                             # 12
    '  const uint32_t e = Be32(base, obj + i * 68 + 12);',                      # 13 a loop index (a tree-wide `i` must not resolve)
    '  return n + t + v + w + p + e > 0 && s > 0.f;',                           # 14
    '}', ''])


class TreeConsts(F.Consts):
    """a tree-wide constant index holding a stray `i` (some src file's constexpr) and a real k-constant"""
    def all(self):
        return {'i': 3, 'kStray': 5}


def part_b():
    loc = F.Consts.scan(F.mask(OURS_CPP))
    body = F.mask('\n'.join(OURS_CPP.split('\n')[2:15]))
    o = F.ours_reads(body, 3, loc, TreeConsts(tempfile.gettempdir()))
    check('B a signed reader: Rd16 in int16_t(...) = b+2 s16', pick(o, 'b', 2, n=2, k='s'), str(o))
    check('B a byte subscript: b[0] = b+0 u8', pick(o, 'b', 0, n=1, k='u'), str(o))
    check('B a guest base + a constexpr offset: mgr+0x7C u32', pick(o, 'mgr', 0x7C, n=4), str(o))
    check('B Be32(h + 0x18) = h+24 u32', pick(o, 'h', 24, n=4), str(o))
    check('B ReadWord(&f, obj + 8, v) = obj+8 u32', pick(o, 'obj', 8, n=4), str(o))
    check('B ReadWord(read, mgr, v) = mgr+0 u32 (the address itself after the memory argument)', pick(o, 'mgr', 0, n=4), str(o))
    check('B a cast load *(const float*)(h + 12) = h+12 f32', pick(o, 'h', 12, n=4, k='f'), str(o))
    check('B a global word = K+0x83763038', pick(o, 'K', 0x83763038), str(o))
    check('B a pointer from a global word = [0x83930050]+80', pick(o, '[0x83930050]', 80), str(o))
    check('B a subscript store and an unknown buffer give no read', not [r for r in o if r['base'] in ('b2', 'h2')], str(o))
    check('B every read is on its own line', sorted({r['at'] for r in o}) == [4, 5, 6, 7, 8, 9, 10], str(sorted({r['at'] for r in o})))
    check('B a loop index is no constant (obj + i * 68 + 12 is not obj+12)', not pick(o, 'obj', 12) and not pick(o, 'obj', 216), str(o))
    old = F.CONST_NAME_RE
    F.CONST_NAME_RE = F.re.compile(r'\w+')
    try:
        o2 = F.ours_reads(body, 3, loc, TreeConsts(tempfile.gettempdir()))
    finally:
        F.CONST_NAME_RE = old
    twin('B any name from the tree-wide index: the loop index resolves (obj+216)', pick(o2, 'obj', 216))


SRC = {
    'src/syn/a.h': '\n'.join([
        'namespace syn {',
        'inline bool Gate(const std::vector<uint8_t>& b) {',       # no read: a record binds it
        '  return b.size() > 4;',
        '}',
        'inline uint32_t Walk(uint8_t* base, uint32_t m) {',
        '  return Rd32(base, m + 8) + Rd32(base, m + 12) + Rd16(base, m + 20);',      # +8, +12 match; +16 missed; +20 u16 vs the game's s16
        '}',
        'inline uint32_t One(uint8_t* base, uint32_t m) {',
        '  return Rd32(base, m + 8);',                             # one offset in common: no bind
        '}',
        'inline uint32_t Tie(uint8_t* base, uint32_t m) {',
        '  return Rd32(base, m + 4) + Rd32(base, m + 8) + Rd32(base, m + 40);',       # +4 / +8 in both in_r3 and in_r5: a tie
        '}',
        '}', '']),
}


class FakeGame:
    def __init__(self, reads):
        self.r = reads

    def reads(self, g):
        return self.r.get(g)


def gr(base, off, n, k='u', use='read'):
    return {'base': base, 'off': off, 'n': n, 'k': k, 'use': use, 'at': f'+{off:X}', 'via': 'field'}


GREADS = {
    'sub_83000100': [gr('in_r4', 0, 1, use='gate'), gr('in_r4', 2, 2, 's', 'gate')],
    'sub_83000200': [gr('in_r3', 8, 4), gr('in_r3', 12, 4), gr('in_r3', 16, 4, use='arg'), gr('in_r3', 20, 2, 's'), gr('in_r3', 99, 4)],
    'sub_83000300': [gr('in_r3', 8, 4), gr('in_r3', 16, 4, use='gate')],
    'sub_83000400': [gr('in_r3', 4, 4), gr('in_r3', 8, 4), gr('in_r5', 4, 4), gr('in_r5', 8, 4)],
}
RECS = [{'name': 'syn_file', 'game': [{'fn': 'sub_83000100', 'base': 'in_r4'}], 'ours': [{'fn': 'syn::Gate', 'base': 'b'}],
         'fields': {'0': 'byte 0', '2': 'count'}}]


def pairs():
    p = collections.defaultdict(lambda: collections.defaultdict(list))
    p[('src/syn/a.h', 'syn::Walk')]['sub_83000200'].append('comment src/syn/a.h:5')
    p[('src/syn/a.h', 'syn::One')]['sub_83000300'].append('comment src/syn/a.h:8')
    p[('src/syn/a.h', 'syn::Tie')]['sub_83000400'].append('comment src/syn/a.h:11')
    return p


def part_c(root):
    def run():
        tree = F.Tree(root)
        return F.collect(None, game=FakeGame(GREADS), records=RECS, pairs=pairs(), tree=tree)[0]
    rows = run()

    def v(rs, fn, off):
        x = [r for r in rs if r['fn'] == fn and r['off'] == off]
        return x[0]['verdict'] if len(x) == 1 else [r['verdict'] for r in x]
    check('C a record binds a function with no read: byte 0 / the count are GAME-ONLY READ (gate)',
          v(rows, 'syn::Gate', 0) == 'GAME-ONLY READ' and v(rows, 'syn::Gate', 2) == 'GAME-ONLY READ', str([r for r in rows if r['fn'] == 'syn::Gate']))
    w = [r for r in rows if r['fn'] == 'syn::Walk']
    check('C an overlap of 3 (+8, +12, +20 at the same width) binds m to in_r3', w and all(r['bind'] == 'overlap 3' for r in w), str(w))
    check('C ... the missed field +16 is GAME-ONLY READ (arg), +8 / +12 OK, +20 WIDTH (u16 vs s16)',
          v(rows, 'syn::Walk', 16) == 'GAME-ONLY READ' and v(rows, 'syn::Walk', 8) == 'OK' and v(rows, 'syn::Walk', 12) == 'OK'
          and v(rows, 'syn::Walk', 20) == 'WIDTH', str(w))
    check('C an overlap of 1 binds nothing', not [r for r in rows if r['fn'] == 'syn::One'], str([r for r in rows if r['fn'] == 'syn::One']))
    check('C two game bases that tie bind nothing', not [r for r in rows if r['fn'] == 'syn::Tie'], str([r for r in rows if r['fn'] == 'syn::Tie']))
    check('C the rank: GAME-ONLY gate rows (the record) first', rows and rows[0]['verdict'] == 'GAME-ONLY READ' and rows[0]['use'] == 'gate'
          and rows[0]['fn'] == 'syn::Gate', str(rows[:1]))
    g99 = [r for r in rows if r['fn'] == 'syn::Walk' and r['off'] == 99]
    check('C a game-only read of a bound base far from ours still shows (in_r3+99)', g99 and g99[0]['verdict'] == 'GAME-ONLY READ', str(g99))
    ours_only = F.judge('x', 'sub_1', [{'base': 'p', 'off': 4, 'n': 4, 'k': 'u', 'at': 'a:1', 'expr': 'e'}], [], [('p', 'in_r3', 'overlap 2')])
    check('C OURS-ONLY READ: ours reads a bound offset the game function does not', ours_only and ours_only[0]['verdict'] == 'OURS-ONLY READ', str(ours_only))
    wd = F.judge('x', 'sub_1', [{'base': 'p', 'off': 0, 'n': 4, 'k': 'u', 'at': 'a:1', 'expr': 'e'}, {'base': 'p', 'off': 4, 'n': 1, 'k': 'u', 'at': 'a:2', 'expr': 'e'}],
                 [gr('in_r3', 0, 4, 'f'), gr('in_r3', 4, 1, 's')], [('p', 'in_r3', 'overlap 2')])
    check('C a whole word read as u32 against the game f32 is OK; a byte u8 against the game s8 is WIDTH',
          [r['verdict'] for r in wd] == ['OK', 'WIDTH'], str([(r['off'], r['verdict']) for r in wd]))
    f = F.facts(rows)
    check('C the rows as code-diff facts (side / fn / field read RECORD+OFF / value / at / kind read)',
          any(x['side'] == 'game' and x['field'] == 'read syn_file+2' and x['value'] == 's16' and x['kind'] == 'read' for x in f), str(f[:2]))
    r1 = planted('bind_any', run)
    twin('C bind any: the 1-overlap function binds', [r for r in r1 if r['fn'] == 'syn::One'])


def part_d():
    try:
        for ref in (PARENT, FIX):
            subprocess.run(['git', 'cat-file', '-e', f'{ref}^{{commit}}'], cwd=F.REPO, check=True, capture_output=True)
    except Exception as e:  # noqa: BLE001 - no history here
        print(f'SKIP D (no frozen refs: {e})')
        return
    game = F.Game()

    def at(ref):
        rows = F.collect(ref, fn='assets::arena_props::PlacementIds', game_fn='sub_8258F698', game=game)[0]
        return {r['off']: r for r in rows if r['record'] == 'placement_table_file'}
    p = at(PARENT)
    check('D1 PLACECOUNT0 parent: PlacementIds never reads byte 0 / the signed count sub_8258F698 gates on (GAME-ONLY READ, gate)',
          all(o in p and p[o]['verdict'] == 'GAME-ONLY READ' and p[o]['use'] == 'gate' for o in (0, 2))
          and p[2]['game_reads'][0]['k'] == 's', str({k: (r['verdict'], r['use']) for k, r in p.items()}))
    t = at(FIX)
    check('D1 PLACECOUNT0 fix: both read (OK, s16 against s16)', all(o in t and t[o]['verdict'] == 'OK' for o in (0, 2)),
          str({k: (r['verdict'], r['use']) for k, r in t.items()}))


def main():
    part_a()
    part_b()
    root = tempfile.mkdtemp(prefix='qfieldsdiff_')
    try:
        for rel, text in SRC.items():
            p = os.path.join(root, rel.replace('/', os.sep))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(text)
        part_c(root)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    part_d()
    print(f'{len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
