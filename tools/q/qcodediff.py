r"""q codediff: per pair (OUR function, the GAME function it mirrors), the game's render-state facts against ours, field by field
(S111 CODEDIFF, piece 4 of the code-diff tool; docs/research/s111/WHOLEDIFF_PLAN.md A.4).

    py tools/q/q.py codediff [--fn NAME | --game sub_X] [--field F] [--flag DIFF ...] [--ref REF] [--json]

The three inputs, each read by its own tool (nothing re-derived here):
  pairs   which game functions each of ours mirrors: `q pairs` (tools/q/qpairs.py) MIRRORS rows (tools/q/qpairs_rel.py: a hook, a driver,
          a handler, a logger or an input reader WATCHES / CITES the game function and is not paired) of the PRECISE sources (comment, code, claim,
          seam-why; a `seam` row is topic-level, every game row of a topic against every ours row, and is not a pair); a kTable row's
          own `addr` cell (q ostate's `game` list); q rstdiff's KINDS (a kind's ours block against its game windows' functions; a
          window's `slots` and a kind's narrowed `fields` narrow the fields). One DrawState block of ours is one draw: a function's
          blocks are compared apart, each with the citations on its own lines (a function of one block: all of them), and a block of
          rstdiff kinds never against a game function only another kind names; qrstdiff.OURS_ONLY blocks (no game draw) are not
          compared. ARM BY ARM (cite_scope, kind_arm): a fact tagged with its own kind stands against its kinds' functions only; a
          block with a fact under several kinds (FrontEnd::Record: every family through one block) takes a citation only for what it
          sits on (the block start: every field; a fact's line or the line above: those fields; elsewhere: nothing), and an arm read
          per pass class stands against its family's node. A bind record's stores (FrontEnd::BindMaterial) are one unit per if-arm
          (S111 CDARMS, tools/q/qcdarms.py arm_scope: the cite_scope rule over the arm's line and each fact's line, the line above and
          its attached comment block; a game function whose known pass is not the one the arm's comment scopes it to is out).
          A fact whose line (or the line above, or for a bind arm its attached comment) cites game functions stands against
          the nearest setter of its field from them: the cited function, else its callers up the game's call graph (3 levels; the q
          db's call / tail edges).
  game    `q gstate --facts` (tools/q/qgstate.py): every setter call with its constant(s); a restore / save is not a set; a value
          that is not a constant (`reg ...`) is UNSETTLED.
  ours    `q ostate` (tools/q/qostate.py) at the working tree or --ref.
The units are converted HERE, one named table each (UNITS): the game's alpha func codes to VkCompareOp by the names of
db/facts/enums.json `alpha_func`; its z func codes through qrstdiff.ZFUNC_CODE, then the reversed-Z mirror pass_state.h ForwardZ (read
from the file and run by qostate's evaluator, as qrstdiff applies it); our Vk cull literal to the game's on / off (VK_CULL_ON); the
game's blend code to (op, src, dst) by qrstdiff.BLEND_CODES and our Vk numbers to the same names (qrstdiff._norm_ours).
Same condition: a fact of ours read per pass class (`pass class X`) pairs only with the game functions its class's kTable row (or an
rstdiff kind of those classes) names, when the game function is one they name; a fact that tests the caller's InstancedDraw field
(`d.alpha_test < 0`, or the value `caller InstancedDraw::alpha_test`, or a local initialised from it such as DrawInstanced's `tested`,
for every field but the alpha func / ref) is the CALLER's, compared at the callers' stores (their own pairs; a store states
alpha_test / cull only, and a caller that leaves the field at -1 has no store), not in the draw. Our alpha func 7 (ALWAYS:
AlphaDynamic's untested arm) is an alpha test OFF, not a func; the DrawState zero's
blend factors go with its blend-off row when the blend-on path sets them again.
Fields: gstate's setter keys. A function of ours compares what its sources can state: a DrawState block all of FIELDS; an InstancedDraw
caller store alpha_test / cull; a kTable row its columns. Ours-only fields: blend_op / src / dst stand against the game's blend code (a
game blend code is never GAME-ONLY beside them), no_blend (the tick-blend tag) is never compared.
Verdicts: DIFF (both set, values differ), GAME-ONLY, OURS-ONLY (low: the game may set it in a caller), UNSETTLED (a non-constant on
either side, a rule of ours; a game function that sets several values of the field when ours has only values among them, because the
facts carry no draw position; ours' extra value beside a game set that an rstdiff `adds` window puts on one path only), DEPARTED (a
DIFF / GAME-ONLY declared at the site), OK (also moot: z func / write when either side has no z test, the blend factors with no
blend, alpha func / ref with no alpha test). Rank: DIFF, GAME-ONLY, UNSETTLED, OURS-ONLY, DEPARTED,
OK; then the scene before 2D (an rstdiff kind with mirror False is 2D); then our call sites (`q ours callers`, more first).
Departures (Jarrod 2026-11-27, docs/DECISIONS.md 20:3x row (4)): declared AT THE SITE, never in a baseline file, on our fact's line or
the line above (a GAME-ONLY: the fact's DrawState block line, or a store / table row line of the function):
    // DEPARTS: <class> <FILE>:<row ref> [@sub_X ...]; <reason of 12+ characters>
class = game-bug-fix | mod | cinecam | habit | design; FILE = GAME_BUG_FIXES (docs/GAME_BUG_FIXES.md) | DECISIONS (docs/DECISIONS.md) |
360_HABITS (docs/renderer/360_HABITS.md); the row ref is a heading's words, a table row's first cell (`GBF-3`, a habit's `15`) or a
list row's head (`2026-11-27 20:4x`), and must be found in that file. game-bug-fix names a GAME_BUG_FIXES row, habit a 360_HABITS row.
An `@sub_X` after the row ref scopes the departure to the pairs with that game function (a block paired with several game
functions departs from one of them only); on another pair the marker is refused and says its scope.
A mod is accepted only on a fact whose condition names the mod's switch (its toggle-OFF path must be the game's): a cvar of
src/launcher/report_facts.inc `mod |` rows, or a no-argument bool function defined in a src file that names one of those cvars
(floor_blood::On). A refused marker leaves the verdict and says why.
check.py row `codediff`: the pairs whose OUR function has a fact in a src file of the diff; a DIFF / GAME-ONLY not DEPARTED fails, so
does a bad DEPARTS marker in a src file of the diff; a diff that edits db/claims or db/seams (q pairs' claim / seam-why rows) also
fails on a DIFF / GAME-ONLY of the whole tree that its base (src + db at the base, git archive) does not have.
"""
import argparse
import collections
import fnmatch
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qostate as O     # noqa: E402
import qrstdiff as R    # noqa: E402
import qcdarms as A     # noqa: E402

REPO = O.REPO
VERDICTS = ('DIFF', 'GAME-ONLY', 'UNSETTLED', 'OURS-ONLY', 'DEPARTED', 'OK')
RANK = {v: i for i, v in enumerate(VERDICTS)}
FAILING = ('DIFF', 'GAME-ONLY')
PRECISE = ('comment', 'code', 'claim', 'seam-why')
FIELDS = ('z_enable', 'z_write', 'z_func', 'blend_enable', 'blend_code', 'blend_op', 'blend_src', 'blend_dst', 'cull',
          'colour_write_mask', 'stencil_enable', 'alpha_test', 'alpha_func', 'alpha_ref')
DERIVED = ('blend_op', 'blend_src', 'blend_dst')        # the game states these through its blend code
NEVER = ('no_blend',)                                   # ours only, never compared
ONOFF = ('z_enable', 'z_write', 'blend_enable', 'stencil_enable', 'alpha_test')
SCOPE = {'table': tuple(O.TABLE_FIELD.values())}        # a caller unit's scope is the fields it states (and gates): scope_of
GATE_SKIP = ('alpha_test', 'alpha_func', 'alpha_ref')   # the draw's fields a caller's alpha_test does not gate (its own row; their gate is it)
CLASS_TERM = r'(?:\w+::)*Of\s*\(\s*\w+\s*\)\s*\.\s*\w+'  # a kTable column read as a condition: true where the fact exists (q ostate drops the classes it fails)
LEFT_WHY = ("the caller leaves InstancedDraw::alpha_test at -1: the draw follows each material's own flag (Material::alpha_test), "
            'which no fact holds per material')
