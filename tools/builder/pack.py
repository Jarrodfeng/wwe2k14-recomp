"""Builder pack: turn a probe's note into one Markdown file holding the code at every site the note cites (BPK, 2026-11-20).

    py -I tools/builder/pack.py NOTE.md [NOTE.md ...] [--at REV] [--out FILE] [--max-kb N] [--no-q] [--no-bare]
    py -I tools/builder/pack.py --selftest

What it does (docs/agents/builder_pack.md):
  1. sites: every citation of OUR code in the notes (src\\x.h:109, forward slashes, ranges :117-122, lists :1472,1503, bare names
     rf_c_core.cpp:479 resolved by a unique basename under src\\ then tools\\, a bare `:573` continuing the file before it, `line N` / `(N-M)`
     after a file, a function named beside a file). The sentence each sits in and its heading are kept; sites under a "Fix" heading rank first.
  2. never game code: a citation into generated\\ or game_data\\ (listing 260.cpp:16920, recomp.151.cpp:10331) or a guest address
     (0x82......, sub_82......) is a POINTER only: the path / address and the q command that reads it. Its text is never read or copied.
  3. drift: each cited line is mapped from the note's commit (git log -1 on the note) to the target tree (--at, default HEAD) with difflib:
     "L109 -> L112" when it moved, "GONE" when the lines were rewritten (then the function the note names, or the one that held the lines).
  4. blocks: the enclosing function / struct (whole up to 80 lines, else 25 lines either side of the cited range); sites in one function merge.
  5. context per enclosing symbol from the q "ours" index (read-only, the main checkout's db): callers, builds (ON / OFF / tool), the gate
     lanes and units that cover the file, and the readers of a layout / constant the note says changes.
  6. one Markdown file: header (notes + commits, target, counts), contents in the note's order, then the blocks. Over --max-kb (default 120)
     the non-fix blocks shrink to 8 lines either side of their cited lines.
Packs are scratch: default out\\packs\\<note>.md in the main checkout. Never commit one.
"""
import argparse
import difflib
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)     # companions.py beside this file (py -I leaves the script folder off sys.path)
ROOT = os.path.dirname(os.path.dirname(HERE))

FULL_MAX = 80          # a whole enclosing symbol up to this many lines
WINDOW = 25            # else this many lines either side of the cited range
CLIP_LINES = 40        # S105 QB: the hard cap's last level (0 lines of context) clips one cited range to this many lines
HARD_LEVELS = (8, 4, 2, 1, 0)   # S105 QB: the hard cap's levels: lines of context either side of a cited line, fix blocks included
SHRINK = 8             # over the size cap: non-fix blocks keep this many lines either side
MERGE_GAP = 2          # blocks of one file this close merge

CODE_EXT = ('h', 'hpp', 'hh', 'c', 'cc', 'cpp', 'cxx', 'inl', 'inc', 'frag', 'vert', 'comp', 'glsl', 'hlsl', 'java', 'kt')
TEXT_EXT = ('py', 'ps1', 'psm1', 'json', 'toml', 'cmake', 'txt', 'md', 'bat', 'cmd', 'yaml', 'yml')
EXT = CODE_EXT + TEXT_EXT
LANG = {'h': 'cpp', 'hpp': 'cpp', 'hh': 'cpp', 'c': 'c', 'cc': 'cpp', 'cpp': 'cpp', 'cxx': 'cpp', 'inl': 'cpp', 'inc': 'cpp', 'frag': 'glsl',
        'vert': 'glsl', 'comp': 'glsl', 'glsl': 'glsl', 'hlsl': 'hlsl', 'py': 'python', 'ps1': 'powershell', 'psm1': 'powershell', 'json': 'json',
        'toml': 'toml', 'md': 'markdown', 'java': 'java', 'kt': 'kotlin', 'cmake': 'cmake', 'yaml': 'yaml', 'yml': 'yaml'}

_NUMS = r'\d+(?:\s*[-–]\s*\d+)?(?:\s*,\s*:?\d+(?:\s*[-–]\s*\d+)?)*'
FILE_RX = re.compile(r'(?<![\w./\\-])((?:[A-Za-z0-9_.\-]+[\\/])*[A-Za-z0-9_\-][A-Za-z0-9_.\-]*\.(?:' + '|'.join(EXT) + r'))(?![\w])'
                     r'(?::(' + _NUMS + r'))?')
CONT_RX = re.compile(r'(?<![\w.)\]:])(?<!\d):(' + _NUMS + r')(?![\w:])')
LINEWORD_RX = re.compile(r'\blines?\s+(' + _NUMS.replace(r'\s*,\s*:?', r'\s*(?:,|and)\s*') + r')')
PAREN_RX = re.compile(r'^\W{0,3}\((\d+\s*[-–]\s*\d+)\)')
ADDR_RX = re.compile(r'\b(0x8[23][0-9A-Fa-f]{6})\b')
SUB_RX = re.compile(r'\b(sub_8[23][0-9A-Fa-f]{6})\b')
NAME_RX = re.compile(r'`(~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*)(?:\(\))?`')
FIX_RX = re.compile(r'\bfix(?:es)?\b|\bsites the build will touch\b', re.I)   # "Fix", "Fix sites", "The fix", "Sites the build will touch"
LAYOUT_RX = re.compile(r'\b(layout|constant|static_assert|sizeof|readers? of|widen|bump)', re.I)
NOT_NAMES = {'true', 'false', 'nullptr', 'bool', 'int', 'uint32_t', 'uint64_t', 'float', 'void', 'const', 'auto', 'static', 'return', 'if',
             'else', 'for', 'while', 'switch', 'case', 'break', 'continue', 'struct', 'class', 'enum', 'namespace', 'inline', 'char'}
CONTROL = ('if', 'for', 'while', 'switch', 'catch', 'do', 'else', 'try', 'return', 'case', 'default', 'sizeof', 'decltype')


def _ranges(spec):
    out = []
    for part in re.split(r'\s*(?:,|and)\s*', spec or ''):
        part = part.strip().lstrip(':')
        if not part:
            continue
        m = re.match(r'(\d+)\s*[-–]\s*(\d+)$', part)
        if m:
            a, b = int(m.group(1)), int(m.group(2))
            out.append((min(a, b), max(a, b)))
        elif part.isdigit():
            out.append((int(part), int(part)))
    return out


# ---------------------------------------------------------------- trees

def git(*args, root=ROOT, check=True):
    r = subprocess.run(['git', '-C', root] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120)
    if check and r.returncode != 0:
        raise RuntimeError(f'git {" ".join(args)}: {r.stderr.strip()[:300]}')
    return r.stdout


