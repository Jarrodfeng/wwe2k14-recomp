"""Companion blocks of a builder pack (BPK round 2, 2026-11-20): code a fix predictably touches beyond the note's own sites.

Each companion block carries the rule that made it, so the refind can split hits and bytes per rule:
  head     every cited code file's head: the include block and file-scope declarations up to its first function (<= HEAD_MAX lines);
           new includes and helpers go there
  plants   a planted-fault table (`*_plants.h`) the cited file includes, or one beside it with the same name prefix (<= PLANTS_MAX lines)
  readers  every other place that names a constant / enumerator / field / counter the note names beside a citation (k..., ALL_CAPS,
           snake_case, member_), 3 lines either side, <= READERS_MAX places a name; a name in more than GENERIC_FILES files is skipped
  unit     tools\\ unit / selftest / rig sources that name a cited file (directly or through the build script that compiles it) or a cited
           symbol: the test functions that mention a cited symbol (whole up to 60 lines, else 10 lines either side), else the file's head
           and its main / selftest function; <= UNIT_FILES files, <= UNIT_LINES lines a file
  lane     the gate lane json files that name those units or the cited files (<= LANE_FILES files, <= LANE_LINES lines each)
Only files git tracks at the target revision are read (tree.grep / tree.read): never generated\\, game_data\\, out\\, thirdparty\\.
Where the symbols come from: a text search of the TARGET tree (git grep at that revision), not the q ours index: q's index is today's
code, and a pack is often built for an older tree (q cannot answer refs / covers at a past revision: logged with q missed).
"""
import os
import re

HEAD_MAX = 40
PLANTS_MAX = 120
READERS_CTX = 3
READERS_MAX = 12
READERS_TOTAL = 100      # places across all names in one pack (round 3: a total cap; it does not bind on the 8 refind pairs' packs)
GENERIC_FILES = 40
UNIT_FILES = 6
UNIT_LINES = 120
UNIT_FN_MAX = 60
LANE_FILES = 3
LANE_LINES = 40

CONST_RX = re.compile(r'^(?:k[A-Z]\w+|[A-Z][A-Z0-9]*_[A-Z0-9_]+|[a-z][a-z0-9]*(?:_[a-z0-9]+)+_?|[a-z][a-z0-9]+_)$')
GENERIC = {'Record', 'Append', 'Finish', 'Reset', 'Get', 'Set', 'Put', 'Read', 'Write', 'Init', 'Update', 'Draw', 'Apply', 'Collect', 'Find',
           'Slot', 'main', 'Run', 'Check', 'Load', 'Store', 'Make', 'Build', 'size', 'count', 'have', 'frame', 'state'}
CODE = ('h', 'hpp', 'hh', 'c', 'cc', 'cpp', 'cxx', 'inl', 'inc', 'frag', 'vert', 'comp', 'glsl', 'py')


class CompList(list):
    """the companion blocks, plus .notes: what a rule skipped or cut (printed in the pack header)"""
    def __init__(self, *a):
        super().__init__(*a)
        self.notes = []


def _tail(n):
    return re.split(r'::|\.', n)[-1]


def _ext(p):
    return p.rsplit('.', 1)[-1].lower() if '.' in p else ''


class Taken:
    """line ranges already shown, per file: a companion span keeps only the parts no earlier block holds"""
    def __init__(self, blocks):
        self.r = {}
        for b in blocks:
            self.add(b.file, b.start, b.end)

    def add(self, f, a, b):
        self.r.setdefault(f, []).append((a, b))

    def holds(self, f, ln):
        return any(a <= ln <= b for a, b in self.r.get(f, []))

    def cut(self, f, lo, hi):
        """[lo, hi] minus the taken ranges -> pieces of >= 2 lines"""
        pieces = [(lo, hi)]
        for a, b in sorted(self.r.get(f, [])):
            nxt = []
            for x, y in pieces:
                if b < x or a > y:
                    nxt.append((x, y))
                    continue
                if x < a:
                    nxt.append((x, a - 1))
                if b < y:
                    nxt.append((b + 1, y))
            pieces = nxt
        return [(x, y) for x, y in pieces if y - x >= 1]


# The rules a pack uses by default. `lane` is OFF: on the DEV pairs (no cap) it added 43.9 KB and held 0 fix hunks (2026-11-20, round 2:
# "drop a rule that adds bytes and no hits"); it stays callable (rules=ALL) and in the selftest.
ENABLED = ('head', 'plants', 'readers', 'unit')
ALL = ('head', 'plants', 'readers', 'unit', 'lane')