NO_PAIR = '(no pair)'
MOOT = {'z_func': 'z_enable', 'z_write': 'z_enable', 'blend_code': 'blend_enable', 'blend_op': 'blend_enable',
        'blend_src': 'blend_enable', 'blend_dst': 'blend_enable', 'alpha_func': 'alpha_test', 'alpha_ref': 'alpha_test'}
VK_CULL_ON = {0: 0, 1: 1, 2: 1, 3: 1}                   # VkCullModeFlags (none, front, back, both) -> the game's cull enable
VK_BY_NAME = {name: vk for vk, name in R.CMP.items()}   # VkCompareOp numbers by name (qrstdiff.CMP)
CLASSES = ('game-bug-fix', 'mod', 'cinecam', 'habit', 'design')
REF_FILES = {'GAME_BUG_FIXES': 'docs/GAME_BUG_FIXES.md', 'DECISIONS': 'docs/DECISIONS.md', '360_HABITS': 'docs/renderer/360_HABITS.md'}
CLASS_FILE = {'game-bug-fix': 'GAME_BUG_FIXES', 'habit': '360_HABITS'}
MARK_RE = re.compile(r'\bDEPARTS:[ \t]*(?P<body>[^\n]*)')
MARK_BODY = re.compile(r'(?P<cls>[\w-]+)\s+(?P<file>[A-Z0-9_]+):(?P<ref>[^;\n]*);\s*(?P<why>.*)$')
MOD_ROWS = 'src/launcher/report_facts.inc'
GAME, DEFER = 'GAME', 'DEFER'
# selftest only: a planted twin the selftest must see fail ('raw_afunc' alpha codes uncoverted, 'no_forwardz', 'no_defer',
# 'any_ref' a DEPARTS row ref not looked up, 'mod_anywhere' a mod departure accepted outside its switch)
PLANT = os.environ.get('WWE_QCODEDIFF_PLANT', '')


class CodediffError(Exception):
    pass


# ------------------------------------------------------------------ the units (one named table each)
def alpha_func_vk():
    """the game's alpha function code -> VkCompareOp, by the names db/facts/enums.json `alpha_func` gives each code"""
    import qenum
    vals = qenum.load()['enums']['alpha_func']['values']
    return {int(k): VK_BY_NAME[v['name']] for k, v in vals.items() if v.get('name') in VK_BY_NAME}


class Units:
    """the conversions, all from the game's own tables (S111 CDFOLLOW, CODEDIFF review finding 7): the game's z func code goes through
    qrstdiff.ZFUNC_CODE (sub_82C237D8's cmpwi chain), then, for a main-view pair (a kind whose `mirror` is True, as rstdiff), through the
    reversed-Z rule stated once in qrstdiff.MIRROR. Our pass_state.h ForwardZ is OUR side only (q ostate runs it on our rows): a fault in
    it is then a DIFF, not the same mistake on both sides. ps_text is read only by the planted twin 'game_forwardz' (the old circle)."""
    def __init__(self, ps_text=None):
        self.afunc = alpha_func_vk()
        self.ctx = O.Ctx([O.CD._mask(ps_text)], '') if ps_text and PLANT == 'game_forwardz' else None

    def forward_z(self, vk, mirror=True):
        if PLANT == 'no_forwardz' or not (mirror or PLANT == 'mirror_all'):
            return vk
        if self.ctx is not None:   # planted twin: the game side through our own ForwardZ
            got = O.eval_val(f'ForwardZ({vk})', self.ctx)
            return next(iter(got)) if len(got) == 1 and isinstance(next(iter(got)), int) else vk
        return R.MIRROR.get(vk, vk)

    def game(self, field, v, mirror=True):
        """one game constant -> the compared space: (field, value) pairs (a blend code gives op / src / dst too); mirror = the pair is
        a main-view one (its z func is reversed)"""
        if field == 'alpha_func':
            return [(field, v if PLANT == 'raw_afunc' else self.afunc.get(v, ('reg', f'alpha func code {v} (not in enums.json)')))]
        if field == 'z_func':
            vk = R.ZFUNC_CODE.get(v)
            return [(field, self.forward_z(vk, mirror) if vk is not None else ('reg', f'z func code {v} (not in qrstdiff.ZFUNC_CODE)'))]
        if field == 'blend_code':
            t = R.BLEND_CODES.get(v)
            out = [(field, v)]
            for f, x in zip(DERIVED, t or (None,) * 3):
                out.append((f, x if t else ('reg', f'blend code {v} (its tuple is not in qrstdiff.BLEND_CODES)')))
            return out
        if field == 'colour_write_mask':   # recorder slot 13 (sub_82C23910) records render state 53 as 15 for a non-zero word, 0 for zero
            return [(field, 0xF if v else 0)]
        if field in ONOFF or field == 'cull':
            return [(field, 1 if v else 0)]
        return [(field, v)]

    @staticmethod
    def ours(field, v):
        if field == 'cull':
            return VK_CULL_ON.get(v, v)
        if field in DERIVED:
            return R._norm_ours(field, v)
        if field in ONOFF:
            return 1 if v else 0
        return v


def show(field, v):
    if v == DEFER:
        return "the caller's (InstancedDraw)"
    if isinstance(v, tuple):
        return f'{v[0]} {v[1]}'
    if field in ('z_func', 'alpha_func') and isinstance(v, int):
        return f'{R.CMP.get(v, v)}'
    if field == 'cull' and isinstance(v, int):
        return 'on' if v else 'off'
    if field == 'colour_write_mask' and isinstance(v, int):
        return f'0x{v:X}'
    return str(v)