class _CatFile:
    """one `git cat-file --batch` process for all reads (fast on Windows)"""
    def __init__(self, root):
        self.p = subprocess.Popen(['git', '-C', root, 'cat-file', '--batch'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def read(self, spec):
        self.p.stdin.write((spec + '\n').encode('utf-8'))
        self.p.stdin.flush()
        head = self.p.stdout.readline().decode('utf-8', 'replace').split()
        if len(head) < 3 or head[-1] == 'missing':
            return None
        size = int(head[2])
        data = self.p.stdout.read(size)
        self.p.stdout.read(1)
        return data.decode('utf-8', 'replace') if head[1] == 'blob' else None

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


_CAT = {}


def _cat(root):
    if root not in _CAT:
        _CAT[root] = _CatFile(root)
    return _CAT[root]


class GitTree:
    def __init__(self, rev, root=ROOT):
        self.root = root
        self.rev = git('rev-parse', rev + '^{commit}', root=root).strip()
        self.label = self.rev[:9]
        self._paths = None
        self._cache = {}

    # this tool's own invented fixtures are tracked: a real note's bare `widget.cpp` must never resolve into them (review 5)
    EXCLUDE = ('tools/builder/fixtures/',)

    def paths(self):
        if self._paths is None:
            self._paths = [p for p in git('ls-tree', '-r', '--name-only', self.rev, root=self.root).splitlines() if not p.startswith(self.EXCLUDE)]
        return self._paths

    def read(self, path):
        """only what git tracks at this revision can be read (hard rule 1: generated\\, game_data\\, out\\, thirdparty\\ are never tracked)"""
        if path not in self._cache:
            t = _cat(self.root).read(f'{self.rev}:{path}')
            self._cache[path] = None if t is None else t.splitlines()
        return self._cache[path]

    def grep(self, words, prefixes=('src', 'tools', 'probes')):
        """-> [(path, line, text)] of tracked files at this revision holding any of the words (whole words, fixed strings)"""
        words = [w for w in words if w]
        if not words:
            return []
        args = ['grep', '-n', '-w', '-I', '-F', '--full-name']
        for w in words:
            args += ['-e', w]
        out = git(*(args + [self.rev, '--'] + list(prefixes)), root=self.root, check=False)
        res = []
        pre = self.rev + ':'
        for line in out.splitlines():
            if line.startswith(pre):
                line = line[len(pre):]
            m = re.match(r'^(.*?):(\d+):(.*)$', line)
            if m and not m.group(1).startswith(self.EXCLUDE):
                res.append((m.group(1), int(m.group(2)), m.group(3)))
        return res

    def untracked_hits(self, base):
        return []


UNTRACKED = ('generated/', 'game_data/', 'out/', 'thirdparty/')


class DirTree:
    """a folder tree (the selftest fixtures). roots: first hit wins. alias {disk_prefix: shown_prefix}. Paths under UNTRACKED stand for what
    git does not track: never listed, read or grepped (only reported as untracked)."""
    def __init__(self, roots, alias=None, label='dir', hide=()):
        self.roots = roots
        self.alias = alias or {}
        self.label = label
        self.rev = None
        self._paths = None
        self._all = None
        self.hide = set(hide)      # paths this version of the tree does not have (a file added or renamed later)

    def _all_paths(self):
        if self._all is None:
            s = set()
            for r in self.roots:
                for dp, _, fs in os.walk(r):
                    for f in fs:
                        s.add(self._show(os.path.relpath(os.path.join(dp, f), r).replace('\\', '/')))
            self._all = sorted(s - self.hide)
        return self._all

    def untracked_hits(self, base):
        return [p for p in self._all_paths() if p.startswith(UNTRACKED) and p.rsplit('/', 1)[-1] == base]

    def grep(self, words, prefixes=('src', 'tools', 'probes')):
        rx = re.compile(r'(?<![\w])(?:' + '|'.join(re.escape(w) for w in words if w) + r')(?![\w])') if words else None
        res = []
        if rx is None:
            return res
        for p in self.paths():
            if not p.startswith(tuple(x + '/' for x in prefixes)):
                continue
            for i, l in enumerate(self.read(p) or [], 1):
                if rx.search(l):
                    res.append((p, i, l))
        return res

    def _show(self, rel):
        for a, b in self.alias.items():
            if rel.startswith(a):
                return b + rel[len(a):]
        return rel

    def _disk(self, rel):
        for a, b in self.alias.items():
            if rel.startswith(b):
                return a + rel[len(b):]
        return rel

    def paths(self):
        if self._paths is None:
            self._paths = [p for p in self._all_paths() if not p.startswith(UNTRACKED)]
        return self._paths

    def read(self, path):
        if path.startswith(UNTRACKED) or path in self.hide:
            return None
        for r in self.roots:
            p = os.path.join(r, self._disk(path))
            if os.path.isfile(p):
                with open(p, encoding='utf-8', errors='replace') as f:
                    return f.read().splitlines()
        return None


# ---------------------------------------------------------------- note parsing

class Note:
    def __init__(self, name, text, rev, tree, tree_note=''):
        self.name, self.text, self.rev, self.tree = name, text, rev, tree
        self.tree_note = tree_note      # why the note's tree is what it is (e.g. not committed: the target tree stands in)
        self.unmapped = False           # set by build() when the target tree stands in for the note's tree
        self.skipped_out = []


class Site:
    def __init__(self, **k):
        self.note = k['note']; self.order = k['order']; self.token = k['token']; self.lines = k['lines']
        self.heading = k['heading']; self.sentence = k['sentence']; self.fix = k['fix']; self.names = k['names']
        self.item_line = k['item_line']
        self.file = None; self.status = 'missing'; self.candidates = []
        self.drift = 'same'; self.mapped = list(self.lines); self.drift_note = ''; self.gone_regions = []
        self.cited_lines = {x for a, b in self.lines for x in range(a, b + 1)}
        self.old_file = None
        self.block = None
        self.clips = []      # S105 QB: cited ranges the last hard-cap level cut (review 1)


class Pointer:
    def __init__(self, key, text, cmd, where):
        self.key, self.text, self.cmd, self.where = key, text, cmd, where


def items_of(text):
    """split a note into items: list items, table rows, paragraphs; each with its heading path and first line number"""
    items, cur, heads, in_fence = [], None, [], False

    def flush():
        nonlocal cur
        if cur and cur['text'].strip():
            items.append(cur)
        cur = None
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip()
        if line.strip().startswith('```'):
            in_fence = not in_fence
            flush()
            continue
        m = re.match(r'^(#{1,6})\s+(.*)$', line) if not in_fence else None
        if m:
            flush()
            lvl = len(m.group(1))
            heads = [h for h in heads if h[0] < lvl] + [(lvl, m.group(2).strip())]
            items.append({'text': m.group(2).strip(), 'heads': list(heads), 'line': n, 'is_head': True})
            continue
        if not line.strip():
            flush()
            continue
        new = in_fence or re.match(r'^\s*(?:\d+[.)]|[-*+])\s', line) or line.lstrip().startswith('|')
        if new or cur is None:
            flush()
            cur = {'text': line.strip(), 'heads': list(heads), 'line': n, 'is_head': False}
        else:
            cur['text'] += ' ' + line.strip()
    flush()
    return items


def _is_pointer_path(p):
    q = p.replace('\\', '/')
    base = q.rsplit('/', 1)[-1]
    return (q.startswith(('generated/', 'game_data/')) or '/generated/' in q or '/game_data/' in q
            or re.match(r'^(?:recomp\.)?\d+\.(?:cpp|h)$', base) is not None or base.startswith('recomp.'))


_SENT_END = re.compile(r'(?<=[.;!?])\s+(?=[A-Z0-9`(*\[])|(?<=:)\s+(?=\d+\.\s)')


def sentence_at(t, pos, short=80, cap=700):
    """the sentence of item text t that holds position pos (plus the next one when it is short); a list number in front is kept"""
    cuts = [0] + [m.end() for m in _SENT_END.finditer(t)] + [len(t)]
    k = max(i for i, c in enumerate(cuts[:-1]) if c <= pos)
    a, b = cuts[k], cuts[k + 1]
    if b - a < short and k + 2 < len(cuts):
        b = cuts[k + 2]
    s = t[a:b].strip()
    lead = re.match(r'^\s*(\d+[.)])\s', t)
    if lead and a > 0:
        s = f'{lead.group(1)} ... {s}'
    return s if len(s) <= cap else s[:cap] + ' ...'


def parse_note(note, start_order=0):
    """-> (sites, pointers). Pointers are keyed so repeats merge."""
    sites, pointers = [], {}
    order = start_order
    last_file_in_section = None
    last_heads = None
    for it in items_of(note.text):
        heads = it['heads']
        sub = [h for h in heads if h[0] >= 2]
        hname = _short(' / '.join(h[1] for h in (sub or heads)), 140)
        fix = any(FIX_RX.search(h[1]) for h in heads if h[0] >= 2)   # the H1 title does not count
        if it['is_head']:
            last_file_in_section = None
            last_heads = heads
            continue
        if heads != last_heads:
            last_file_in_section = None
            last_heads = heads
        t = it['text']
        sentence = t if len(t) <= 700 else t[:700] + ' ...'
        # guest addresses and sub_X: pointers
        for rx, kind in ((SUB_RX, 'sub'), (ADDR_RX, 'addr')):
            for m in rx.finditer(t):
                k = m.group(1)
                kk = k.lower()
                if kk not in pointers:
                    cmd = f'py tools\\q\\q.py decomp {k}' if kind == 'sub' else f'py tools\\q\\q.py addr {k}'
                    pointers[kk] = Pointer(kk, k, cmd, [])
                pointers[kk].where.append(f'{note.name}:{it["line"]}')
        # file tokens, in text order, with their zones
        toks = []
        for m in FILE_RX.finditer(t):
            toks.append({'path': m.group(1), 'spec': m.group(2), 's': m.start(), 'e': m.end()})
        conts = [m for m in CONT_RX.finditer(t)]
        # a bare :N belongs to the last file token before it (same item), else the last file of the section
        events = sorted([('f', x['s'], x) for x in toks] + [('c', m.start(), m) for m in conts], key=lambda e: e[1])
        cur_file = last_file_in_section
        per = []      # (token dict, ranges, zone start, zone end)
        for i, (kind, pos, obj) in enumerate(events):
            if kind == 'f':
                rng = _ranges(obj['spec'])
                cur_file = obj
                per.append([obj, rng, pos])
            else:
                if any(x['s'] <= pos < x['e'] for x in toks):
                    continue          # part of a file:N token
                if cur_file is None:
                    continue
                rng = _ranges(obj.group(1))
                if cur_file in [p[0] for p in per]:
                    per.append([cur_file, rng, pos])
                else:     # continues a file from an earlier item of this section
                    per.append([cur_file, rng, pos])
        # zones: from this event to the next file token; names and "line N" / "(N-M)" in the zone
        for j, p in enumerate(per):
            tok, rng, pos = p
            nxt = [q[2] for q in per[j + 1:] if q[0] is not tok or q[2] > pos]
            zend = min(nxt) if nxt else len(t)
            prev = [q[2] for q in per[:j]]
            zstart = max(prev) if prev else 0
            zone_after = t[tok['e'] if tok['s'] == pos else pos:zend]
            if not rng and tok['s'] == pos:
                m = PAREN_RX.match(t[tok['e']:zend]) or None
                m2 = LINEWORD_RX.search(zone_after[:120])
                if m:
                    rng = _ranges(m.group(1))
                elif m2:
                    rng = _ranges(m2.group(1))
            names = []
            for m in NAME_RX.finditer(t[zstart:zend]):
                nm = m.group(1)
                if nm in NOT_NAMES or nm.lower().startswith('sub_') or FILE_RX.fullmatch(nm):
                    continue
                if nm not in names:
                    names.append(nm)
            p[1] = rng
            p.append(names)
        for tok, rng, pos, names in per:
            path = tok['path']
            if _is_pointer_path(path) or re.search(r'\blisting\s+$', t[max(0, tok['s'] - 12):tok['s']]):
                key = path.replace('\\', '/').lower()
                spec = ','.join(f'{a}-{b}' if a != b else str(a) for a, b in rng)
                subs = sorted({m.group(1) for m in SUB_RX.finditer(t)})
                cmd = ('q decomp of the function that holds it: ' + ', '.join(f'py tools\\q\\q.py decomp {s}' for s in subs[:4])) if subs else \
                    'a generated listing: name its function and read it with  py tools\\q\\q.py decomp sub_X'
                if key not in pointers:
                    pointers[key] = Pointer(key, path.replace('\\', '/') + (f':{spec}' if spec else ''), cmd, [])
                pointers[key].where.append(f'{note.name}:{it["line"]}')
                continue
            if path.replace('\\', '/').startswith(('out/', 'thirdparty/')):
                note.skipped_out.append(path.replace('\\', '/'))
                continue
            sites.append(Site(note=note, order=order, token=path, lines=rng, heading=hname, sentence=sentence_at(t, pos), fix=fix, names=names,
                              item_line=it['line']))
            order += 1
        if toks:
            last_file_in_section = toks[-1]
    return sites, list(pointers.values())


# ---------------------------------------------------------------- resolve + drift

def resolve(token, tree, bare=True):
    """-> (status, path, candidates). status ok | ambiguous | missing"""
    t = token.replace('\\', '/').lstrip('./')
    paths = tree.paths()
    pset = getattr(tree, '_pset', None)
    if pset is None:
        pset = set(paths)
        tree._pset = pset
    if '/' in t:
        if t in pset:
            return 'ok', t, []
        suf = [p for p in paths if p.endswith('/' + t)]
        if len(suf) == 1:
            return 'ok', suf[0], []
        if len(suf) > 1:
            return 'ambiguous', None, suf
        return 'missing', None, []
    if not bare:
        return 'missing', None, []
    base = getattr(tree, '_base', None)
    if base is None:
        base = {}
        for p in paths:
            base.setdefault(p.rsplit('/', 1)[-1], []).append(p)
        tree._base = base
    cands = [p for p in base.get(t, []) if not _is_pointer_path(p)]
    for pref in ('src/', 'tools/'):
        c = [p for p in cands if p.startswith(pref)]
        if len(c) == 1:
            return 'ok', c[0], []
        if len(c) > 1:
            return 'ambiguous', None, c
    if len(cands) == 1:
        return 'ok', cands[0], []
    if len(cands) > 1:
        return 'ambiguous', None, cands
    un = tree.untracked_hits(t)
    if un:
        return 'untracked', None, un      # exists only where git tracks nothing (generated\, out\, thirdparty\ ...): a pointer, never text
    return 'missing', None, []


_MAPS = {}


def line_map(old, new):
    """old line (1-based) -> new line, for lines inside equal runs; others absent"""
    key = (id(old), id(new))
    hit = _MAPS.get(key)
    if hit is not None and hit[0] is old and hit[1] is new:     # the entry holds its lists: an id reused by other lists never matches
        return hit[2]
    m = {}
    sm = difflib.SequenceMatcher(None, old, new, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            for k in range(i2 - i1):
                m[i1 + k + 1] = j1 + k + 1
    _MAPS[key] = (old, new, m)
    return m


def _r(a, b):
    return f'L{a}' + (f'-{b}' if b != a else '')


def apply_drift(site, note_tree, target, old_path=None):
    """map the cited lines from the note's tree to the target. Never claims a move it has not established: a line inside a rewritten
    run is GONE (its region printed), a range with some lines gone is "partly gone", a file the note's tree lacks is "not mapped"."""
    site.cited_lines = {x for a, b in site.lines for x in range(a, b + 1)}
    if not site.lines:
        return
    new = target.read(site.file)
    if new is None:
        site.drift, site.mapped = 'notarget', []
        return
    if note_tree is None or getattr(site.note, 'unmapped', False):
        site.drift, site.mapped = 'nonote', list(site.lines)
        site.drift_note = 'not mapped: the note has no commit; lines are read as written'
        return
    old = note_tree.read(old_path or site.file)
    if old is None:
        site.drift, site.mapped = 'notmapped', list(site.lines)
        site.drift_note = 'not mapped: the file is not in the note\'s tree, its lines are taken as they are in the target'
        return
    if old is new or old == new:
        site.drift, site.mapped = 'same', list(site.lines)
        return
    m = line_map(old, new)
    mapped, pairs, cited, kinds = [], [], set(), set()
    for a, b in site.lines:
        hit = {x: m[x] for x in range(a, b + 1) if x in m}
        if len(hit) < b - a + 1:
            # where the rewritten lines sit now: between the nearest unchanged lines above and below
            above = [m[x] for x in range(a - 1, max(0, a - 200), -1) if x in m][:1]
            below = [m[x] for x in range(b + 1, b + 200) if x in m][:1]
            reg = ((above[0] + 1) if above else 1, (below[0] - 1) if below else len(new))
            site.gone_regions.append(reg)
            if not hit:
                kinds.add('gone')
                pairs.append(f'{_r(a, b)} -> GONE (now somewhere in {_r(*reg)})')
                continue
            kinds.add('partly gone')
            lo, hi = min(min(hit.values()), reg[0]), max(max(hit.values()), reg[1])
            mapped.append((lo, hi))
            cited |= set(hit.values())
            pairs.append(f'{_r(a, b)} -> partly gone: kept {", ".join(f"L{v}" for v in sorted(hit.values())[:6])}, rewritten part in {_r(*reg)}')
            continue
        c, d = min(hit.values()), max(hit.values())
        mapped.append((c, d))
        cited |= set(hit.values())          # only lines that map from cited lines are marked
        inserted = (d - c) - (b - a)
        if (c, d) != (a, b):
            kinds.add('moved')
            pairs.append(f'{_r(a, b)} -> {_r(c, d)}' + (f' ({inserted} lines inserted inside, not marked)' if inserted > 0 else ''))
    site.mapped, site.cited_lines = mapped, cited
    if kinds == {'gone'}:
        site.drift = 'gone'
    elif 'gone' in kinds or 'partly gone' in kinds:
        site.drift = 'partly gone'
    elif 'moved' in kinds:
        site.drift = 'moved'
    else:
        site.drift = 'same'
    site.drift_note = ', '.join(pairs)


def check_eof(site, target):
    """a cited range past the end of the target file is never clamped and marked: the site is dropped and said so"""
    lines = target.read(site.file)
    if lines is None:
        return
    n = len(lines)
    keep = [(a, b) for a, b in site.mapped if b <= n]
    if len(keep) < len(site.mapped):
        bad = [(a, b) for a, b in site.mapped if b > n]
        site.drift_note = (site.drift_note + '; ' if site.drift_note else '') + f'{", ".join(_r(a, b) for a, b in bad)}: line beyond EOF (the file has {n} lines)'
        site.mapped = keep
        if not keep and site.lines:
            site.status = 'beyond'


# ---------------------------------------------------------------- enclosing symbols

class Sym:
    def __init__(self, kind, name, start, end, depth):
        self.kind, self.name, self.start, self.end, self.depth = kind, name, start, end, depth

    def __repr__(self):
        return f'{self.kind} {self.name} {self.start}-{self.end}'


def _classify(header):
    h = re.sub(r'\s+', ' ', header).strip()
    h = re.sub(r'^(?:\[\[[^\]]*\]\]\s*)+', '', h)
    if not h:
        return 'block', ''
    if re.match(r'^(?:inline\s+)?namespace\b', h) or re.match(r'^extern\s*"[^"]*"\s*$', h):
        return 'namespace', ''      # (the scanner replaces string text by "": extern "C" { arrives as extern "")
    if re.search(r'\)\s*(?:const\s*)?(?:noexcept\s*)?:(?!:).*[\w>]$', h) and '(' in h:
        return 'initlist', ''       # Foo(int a) : x_(a), y_{a}: the brace of a member in a constructor's init list
    first = re.match(r'^[A-Za-z_]\w*', h)
    if first and first.group(0) in CONTROL:
        return 'control', ''
    if '](' in h or h.endswith(']') or re.search(r'\]\s*(?:mutable\s*)?(?:->[^{]*)?$', h) or re.search(r'\]\s*\([^)]*\)\s*(?:mutable|->.*)?$', h):
        return 'lambda', ''
    m = re.match(r'^(?:template\s*<.*>\s*)?(?:typedef\s+)?(struct|class|union|enum(?:\s+class|\s+struct)?)\s+(?:alignas\([^)]*\)\s*)?([A-Za-z_]\w*)?', h)
    if m and '(' not in h.split(':')[0]:
        if '=' in h and not h.startswith('enum'):
            pass
        else:
            return ('enum' if m.group(1).startswith('enum') else m.group(1)), (m.group(2) or '')
    if '=' in h and '(' not in h.split('=')[0]:
        nm = re.findall(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*=\s*$', h.split('{')[0]) or re.findall(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*=', h)
        return 'data', (nm[-1] if nm else '')
    if '(' in h:
        # the first top-level '(' and the (qualified) name before it
        depth = 0
        for i, ch in enumerate(h):
            if ch == '<':
                depth += 1
            elif ch == '>' and depth:
                depth -= 1
            elif ch == '(' and depth == 0:
                pre = h[:i].rstrip()
                m2 = re.search(r'((?:[A-Za-z_]\w*\s*(?:<[^()]*>)?\s*::\s*)*(?:operator\s*\S+|~?[A-Za-z_]\w*))$', pre)
                if not m2:
                    return 'block', ''
                name = re.sub(r'\s+', '', m2.group(1))
                if name in CONTROL:
                    return 'control', name
                if re.match(r'^[A-Z][A-Z0-9_]+$', name) and not pre[:m2.start()].strip():
                    # REX_HOOK_RAW(sub_X, 4) { / PPC_FUNC_IMPL(sub_X) { / TEST(Suite, Case) {: a macro-made function, named by its
                    # identifier arguments (numbers left out), never by the macro: two TEST(...) are two different functions
                    inner = h[i + 1:h.find(')', i)] if ')' in h[i:] else h[i + 1:]
                    args = [a.strip() for a in inner.split(',') if re.match(r'^\s*[A-Za-z_]\w*\s*$', a)]
                    return 'macro', ('.'.join(args) if args else name)
                if not pre[:m2.start()].strip() and '::' not in name and not re.search(r'\)\s*:\s*\w', h):
                    return 'call', name   # no return type: a call / init (Foo(x) {), or a macro block
                return 'function', name
    m3 = re.match(r'^(?:[\w:<>,*&\s]+?)\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?$', h)
    if m3:
        return 'data', m3.group(1)
    return 'block', ''


class UntrustedSyms(list):
    """an empty symbol list that says the brace scan did not balance"""
    untrusted = True


def c_symbols(lines):
    """brace scan of C / C++ / GLSL text (comments, strings, preprocessor lines skipped) -> [Sym] of functions / structs / data tables"""
    syms, stack = [], []
    header, hstart = [], None
    state = None
    n = len(lines)
    li = 0
    pp = []                  # #if nesting: [stack at the #if, stack at the end of its first branch]
    unbalanced = [False]
    while li < n:
        line = lines[li]
        lno = li + 1
        i = 0
        L = len(line)
        if state is None and line.lstrip().startswith('#'):
            # a preprocessor line (and its \ continuations). #if / #else / #endif: each branch starts from the brace stack the #if saw,
            # so two branches that each open a function's brace do not leave the stack one deep for the rest of the file
            d = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b', line)
            if d:
                k = d.group(1)
                if k.startswith('if'):
                    pp.append([list(stack), None])
                elif k in ('elif', 'else') and pp:
                    if pp[-1][1] is None:
                        pp[-1][1] = list(stack)      # the stack at the end of the first branch: what the code after #endif sees
                    stack[:] = list(pp[-1][0])
                elif k == 'endif' and pp:
                    saved = pp.pop()
                    if saved[1] is not None:
                        stack[:] = saved[1]
            while li < n and lines[li].rstrip().endswith('\\'):
                li += 1
            li += 1
            continue
        while i < L:
            ch = line[i]
            if state == 'block':
                j = line.find('*/', i)
                if j < 0:
                    i = L
                    break
                state = None
                i = j + 2
                continue
            if state in ('"', "'"):
                if ch == '\\':
                    i += 2
                    continue
                if ch == state:
                    state = None
                i += 1
                continue
            if state and state.startswith('R'):
                end = ')' + state[1:] + '"'
                j = line.find(end, i)
                if j < 0:
                    i = L
                    break
                state = None
                i = j + len(end)
                continue
            if ch == '/' and line.startswith('//', i):
                break
            if ch == '/' and line.startswith('/*', i):
                state = 'block'
                i += 2
                continue
            if ch == 'R' and line.startswith('R"', i) and (i == 0 or not (line[i - 1].isalnum() or line[i - 1] == '_')):
                j = line.find('(', i)
                if j > 0:
                    state = 'R' + line[i + 2:j]
                    header.append('""')
                    i = j + 1
                    continue
            if ch in ('"', "'"):
                if ch == "'" and i > 0 and line[i - 1].isalnum() and i + 1 < L and line[i + 1].isalnum():
                    i += 1          # a digit separator 1'000
                    continue
                state = ch
                header.append('""')
                i += 1
                continue
            if ch == '{':
                in_fn = any(s['kind'] in ('function', 'lambda', 'control', 'block', 'call', 'macro') for s in stack)
                kind, name = _classify(''.join(header)) if not in_fn else ('block', '')
                if not in_fn and kind in ('call', 'macro'):
                    kind = 'function'     # NAME(args) { at namespace / class level: a macro-made function (PPC_FUNC_IMPL, REX_HOOK)
                if kind == 'data' and any(s['kind'] in ('struct', 'class', 'union') for s in stack):
                    kind = 'block'        # a member's brace init, not a table
                stack.append({'kind': kind, 'name': name, 'start': hstart or lno, 'header': header + ['{'], 'hstart': hstart})
                header, hstart = [], None
                i += 1
                continue
            if ch == '}':
                if stack:
                    s = stack.pop()
                    if s['kind'] in ('function', 'struct', 'class', 'union', 'enum', 'data') and s['name']:
                        quals = [x['name'] for x in stack if x['kind'] in ('struct', 'class', 'union') and x['name']]
                        syms.append(Sym(s['kind'], '::'.join(quals + [s['name']]), s['start'], lno, len(stack)))
                    if s['kind'] == 'initlist':      # back to the constructor's header: its body brace comes next
                        header, hstart = s['header'] + ['}'], s['hstart']
                        i += 1
                        continue
                else:
                    unbalanced[0] = True
                header, hstart = [], None
                i += 1
                continue
            if ch == ';':
                header, hstart = [], None
                i += 1
                continue
            if not ch.isspace() and hstart is None:
                hstart = lno
            header.append(ch)
            i += 1
        header.append(' ')
        li += 1
    if stack or unbalanced[0]:
        # the braces did not balance (a macro or #if shape this scan does not follow): no symbol is trusted for this file;
        # every site in it falls back to the plain window, and the pack says so
        return UntrustedSyms()
    for s in syms:      # a function's / struct's own comment lines above its header belong to it (a fix often edits them)
        k = s.start - 1
        while k >= 1 and s.start - k <= 20 and re.match(r'^\s*(?://|/\*|\*|template\s*<)', lines[k - 1]):
            k -= 1
        s.start = k + 1
    syms.sort(key=lambda s: (s.start, -s.end))
    return syms


def _py_in_string(lines):
    """per line (0-based): True when the line starts inside a triple-quoted string (its indentation says nothing about blocks)"""
    inside, q = [], None
    for line in lines:
        inside.append(q is not None)
        i = 0
        while i < len(line):
            if q is None:
                if line[i] == '#':
                    break
                if line.startswith(('"""', "'''"), i):
                    q = line[i:i + 3]
                    i += 3
                    continue
                if line[i] in '"\'':
                    j = i + 1
                    while j < len(line) and line[j] != line[i]:
                        j += 2 if line[j] == '\\' else 1
                    i = j + 1
                    continue
                i += 1
            else:
                j = line.find(q, i)
                if j < 0:
                    break
                q = None
                i = j + 3
    return inside


def py_symbols(lines):
    out = []
    defs = []
    instr = _py_in_string(lines)
    for i, line in enumerate(lines, 1):
        if instr[i - 1]:
            continue
        m = re.match(r'^(\s*)(?:async\s+)?(def|class)\s+([A-Za-z_]\w*)', line)
        if m:
            defs.append((len(m.group(1).expandtabs()), m.group(2), m.group(3), i))
    for k, (ind, kind, name, start) in enumerate(defs):
        end = _py_end(lines, defs[k], instr)
        while end > start and not lines[end - 1].strip():
            end -= 1
        quals = [d[2] for d in defs[:k] if d[0] < ind and d[3] < start and _py_end(lines, d, instr) >= start]
        out.append(Sym('function' if kind == 'def' else 'class', '.'.join(quals + [name]), start, end, len(quals)))
    return out


def _py_end(lines, d, instr):
    ind, _, _, start = d
    for j in range(start, len(lines)):
        l2 = lines[j]
        if instr[j]:
            continue
        if l2.strip() and not l2.lstrip().startswith('#') and len(l2) - len(l2.lstrip()) <= ind:
            return j
    return len(lines)


_SYMS = {}


def symbols(path, lines):
    key = (path, id(lines))
    hit = _SYMS.get(key)
    if hit is not None and hit[0] is lines:      # the entry holds its lines: an id reused by another list never matches
        return hit[1]
    ext = path.rsplit('.', 1)[-1].lower()
    if ext in CODE_EXT:
        syms = c_symbols(lines)
    elif ext == 'py':
        syms = py_symbols(lines)
    else:
        syms = []
    _SYMS[key] = (lines, syms)
    return syms


def enclosing(syms, a, b):
    """the symbol a cited range sits in: the innermost function, else the innermost struct / data"""
    fn = [s for s in syms if s.kind == 'function' and s.start <= a and b <= s.end]
    if fn:
        return max(fn, key=lambda s: (s.start, -s.end))
    fn = [s for s in syms if s.kind == 'function' and s.start <= a <= s.end]
    if fn:
        return max(fn, key=lambda s: s.start)
    other = [s for s in syms if s.start <= a and b <= s.end and s.end - s.start >= 2]
    if other:
        return max(other, key=lambda s: (s.start, -s.end))
    return None


def by_name(syms, name):
    tail = name.split('::')[-1].split('.')[-1]
    hit = [s for s in syms if s.name == name or s.name.endswith('::' + name) or s.name.split('::')[-1].split('.')[-1] == tail]
    return hit


# ---------------------------------------------------------------- blocks

class Block:
    def __init__(self, file, start, end):
        self.file, self.start, self.end = file, start, end
        self.sites, self.syms, self.cited = [], [], set()
        self.fix = False
        self.shrunk = False
        self.names = []
        self.context = []
        self.idx = 0
        self.rule = 'cite'      # cite = the note's own sites; head / plants / readers / unit / lane = a companion rule (companions.py)
        self.why = ''


def site_spans(site, target, shrink, ctx=None):
    """-> [(start, end, cited lines set, sym or None)] for one site in the target tree"""
    lines = target.read(site.file)
    if lines is None:
        return []
    n = len(lines)
    syms = symbols(site.file, lines)
    out = []
    ranges = site.mapped if site.drift != 'gone' else []
    for a, b in ranges:
        if a < 1 or b > n:
            continue          # never clamped (check_eof already reported it)
        s = enclosing(syms, a, b)
        cited = {x for x in site.cited_lines if a <= x <= b}
        if shrink:
            c = SHRINK if ctx is None else ctx
            lo, hi = a - c, b + c
            if s:
                lo, hi = max(lo, s.start), min(hi, s.end)
            if ctx == 0:
                cut_hi = max(lo, a) + CLIP_LINES - 1      # S105 QB: the last level never skips a site: a huge cited range keeps its first CLIP_LINES lines
                if cut_hi < min(hi, b):
                    site.clips.append((cut_hi + 1, b))     # ... and says what it did not show (review 1)
                hi = min(hi, cut_hi)
        elif s and s.end - s.start + 1 <= FULL_MAX:
            lo, hi = s.start, s.end
        else:
            lo, hi = a - WINDOW, b + WINDOW
            if s:
                lo, hi = max(lo, s.start), min(hi, s.end)
        out.append((max(1, lo), min(n, hi), cited, s))
    if not ranges:
        # no line (or the lines are gone): the function the note names beside the citation, else the one that held the lines at note time
        names = list(site.names)
        if site.drift == 'gone' and site.note.tree is not None:
            old = site.note.tree.read(site.file)
            if old is not None:
                for a, b in site.lines:
                    s0 = enclosing(symbols(site.file, old), a, b)
                    if s0 and s0.name not in names:
                        names.append(s0.name)
        for nm in names:
            hit = by_name(syms, nm)
            if hit:
                s = hit[0]
                size = (SHRINK if ctx is None else max(ctx, 1)) * 2 if shrink else FULL_MAX
                out.append((s.start, min(s.end, s.start + size), set(), s))
                break
        else:
            # a constant / field / macro named beside the citation (no braces, so no symbol): its first defining line, 8 lines either side
            for nm in names:
                tail = re.escape(nm.split('::')[-1])
                rx = re.compile(r'\b' + tail + r'\b\s*(?:=|\{|;|\[)|#define\s+' + tail + r'\b')
                hit = [i for i, l in enumerate(lines, 1) if rx.search(l)]
                if hit:
                    h = hit[0]
                    cc = SHRINK if (not shrink or ctx is None) else ctx
                    out.append((max(1, h - cc), min(n, h + cc), {h} if site.drift == 'gone' else set(), None))
                    break
    return out


def make_blocks(sites, target, shrink_nonfix, fix_ctx=None):
    for st in sites:
        st.clips = []
    """fix_ctx (S105 QB): None = fix blocks whole; N = the hard-cap level: fix blocks too keep only N lines either side of their cited lines"""
    per_file = {}
    for st in sites:
        if st.status != 'ok':
            continue
        if fix_ctx is not None and st.fix:
            spans_ = site_spans(st, target, True, fix_ctx)
        else:
            spans_ = site_spans(st, target, shrink_nonfix and not st.fix, None if fix_ctx is None else min(fix_ctx, SHRINK))
        for lo, hi, cited, sym in spans_:
            per_file.setdefault(st.file, []).append([lo, hi, cited, sym, st])
    blocks = []
    for f, spans in per_file.items():
        spans.sort(key=lambda x: (x[0], x[1]))
        cur = None
        for lo, hi, cited, sym, st in spans:
            if cur and lo <= cur.end:      # overlapping spans (sites in one function) merge; neighbours stay apart
                cur.end = max(cur.end, hi)
            else:
                cur = Block(f, lo, hi)
                blocks.append(cur)
            cur.cited |= cited
            if st not in cur.sites:
                cur.sites.append(st)
            if sym and sym not in cur.syms:
                cur.syms.append(sym)
            cur.fix = cur.fix or st.fix
            cur.shrunk = cur.shrunk or (shrink_nonfix and not st.fix) or (fix_ctx is not None and st.fix)
    for b in blocks:
        b.names = list(dict.fromkeys(s.name for s in b.syms))
        b.shrunk = b.shrunk and (fix_ctx is not None or not b.fix)
        for st in b.sites:
            st.block = b
    blocks.sort(key=lambda b: (0 if b.fix else 1, min(s.order for s in b.sites), b.start))
    for i, b in enumerate(blocks, 1):
        b.idx = i
    return blocks


# ---------------------------------------------------------------- q context (read-only, the main checkout's ours index)

class QContext:
    def __init__(self):
        self.ok = False
        self.why = ''
        self.cov = {}
        self._callers = {}
        try:
            qdir = os.path.join(ROOT, 'tools', 'q')
            if qdir not in sys.path:
                sys.path.insert(0, qdir)
            import qours
            self.qo = qours
            import contextlib
            import io
            with contextlib.redirect_stderr(io.StringIO()):
                path = qours.db_path(ROOT)
            self.con = qours.connect(path)
            self.args = type('A', (), {'limit': 30, 'via': 0, 'json': True, 'tools': False, 'all': False})()
            self.ok = True
            self.why = f'q ours index (read-only): {path}'
        except Exception as e:
            self.why = f'q ours index not available: {e}'

    def callers(self, name, file):
        k = (name, file)
        if k not in self._callers:
            self._callers[k] = self._callers_uncached(name, file)
        return self._callers[k]

    def _callers_uncached(self, name, file):
        try:
            r = self.qo.cmd_callers(self.con, name, self.args)
        except Exception:
            tail = name.split('::')[-1]
            if tail == name:
                return None
            try:
                r = self.qo.cmd_callers(self.con, tail, self.args)
            except Exception:
                return None
        ms = [m for m in r['matches'] if (m['file'] or '').replace('\\', '/') == file] or r['matches']
        if not ms:
            return None
        builds = sorted({m['builds'] for m in ms})
        sites = []
        for m in ms:
            for g in m['sites']:
                t = f'{g["func"]} ({g["file"]}:{g["lines"][0]}, {self.qo.bstr(g["builds"])})'
                if t not in sites:
                    sites.append(t)
        return {'builds': '/'.join(builds), 'callers': sites}

    def covers(self, file):
        """q ours covers FILE, the same queries as qours.cmd_covers (lanes_for + the tus / incs tables) without its scripts_naming walk
        (qours.load_lanes reads the lane json once per process since BPK round 2)"""
        if file in self.cov:
            return self.cov[file]
        try:
            qo = self.qo
            if not self.con.execute('SELECT 1 FROM files WHERE path = ?', (file,)).fetchone():
                raise RuntimeError(f'{file}: not in the q ours index (new since the index, or not ours)')
            direct = qo.lanes_for(ROOT, file)
            units = [r['file'] for r in self.con.execute('SELECT DISTINCT t.file FROM tus t LEFT JOIN incs i ON i.tu = t.id WHERE t.build = 4 AND '
                                                         '(t.file = ? OR i.file = ?) ORDER BY t.file', (file, file))]
            m = 0
            for r in self.con.execute('SELECT DISTINCT t.build FROM tus t LEFT JOIN incs i ON i.tu = t.id WHERE t.file = ? OR i.file = ?', (file, file)):
                m |= r['build']
            via = sorted({h['lane'] for u in units[:12] for h in qo.lanes_for(ROOT, u) if not h.get('retired')})
            named = sorted({h['lane'] for h in direct if h['via'] != ['affects-glob'] and not h.get('retired')})
            globs = sorted({h['lane'] for h in direct if h['via'] == ['affects-glob'] and not h.get('retired')})
            res = {'builds': qo.bstr(m), 'named': named, 'globs': globs, 'units': units, 'via_units': via}
        except Exception as e:
            res = {'error': str(e)[:160]}
        self.cov[file] = res
        return res

    def readers(self, name, exclude):
        try:
            r = self.qo.cmd_refs(self.con, name, self.args)
        except Exception:
            return None
        out = []
        for m in r['matches']:
            for g in m['sites']:
                f = g['file'] or ''
                if f.replace('\\', '/') == exclude:
                    continue
                t = f'{f}:{",".join(str(x) for x in g["lines"][:4])} ({g["func"]})'
                if t not in out:
                    out.append(t)
        return out


def add_context(blocks, q, one_line=False):
    if not q.ok:
        return
    for b in blocks:
        ctx = []
        for s in b.syms[:3]:
            if s.kind not in ('function', 'struct', 'class', 'union', 'enum', 'data'):
                continue
            c = q.callers(s.name, b.file) if s.kind == 'function' else None
            if c:
                cl = c['callers']
                ctx.append(f'`{s.name}`: compiled in {c["builds"]}; callers {len(cl)}: ' + '; '.join(cl[:8]) + (' ...' if len(cl) > 8 else ''))
            elif s.kind == 'function':
                ctx.append(f'`{s.name}`: q ours has no callers / no such symbol (new, static or renamed since the index)')
        if b.file.rsplit('.', 1)[-1].lower() in ('md', 'txt'):
            b.context = ctx
            continue
        cov = q.covers(b.file)
        if 'error' in cov:
            ctx.append(f'covers: {cov["error"]}')
        else:
            ctx.append(f'file compiled in {cov["builds"]}; gate lanes naming it: {", ".join(cov["named"][:10]) or "none"}'
                       + (f'; units / rigs: {", ".join(u.rsplit("/", 1)[-1] for u in cov["units"][:8])}' if cov['units'] else '')
                       + (f'; lanes via units: {", ".join(cov["via_units"][:8])}' if cov['via_units'] else '')
                       + (f'; glob-only lanes {len(cov["globs"])}' if cov['globs'] else ''))
        seen = set()
        for st in b.sites:
            if not LAYOUT_RX.search(st.sentence):
                continue
            for nm in st.names:
                tail = nm.split('::')[-1]
                if nm in seen or not re.match(r'^k[A-Z]\w*$|^[A-Z][A-Za-z0-9]+$', tail) or tail in b.names:
                    continue
                seen.add(nm)
                rd = q.readers(nm, b.file)
                if rd:
                    ctx.append(f'other files using `{nm}` ({len(rd)}): ' + '; '.join(rd[:12]) + (' ...' if len(rd) > 12 else ''))
                if len(seen) >= 4:
                    break
        b.context = ctx


# ---------------------------------------------------------------- render

def _short(t, n):
    t = re.sub(r'\s+', ' ', t).strip()
    return t if len(t) <= n else t[:n - 4] + ' ...'


def _rng(r):
    return ','.join(f'{a}-{b}' if a != b else str(a) for a, b in r)


class Pack:
    pass


def render(p):
    out = []
    W = out.append
    W(f'# Builder pack: {" + ".join(n.name for n in p.notes)}')
    W('')
    W('Scratch, never committed (tools/builder/pack.py). Code below is OUR code at the target tree; game code appears only as pointers.')
    W('')
    for n in p.notes:
        W(f'- note `{n.name}` written at {n.rev[:9] if n.rev else "(no commit)"}' + (f' - {n.tree_note}' if n.tree_note else ''))
    W(f'- target tree: {p.target.label}{" (" + p.target_subject + ")" if p.target_subject else ""}')
    c = p.counts
    W(f'- sites {c["sites"]} (resolved {c["resolved"]}), moved {c["moved"]}, gone {c["gone"]}, ambiguous {c["ambiguous"]}, not found {c["missing"]}, '
      f'pointers {c["pointers"]}; blocks {c["blocks"]}, shrunk {c["shrunk"]}; size {{SIZE}} KB (cap {p.max_kb})')
    W(f'- q: {p.qwhy}' + (' (callers / builds / lanes are from the main checkout\'s index, today\'s line numbers)' if p.qok else ''))
    if c['shrunk']:
        W(f'- over the cap: {c["shrunk"]} non-fix blocks shrunk to {SHRINK} lines either side of their cited lines (fix blocks kept whole)' if p.hard_level is None
          else f'- over the cap: {c["shrunk"]} blocks shrunk (the hard cap: fix blocks too, level {p.hard_level})')
    comp = {}
    for b in p.blocks:
        if b.rule != 'cite':
            comp[b.rule] = comp.get(b.rule, 0) + 1
    if comp or p.dropped:
        W('- companions: ' + ', '.join(f'{r} {k}' for r, k in comp.items())
          + (f'; dropped to fit the cap: {", ".join(f"{r} {k}" for r, k in p.dropped.items())}' if p.dropped else ''))
    for cn in getattr(p, 'companion_notes', []):
        W(f'- {cn}')
    untr = sorted({b.file for b in p.blocks if b.rule == 'cite' and getattr(symbols(b.file, p.target.read(b.file) or []), 'untrusted', False)})
    if untr:
        W(f'- brace scan not trusted (unbalanced: windows, no function names): {", ".join(untr)}')
    W('')
    W('## Contents (in the notes\' order)')
    W('')
    for s in sorted(p.sites, key=lambda s: s.order):
        where = (s.file or s.token) + (f':{_rng(s.lines)}' if s.lines else '')
        tag = '[FIX] ' if s.fix else ''
        if s.status == 'ambiguous':
            dest = 'AMBIGUOUS: ' + ', '.join(s.candidates[:6])
        elif s.status == 'missing':
            dest = 'not found in the target tree'
        elif s.block:
            dest = f'-> [B{s.block.idx}](#b{s.block.idx})' + (' [CLIPPED: ' + ', '.join(f'L{x}-{y}' for x, y in s.clips) + ' not shown]' if s.clips else '')
        elif s.drift == 'gone':
            dest = ('GONE: the cited lines were rewritten since the note (now somewhere in ' + ', '.join(f'L{x}-{y}' for x, y in s.gone_regions)
                    + '; no function named to show instead)')
        else:
            dest = '(file named, no line: no block)'
        if s.status == 'beyond':
            dest = 'dropped'
        if s.status == 'untracked':
            dest = 'UNTRACKED: a pointer, never expanded'
        dr = f' ({s.drift_note})' if s.drift_note else (' (file not in the target tree)' if s.drift == 'notarget' else '')
        W(f'{s.order + 1}. {tag}`{where}`{dr} {dest} - {_short(s.sentence, 110)}')
    W('')
    unres = [s for s in p.sites if s.status != 'ok']
    if unres:
        W('## Not resolved')
        W('')
        for s in unres:
            if s.status == 'ambiguous':
                W(f'- AMBIGUOUS `{s.token}{":" + _rng(s.lines) if s.lines else ""}` ({s.note.name}:{s.item_line}): candidates {", ".join(s.candidates)}. Not guessed.')
            elif s.status == 'untracked':
                W(f'- UNTRACKED `{s.token}` ({s.note.name}:{s.item_line}): only in {", ".join(s.candidates[:3])} (not tracked by git: a pointer, never expanded)')
            elif s.status == 'beyond':
                W(f'- DROPPED `{s.file}:{_rng(s.lines)}` ({s.note.name}:{s.item_line}): {s.drift_note}')
            else:
                W(f'- NOT FOUND `{s.token}` ({s.note.name}:{s.item_line})')
        W('')
    if p.skipped_out:
        W(f'- out\\ paths named (scratch, never expanded): {", ".join(sorted(set(p.skipped_out))[:8])}')
        W('')
    if p.pointers:
        W('## Pointers into game code (never copied: read them with q)')
        W('')
        for x in p.pointers:
            W(f'- `{x.text}` - {x.cmd}  (cited at {", ".join(sorted(set(x.where))[:4])})')
        W('')
    W('## Blocks')
    rule_bytes = {}
    comp_head = False
    for b in p.blocks:
        lines = p.target.read(b.file) or []
        mark0 = len(out)
        if b.rule != 'cite':
            if not comp_head:
                W('')
                W('## Companions (code a fix of these sites usually touches too; one labelled rule each, docs/agents/builder_pack.md)')
                comp_head = True
            W('')
            W(f'### C{b.idx} [{b.rule}] {b.file}:{b.start}-{b.end} - {b.why}')
            _code(W, b, lines)
            rule_bytes[b.rule] = rule_bytes.get(b.rule, 0) + len('\n'.join(out[mark0:]).encode('utf-8')) + 1
            continue
        W('')
        nm = ', '.join(f'`{n}`' for n in b.names) or '(no enclosing function: a window)'
        W(f'<a id="b{b.idx}"></a>')
        W(f'### B{b.idx} {"[FIX] " if b.fix else ""}{b.file}:{b.start}-{b.end} {nm}{" (shrunk to the cap)" if b.shrunk else ""}')
        for st_ in sorted(b.sites, key=lambda s: s.order):
            for x_, y_ in st_.clips:
                W(f'CLIPPED: L{x_}-{y_} of `{st_.file}` not shown (the hard cap): `py tools\\q\\q.py show {st_.file}:{x_}-{y_}`')
        W('')
        groups = {}
        for s in sorted(b.sites, key=lambda s: s.order):
            groups.setdefault((s.note.name, s.heading, s.sentence), []).append(s)
        for (nn, hd, sen), ss in groups.items():
            parts = []
            for s in ss:
                dr = ''
                if s.drift == 'gone':
                    dr = (' GONE: the cited lines were rewritten since the note (now somewhere in '
                          + ', '.join(f'L{x}-{y}' for x, y in s.gone_regions) + ')' + (f'; showing `{b.names[0]}` by name' if b.names else ''))
                elif s.drift_note:
                    dr = f' drift {s.drift_note}'
                cites = f':{_rng(s.lines)}' if s.lines else ' (no line)'
                parts.append(f'`{s.token}{cites}`{dr}')
            W(f'- {nn} > {hd or "(top)"} > {", ".join(parts)}: "{_short(sen, 600)}"')
        for cl in b.context:
            W(f'- q: {cl}')
        _code(W, b, lines)
        rule_bytes['cite'] = rule_bytes.get('cite', 0) + len('\n'.join(out[mark0:]).encode('utf-8')) + 1
    p.rule_bytes = rule_bytes
    text = '\n'.join(out) + '\n'
    return text.replace('{SIZE}', f'{len(text.encode("utf-8")) / 1024:.1f}')


def _code(W, b, lines):
    W('')
    W('```' + LANG.get(b.file.rsplit('.', 1)[-1].lower(), ''))
    width = len(str(b.end))
    ext = b.file.rsplit('.', 1)[-1].lower()
    cut = 300 if ext in ('md', 'txt') else 400 if ext == 'json' else 0
    for ln in range(b.start, b.end + 1):
        if ln - 1 >= len(lines):
            break
        mark = '>>' if ln in b.cited else '  '
        text = lines[ln - 1]
        if cut and len(text) > cut:
            text = text[:cut] + ' ...(line cut)'
        W(f'{mark}{ln:>{width}} | {text}')
    W('```')


# ---------------------------------------------------------------- build

# over the cap, companions go in this order: the DEV hits per KB with no cap, 2026-11-20 (readers 0.02, unit 0.03, head 0.04, plants 0.04)
DROP_ORDER = ('lane', 'readers', 'unit', 'head', 'plants')


def build(notes, target, max_kb=120, use_q=True, bare=True, target_subject='', companions=True, drift=True, companion_rules=None):
    """drift=False is the planted fault for the drift mapper (refind / selftest): cited lines are taken as they are, no mapping"""
    t0 = time.time()
    sites, pointers = [], {}
    for n in notes:
        s, ptrs = parse_note(n, start_order=len(sites))
        sites += s
        for x in ptrs:
            if x.key in pointers:
                pointers[x.key].where += x.where
            else:
                pointers[x.key] = x
    for n in notes:
        if n.tree is None:      # an uncommitted note (a probe hands its note over before it is committed): the target stands in
            n.tree = target
            n.unmapped = True   # each site says "not mapped" (apply_drift)
            n.tree_note = ((n.tree_note + '; ') if n.tree_note else '') + 'not committed: the target tree is used as the note\'s tree (no drift mapping)'
    for s in sites:
        s.status, s.file, s.candidates = resolve(s.token, target, bare=bare)
        st2, f2 = None, None
        if s.note.tree is not target:
            st2, f2, c2 = resolve(s.token, s.note.tree, bare=bare)
        if s.status != 'ok' and st2 == 'ok':
            s.status, s.file = 'missing', None      # a file renamed / removed since the note: say so
            s.token = f2 + ' (in the note\'s tree only)'
        if s.status == 'ok' and drift:
            # the file as the note saw it: the token resolved in the note's own tree (a bare name may point elsewhere there)
            apply_drift(s, s.note.tree, target, old_path=f2 if st2 == 'ok' else s.file)
        if s.status == 'ok':
            check_eof(s, target)
    q = QContext() if use_q else None
    p = Pack()
    p.notes, p.target, p.sites, p.pointers, p.max_kb = notes, target, sites, list(pointers.values()), max_kb
    p.target_subject = target_subject
    p.qok = bool(q and q.ok)
    p.qwhy = q.why if q else 'off (--no-q)'
    p.skipped_out = [x for n in notes for x in getattr(n, 'skipped_out', [])]
    p.companion_notes = []
    cap = max_kb * 1024
    p.dropped = {}
    p.hard_level = None

    def finish(blocks):
        for i, b in enumerate(blocks, 1):
            b.idx = i
        p.blocks = blocks
        p.counts = {
            'sites': len(sites), 'resolved': sum(1 for s in sites if s.status == 'ok'),
            'moved': sum(1 for s in sites if s.drift in ('moved', 'partly gone')), 'gone': sum(1 for s in sites if s.drift == 'gone'),
            'ambiguous': sum(1 for s in sites if s.status == 'ambiguous'), 'missing': sum(1 for s in sites if s.status in ('missing', 'beyond')),
            'untracked': sum(1 for s in sites if s.status == 'untracked'),
            'pointers': len(p.pointers), 'blocks': len(blocks), 'shrunk': sum(1 for b in blocks if b.shrunk),
            'companions': sum(1 for b in blocks if b.rule != 'cite'), 'dropped': sum(p.dropped.values())}
        p.md = render(p)
        return len(p.md.encode('utf-8'))

    def drop_companions():
        # the rule with the fewest hits per KB on the DEV pairs first (DROP_ORDER), the last block of each rule first
        for rule in DROP_ORDER:
            while len(p.md.encode('utf-8')) > cap and any(b.rule == rule for b in comps):
                k = max(i for i, b in enumerate(comps) if b.rule == rule)
                comps.pop(k)
                p.dropped[rule] = p.dropped.get(rule, 0) + 1
                finish(cite + comps)

    comps = []
    cite = make_blocks(sites, target, False)
    if q:
        add_context(cite, q)
    if companions:
        import companions as C
        comps = C.companions(sys.modules[__name__], sites, cite, target, rules=companion_rules)
        p.companion_notes = list(getattr(comps, 'notes', []))
    # over the cap: companions go first (DEV: 0.02-0.04 hits per KB against 0.19 for the note's own blocks), then non-fix blocks shrink
    if finish(cite + comps) > cap:
        drop_companions()
    if len(p.md.encode('utf-8')) > cap:
        for s in sites:
            s.block = None
        cite = make_blocks(sites, target, True)
        if q:
            add_context(cite, q)
        finish(cite + comps)
        drop_companions()
    # S105 QB: the cap is HARD and SHRINKS, never skips (a builder was handed 327 KB over the cap and skipped the pack): fix blocks shrink too,
    # level by level (8, 4, 2, 1, 0 lines either side of a cited line), then every companion goes; the cited lines themselves always stay
    p.hard_level = None
    if len(p.md.encode('utf-8')) > cap:
        for lvl in HARD_LEVELS:
            for s in sites:
                s.block = None
            cite = make_blocks(sites, target, True, fix_ctx=lvl)
            if q and lvl != 0:      # level 0 = no context (review 3); the callers lookups of the levels before are cached in QContext
                add_context(cite, q)
            p.hard_level = lvl
            finish(cite + comps)
            drop_companions()
            if len(p.md.encode('utf-8')) <= cap:
                break
        if len(p.md.encode('utf-8')) > cap and comps:
            comps = []
            finish(cite)
    p.over_cap = len(p.md.encode('utf-8')) > cap
    no_cite = not [b for b in p.blocks if b.rule == 'cite']
    bad = [x for x in sites if x.status in ('ambiguous', 'untracked', 'beyond') or (x.status == 'ok' and x.block is None and x.drift != 'gone')]
    # exit 3 only when sites were given that could not be turned into code (ambiguous / untracked / past the end / resolved but no block);
    # every site "not found" = new files the note plans to create: a warning, exit 0 (review 4); a pointers-only note carries the note, exit 0
    p.no_code = bool(sites) and no_cite and bool(bad)
    p.all_missing = bool(sites) and no_cite and not bad and all(x.status == 'missing' for x in sites)
    p.pointers_only = not sites and bool(p.pointers)
    if p.over_cap:
        p.md = p.md.replace('\n## Contents', f'\n- STILL OVER THE CAP at the last level ({CLIP_LINES} lines per cited range, no context, no companions): too many sites for {max_kb} KB, raise --max-kb\n\n## Contents', 1)
    elif p.hard_level is not None:
        p.md = p.md.replace('\n## Contents', f'\n- HARD CAP: fix blocks too were shrunk to {p.hard_level} lines of context either side of the cited lines (' + ('some cited ranges were CLIPPED: see the CLIPPED lines' if any(x.clips for x in sites) else 'cited lines always kept') + ')\n\n## Contents', 1)
    if p.all_missing:
        p.md = p.md.replace('\n## Contents', f'\n- WARNING: {len(sites)} sites not found in the target tree: new files the note plans to create? (no code to show)\n\n## Contents', 1)
    if p.pointers_only:
        p.md = p.md.replace('\n## Contents', '\n- NO CODE RESOLVED: the note cites game code (pointers) only; nothing of ours to edit in this pack\n\n## Contents', 1)
    if p.no_code:
        p.md = p.md.replace('\n## Contents', '\n- NO CODE RESOLVED: every site resolved to a name or a pointer only (no file:line of a tracked file); this pack holds no code to edit\n\n## Contents', 1)
    p.secs = time.time() - t0
    return p


def note_from_file(path, root=ROOT):
    """a note on disk -> Note with its commit and that commit's tree. git runs from the note's OWN folder (its repo or worktree: a note
    committed only on a worktree branch is found there), so no path arithmetic across drives. No commit -> tree None (build() then uses
    the target tree and the header says so)."""
    ap = os.path.abspath(path)
    d = os.path.dirname(ap)
    rev, nroot, why = None, None, ''
    top = git('rev-parse', '--show-toplevel', root=d, check=False).strip()
    if top:
        nroot = top
        rev = git('log', '-1', '--format=%H', '--', os.path.basename(ap), root=d, check=False).strip() or None
        if rev and git('status', '--porcelain', '--', os.path.basename(ap), root=d, check=False).strip():
            # the text read is the file on disk: its new cites were written against today's files, not the old commit's (review 2, B1)
            why = f'committed at {rev[:9]}, with uncommitted edits on disk: read as uncommitted'
            rev = None
        elif not rev:
            why = 'not committed in its repo'
    else:
        why = 'outside any git repo'
    with open(ap, encoding='utf-8', errors='replace') as f:
        text = f.read()
    tree = GitTree(rev, nroot) if rev else None
    return Note(os.path.basename(path), text, rev, tree, tree_note=why)


def note_at(rel, rev, root=ROOT):
    """a committed note read at one commit"""
    text = _cat(root).read(f'{rev}:{rel}')
    if text is None:
        raise RuntimeError(f'{rel} not in {rev}')
    return Note(rel.rsplit('/', 1)[-1], text, git('rev-parse', rev, root=root).strip(), GitTree(rev, root))


def main_checkout():
    try:
        common = git('rev-parse', '--git-common-dir').strip()
        common = os.path.abspath(os.path.join(ROOT, common))
        if os.path.basename(common) == '.git':
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if '--selftest' in argv:
        sys.path.insert(0, HERE)
        import pack_selftest
        return pack_selftest.main()
    ap = argparse.ArgumentParser(prog='pack.py', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('notes', nargs='+')
    ap.add_argument('--at', default='HEAD', help='the target tree (default HEAD)')
    ap.add_argument('--out', default=None)
    ap.add_argument('--max-kb', type=int, default=120)
    ap.add_argument('--no-q', action='store_true', help='leave out the q ours context')
    ap.add_argument('--no-bare', action='store_true', help='(test only) switch the bare-filename resolver off')
    ap.add_argument('--no-companions', action='store_true', help='only the note\'s own sites (no head / plants / readers / unit blocks)')
    ap.add_argument('--rules', default=None, help='companion rules, comma list (default head,plants,readers,unit; lane is off by default)')
    a = ap.parse_args(argv)
    notes = [note_from_file(n) for n in a.notes]
    target = GitTree(a.at)
    subj = git('log', '-1', '--format=%s', target.rev, check=False).strip()[:90]
    p = build(notes, target, max_kb=a.max_kb, use_q=not a.no_q, bare=not a.no_bare, target_subject=subj, companions=not a.no_companions,
              companion_rules=tuple(a.rules.split(',')) if a.rules else None)
    out = a.out or os.path.join(main_checkout(), 'out', 'packs', '+'.join(os.path.splitext(n.name)[0] for n in notes) + '.md')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(p.md)
    c = p.counts
    if p.all_missing:
        print(f'pack: warning: all {c["sites"]} sites not found in the target tree (new files?): the pack holds no code', file=sys.stderr)
    if p.no_code:
        print('pack: NO CODE RESOLVED: the site list resolved to names / pointers only (no tracked file:line); write the note section '
              '`## Sites the build will touch` as file:line and re-run. See the Not resolved section of the pack: ' + out, file=sys.stderr)
    print(f'pack: {out}  sites {c["sites"]} (resolved {c["resolved"]}) moved {c["moved"]} gone {c["gone"]} ambiguous {c["ambiguous"]} '
          f'not found {c["missing"]} pointers {c["pointers"]} blocks {c["blocks"]} shrunk {c["shrunk"]}  '
          f'{len(p.md.encode("utf-8")) / 1024:.1f} KB  {p.secs:.1f} s{"  OVER CAP" if p.over_cap else ""}{"  HARD-SHRUNK to %d" % p.hard_level if p.hard_level is not None else ""}')
    return 3 if p.no_code else 0


if __name__ == '__main__':
    sys.exit(main())
