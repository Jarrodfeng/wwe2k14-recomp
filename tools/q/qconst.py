r"""q const: which game code writes shader constant register N, and from what (S103 lane QC).

    py tools/q/q.py const N [--vs | --ps] [--json]       the writers of constant register N
    py tools/q/q.py const --name NAME [--vs | --ps]       NAME (g_f4EyePos / EyePos): its register per effect, then its writers
    py tools/q/q.py const --setters                       the constant-setter table discovered from the code (+ its check)
    py tools/q/q.py const N --callers K                   also the callers (K of them) of each writing function
    py tools/q/q.py const N --unresolved                  also list the call sites whose register could not be resolved
    py tools/q/q.py const --stats                         scan totals only
    other: --no-cache (rescan), --window W (default 14), --root DIR, --raw (no formula simplification)

What it does (mechanical, over QD's interpreter `qdecomp` and QF's field index; nothing guessed):
 1. SETTERS, discovered from the code: the command recorders. A recorder is a small function that picks the per-thread
    command buffer (immediates 1048 and 68 in the index) and stores a constant tag at +0 of its header argument (r3),
    r4 at +16 (the register), r6 at +20 (the count), payload r5. The tag selects the replay op; the op table
    (docs/native/d3d/A_replay_ops.md, `KINDS` below) says which tags set shader constants. The check
    (`check_setters`) fails when a recorder with a constant tag exists in the code but not in the table, or the other way.
 2. SITES: every call of every setter (edges of the q index, arguments from the decomp call-site records). r4 (the register) is
    (a) a literal, (b) a halfword field of an object (`lhz r4,OFF(rX)`), resolved through the field index (the `sth` that stores
    a name handle at OFF) to the constant NAME the handle was resolved from, then to registers via src/renderer/shaders/fx/*.fx.json,
    or (c) unresolved, said so. A write of r4 more than `--window` (14) instructions before the call is unresolved, not a literal.
 3. DATA: r5 (a stack slot with its stores folded into a formula, an image address decoded, or the raw expression) and the count.
The scan is cached under out/harness/index/qconst/ keyed by the sha1 of the generated sources (as QD does). Output is derived from the
game and stays in the gitignored out/ folder. Notation and limits: docs/agents/q_const.md.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qdecomp  # noqa: E402

REPO_OURS = os.path.dirname(os.path.dirname(HERE))          # the checkout this file lives in (fx.json are our own files)
FX_DIR = os.path.join(REPO_OURS, 'src', 'renderer', 'shaders', 'fx')
VERSION = 1
WINDOW = 14
NAME_RESOLVER = 0x82BAAEE8      # sub_82BAAEE8: name string buffer -> handle (+16 register u16, +18 count u16)  [T3b row 3, T3 section 3]
MAX_WRITERS = 40               # field writers decompiled per handle offset

# replay op (= recorder tag) -> (stage, what), from docs/native/d3d/A_replay_ops.md (cols op / name). vsps = both banks.
KINDS = {}
for _t, _s, _w in ([(21, 'vs', 'float range'), (27, 'vs', 'bool range'), (28, 'ps', 'float range'), (34, 'ps', 'bool range'),
                    (35, 'vsps', 'float range'), (41, 'vsps', 'bool range')]
                   + [(22 + i, 'vs', 'float ' + c) for i, c in enumerate(('x', 'y', 'z', 'w', 'xyzw'))]
                   + [(29 + i, 'ps', 'float ' + c) for i, c in enumerate(('x', 'y', 'z', 'w', 'xyzw'))]
                   + [(36 + i, 'vsps', 'float ' + c) for i, c in enumerate(('x', 'y', 'z', 'w', 'xyzw'))]
                   + [(84 + i, 'vs', 'banked float ' + c) for i, c in enumerate(('x', 'y', 'z', 'w', 'xyzw'))]):
    KINDS[_t] = (_s, _w)
TAG_WINDOWS = ((21, 41), (84, 88))      # the replay-op ranges that hold constant ops: a recorder tag in here must be in KINDS


class ConstError(Exception):
    pass


# ------------------------------------------------------------------ small helpers
def hx(a):
    return f'0x{a:08X}'


def argmap(args):
    d = {}
    for s in args:
        k, _, v = s.partition('=')
        d[k.strip()] = v.strip()
    return d


LIT_RE = re.compile(r'^(-?\d+|0x[0-9A-Fa-f]+)$')
FIELD_RE = re.compile(r'^\[(.+)\+(\d+)\]$')


def name_key(n):
    """g_f4EyePos / f4EyePos / EyePos -> EyePos; the same stripping for the fx.json field names."""
    n = re.sub(r'^g_', '', n)
    m = re.match(r'^(?:f[1-4]|i[1-4]?|b[1-4]?|f|i|b|mat[0-9]?|mtx|fv|iv)(?=[A-Z0-9])', n)
    return n[m.end():] if m else n


_FX = None


def load_fx():
    """{field key: {'vs'|'ps': {effect: [(register, count, field), ...]}}} from the committed fx.json files."""
    global _FX
    if _FX is not None:
        return _FX
    fx = {}
    for p in sorted(glob.glob(os.path.join(FX_DIR, '*.fx.json'))):
        try:
            with open(p, encoding='utf-8') as fh:
                d = json.load(fh)
        except (OSError, ValueError):
            continue
        eff = d.get('effect') or os.path.basename(p)
        for stage, sname in (('vertex', 'vs'), ('fragment', 'ps')):
            for v in d.get(stage, []):
                for c in v.get('constants', []):
                    e = fx.setdefault(name_key(c['field']), {}).setdefault(sname, {}).setdefault(eff, set())
                    e.add((c['register'], c.get('count', 1), c['field']))
    _FX = fx
    return fx


def fx_cover(stage, n):
    """[(field, effect count)] of the fx constants covering register n in this stage ('vs' / 'ps')."""
    out = {}
    for key, st in load_fx().items():
        for eff, regs in st.get(stage, {}).items():
            for reg, cnt, field in regs:
                if reg <= n < reg + max(cnt, 1):
                    out.setdefault((field, reg, cnt), set()).add(eff)
    return out


# ------------------------------------------------------------------ setter discovery
def discover_setters(con, dec):
    """-> {tag: {addr, name, size}}: recorders found in the code. `dec(addr)` = qdecomp record."""
    rows = con.execute('SELECT addr, name, size FROM funcs WHERE size <= 300 AND has_body = 1 AND addr IN '
                       '(SELECT func FROM imms WHERE value = 1048) AND addr IN (SELECT func FROM imms WHERE value = 68) '
                       'ORDER BY addr').fetchall()
    found = {}
    for r in rows:
        try:
            d = dec(r['addr'])
        except qdecomp.DecompError:
            continue
        tag = None
        consts = {int(c) for c in d['summary']['consts'] if c.isdigit()}
        for ln in d['lines']:
            m = re.match(r'^\s*\+[0-9A-F]+\s+\[(.*)\] = (\d+)\s+\(u32\)\s*$', ln)
            if m and (m.group(1) == 'in_r3' or not re.search(r'\+\d+$', m.group(1))) and int(m.group(2)) in consts                     and 1 <= int(m.group(2)) <= 255 and int(m.group(2)) not in (68,):
                tag = int(m.group(2))
                break
        if tag is None:
            continue
        found.setdefault(tag, []).append({'addr': r['addr'], 'name': r['name'], 'size': r['size']})
    return found


def check_setters(found, kinds=None):
    """-> [problem strings]. A recorder whose constant tag lies in a replay-op window of constant ops but is missing from the
    table, a table tag with no recorder, a tag with two recorders."""
    kinds = KINDS if kinds is None else kinds
    probs = []
    for tag, recs in sorted(found.items()):
        in_win = any(a <= tag <= b for a, b in TAG_WINDOWS)
        if in_win and tag not in kinds:
            probs.append(f'recorder {recs[0]["name"]} (tag {tag}) is in the constant-op window but not in the setter table')
        if len(recs) > 1:
            probs.append(f'tag {tag} has {len(recs)} recorders: ' + ', '.join(r['name'] for r in recs))
    for tag in sorted(kinds):
        if tag not in found:
            probs.append(f'table tag {tag} ({kinds[tag][0]} {kinds[tag][1]}) has no recorder in the code')
    return probs


# ------------------------------------------------------------------ expression simplifier (best effort, raw kept)
def _match(s, i, open_c='(', close_c=')'):
    d = 0
    for j in range(i, len(s)):
        if s[j] == open_c:
            d += 1
        elif s[j] == close_c:
            d -= 1
            if d == 0:
                return j
    return -1


def _unwrap(s):
    s = s.strip()
    while s.startswith('(') and _match(s, 0) == len(s) - 1:
        s = s[1:-1].strip()
    return s


def _peel(s):
    """frsp(fcfid(extsw(X))) -> float(X); extsw(X) -> X; frsp(Y) / fcfid(Y) -> Y."""
    for _ in range(40):
        m = re.search(r'\b(frsp|fcfid|extsw)\(', s)
        if not m:
            break
        o = m.end() - 1
        c = _match(s, o)
        if c < 0:
            break
        inner = s[o + 1:c]
        name = m.group(1)
        u = _unwrap(inner)
        mm = re.match(r'^fcfid\((.*)\)$', u, re.S) if name == 'frsp' else None
        if mm:
            v = _unwrap(mm.group(1))
            m2 = re.match(r'^extsw\((.*)\)$', v, re.S)
            rep = f'float({_unwrap(m2.group(1)) if m2 else v})'
        else:
            rep = inner
        s = s[:m.start()] + rep + s[c + 1:]
    return s


def _alias_base(s):
    """alias the most common long base of `[BASE+N]` field loads as `rec`; -> (text, legend or None)."""
    groups = []
    st = []
    for i, ch in enumerate(s):
        if ch == '[':
            st.append(i)
        elif ch == ']' and st:
            j = st.pop()
            groups.append((j, i))
    cnt = {}
    for j, i in groups:
        inner = s[j + 1:i]
        m = re.match(r'^(\[.*\])\+\d+$', inner, re.S)
        if m and len(m.group(1)) > 24:
            cnt[m.group(1)] = cnt.get(m.group(1), 0) + 1
    if not cnt:
        return s, None
    base = max(cnt, key=lambda k: (cnt[k], len(k)))
    return s.replace('[' + base + '+', '[rec+'), 'rec = ' + base


_LS_RE = re.compile(r'^\s*\+([0-9A-F]+)\s+local\[sp\+(\d+)\] = (.*)\s+\(([^()]*)\)\s*$')
_CALL_RE = re.compile(r'^\s*\+([0-9A-F]+)\s+(?:call#[0-9A-F]+|tail)\s+sub_([0-9A-F]{8})')
_RD_RE = re.compile(r'^\s*\+([0-9A-F]+)\s+(r\d+@\+[0-9A-F]+) = (.*)$')


def local_stores(lines):
    """[(offset, slot, expr, type)] from `+OFF  local[sp+N] = EXPR  (type)` lines, and regdefs {'r6@+88': expr}."""
    st, regdef = [], {}
    for ln in lines:
        if ' = ' not in ln:
            continue
        m = _LS_RE.match(ln) if 'local[sp+' in ln else None
        if m:
            st.append((int(m.group(1), 16), int(m.group(2)), m.group(3).rstrip(), m.group(4)))
            continue
        m = _RD_RE.match(ln)
        if m:
            regdef[m.group(2)] = m.group(3).strip()
    return st, regdef


def simplify(expr, call_off, stores, regdef):
    s = expr
    for _ in range(6):
        def lrep(m):
            slot, off = int(m.group(1)), int(m.group(2), 16)
            best = None
            for so, sl, ex, _t in stores:
                if sl == slot and so < off:
                    best = ex
            return f'({best})' if best is not None else m.group(0)
        n = re.sub(r'L(\d+)@\+([0-9A-F]+)', lrep, s)
        n = re.sub(r'r\d+@\+[0-9A-F]+', lambda m: f'({regdef[m.group(0)]})' if m.group(0) in regdef else m.group(0), n)
        if n == s:
            break
        s = n
    s = re.sub(r'(-?\d[\d.]*(?:e[+\-]?\d+)?)(?: \(1/\d+\))?\[0x[0-9A-Fa-f]{8}\]', r'\1', s)
    s = _peel(s)
    return s


def data_formula(r, call_off, r5, count, raw, ignore=()):
    """-> (kind, text, legend). Folds the stack-slot stores before the call into a tuple per 16-byte unit."""
    m = re.match(r'^sp\+(\d+)$', r5 or '')
    if m:
        base = int(m.group(1))
        if '_ls' not in r:
            r['_ls'] = local_stores(r['lines'])
        stores, regdef = r['_ls']
        prev = -1                      # stack slots are only meaningful since the last call that is not a setter itself
        for ln in r['lines']:
            m = _CALL_RE.match(ln)
            if m and int(m.group(1), 16) < call_off and (m.group(2) or '') not in ignore:
                prev = max(prev, int(m.group(1), 16))
        nunits = count if isinstance(count, int) and 1 <= count <= 4 else 1
        slots = {}
        for off, slot, ex, ty in stores:
            if prev < off < call_off and base <= slot < base + 16 * nunits:
                slots.setdefault(slot, []).append((ex, ty))
        if not slots:
            return 'stack', f'sp+{base}: no store seen before the call', None
        legend = None
        parts = []
        f32 = all(all(t == 'f32' for _e, t in v) for v in slots.values()) and len(slots) % 4 == 0 \
            and all((s - base) % 4 == 0 for s in slots)
        out = {}
        for slot in sorted(slots):
            vals = [e if raw else simplify(e, call_off, stores, regdef) for e, _t in slots[slot]]
            out[slot] = vals
        texts = {}
        for slot, vals in out.items():
            t = vals[-1] if len(vals) == 1 else ' | '.join(vals[:1]) + ' | (a later store on some paths) ' + ' | '.join(vals[1:])
            texts[slot] = t
        whole = '\n'.join(texts.values())
        aliased, legend = ('\n'.join(texts.values()), None) if raw else _alias_base(whole)
        if legend:
            for slot, t in zip(texts, aliased.split('\n')):
                texts[slot] = t
        if f32 and not raw:
            comp = [texts[s] for s in sorted(texts)]
            for u in range(0, len(comp), 4):
                parts.append('(' + ', '.join(comp[u:u + 4]) + ')')
            return 'formula', ' ; '.join(parts), legend
        for slot in sorted(texts):
            ty = slots[slot][-1][1]
            parts.append(f'sp+{slot}[{ty}] = {texts[slot]}')
        return 'stores', '; '.join(parts), legend
    m = re.match(r'^0x([0-9A-Fa-f]{8})$', r5 or '')
    if m:
        a = int(m.group(1), 16)
        img = qdecomp.Image()
        n = (count if isinstance(count, int) and count > 0 else 1) * 4
        if img.data is not None and a >= qdecomp.IMAGE_BASE and (a - qdecomp.IMAGE_BASE + 4 * n) <= len(img.data):
            import struct
            fl = struct.unpack('>' + 'f' * n, img.data[a - qdecomp.IMAGE_BASE:a - qdecomp.IMAGE_BASE + 4 * n])
            return 'image', '(' + ', '.join(qdecomp.fmt_float(float(x)) for x in fl[:4]) + ')' + (' ...' if n > 4 else '') + \
                ' (image: initial values)', None
        return 'address', f'{r5} (not in the image dump)', None
    return 'expr', r5 or '(r5 not shown)', None


# ------------------------------------------------------------------ the engine
class Engine:
    """con = the q index; dec = decompile(addr) -> record; writers(off) -> [func addr] of 2-byte stores at an offset;
    strings(addr) -> text|None; insns(addr) -> [Insn] of the function (for the window check)."""

    def __init__(self, con, dec, writers, strings, insns, window=WINDOW, raw=False):
        self.con, self.writers, self.strings, self.insns = con, writers, strings, insns
        self._dec, self._decm = dec, {}
        self.ignore = set()
        self.window, self.raw = window, raw
        self._handles = {}
        self._ins = {}

    def dec(self, addr):
        r = self._decm.get(addr)
        if r is None:
            r = self._decm[addr] = self._dec(addr)
        return r

    # ---- handles: [X+OFF] = [ret#S+16] (u16)  <- the call at S resolves a name buffer; the buffer is filled with a string
    def handles(self, off, kind=16):
        key = (off, kind)
        if key in self._handles:
            return self._handles[key]
        out = []
        ws = self.writers(off)
        for w in ws[:MAX_WRITERS]:
            try:
                r = self.dec(w)
            except qdecomp.DecompError:
                continue
            pat = re.compile(r'^\s*\+[0-9A-F]+\s+\[[^\n]*?\+' + str(off) + r'\] = \[ret#([0-9A-F]+)\+' + str(kind) + r'\]\s+\(u16\)')
            for ln in r['lines']:
                m = pat.match(ln)
                if not m:
                    continue
                s_off = int(m.group(1), 16)
                site = r['addr'] + s_off
                ent = {'writer': r['addr'], 'writer_name': r['name'], 'site': site, 'buf': None, 'string': None, 'name': None}
                sites = r['summary']['sites']
                for callee, lst in sites.items():
                    for s in lst:
                        if s['site'] == site:
                            ent['resolver'] = int(callee, 16)
                            a = argmap(s['args'])
                            mb = re.match(r'^0x([0-9A-Fa-f]{8})$', a.get('r4', ''))
                            if mb:
                                ent['buf'] = int(mb.group(1), 16)
                if ent['buf'] is not None:
                    for callee, lst in sites.items():
                        for s in lst:
                            a = argmap(s['args'])
                            if a.get('r3') == f'0x{ent["buf"]:08X}' and s['site'] != site:
                                ms = re.match(r'^0x([0-9A-Fa-f]{8})$', a.get('r4', ''))
                                if ms:
                                    t = self.strings(int(ms.group(1), 16))
                                    if t:
                                        ent['string'] = int(ms.group(1), 16)
                                        ent['name'] = t
                out.append(ent)
        self._handles[key] = out
        return out

    def window_ok(self, caller, site):
        """-> (ok, distance|None): the last write of r4 before the call at `site`, in instructions."""
        if caller not in self._ins:
            try:
                self._ins[caller] = self.insns(caller)
            except Exception:
                self._ins[caller] = []
        ins = self._ins[caller]
        idx = next((i for i, x in enumerate(ins) if x.addr == site), None)
        if idx is None:
            return True, None
        for k in range(idx - 1, -1, -1):
            x = ins[k]
            if x.mn in ('bl', 'bctrl', 'blrl'):
                return True, None
            if x.mn.startswith(qdecomp.NO_DEST):
                continue
            ops = qdecomp.split_ops(x.ops)
            if ops and ops[0] == 'r4':
                d = idx - k
                return d <= self.window, d
        return True, None

    def resolve_reg(self, caller, site, am):
        r4 = am.get('r4')
        if r4 is None:
            return {'kind': 'unresolved', 'why': 'r4 not written in the call block since the last call (set earlier / across a branch)'}
        ok, dist = self.window_ok(caller, site)
        if not ok:
            return {'kind': 'unresolved', 'why': f'r4 written {dist} instructions before the call (window {self.window})', 'raw': r4}
        if LIT_RE.match(r4):
            return {'kind': 'literal', 'value': int(r4, 0)}
        m = FIELD_RE.match(r4)
        if m:
            off = int(m.group(2))
            r = self.dec(caller)
            reads = {(x['addr'], x['w']) for x in r['summary']['reads']}
            if (m.group(0)[1:-1], 'u16') in reads or any(a == r4[1:-1] and w == 'u16' for a, w in reads):
                hs = self.handles(off, 16)
                names = []
                for h in hs:
                    if h['name'] and h['name'] not in [n['name'] for n in names]:
                        names.append({'name': h['name'], 'writer': h['writer'], 'string': h['string'], 'site': h['site']})
                if names:
                    return {'kind': 'name', 'field': r4, 'off': off, 'names': names, 'ambiguous': len(names) > 1}
                return {'kind': 'unresolved', 'why': f'u16 field +{off}: no name-handle store found ({len(hs)} handle stores, none with a string)',
                        'raw': r4}
            return {'kind': 'unresolved', 'why': 'field is not a u16 load', 'raw': r4}
        return {'kind': 'unresolved', 'why': 'not a literal or a halfword field', 'raw': r4}

    def resolve_count(self, caller, am, reg):
        r6 = am.get('r6')
        if r6 is None:
            return {'kind': 'unresolved', 'why': 'r6 not shown'}
        if LIT_RE.match(r6):
            return {'kind': 'literal', 'value': int(r6, 0)}
        m = FIELD_RE.match(r6)
        if m and reg.get('kind') == 'name':
            off = int(m.group(2))
            for h in self.handles(off, 18):
                for n in reg['names']:
                    if h['writer'] == n['writer'] and h['site'] == n['site']:
                        return {'kind': 'handle_count', 'raw': r6}
        return {'kind': 'unresolved', 'why': 'not a literal', 'raw': r6}

    def site_hit(self, setter, caller, s):
        am = argmap(s['args'])
        reg = self.resolve_reg(caller, s['site'], am)
        cnt = self.resolve_count(caller, am, reg)
        r = self.dec(caller)
        stage, what = KINDS[setter['tag']]
        c = cnt['value'] if cnt['kind'] == 'literal' else None
        dk, dt, legend = data_formula(r, s['site'] - r['addr'], am.get('r5'), c, self.raw, self.ignore)
        return {'func': caller, 'func_name': r['name'], 'site': s['site'], 'tail': s['tail'], 'setter': setter['addr'],
                'tag': setter['tag'], 'stage': stage, 'what': what, 'reg': reg, 'count': cnt, 'data_kind': dk,
                'data': dt, 'legend': legend, 'r5': am.get('r5'), 'args': s['args']}

    def scan(self, setters):
        hits = []
        self.ignore = {f'{v["addr"]:08X}' for v in setters.values()}
        for tag, st in sorted(setters.items()):
            rows = self.con.execute('SELECT DISTINCT src FROM edges WHERE dst = ? ORDER BY src', (st['addr'],)).fetchall()
            for row in rows:
                src = row['src']
                if src == st['addr']:
                    continue
                try:
                    r = self.dec(src)
                except qdecomp.DecompError:
                    continue
                for s in r['summary']['sites'].get(f'{st["addr"]:08X}', []):
                    hits.append(self.site_hit({**st, 'tag': tag}, src, s))
        return hits


# ------------------------------------------------------------------ real data sources + cache
def cache_key():
    shas = []
    for p in sorted(glob.glob(os.path.join(qdecomp.GEN, 'wwe2k14_recomp.*.cpp'))):
        shas.append(os.path.basename(p) + ':' + qdecomp._cached_sha1(p))
    return hashlib.sha1(('v%d;' % VERSION + ';'.join(shas)).encode()).hexdigest()


def cache_dir():
    return os.path.join(os.path.dirname(qdecomp.CACHE_DIR), 'qconst')


def make_engine(con, root=None, window=WINDOW, raw=False):
    sys.path.insert(0, HERE)
    import qfield
    import build_fields as bf
    gen, image, db = bf.default_paths(root, None)
    fcon = None
    if os.path.exists(db):
        fcon = sqlite3.connect(f'file:{db}?mode=ro', uri=True)

    def writers(off):
        if fcon is None:
            return []
        return [r[0] for r in fcon.execute("SELECT DISTINCT func FROM rows WHERE off = ? AND rw = 'W' AND width = 2 ORDER BY func", (off,))]

    def strings(a):
        r = con.execute('SELECT text FROM strings WHERE addr = ?', (a,)).fetchone()
        return r['text'] if r else None

    def insns(addr):
        f = con.execute('SELECT * FROM funcs WHERE addr = ?', (addr,)).fetchone()
        return qdecomp.read_listing(os.path.join(qdecomp.GEN, f['file']), f['line'], f['name'], f['addr'])[0]

    eng = Engine(con, lambda a: qdecomp.decompile(con, a), writers, strings, insns, window, raw)
    eng.fcon = fcon      # S105 D (F-2): closed by get_scan after the scan: an open reader makes a field-index rebuilder's swap fail on Windows
    return eng, (fcon is not None)


def get_scan(con, a):
    """-> dict {setters, problems, hits, secs, cached}; cached under out/harness/index/qconst/."""
    key = cache_key()
    cpath = os.path.join(cache_dir(), f'scan.{key[:16]}.w{a.window}.{"raw" if a.raw else "s"}.json')
    if not a.no_cache and os.path.exists(cpath):
        try:
            with open(cpath, encoding='utf-8') as fh:
                d = json.load(fh)
            d['cached'] = True
            return d
        except (OSError, ValueError):
            pass
    t0 = time.time()
    eng, have_fields = make_engine(con, getattr(a, 'root', None), a.window, a.raw)
    found = discover_setters(con, eng.dec)
    problems = check_setters(found)
    setters = {t: v[0] for t, v in found.items() if t in KINDS}
    hits = eng.scan(setters)
    if getattr(eng, 'fcon', None) is not None:
        eng.fcon.close()
    d = {'version': VERSION, 'key': key, 'setters': {str(t): {**v, 'tag': t} for t, v in setters.items()},
         'problems': problems, 'hits': hits, 'secs': round(time.time() - t0, 2), 'cached': False, 'have_fields': have_fields}
    if not have_fields:      # S105 D (F-4): a scan made without the field index (name handles unresolved) must not be cached under the same key: the next call, with the index built, would be served it
        return d
    try:
        os.makedirs(cache_dir(), exist_ok=True)
        for old in os.listdir(cache_dir()):
            if old.startswith('scan.') and old != os.path.basename(cpath) and old.endswith(('.s.json', '.raw.json')) \
                    and f'.w{a.window}.' in old:
                try:
                    os.remove(os.path.join(cache_dir(), old))
                except OSError:
                    pass
        with open(cpath + '.tmp', 'w', encoding='utf-8') as fh:
            json.dump(d, fh)
        os.replace(cpath + '.tmp', cpath)
    except OSError:
        pass
    return d


# ------------------------------------------------------------------ queries
def stage_hit(stage, want):
    return want is None or stage == 'vsps' or stage == want


def hit_covers(h, n, want):
    """-> 'literal' | 'name' | None for register n in the wanted stage."""
    if not stage_hit(h['stage'], want):
        return None
    reg = h['reg']
    if reg['kind'] == 'literal':
        c = h['count']['value'] if h['count']['kind'] == 'literal' else 1
        if reg['value'] <= n < reg['value'] + max(c, 1):
            return 'literal'
        return None
    if reg['kind'] == 'name':
        stages = [want] if want else (['vs', 'ps'] if h['stage'] == 'vsps' else [h['stage']])
        if h['stage'] != 'vsps' and want is None:
            stages = [h['stage']]
        for nm in reg['names']:
            st = load_fx().get(name_key(nm['name']), {})
            for sn in stages:
                for eff, regs in st.get(sn, {}).items():
                    for rr, cnt, _f in regs:
                        if rr <= n < rr + max(cnt, 1):
                            return 'name'
    return None


def hit_named(h, key, regs):
    reg = h['reg']
    if reg['kind'] == 'name':
        return any(name_key(n['name']) == key for n in reg['names'])
    if reg['kind'] == 'literal':
        return reg['value'] in regs
    return False


def fmt_stage(h):
    return {'vs': 'vs', 'ps': 'ps', 'vsps': 'vs+ps'}[h['stage']] + ' ' + h['what']


def fmt_reg(h):
    r = h['reg']
    if r['kind'] == 'literal':
        return f'r4 = {r["value"]} (literal)'
    if r['kind'] == 'name':
        nm = ' | '.join(n['name'] for n in r['names'])
        regs = set()
        for n in r['names']:
            for sn, effs in load_fx().get(name_key(n['name']), {}).items():
                for eff, rs in effs.items():
                    for rr, cnt, _f in rs:
                        regs.add(f'{sn} c{rr}' + (f'+{cnt}' if cnt > 1 else ''))
        txt = f'r4 = {r["field"]} -> name {nm}' + (' (AMBIGUOUS: several handle stores)' if r['ambiguous'] else '')
        return txt + ('  [fx: ' + ', '.join(sorted(regs)) + ']' if regs else '  [fx: no effect names it]')
    return f'r4 UNRESOLVED: {r["why"]}' + (f' ({r["raw"]})' if r.get('raw') else '')


def fmt_count(h):
    c = h['count']
    if c['kind'] == 'literal':
        return str(c['value'])
    if c['kind'] == 'handle_count':
        return f'{c["raw"]} (= the handle\'s count)'
    return f'? ({c["why"]})'


def print_hits(con, hits, a, dbi, title):
    byf = {}
    for h in hits:
        byf.setdefault(h['func'], []).append(h)
    print(f'{title}: {len(hits)} writes in {len(byf)} functions')
    for f, hs in sorted(byf.items()):
        e = dbi.get(f)
        nm = hs[0]['func_name'] + (f' ({e["name"]})' if e and e.get('name') else '')
        print(f'{nm}  {hx(f)}')
        for h in sorted(hs, key=lambda x: x['site']):
            print(f'  {hx(h["site"])}  {fmt_stage(h)}  sub_{h["setter"]:08X}  {fmt_reg(h)}  count {fmt_count(h)}')
            print(f'      data r5={h["r5"]}: {h["data"]}')
            if h.get('legend'):
                print(f'      ({h["legend"]})')
        if a.callers:
            for ln in qdecomp.callers_summary(con, f, a.callers, dbi):
                print('  ' + ln)


def run_query(con, a):
    d = get_scan(con, a)
    dbi = qdecomp.db_index()
    unres = [h for h in d['hits'] if h['reg']['kind'] == 'unresolved']
    if a.stats:
        kinds = {}
        for h in d['hits']:
            kinds[h['reg']['kind']] = kinds.get(h['reg']['kind'], 0) + 1
        print(f'setters {len(d["setters"])}, call sites {len(d["hits"])}: {kinds}; scan {d["secs"]} s' + (' (cached)' if d['cached'] else ''))
        return 0
    if a.setters:
        print(f'constant setters found in the code ({len(d["setters"])}):')
        for t in sorted(d['setters'], key=int):
            s = d['setters'][t]
            n = sum(1 for h in d['hits'] if h['setter'] == s['addr'])
            e = dbi.get(s['addr'])
            print(f'  tag {int(t):3d}  sub_{s["addr"]:08X}  size {s["size"]:3d}  {KINDS[int(t)][0]:4s} {KINDS[int(t)][1]:18s} {n:4d} sites'
                  + (f'  ({e["name"]})' if e and e.get('name') else ''))
        print('setter-table check: ' + ('OK' if not d['problems'] else 'FAIL'))
        for p in d['problems']:
            print('  PROBLEM: ' + p)
        return 1 if d['problems'] else 0
    want = 'vs' if a.vs else 'ps' if a.ps else None
    if a.name:
        key = name_key(a.name)
        fxs = load_fx().get(key)
        regs = set()
        print(f'name {a.name}  (key {key}):')
        if not fxs:
            print('  no effect in src/renderer/shaders/fx names it')
        else:
            for sn in ('vs', 'ps'):
                if want and sn != want:
                    continue
                by = {}
                for eff, rs in fxs.get(sn, {}).items():
                    for rr, cnt, field in rs:
                        by.setdefault((rr, cnt, field), []).append(eff)
                        regs.add(rr)
                for (rr, cnt, field), effs in sorted(by.items()):
                    print(f'  {sn} c{rr}' + (f'+{cnt}' if cnt > 1 else '') + f'  field {field}: {len(effs)} effects: ' + ', '.join(sorted(effs)[:8])
                          + (' ...' if len(effs) > 8 else ''))
        hits = [h for h in d['hits'] if hit_named(h, key, regs) and stage_hit(h['stage'], want)]
        print_hits(con, hits, a, dbi, 'writers')
    else:
        n = int(a.n, 0)
        cov = fx_cover('vs', n) if want != 'ps' else {}
        covp = fx_cover('ps', n) if want != 'vs' else {}
        for st, c in (('vs', cov), ('ps', covp)):
            for (field, rr, cnt), effs in sorted(c.items()):
                print(f'fx: {st} c{n} = {field} (register {rr}' + (f', count {cnt}' if cnt > 1 else '') + f') in {len(effs)} effects')
        hits = [h for h in d['hits'] if hit_covers(h, n, want)]
        print_hits(con, hits, a, dbi, f'c{n}' + (f' ({want})' if want else ''))
    nu = len(unres)
    print(f'not decided: {nu} of {len(d["hits"])} call sites have an unresolved register (they may write this one too)'
          + ('' if a.unresolved else '; --unresolved lists them'))
    if not d['have_fields']:
        print('note: no field index (out/harness/index/fields.s<SCHEMA>.db; any `q field` call builds it): name handles could not be resolved')
    if a.unresolved:
        for h in sorted(unres, key=lambda x: x['site'])[:200]:
            print(f'  {hx(h["site"])} {h["func_name"]} {fmt_stage(h)}: {h["reg"]["why"]}' + (f' [{h["reg"]["raw"]}]' if h['reg'].get('raw') else ''))
    print(f'({d["secs"]} s scan' + (', cached)' if d['cached'] else ')'))
    if a.json:
        print(json.dumps(hits, indent=1))
    return 0


def main(argv, con, resolve=None):
    ap = argparse.ArgumentParser(prog='q const', description=__doc__.split('\n\n')[0])
    ap.add_argument('n', nargs='?', help='constant register number')
    ap.add_argument('--name')
    ap.add_argument('--vs', action='store_true')
    ap.add_argument('--ps', action='store_true')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--setters', action='store_true')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--callers', type=int, default=0, metavar='K')
    ap.add_argument('--unresolved', action='store_true')
    ap.add_argument('--no-cache', action='store_true')
    ap.add_argument('--window', type=int, default=WINDOW)
    ap.add_argument('--raw', action='store_true', help='no formula simplification')
    ap.add_argument('--root')
    a = ap.parse_args(argv)
    if not (a.n or a.name or a.setters or a.stats):
        ap.error('give a register number, --name NAME, --setters or --stats')
    return run_query(con, a)