# ------------------------------------------------------------------ the departure markers
class Marks:
    """DEPARTS markers of one tree: read(rel) -> text (the tree's reader); the row files and the mod switches read from it too"""
    def __init__(self, read, switch_files=None):
        self.read = read
        self._lines, self._docs = {}, {}
        self._switches = None
        self._switch_files = switch_files

    def lines(self, rel):
        if rel not in self._lines:
            try:
                self._lines[rel] = self.read(rel).split('\n')
            except (OSError, O.OstateError):
                self._lines[rel] = []
        return self._lines[rel]

    def ref_found(self, key, ref):
        rel = REF_FILES[key]
        if rel not in self._docs:
            try:
                self._docs[rel] = self.read(rel).split('\n')
            except (OSError, O.OstateError):
                self._docs[rel] = []
        for ln in self._docs[rel]:
            s = ln.strip()
            if s.startswith('#') and ref in s:
                return True
            if s.startswith('|') and s.count('|') >= 2 and s.split('|')[1].strip() == ref:
                return True
            if s.startswith('- ') and s[2:].startswith(ref):
                return True
        return False

    def switches(self):
        """(mod cvars, names of the bool readers of them): report_facts.inc `mod |` rows; the files naming a cvar (switch_files)"""
        if self._switches is None:
            cvars = []
            for ln in self.lines(MOD_ROWS):
                m = re.match(r'\s*mod\s*\|\s*(\w+)\s*\|', ln)
                if m:
                    cvars.append(m.group(1))
            readers = set()
            for rel in (self._switch_files(cvars) if self._switch_files else ()):
                txt = '\n'.join(self.lines(rel))
                if any(re.search(r'\b' + re.escape(c) + r'\b', txt) for c in cvars):
                    readers |= set(re.findall(r'\binline\s+bool\s+(\w+)\s*\(\s*\)', txt))
            self._switches = (cvars, sorted(readers))
        return self._switches

    def behind_switch(self, cond):
        if PLANT == 'mod_anywhere':
            return True
        cvars, readers = self.switches()
        c = cond or ''
        return any(re.search(r'\b' + re.escape(x) + r'\b', c) for x in cvars) or any(re.search(r'\b' + re.escape(x) + r'\s*\(', c) for x in readers)

    def parse(self, body):
        """-> (marker dict, None) or (None, why refused) for the text after `DEPARTS:`"""
        m = MARK_BODY.match(body.strip())
        if not m:
            return None, 'not of the form `DEPARTS: <class> <FILE>:<row ref>; <reason>`'
        cls, key, ref, why = m.group('cls'), m.group('file'), m.group('ref').strip(), m.group('why').strip()
        games = []   # the marker's scope: `<row ref> @sub_X [@sub_Y ...]` = only the pairs with those game functions (main session 2026-11-27 21:3x)
        while (sm := re.search(r'\s+@(\S+)$', ref)):
            if not re.fullmatch(r'sub_[0-9A-Fa-f]{8}', sm.group(1)):
                return None, f'the scope @{sm.group(1)} is not a game function name (@sub_XXXXXXXX)'
            games.insert(0, 'sub_' + sm.group(1)[4:].upper())
            ref = ref[:sm.start()].strip()
        if cls not in CLASSES:
            return None, f'class {cls!r} is not one of {", ".join(CLASSES)}'
        if key not in REF_FILES:
            return None, f'{key}: the row file is one of {", ".join(REF_FILES)}'
        if CLASS_FILE.get(cls) and CLASS_FILE[cls] != key:
            return None, f'a {cls} departure names its {CLASS_FILE[cls]} row, not {key}'
        if not ref:
            return None, 'no row ref'
        if PLANT != 'any_ref' and not self.ref_found(key, ref):
            return None, f'{key}:{ref}: no such row in {REF_FILES[key]}'
        if len(why) < 12:
            return None, f'the reason is under 12 characters ({why!r})'
        return {'class': cls, 'file': key, 'ref': ref, 'why': why, **({'games': games} if games else {})}, None

    def at(self, where):
        """the marker on file:line or the line above -> (marker, None) / (None, refusal) / (None, None) when there is none"""
        rel, _, ln = where.rpartition(':')
        if not ln.isdigit():
            return None, None
        ls = self.lines(rel)
        for x in (int(ln), int(ln) - 1):
            if 1 <= x <= len(ls):
                m = MARK_RE.search(ls[x - 1])
                if m:
                    return self.parse(m.group('body'))
        return None, None

    def scan(self, rel):
        """every DEPARTS marker of one file -> [(line, refusal)] of the bad ones"""
        bad = []
        for i, ln in enumerate(self.lines(rel), 1):
            m = MARK_RE.search(ln)
            if m:
                _mk, why = self.parse(m.group('body'))
                if why:
                    bad.append((i, why))
        return bad


# ------------------------------------------------------------------ the join
# qrstdiff's recorder slot names -> gstate's setter keys (a window's `slots` names the fields it stands for)
SLOT_FIELD = {'blend': ('blend_enable',), 'atest': ('alpha_test',), 'blend_code': ('blend_code',) + DERIVED, 'afunc_code': ('alpha_func',),
              'aref': ('alpha_ref',), 'ztest': ('z_enable',), 'zwrite': ('z_write',), 'zfunc_code': ('z_func',), 'cull': ('cull',),
              'stencil': ('stencil_enable',), 'cmask': ('colour_write_mask',)}


def kinds_index():
    """qrstdiff KINDS -> ({kind: {sub_X: fields or None (all)}}, {kind: mirror}): a window with `slots` stands for those fields only;
    a kind that narrows its own `fields` below rstdiff's whole list (the scene classes: z test / func only, their blend, cull and
    z write being per-part rules of ours) stands for those fields only"""
    games, mirror = {}, {}
    for k in R.KINDS:
        gs = games.setdefault(k['kind'], {})
        kf = None
        if set(k.get('fields') or R.FIELDS) != set(R.FIELDS) and PLANT != 'kind_fields':
            kf = {O.FIELD_KEY[x] for x in k['fields'] if x in O.FIELD_KEY}
            kf |= set(DERIVED) if 'blend' in k['fields'] and {'blend_op', 'blend_src', 'blend_dst'} <= set(k['fields']) else set()
        for w in k['game']:
            g = f"sub_{w['fn']:08X}"
            fl = None if not w.get('slots') else {x for s in w['slots'] for x in SLOT_FIELD.get(s, ())}
            if kf is not None:
                fl = kf if fl is None else fl & kf
            gs[g] = None if (g in gs and gs[g] is None) or fl is None else (gs.get(g) or set()) | fl
        mirror[k['kind']] = k.get('mirror', True)
    return games, mirror


def block_pair_fields(kinds, g, scoped, kgames):
    """-> the fields one block of ours stands for against game function g: None = every field, () = not this pair. scoped = the pair's
    sources with the fields each stands for (cite_scope). A block of rstdiff kinds pairs with a game function another kind names only
    through its own kinds; a kind's window `slots` / narrowed `fields` and a citation's own facts narrow the fields; a kTable cell or a
    citation at the block's start stands for every field."""
    if PLANT == 'no_scope':
        return None
    ks = [kind_of(k) for k in kinds]
    mine = [kgames[k][g] for k in ks if g in kgames.get(k, {})]
    if ks and not mine and any(g in v for v in kgames.values()):
        return ()
    got = mine + [fl for s, fl in scoped if not s.startswith('rstdiff ')]
    if not got:
        return ()
    if any(x is None for x in got):
        return None
    return set().union(*got)


def kind_of(label):
    return label.split(' (')[0]


def table_classes(ours):
    """{sub_X: {class row name}}: the game functions each kTable row's addr cell names, and the game functions of each rstdiff kind
    that names its pass classes (qrstdiff KINDS `classes`)"""
    out = collections.defaultdict(set)
    for f in ours:
        m = re.fullmatch(r'pass_state::kTable\[(\w+)\]', f['fn'])
        if m:
            for g in f.get('game', ()):
                out[g].add(m.group(1))
    for k in R.KINDS:
        for w in k['game'] if k.get('classes') else ():
            out[f"sub_{w['fn']:08X}"] |= set(k['classes'])
    return out


def pairs_of(ours, pair_rows, have_game, kgames, lines_of=None):
    """-> ({fn: {sub_X: [source]}}, {(file, line): {sub_X}}) the pairs of every function of ours that has facts, and the line citations.
    A DEPARTS marker's own line (its `@sub_X` scope) is no citation: the marker names the pair it departs from, it does not make one."""
    pairs = collections.defaultdict(lambda: collections.defaultdict(list))
    cites = collections.defaultdict(set)
    for f in ours:
        fn = f['fn']
        for g in f.get('game', ()) if f['kind'] == 'table' else ():
            if g in have_game:
                pairs[fn][g].append('kTable addr')
        for lab in f.get('kinds', ()):
            for g in kgames.get(kind_of(lab), ()):
                if g in have_game and f'rstdiff {kind_of(lab)}' not in pairs[fn][g]:
                    pairs[fn][g].append(f'rstdiff {kind_of(lab)}')
    for fn, rows in pair_rows.items():
        for r in rows:
            g = 'sub_' + r['game']
            if r['src'] not in PRECISE or r['rel'] != 'MIRRORS':     # S111 PAIRMEAN: only a port pairs (q pairs' relation, tools/q/qpairs_rel.py)
                continue
            if r['src'] in ('comment', 'code') and lines_of and PLANT != 'marker_cites':
                ls = lines_of(r['file'])
                if 1 <= r['line'] <= len(ls) and MARK_RE.search(ls[r['line'] - 1]):
                    continue
            if r['src'] in ('comment', 'code'):
                cites[(r['file'], r['line'])].add(g)
            if g not in have_game:
                continue
            src = f"{r['src']} {r['file']}:{r['line']}"
            if src not in pairs[fn][g]:
                pairs[fn][g].append(src)
    return pairs, cites