def companions(P, sites, blocks, target, rules=None):
    """-> [Block] with .rule and .why, in rule order head, plants, readers, unit, lane"""
    rules = ENABLED if rules is None else rules
    out = CompList()
    notes = out.notes
    taken = Taken(blocks)
    files = []
    for b in blocks:
        if b.file not in files and _ext(b.file) in CODE:
            files.append(b.file)

    def add(rule, f, lo, hi, why, anchors=()):
        if rule not in rules:
            return 0
        lines = target.read(f)
        if lines is None:
            return 0
        lo, hi = max(1, lo), min(len(lines), hi)
        n = 0
        for x, y in taken.cut(f, lo, hi):
            blk = P.Block(f, x, y)
            blk.rule, blk.why = rule, why
            blk.cited = {a for a in anchors if x <= a <= y}
            out.append(blk)
            taken.add(f, x, y)
            n += y - x + 1
        return n

    # a. head
    for f in files:
        lines = target.read(f) or []
        syms = P.symbols(f, lines)
        fn = [s.start for s in syms if s.kind == 'function']
        end = min(min(fn) - 1 if fn else len(lines), HEAD_MAX, len(lines))
        if end >= 3:
            add('head', f, 1, end, 'the file head (includes, file-scope declarations) of a cited file')

    # the names: beside the citations (in note order) and the enclosing symbols of the cited blocks
    beside = []
    for s in sorted(sites, key=lambda s: s.order):
        if s.status != 'ok':
            continue
        for nm in s.names:
            t = _tail(nm)
            if t not in beside:
                beside.append(t)
    enclosing = []
    for b in blocks:
        for nm in b.names:
            t = _tail(nm)
            if t not in enclosing:
                enclosing.append(t)
    consts = [n for n in beside if CONST_RX.match(n) and len(n) >= 4]
    symbols_ = [n for n in dict.fromkeys(beside + enclosing) if len(n) >= 5 and n not in GENERIC and not n.startswith('sub_')]
    bases = [f.rsplit('/', 1)[-1] for f in files]
    words = list(dict.fromkeys(consts + symbols_ + bases))
    hits = target.grep(words) if words else []
    by_word = {}
    rx = {w: re.compile(r'(?<![\w])' + re.escape(w) + r'(?![\w])') for w in words}
    for p, ln, text in hits:
        for w in words:
            if w in text and rx[w].search(text):
                by_word.setdefault(w, []).append((p, ln))
    nfiles = {w: len({p for p, _ in v}) for w, v in by_word.items()}

    # e. plants
    paths = target.paths()
    for f in files:
        lines = target.read(f) or []
        d, base = (f.rsplit('/', 1) + [''])[:2] if '/' in f else ('', f)
        found = []
        for l in lines[:120]:
            m = re.match(r'\s*#\s*include\s+"([^"]*_plants\.h)"', l)
            if m:
                inc = m.group(1).split('/')[-1]
                c = [p for p in paths if p.endswith('/' + inc) and p.rsplit('/', 1)[0] == d] or [p for p in paths if p.endswith('/' + inc)]
                if len(c) == 1:
                    found.append(c[0])
        prefix = base.split('_')[0].split('.')[0]
        for p in paths:
            if p.rsplit('/', 1)[0] == d and p.endswith('_plants.h') and p.rsplit('/', 1)[-1].startswith(prefix + '_') and p not in found:
                found.append(p)
        for p in found:
            if p != f:
                add('plants', p, 1, PLANTS_MAX, f'the planted-fault table of {base}')

    # d. readers of the constants / fields / counters the note names beside a citation. Nothing is cut silently: a name in too many
    # files or on the stop list is listed in the header as skipped, a name with more places than shown says "N more places not shown"
    total = 0
    for w in consts:
        if 'readers' not in rules:
            break
        locs = [(p, ln) for p, ln in by_word.get(w, []) if not taken.holds(p, ln)]
        if not locs:
            continue
        if w in GENERIC or w.lower() in {g.lower() for g in GENERIC}:
            notes.append(f'readers: `{w}` skipped (a stop-list name)')
            continue
        if nfiles.get(w, 0) > GENERIC_FILES:
            notes.append(f'readers: `{w}` skipped (named in {nfiles[w]} files, over {GENERIC_FILES})')
            continue
        locs.sort(key=lambda x: (0 if x[0].startswith('src/') else 1, x[0], x[1]))
        room = max(0, min(READERS_MAX, READERS_TOTAL - total))
        shown = locs[:room]
        if not shown:
            notes.append(f'readers: `{w}` skipped ({len(locs)} places; the pack\'s total of {READERS_TOTAL} reader places is used up)')
            continue
        total += len(shown)
        more = len(locs) - len(shown)
        for k, (p, ln) in enumerate(shown):
            last = k == len(shown) - 1
            why = f'names `{w}` (named beside a citation; place {k + 1} of {len(locs)})' + (f'; {more} more places not shown' if last and more else '')
            add('readers', p, ln - READERS_CTX, ln + READERS_CTX, why, anchors=(ln,))
        if more:
            notes.append(f'readers: `{w}` {len(shown)} of {len(locs)} places shown ({more} more places not shown)')

    # b. units: tools\ sources that name a cited file (or its build script does) or a cited symbol
    cited = set(files)
    ref_base, ref_sym = {}, {}
    for b in bases:
        for p, ln in by_word.get(b, []):
            if p.startswith('tools/') and p not in cited:
                ref_base.setdefault(p, set()).add(b)
    for p in list(ref_base):
        if p.endswith('.ps1'):     # a build script that compiles the cited file: the unit sources it names
            for l in target.read(p) or []:
                for m in re.finditer(r'([\w./\\-]+\.(?:cpp|py))', l):
                    tok = m.group(1).replace('\\', '/').split('/')[-1]
                    c = [q for q in paths if q.startswith('tools/') and q.endswith('/' + tok)]
                    if len(c) == 1 and c[0] not in cited:
                        ref_base.setdefault(c[0], set()).update(ref_base[p])
    for w in symbols_:
        if nfiles.get(w, 0) > GENERIC_FILES:
            continue
        for p, ln in by_word.get(w, []):
            if p.startswith('tools/') and p not in cited:
                ref_sym.setdefault(p, {}).setdefault(w, []).append(ln)
    cands = [p for p in set(ref_base) | set(ref_sym) if _ext(p) in CODE and '/gate/lanes/' not in p]
    cands.sort(key=lambda p: (-len(ref_sym.get(p, {})), -len(ref_base.get(p, ())), p))
    units = cands[:UNIT_FILES]
    for u in units:
        lines = target.read(u) or []
        syms = P.symbols(u, lines)
        budget = UNIT_LINES
        hit_lines = sorted({ln for v in ref_sym.get(u, {}).values() for ln in v})
        names = sorted(ref_sym.get(u, {}))
        if hit_lines:
            seen = set()
            for ln in hit_lines:
                if budget <= 0:
                    break
                s = P.enclosing(syms, ln, ln)
                if s and s.kind == 'function' and s.end - s.start + 1 <= UNIT_FN_MAX:
                    lo, hi = s.start, s.end
                else:
                    lo, hi = ln - 10, ln + 10
                    if s:
                        lo, hi = max(lo, s.start), min(hi, s.end)
                if (lo, hi) in seen:
                    continue
                seen.add((lo, hi))
                hi = min(hi, lo + budget - 1)
                budget -= add('unit', u, lo, hi, f'a unit / test naming {", ".join("`" + n + "`" for n in names[:4])}', anchors=hit_lines)
        else:
            fn = [s.start for s in syms if s.kind == 'function']
            end = min(min(fn) - 1 if fn else len(lines), HEAD_MAX)
            budget -= add('unit', u, 1, end, f'a unit that compiles {", ".join(sorted(ref_base.get(u, ())))}: its head')
            mains = [s for s in syms if s.kind == 'function' and _tail(s.name) in ('main', 'selftest', 'Selftest', 'SelfTest', 'run_selftest')]
            for s in mains[:1]:
                add('unit', u, s.start, min(s.end, s.start + min(HEAD_MAX, max(budget, 0)) - 1), 'its main / selftest (the test table)')

    # c. the lane json files naming those units or the cited files
    stems = list(dict.fromkeys([u.rsplit('/', 1)[-1].rsplit('.', 1)[0] for u in units] + bases))
    if stems:
        lh = target.grep(stems, prefixes=('tools/gate/lanes',))
        score = {}
        for p, ln, text in lh:
            for s_ in stems:
                if s_ in text:
                    score.setdefault(p, set()).add(s_)
        lanes = sorted(score, key=lambda p: (-len(score[p]), p))[:LANE_FILES]
        for p in lanes:
            add('lane', p, 1, LANE_LINES, f'a gate lane naming {", ".join(sorted(score[p])[:4])}')
    return out