def nearest_setters(near, field, sets, game_callers, depth=3):
    """the game functions a fact citing `near` stands against for one field: the cited functions that set it, else their callers
    that do, up the call graph (the state a cited draw runs under is set by it or by a caller of it); None when no level sets it"""
    level, seen = set(near), set(near)
    for _ in range(depth + 1):
        hit = {g for g in level if field in sets.get(g, ())}
        if hit:
            return hit
        nxt = set()
        for g in level:
            nxt |= set(game_callers(g)) - seen
        if not nxt or len(seen) > 400:
            return None
        seen |= nxt
        level = nxt
    return None


def caller_vars(facts):
    out = set()
    for f in facts:
        v = f['value']
        if isinstance(v, dict) and 'caller' in v:
            m = O.SOURCE_RE.search(v.get('expr') or '')
            if m:
                out.add(m.group(1))
    return out


LOCAL_DECL = r'\b(?:const\s+)?(?:bool|auto|int|int8_t|uint8_t)\s+{name}\s*=\s*([^;]+);'


def caller_locals(facts, cvars, lines_of):
    """the locals a condition of ours reads that are the caller's InstancedDraw::alpha_test (FrontEnd::DrawInstanced: `tested` =
    `d.alpha_test < 0 ? bool(b.alpha_test) : d.alpha_test != 0`): a word of a fact's condition whose nearest declaration above the
    fact (lines_of(file) -> its lines) is initialised from VAR.alpha_test, VAR a caller variable of the block (caller_vars)"""
    out = set()
    if not cvars or PLANT == 'no_local_defer':
        return out
    src = re.compile(r'\b(?:' + '|'.join(re.escape(x) for x in sorted(cvars)) + r')\s*(?:\.|->)\s*alpha_test\b')
    for f in facts:
        rel, _, ln = f['at'].rpartition(':')
        if not ln.isdigit():
            continue
        for w in set(re.findall(r'(?<![\w.>:])([A-Za-z_]\w*)\b(?!\s*(?:\(|\.|->|::))', f.get('cond') or '')) - out:
            ls = lines_of(rel)[:int(ln)]
            for text in reversed(ls):
                m = re.search(LOCAL_DECL.format(name=re.escape(w)), text)
                if m:
                    if src.search(m.group(1)):
                        out.add(w)
                    break
    return out


def deferred(f, cvars, clocals=()):
    """the caller's: a value read from its InstancedDraw field, a condition on the field, or (a field other than the alpha func /
    ref, whose gate is the alpha_test row) a condition on a local that is the field (caller_locals)"""
    if PLANT == 'no_defer':
        return False
    v = f['value']
    if isinstance(v, dict) and 'caller' in v:
        return True
    c = f.get('cond') or ''
    if any(re.search(r'\b' + re.escape(x) + r'\s*(?:\.|->)\s*alpha_test\b', c) for x in cvars):
        return True
    return f['field'] not in ('alpha_func', 'alpha_ref') and any(re.search(r'(?<![\w.>:])' + re.escape(x) + r'\b(?!\s*(?:\(|\.|->|::))', c) for x in clocals)


def keep_for(f, g, tclasses, cites, sets, game_callers, passes=None):
    """does ours fact f stand against game function g (the same condition). A fact whose line (or the line above) cites game
    functions stands against the nearest setter of its field from them (nearest_setters); a per-class fact against its class's
    kTable functions."""
    if PLANT == 'no_scope':
        return True
    rel, _, ln = f['at'].rpartition(':')
    if ln.isdigit():
        near = cites.get((rel, int(ln)), set()) | cites.get((rel, int(ln) - 1), set())
        for a in f.get('attached') or ():   # S111 CDARMS: a bind arm's comment block above its store
            near |= cites.get((rel, a), set())
        near = {x for x in near if not A.out_of_scope(x, [f], passes)}
        if near:
            fields = (f['field'],) + (('blend_code',) if f['field'] in DERIVED else ())
            ok = set()
            for fld in fields:
                ok |= nearest_setters(near, fld, sets, game_callers) or set()
            if g not in ok:
                return False
    if f.get('classes') and g in tclasses and not set(f['classes']) & tclasses[g]:
        return False
    return True


def kind_arm(f, g, kgames):
    """a fact of ours tagged with rstdiff kinds (its block's arm for those kinds: q2d::Append's `[in-scene row]` / `[final-target row]`)
    stands against a kind's game function only when one of ITS kinds names it; a game function no kind names (a citation's) is not
    kept from it here"""
    ks = {kind_of(k) for k in f.get('kinds', ())}
    if not ks or PLANT == 'kind_any_arm' or not any(g in v for v in kgames.values()):
        return True
    return any(g in kgames.get(k, {}) for k in ks)


def ours_value(f, units, cvars, clocals=()):
    v = f['value']
    if deferred(f, cvars, clocals):
        return DEFER
    if isinstance(v, bool):
        v = int(v)
    if isinstance(v, int):
        return units.ours(f['field'], v)
    if isinstance(v, dict):
        if v.get('game'):
            return GAME
        for k in ('material', 'rule', 'caller'):
            if k in v:
                return (k, str(v[k]))
    return ('rule', str(v))


def game_values(facts, units, mirror=True):
    """[(field, value, at)] of one game function's set facts in the compared space (mirror: a main-view pair, Units.forward_z)"""
    out = []
    for f in facts:
        if f['kind'] != 'set':
            continue
        v = f['value']
        vals = v if isinstance(v, list) else [v]
        for x in vals:
            if isinstance(x, dict):
                conv = [(f['field'], ('reg', str(x.get('reg'))))] + ([(d, ('reg', str(x.get('reg')))) for d in DERIVED] if f['field'] == 'blend_code' else [])
            else:
                conv = units.game(f['field'], x, mirror)
            for fld, cv in conv:
                if fld in FIELDS:
                    out.append((fld, cv, f['at'] + (f" (via {f['via']})" if f.get('via') else '')))
    return out


def scope_of(facts):
    """the fields a unit of ours can state: a DrawState block every field, a kTable row its columns, a caller unit (S111 CDFOLLOW) the
    fields it stores and the draw fields its alpha_test gates"""
    s = set()
    for f in facts:
        if f['kind'] in ('set', 'default', 'alpha'):
            s |= set(FIELDS)
        elif f['kind'] in ('caller', 'gated'):
            s.add(f['field'])
        else:
            s |= set(SCOPE.get(f['kind'], ()))
    return s


def pair_mirror(kinds, g, kgames, kmirror):
    """is the pair (a unit of ours of these rstdiff kinds, game function g) a main-view one (its z func reversed)? The kinds of the unit
    that name g decide (rstdiff's `mirror`), else every kind of the unit; a unit of no kind is a scene one"""
    ks = {kind_of(k) for k in kinds}
    flags = [kmirror.get(k, True) for k in ks if g in kgames.get(k, {})] or [kmirror.get(k, True) for k in ks]
    return not flags or any(flags)


def unit_of(f):
    """the unit of ours a fact belongs to: its DrawState block, else (S111 CDARMS) its bind store's if-arm, else (S111 CDFOLLOW) its
    InstancedDraw declaration, else None"""
    return f.get('block') or (f.get('arm') if PLANT != 'one_arm' else None) or (None if PLANT == 'one_unit' else f.get('decl'))


def unit_span(unit, facts):
    """-> (file, first line, last line) of a caller unit: its declaration to its last fact"""
    rel, _, ln = unit.rpartition(':')
    lines = [int(f['at'].rpartition(':')[2]) for f in facts if f['at'].rpartition(':')[0] == rel and f['at'].rpartition(':')[2].isdigit()]
    return rel, int(ln), max(lines + [int(ln)])


def decl_sources(srcs, unit, spans):
    """S111 CDFOLLOW (CODEDIFF review finding 3): the pair sources of one caller unit (one InstancedDraw declaration) of a function with
    several: a citation goes to the unit(s) nearest it (inside the unit's lines = 0, else the distance to its first / last line);
    an rstdiff kind / kTable source stands for every unit. A function of one unit keeps all its sources."""
    if len(spans) <= 1 or PLANT == 'one_unit':
        return srcs

    def dist(sp, rel, ln):
        if sp[0] != rel:
            return float('inf')
        return 0 if sp[1] <= ln <= sp[2] else min(abs(ln - sp[1]), abs(ln - sp[2]))
    out = []
    for s in srcs:
        m = CITE_SRC.fullmatch(s)
        if not m:
            out.append(s)
            continue
        d = {u: dist(sp, m.group(1), int(m.group(2))) for u, sp in spans.items()}
        if d[unit] == min(d.values()):
            out.append(s)
    return out


def gate_facts(stores, draws, lines_of):
    """S111 CDFOLLOW (finding 2's limit): the draw fields a caller's InstancedDraw::alpha_test gates, read at the caller. For each
    alpha_test store of a unit that reaches a draw function (`calls`), the draw's facts of every field it defers to the caller (a
    condition on the field or on a local that is it: FrontEnd::DrawInstanced's blend under `tested`) are evaluated with the stored
    value: an arm whose condition then fails is dropped, the DrawState zero goes when an arm holds unconditionally. A store that leaves
    the field at -1 (the material's flag) gates them with the material: UNSETTLED at the compare."""
    out = []
    if PLANT == 'no_gate':
        return out
    for s in stores:
        if s['field'] != 'alpha_test' or s['kind'] != 'caller' or not s.get('calls'):
            continue
        D = [f for f in draws.get(s['calls'], ()) if f['field'] not in NEVER]
        cv = caller_vars(D)
        cl = caller_locals(D, cv, lines_of)
        gated = sorted({f['field'] for f in D if f['field'] not in GATE_SKIP and deferred(f, cv, cl)}, key=FIELDS.index)
        v = s['value']
        extra = {k: s[k] for k in ('decl', 'calls', 'left') if s.get(k) is not None}
        if not isinstance(v, int):
            for fld in gated:
                out.append(O._fact(s['fn'], fld, v if isinstance(v, dict) else {'rule': str(v)}, s.get('cond'), s['at'], 'gated',
                                   src=f"{s['calls']}: follows InstancedDraw::alpha_test ({s.get('src')})", **extra))
            continue
        known = {CLASS_TERM: True}
        for x in cv:
            a = re.escape(x) + r'\s*(?:\.|->)\s*alpha_test\s*'
            known.update({a + r'<\s*0': v < 0, a + r'>=\s*0': v >= 0, a + r'!=\s*0': v != 0, a + r'==\s*0': v == 0})
        for w in cl:
            known[re.escape(w)] = v != 0
        ctx = O.Ctx([], '', known)
        for fld in gated:
            got = []
            for f in D:
                if f['field'] != fld:
                    continue
                ok = True if not f.get('cond') else O.eval_cond(f['cond'], ctx, {})
                if ok is not False:
                    got.append((f, None if ok is True else f['cond']))
            if any(f['kind'] != 'default' and c is None for f, c in got):
                got = [(f, c) for f, c in got if f['kind'] != 'default']
            for f, c in got:
                out.append(O._fact(s['fn'], fld, f['value'], O._and(s.get('cond'), c), s['at'], 'gated',
                                   src=f"{f['fn']} @{f['at']} under InstancedDraw::alpha_test = {v}", **extra))
    return out


def _all_zero(vals):
    ints = [v for v in vals if v not in (GAME, DEFER)]
    return bool(ints) and all(v == 0 for v in ints) and len(ints) == len([v for v in vals if v != DEFER])


def judge(field, gv, ov, partial=None):
    """gv [value], ov [value] (ours DEFER = the caller's) -> (verdict, why, extra values of ours). partial: the rstdiff window that
    says the game's set is on one path only (the other paths keep an inherited value no fact holds)"""
    if not ov:
        return 'GAME-ONLY', 'the game sets it, ours never does', set()
    if not gv:
        return 'OURS-ONLY', 'ours sets it, the paired game function does not (it may be set by a caller of it)', set()
    real = [v for v in ov if v != DEFER]
    if not real:
        return 'OK', "ours takes the caller's InstancedDraw field: compared at each caller's store (its own pairs)", set()
    regs = [v for v in gv if isinstance(v, tuple)]
    if regs:
        if all(v == GAME for v in real):
            return 'OK', "both follow the game's own word", set()
        return 'UNSETTLED', f'the game value is not a constant ({regs[0][1]})', set()
    rules = [v for v in real if isinstance(v, tuple) and v[0] == 'rule']
    if rules:
        return 'UNSETTLED', f'ours is a rule of ours ({rules[0][1]})', set()
    if GAME in real:
        if all(v == GAME for v in real):
            return 'OK', "ours follows the game's own word", set()
        return 'UNSETTLED', 'ours partly follows the game, partly a literal', set()
    g, o = set(gv), set(real)
    miss, extra = g - o, o - g
    if not miss and not extra:
        return 'OK', ("ours' other arm takes the caller's InstancedDraw field: compared at each caller's store (its own pairs)"
                      if DEFER in ov else ''), set()
    if PLANT != 'no_unsettled':
        if extra and not miss and partial:
            return 'UNSETTLED', (f'ours has {", ".join(show(field, v) for v in sorted(extra, key=str))} beside the game\'s value; the game sets it on '
                                 f'one path only ({partial}): the other paths keep an inherited value no fact holds'), set()
        if miss and not extra and len(g) > 1:
            return 'UNSETTLED', (f'the game function sets {len(g)} values ({", ".join(show(field, v) for v in sorted(g, key=str))}) for its several '
                                 f'draws / its exit; ours has {", ".join(show(field, v) for v in sorted(o, key=str))}: which of its draws ours is '
                                 'is not in the facts (no draw position, no path condition)'), set()
    bits = []
    if miss:
        bits.append('the game has ' + ', '.join(show(field, v) for v in sorted(miss, key=str)) + ' (ours never)')
    if extra:
        bits.append('ours has ' + ', '.join(show(field, v) for v in sorted(extra, key=str)) + ' (the game never)')
    return 'DIFF', '; '.join(bits), extra


def adds_windows():
    """{sub_X: [(lo, hi, why)]}: qrstdiff KINDS windows of mode 'adds' (a set on one path only)"""
    out = collections.defaultdict(list)
    for k in R.KINDS:
        for w in k['game']:
            if w.get('mode') == 'adds':
                out[f"sub_{w['fn']:08X}"].append((w['fn'] + w['lo'], w['fn'] + w['hi'], f"rstdiff {k['kind']}: {w.get('why', '')}"))
    return out


def block_sources(srcs, block, facts, n_blocks):
    """the pair sources that stand for one block of one family: all of them when the function has one block; else the rstdiff kinds,
    the kTable cells and the citations on the block's own lines (its start to its last fact)"""
    if not block or n_blocks <= 1:
        return srcs
    rel, _, start = block.rpartition(':')
    lines = [int(f['at'].rpartition(':')[2]) for f in facts if f['at'].rpartition(':')[0] == rel and f['at'].rpartition(':')[2].isdigit()]
    lo, hi = int(start), max(lines + [int(start)])
    out = []
    for s in srcs:
        m = re.fullmatch(r'\S+ (.+):(\d+)', s)
        if not m or s.startswith('rstdiff ') or (m.group(1) == rel and lo <= int(m.group(2)) <= hi):
            out.append(s)
    return out


CITE_SRC = re.compile(r'(?:comment|code|claim|seam-why) (.+):(\d+)')


def cite_scope(srcs, block, facts, n_blocks, kinds):
    """-> [(source, fields)] of the pair sources that stand for one unit of ours (a DrawState block, or a function's stores / rows),
    fields None = every field (an rstdiff kind's own narrowing is block_pair_fields'). A block whose every fact is of one family
    (one rstdiff kind, or none; q2d::Append's arms are tagged one kind each, kind_arm keeps each to its own): block_sources (every
    citation of a one-block function, else the block's own lines), each for every field. A block with a fact of SEVERAL families (a
    fact under 2+ rstdiff kinds: FrontEnd::Record draws every family's colour pass through one block, no arm condition naming the
    family) has arms that serve different families, so a citation
    stands only for what it sits on: on the block's start line or the line above it, every field; on a fact's line or the line above
    it, those facts' fields; anywhere else (a sampler rule, the screen grab's binding, a family's own node cited in a scan) nothing
    (S111 CODEDIFF review; docs/research/s111/SEETHRUBLEND.md: the whole Record block against one character node)."""
    multi = any(len({kind_of(k) for k in f.get('kinds', ())}) > 1 for f in facts)
    if not multi or PLANT == 'cite_anywhere':
        return [(s, None) for s in block_sources(srcs, block, facts, n_blocks)]
    brel, _, bstart = block.rpartition(':') if block else ('', '', '')
    out = []
    for s in srcs:
        m = CITE_SRC.fullmatch(s)
        if not m:
            out.append((s, None))
            continue
        rel, ln = m.group(1), int(m.group(2))
        if rel == brel and bstart.isdigit() and ln in (int(bstart), int(bstart) - 1):
            out.append((s, None))
            continue
        fl = {f['field'] for f in facts if f['at'] in (f'{rel}:{ln}', f'{rel}:{ln + 1}')}
        if fl:
            out.append((s, fl))
    return out


def run(ours, game, pair_rows, units, marks, game_callers=lambda g: (), callers=None, skip_blocks=None, inherit=None, passes=None):
    """THE diff: -> rows. ours = q ostate facts, game = q gstate --facts facts, pair_rows = {our fn: [q pairs row dict]},
    game_callers(sub_X) -> the game functions that call it (the call graph); skip_blocks {file:line: why} = blocks of ours with
    no game draw (qrstdiff.OURS_ONLY); inherit = qgstate_inherit.codediff_hook: an OURS-ONLY row judged against the state the game
    function inherits from its callers (S111 GINHERIT)"""
    skip_blocks = skip_blocks or {}
    gby = collections.defaultdict(list)
    for f in game:
        gby[f['fn']].append(f)
    sets = {g: {fld for f in fs if f['kind'] == 'set' for fld in ((f['field'],) + (DERIVED if f['field'] == 'blend_code' else ()))}
            for g, fs in gby.items()}
    kgames, kmirror = kinds_index()
    adds = adds_windows()
    tclasses = table_classes(ours)
    pairs, cites = pairs_of(ours, pair_rows, set(gby), kgames, marks.lines)
    draws = collections.defaultdict(list)   # a draw function a caller unit reaches (`calls`), by its last name part: its block facts
    for f in ours:
        if f.get('block'):
            draws[f['fn'].rpartition('::')[2]].append(f)
    oby = collections.defaultdict(list)   # one DrawState block of ours = one draw: (fn, block); one InstancedDraw declaration = one
    for f in ours:                        # draw: (fn, decl) (S111 CDFOLLOW); a function's other stores / rows: (fn, None)
        if f['field'] not in NEVER:
            oby[(f['fn'], unit_of(f))].append(f)
    for (fn, unit), facts in list(oby.items()):
        if unit and not any(f.get('block') or f.get('arm') for f in facts):
            facts += gate_facts(facts, draws, marks.lines)
    rows = []
    n_blocks = collections.Counter(fn for (fn, b), fs in oby.items() if b and any(f.get('block') for f in fs))
    spans = collections.defaultdict(dict)   # the caller units (declarations) of each function, with their lines
    for (fn, unit), facts in oby.items():
        if unit and not any(f.get('block') or f.get('arm') for f in facts):
            spans[fn][unit] = unit_span(unit, facts)
    for (fn, block), facts in oby.items():
        if block in skip_blocks:
            continue
        cv = caller_vars(facts)
        cl = caller_locals(facts, cv, marks.lines)
        scope = scope_of(facts)
        kinds = sorted({k for f in facts for k in f.get('kinds', ())})
        family = '2D' if any(not kmirror.get(kind_of(k), True) for k in kinds) else 'scene'
        is_decl = block in spans.get(fn, {})
        is_arm = bool(block) and not is_decl and any(f.get('arm') for f in facts)
        paired = False
        for g, srcs0 in pairs.get(fn, {}).items():
            scoped = ([(s, None) for s in decl_sources(srcs0, block, spans[fn])] if is_decl
                      else A.arm_scope(g, srcs0, facts, passes) if is_arm
                      else cite_scope(srcs0, block, facts, n_blocks[fn], kinds))
            if not scoped:
                continue
            paired = True
            srcs = [s for s, _fl in scoped]
            only = block_pair_fields(kinds, g, scoped, kgames)
            if only == ():
                continue
            if only is not None and PLANT != 'no_class_arm':   # an arm whose condition names g's family (`pass class X`, X a class g's row / kind names) stands against it
                only = set(only) | {f['field'] for f in facts if f.get('classes') and set(f['classes']) & tclasses.get(g, set())}
            gvals = collections.defaultdict(list)
            partial = {}
            for fld, v, at in game_values(gby[g], units, pair_mirror(kinds, g, kgames, kmirror)):
                gvals[fld].append((v, at))
                a = int(at.split()[0], 16)
                for lo, hi, why in adds.get(g, ()):
                    if lo <= a < hi:
                        partial[fld] = why
            kept = [f for f in facts if keep_for(f, g, tclasses, cites, sets, game_callers, passes) and kind_arm(f, g, kgames)]
            pscope = scope_of(kept) if is_decl and PLANT != 'one_unit' else scope   # a caller unit: the fields it states for THIS pair
            ovals = collections.defaultdict(list)
            no_test = []   # ours turns its alpha test off by func 7 (ALWAYS, AlphaDynamic's untested arm): an off test, not a func
            for f in kept:
                val = ours_value(f, units, cv, cl)
                if f['field'] == 'alpha_func' and val == 7 and PLANT != 'always_func':
                    no_test.append(f)
                    continue
                ovals[f['field']].append((val, f))
            untested = bool(no_test) and not ovals.get('alpha_func')
            if any(f['kind'] == 'default' and v == 0 for v, f in ovals.get('blend_enable', ())) and PLANT != 'factor_defaults':
                for d in DERIVED:   # the DrawState zero is blend OFF with zero factors: a factor the blend-on path sets again goes with the blend-off row
                    if any(f['kind'] != 'default' for _v, f in ovals.get(d, ())):
                        ovals[d] = [(v, f) for v, f in ovals[d] if f['kind'] != 'default']
            has_code = bool(ovals.get('blend_code'))
            for fld in FIELDS:
                if only is not None and fld not in only:
                    continue
                gl, ol = gvals.get(fld, []), ovals.get(fld, [])
                if fld == 'blend_code' and not has_code and ol == []:
                    continue   # the game's blend code stands against our op / src / dst
                if not gl and not ol:
                    continue
                if not ol and fld not in pscope:
                    continue
                if fld in DERIVED and not gl and not ol:
                    continue
                en = MOOT.get(fld)
                moot = None
                if en:
                    ge = [v for v, _a in gvals.get(en, [])]
                    oe = [v for v, _f in ovals.get(en, [])]
                    if _all_zero(ge):
                        moot = f'moot: the game sets {en} off here (the {en} row carries any difference)'
                    elif _all_zero(oe):
                        moot = f'moot: ours sets {en} off here (the {en} row carries any difference)'
                    elif en == 'alpha_test' and untested:
                        moot = 'moot: ours tests no alpha here (its func is 7 = ALWAYS on every arm: the alpha_test row carries any difference)'
                dead = [f['dead'] for _v, f in ol if f.get('dead')]
                if dead and PLANT != 'dead_rows':   # S111 PARTZ2: a kTable row no draw reads (q ostate live_rows) states nothing
                    moot = f'moot: no draw reads this kTable row ({dead[0]})'
                if moot:
                    verdict, why, extra = 'OK', moot, set()
                elif ol and any(f.get('left') for _v, f in ol) and PLANT != 'left_skip':
                    verdict, why, extra = 'UNSETTLED', LEFT_WHY, set()
                else:
                    verdict, why, extra = judge(fld, [v for v, _a in gl], [v for v, _f in ol], partial.get(fld))
                inh = None
                if verdict == 'OURS-ONLY' and inherit is not None:   # S111 GINHERIT: the game sets it before this function runs
                    verdict, why, extra, inh = inherit(g, fld, [v for v, _f in ol], [v for v, _a in gvals.get(en, [])] if en else None)
                row = {'fn': fn, 'block': block, 'game': g, 'field': fld, 'verdict': verdict, 'why': why, 'pair': srcs,
                       'ours': [{'value': show(fld, v), 'cond': f.get('cond'), 'at': f['at'], 'kind': f['kind']} for v, f in ol],
                       'game_vals': [{'value': show(fld, v), 'at': a} for v, a in gl],
                       'family': family}
                if inh:
                    row['inherited'] = inh
                if verdict in FAILING:
                    _departs(row, fld, ol, extra, facts, marks)
                rows.append(row)
        if is_decl and not paired and PLANT != 'left_skip':   # a draw of a caller with no game function cited by it: said, not skipped
            left = any(f.get('left') for f in facts)
            for fld in sorted({f['field'] for f in facts}, key=FIELDS.index):
                ol = [f for f in facts if f['field'] == fld]
                rows.append({'fn': fn, 'block': block, 'game': NO_PAIR, 'field': fld, 'verdict': 'UNSETTLED',
                             'why': (LEFT_WHY + '; ' if left else '') + 'no game function is cited next to this draw (its declaration / stores): not compared',
                             'pair': [], 'ours': [{'value': show(fld, ours_value(f, units, cv, cl)), 'cond': f.get('cond'), 'at': f['at'], 'kind': f['kind']} for f in ol],
                             'game_vals': [], 'family': family})
    if callers:
        cnt = callers(sorted({r['fn'] for r in rows}))
        for r in rows:
            r['callers'] = cnt.get(r['fn'], 0)
    rows.sort(key=lambda r: (RANK[r['verdict']], r['family'] != 'scene', -r.get('callers', 0), r['fn'], r['block'] or '', r['game'], FIELDS.index(r['field'])))
    return rows


def _departs(row, fld, ol, extra, facts, marks):
    """a DIFF / GAME-ONLY row -> DEPARTED when every blamed fact of ours carries an accepted DEPARTS marker"""
    if ol:
        blame = [f for v, f in ol if v in extra] or [f for v, f in ol if v != DEFER]
        sites = [(f['at'], f) for f in blame]
    else:
        lines = sorted({f['block'] for f in facts if f.get('block')} or {f['at'] for f in facts})
        sites = [(w, None) for w in lines]
    got, refused = [], []
    for where, f in sites:
        mk, why = marks.at(where)
        if mk and mk.get('games') and row['game'] not in mk['games'] and PLANT != 'no_marker_scope':
            mk, why = None, f"scoped to {', '.join(mk['games'])}: not this pair (vs {row['game']})"
        if mk and mk['class'] == 'mod' and not marks.behind_switch((f or {}).get('cond')):
            mk, why = None, f"a mod departure on a fact not behind the mod's switch (its condition {(f or {}).get('cond')!r} names no mod cvar / reader)"
        if why:
            refused.append(f'{where}: {why}')
        if mk:
            got.append((where, mk))
    if ol and len(got) == len(sites) and sites:
        row['verdict'] = 'DEPARTED'
    elif not ol and got:
        row['verdict'] = 'DEPARTED'
    if got:
        row['departs'] = [{'at': w, **mk} for w, mk in got]
    if refused:
        row['departs_refused'] = refused


# ------------------------------------------------------------------ the inputs of a tree
def ref_root(ref):
    """a git ref's src + db as files (q pairs reads files): git archive into the temp folder, once per commit"""
    if ref.startswith('-'):
        raise CodediffError(f'--ref {ref!r}: a REF starting with - would reach git as an option')
    r = subprocess.run(['git', 'rev-parse', '--verify', ref + '^{commit}'], cwd=REPO, capture_output=True, text=True)
    if r.returncode:
        raise CodediffError(f'--ref {ref}: {r.stderr.strip()}')
    sha = r.stdout.strip()
    root = os.path.join(tempfile.gettempdir(), 'wwe_codediff_ref', sha[:12])
    done = os.path.join(root, '.extracted')
    if not os.path.exists(done):
        os.makedirs(root, exist_ok=True)
        tar = os.path.join(root, '..', sha[:12] + '.tar')
        a = subprocess.run(['git', 'archive', '--format=tar', '-o', tar, sha, 'src', 'db'], cwd=REPO, capture_output=True, text=True)
        if a.returncode:
            raise CodediffError(f'git archive {sha[:12]}: {a.stderr.strip()[:200]}')
        with tarfile.open(tar) as t:
            t.extractall(root, filter='data')
        os.remove(tar)
        open(done, 'w').close()
    return root


def pair_rows_for(fns, root=None):
    import qpairs
    con, _info = qpairs.update(root)
    try:
        return {fn: [qpairs._rowd(r) for r in qpairs.by_ours(con, fn)] for fn in fns if not fn.startswith('pass_state::')}
    finally:
        con.close()


def game_facts():
    import q as Q
    import qgstate
    con = Q.connect(Q.DB)
    idx = qgstate.load_index(con, log=lambda m: print(m, file=sys.stderr))
    return qgstate.facts(idx)


def game_callers_of():
    """sub_X -> [sub_Y] calling it (the q db's call / tail edges), cached"""
    import q as Q
    con = Q.connect(Q.DB)
    cache = {}

    def callers(g):
        if g not in cache:
            a = int(g[4:], 16)
            cache[g] = [f'sub_{s:08X}' for (s,) in con.execute("SELECT DISTINCT src FROM edges WHERE dst = ? AND kind IN ('call', 'tail')", (a,))]
        return cache[g]
    return callers


def switch_files_for(ref):
    def find(cvars):
        if not cvars:
            return []
        args = ['git', 'grep', '-l', '-w'] + sum((['-e', c] for c in cvars), []) + ([ref] if ref else []) + ['--', 'src']
        r = subprocess.run(args, cwd=REPO, capture_output=True, text=True, encoding='utf-8', errors='replace')
        return [ln.split(':', 1)[1] if ref else ln for ln in r.stdout.splitlines() if ln.strip()]
    return find


def callers_count(fns):
    """{fn: our call sites} by `q ours callers NAME` (4 at once)"""
    from concurrent.futures import ThreadPoolExecutor

    def one(fn):
        if fn.startswith('pass_state::'):
            return fn, 0
        r = subprocess.run([sys.executable, os.path.join(HERE, 'q.py'), 'ours', 'callers', fn], cwd=REPO, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', timeout=120)
        m = re.search(r'(\d+) call / reference sites', r.stdout)
        return fn, int(m.group(1)) if m else 0
    with ThreadPoolExecutor(max_workers=4) as ex:
        return dict(ex.map(one, fns))


def inherit_hook(units):
    """the OURS-ONLY judge against the inherited state (tools/q/qgstate_inherit.py codediff_hook, codediff's own judge and units)"""
    import qgstate_inherit
    return qgstate_inherit.codediff_hook(units, judge, MOOT, DERIVED)


def collect(ref=None, files=None, rank=True, inherit=True):
    """-> (rows, problems, info) for the tree at ref (None = the working tree); files: only our functions with a fact in these files"""
    ours, problems, counts = O.collect(ref)
    keep = None
    if files is not None:   # the whole tree is joined (a kTable row's classes scope the blocks), the rows kept are the touched functions'
        fs = set(files)
        keep = {f['fn'] for f in ours if f['at'].rpartition(':')[0] in fs or (f.get('block') or '').rpartition(':')[0] in fs}
    read = O.default_reader(ref)
    try:
        ps = read(O.PS)
    except (OSError, O.OstateError):
        ps = None
        problems.append(f'{O.PS}: not read; ForwardZ not applied')
    units = Units(ps)
    marks = Marks(read, switch_files_for(ref))
    fns = sorted({f['fn'] for f in ours})
    prs = pair_rows_for(fns, ref_root(ref) if ref else None)
    skip, p2 = R.ours_only(read)
    problems += p2
    rows = run(ours, game_facts(), prs, units, marks, game_callers_of(), callers_count if rank else None,
               {f'{rel}:{ln}': why for rel, ln, why in skip},
               inherit_hook(units) if inherit and PLANT != 'no_inherit' else None, A.pass_techniques(read))
    if keep is not None:
        rows = [r for r in rows if r['fn'] in keep]
        fns = sorted(keep)
    return rows, problems, {'ours_facts': len(ours), 'functions': len(fns), **counts}


# ------------------------------------------------------------------ the check.py row
PAIR_DB = ('db/claims/', 'db/seams/')   # q pairs' claim / seam-why rows: a db edit there can pair our code with another game function


def new_failing(now, was):
    """the failing rows of `now` whose (our function, game function, field) does not fail in `was` (block lines move between trees)"""
    old = {(r['fn'], r['game'], r['field']) for r in was if r['verdict'] in FAILING}
    return [r for r in now if r['verdict'] in FAILING and (PLANT == 'db_all_fail' or (r['fn'], r['game'], r['field']) not in old)]


def _fail_line(r):
    at = ', '.join(sorted({o['at'] for o in r['ours']})) or 'no fact of ours'
    return (f"{r['verdict']} {r['fn']} vs {r['game']} {r['field']}: {r['why']} (ours @{at}; a departure: `// DEPARTS: <class> <FILE>:<row>; <why>` there)"
            + (f" [refused: {r['departs_refused'][0]}]" if r.get('departs_refused') else ''))


def check(root, changed, base=None):
    """check.py `codediff`: -> (failures, judged rows, what) for the diff: the pairs of the functions with a fact in its src files
    (every DIFF / GAME-ONLY not DEPARTED), and, when it edits a pair source of q pairs (db/claims, db/seams) and the base is
    given, the whole tree's DIFF / GAME-ONLY rows that the base tree (its src + db, git archive) does not have"""
    src = sorted(p for p in changed if p.startswith('src/') and p.endswith(('.h', '.cpp', '.inc', '.inl', '.hpp')))
    dbs = sorted(p for p in changed if p.startswith(PAIR_DB))
    if not src and not (dbs and base):
        return [], 0, 'no src file and no db/claims / db/seams file in the diff'
    fails, judged, what = [], 0, []
    if src:
        read = O.default_reader(None)
        marks = Marks(read, switch_files_for(None))
        for rel in src:
            for ln, why in marks.scan(rel):
                fails.append(f'{rel}:{ln}: DEPARTS marker refused: {why}')
        rows, problems, info = collect(None, files=src, rank=False)
        fails += [_fail_line(r) for r in rows if r['verdict'] in FAILING]
        judged += len(rows)
        what.append(f"{info['functions']} function(s) of ours with a fact in {len(src)} src file(s) of the diff" + (f'; problems: {problems[0]}' if problems else ''))
    if dbs and base:
        now = collect(None, rank=False)[0]
        was = collect(base, rank=False)[0]
        fails += ['(a pair source in ' + ', '.join(dbs[:2]) + ') ' + _fail_line(r) for r in new_failing(now, was)]
        judged += len(now)
        what.append(f'{len(dbs)} db pair-source file(s) of the diff: the whole tree against the base {base[:10]}')
    return fails, judged, '; '.join(what)


# ------------------------------------------------------------------ the entry
def select(rows, fn=None, game=None, field=None, flags=None):
    want = O.ALIASES.get(field, field) if field else None
    out = []
    for r in rows:
        if want and r['field'] != want:
            continue
        if fn and not (fn == r['fn'] or fnmatch.fnmatchcase(r['fn'], fn) or r['fn'].endswith('::' + fn)):
            continue
        if game and r['game'].lower() != ('sub_' + game.lower().replace('sub_', '').replace('0x', '')):
            continue
        if flags and r['verdict'] not in flags:
            continue
        out.append(r)
    return out


def row_text(r):
    head = (f"{r['verdict']:10s} {r['fn']}" + (f" (block {r['block']})" if r.get('block') else '') + f" vs {r['game']}  {r['field']}  [{r['family']}"
            + (f", {r['callers']} call sites" if 'callers' in r else '') + ']')
    lines = [head, f"           {r['why']}" if r['why'] else None]
    lines.append('           game: ' + ('; '.join(f"{g['value']} @{g['at']}" for g in r['game_vals']) or 'never sets it'))
    if r.get('inherited'):
        inh = r['inherited']
        lines.append(f"           inherited {inh['field']}: " + ('; '.join(inh.get('paths', ())[:3]) or inh.get('via', '')) + (f" (+{inh['n_paths'] - 3} paths)" if inh.get('n_paths', 0) > 3 else ''))
    lines.append('           ours: ' + ('; '.join(f"{o['value']} @{o['at']} ({o['kind']})" + (f" when {o['cond']}" if o['cond'] else '') for o in r['ours']) or 'never sets it'))
    for d in r.get('departs', ()):
        lines.append(f"           DEPARTS {d['class']} {d['file']}:{d['ref']} @{d['at']}: {d['why']}")
    for d in r.get('departs_refused', ()):
        lines.append(f'           DEPARTS refused: {d}')
    lines.append('           pair: ' + ', '.join(r['pair'][:4]) + (f' (+{len(r["pair"]) - 4})' if len(r['pair']) > 4 else ''))
    return '\n'.join(x for x in lines if x)


def main(argv):
    fld = next((argv[i + 1] if v == '--field' and i + 1 < len(argv) else v.split('=', 1)[-1] for i, v in enumerate(argv)
                if v == '--field' or v.startswith('--field=')), None)
    if fld == 'levels':     # S111 LEVELSDIFF: the texture level extractor, its own sources and join (tools/q/qlevels.py)
        import qlevels
        return qlevels.main(argv)
    ap = argparse.ArgumentParser(prog='q codediff', description=__doc__.split('\n')[0])
    if '--field' in argv[:-1] and argv[argv.index('--field') + 1] == 'reads':    # S111 FIELDSDIFF: the fields-read rows (q fieldsdiff)
        import qfieldsdiff
        i = argv.index('--field')
        return qfieldsdiff.main(argv[:i] + argv[i + 2:])
    ap.add_argument('--fn', help='one of our functions (qualified name, a glob, the last name part)')
    ap.add_argument('--game', help='one game function sub_X')
    ap.add_argument('--field', help="a field (gstate's key, or atest / afunc / aref / zfunc ...)")
    ap.add_argument('--flag', nargs='+', choices=VERDICTS, help='only these verdicts')
    ap.add_argument('--ref', help='our code (and its pairs) at this git ref')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-rank', action='store_true', help='skip the call-site count (q ours callers per function)')
    ap.add_argument('--no-inherit', action='store_true', help='leave OURS-ONLY rows unjudged (no q gstate --inherit walk)')
    a = ap.parse_args(argv)
    rows, problems, info = collect(a.ref, rank=not a.no_rank, inherit=not a.no_inherit)
    sel = select(rows, a.fn, a.game, a.field, a.flag)
    tot = collections.Counter(r['verdict'] for r in rows)
    if a.json:
        print(json.dumps({'ref': a.ref or 'the working tree', 'rows': sel, 'totals': dict(tot), 'problems': problems, 'info': info}, indent=1, default=str))
        return 2 if problems else 0
    for r in sel:
        print(row_text(r))
    print(f"totals ({a.ref or 'the working tree'}): {len(rows)} rows over {len({(r['fn'], r['game']) for r in rows})} pairs; "
          + ', '.join(f'{v} {tot.get(v, 0)}' for v in VERDICTS) + (f'; shown {len(sel)}' if len(sel) != len(rows) else ''))
    print(f"sources: q ostate {info['ours_facts']} facts in {info['functions']} functions; q gstate --facts; q pairs (precise rows), kTable addr cells, rstdiff KINDS")
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (CodediffError, O.OstateError) as e:
        print(f'q codediff: {e}', file=sys.stderr)
        sys.exit(2)
