r"""q ostate's wide reader (S111 OSTATEWIDE): the DrawState stores of ours that q ostate's block reader (tools/q/qostate.py collect) never
read, so q codediff never compared them. Called from qostate.collect (its facts join q ostate's; the same fact form, the same evaluator:
qostate's statements / assign_targets / eval_val / eval_cond / ours_state).

Two units, each one DrawState of one function:
  (a) a copy block `rdr::DrawState X = SRC;` (from the declaration to the first `State(X)` that consumes it, else its scope's end): SRC
      is its BASE, whose facts every field inherits (kind `copy`, at the declaration, `base` = SRC, `base_at` = where the base's value
      was set), and the block's own stores override them as a `X{}` block's stores override its zero (an unconditional store replaces,
      a conditional one adds). The base is resolved, never guessed:
        - a DrawState of the same function declared above (a `{}` block: qostate.ours_state; another copy block): its facts;
        - `V.m` / `V->m` where V's declared type T has a DrawState member m: T::m's writers. A function that stores T::m from the
          game's device words (DECODER_RE: `Ld32(base, dev + kDev..)`, rdr_reader::DecodeFixed into DrawFixed::state) makes it the
          game's own word: {'game': True} (q codediff: ours follows the game). Any other writer `W.m = SRC2;` (W declared T): SRC2
          resolved the same way, in the writer's function;
        - else (a parameter, a call, a writer not found) a rule of ours naming the source: {'rule': ...} (UNSETTLED in the diff).
  (b) the stores on a DrawState reached by reference or pointer, `st.depth_test =`, `s->cull =`, `out.state.blend[0].enable =`, anywhere
      in a function, grouped per base expression (one unit per (function, base)): the base's first variable's declared type (a
      parameter or a local), then each member's type from its struct's own definition (the files read, else `git grep` for it). A
      unit's facts are its stores (kind `set`, with their if / for conditions); the state before them is the caller's (no base).
A store whose base is not a DrawState (another struct with a field of the same name: FrontEnd::Record's `Item it`, a guest
material row, a tween range `ReplaceOut::own.blend[0]`) is SET ASIDE with that reason; so is a unit of a DECODER (the game's state
read from its device words is the game side, not ours) and a store after the State(X) that recorded X. counts['aside'] lists them
[(file, line, reason)]; q pairs --coverage prints them beside OUTSIDE (the stores no rule reads: their base type not resolved).
(c) the bind records (bind_facts): a block whose alpha test is a record's flag (`b.alpha_test`, b a FrontEnd::Bound) gets `bind` = the
    writers of that flag (FrontEnd::BindMaterial / BindRule `b->alpha_test = ...`), each a fact of its own function (kind `bind`).
(d) S111 CDARMS (tools/q/qcdarms.py): each bind store carries its if-arm (`arm`, `attached`, `scope`: qcdarms.arm_keys), so q codediff
    compares a bind arm by arm; a base that is a parameter of its function is the state its callers pass (qcdarms.climb, in resolve).
The facts carry `block` = file:line (the copy's declaration, the reference unit's first store) and `wide` = copy | ref, so q codediff and
q pairs --coverage pair each unit through the existing pairing (the function's q pairs citations in scope; a unit has no kTable row or
rstdiff kind). The alpha test (`Dynamic()`) of a wide unit is not read (qostate.alpha_facts reads the `{}` blocks' only).
Selftest: tools/q/qostate_wide_selftest.py. PLANT (selftest only): WWE_QOSTATEWIDE_PLANT = no_override (a copy's own stores never
override its base) | no_arrow (`->` stores not read) | any_type (the base type not checked) | no_decoder (a decoder's unit read as ours).
"""
import functools
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qconstdiff as CD   # noqa: E402
import qostate as O       # noqa: E402
import qcdarms as A       # noqa: E402

WIDE_DIRS = ('src/renderer_scene', 'src/renderer_host', 'src/renderer_adapter')
EXTS = ('.h', '.cpp', '.inc')
COPY_RE = re.compile(r'\b(?:rdr::)?DrawState\s+(?P<var>\w+)\s*=(?!=)\s*(?P<src>[^;{}]+?)\s*;')
FIELD_PATH = (r'(?:depth_test|depth_write|depth_compare|cull|stencil_test'
              r'|blend\s*\[\s*0\s*\](?:\s*\.\s*(?:enable|src_color|dst_color|color_op|write_mask))?)')
STORE_RE = re.compile(r'(?P<op>\.|->)\s*' + FIELD_PATH + r'\s*=(?!=)')
BASE_RE = re.compile(r'((?:\w+\s*(?:\[[^\[\]]*\]\s*)?(?:\.|->)\s*)*\w+\s*(?:\[[^\[\]]*\]\s*)?)$')
DECODER_RE = re.compile(r'\bLd32\s*\(\s*base\s*,\s*dev\s*\+\s*kDev\w+')
ZERO_BLEND_RE = re.compile(r'(?:rdr::)?BlendState\s*\{\s*\}')
NOT_TYPES = {'return', 'else', 'case', 'delete', 'new', 'goto', 'throw', 'typename', 'using', 'namespace', 'sizeof', 'co_return', 'if',
             'while', 'for', 'switch', 'do', 'const', 'static', 'inline', 'constexpr', 'mutable', 'volatile'}
DEPTH = 4


def plant():
    return os.environ.get('WWE_QOSTATEWIDE_PLANT', '')


def is_ds(typ):
    return bool(typ) and typ.split('::')[-1] == 'DrawState'


def short(typ):
    return typ.split('::')[-1] if typ else typ


def parts_of(base):
    """`frame.fixeds[d.fixed].state` -> [('frame', ''), ('fixeds', '[d.fixed]'), ('state', '')] and the ops between them"""
    toks = re.findall(r'(\w+)\s*(\[[^\[\]]*\])?\s*(\.|->)?', base)
    return [(n, ix) for n, ix, _op in toks], [op for _n, _ix, op in toks if op]


def prefix_re(base):
    """a regex for `BASE.` / `BASE->` at a store (spaces free, an index kept as written) -> replaced by `__W.`"""
    ps, _ops = parts_of(base)
    segs = [re.escape(n) + (r'\s*\[[^\[\]]*\]' if ix else '') for n, ix in ps]
    return re.compile(r'(?<![\w.>])' + r'\s*(?:\.|->)\s*'.join(segs) + r'\s*(?:\.|->)\s*')


CTRL_RE = re.compile(r'\b(?:if|for|while|switch|catch)\s*$')


@functools.lru_cache(maxsize=64)
def _blank(t):
    return O._blank_strings(t)


def fn_span(t, src, ln, pos):
    """-> (start, end) of the function holding pos: qsym's (TextSrc.span), else (a .inc file's member function, which qsym does not
    parse) the OUTERMOST brace scope around pos opened right after a `)` (and const / noexcept / override / final) that is no if / for /
    while / switch: a function body (a lambda's sits inside it); start = the line of its parameter list's `(`. None at file scope."""
    sp = None if getattr(src, 'wide_rel', '').endswith('.inc') else src.span(ln)   # (qsym spans a .inc file's member function as the whole file)
    if sp is not None and sp[0] <= pos < sp[1]:
        return sp
    b = _blank(t)
    best, d, i = None, 0, pos - 1
    while i >= 0:
        ch = b[i]
        if ch == '}':
            d += 1
        elif ch == '{':
            if d == 0:
                head = re.search(r'\)\s*(?:(?:const|noexcept|override|final|mutable)\s*)*$', b[max(0, i - 200):i])
                if head:
                    close = i - (len(b[max(0, i - 200):i]) - head.start())
                    k, dd = close, 0
                    while k >= 0:
                        if b[k] == ')':
                            dd += 1
                        elif b[k] == '(':
                            dd -= 1
                            if dd == 0:
                                break
                        k -= 1
                    if k >= 0 and not CTRL_RE.search(b[max(0, k - 20):k]):
                        best = (b.rfind('\n', 0, k) + 1, CD._match(b, i, '{', '}'))
            else:
                d -= 1
        i -= 1
    return best


# ------------------------------------------------------------------ types: declarations and struct members
class Types:
    """the declared type of a variable in a function, and of a struct's member (its definition in the files read, else git grep)"""
    def __init__(self, ref, read, masked):
        self.ref, self.read, self.masked = ref, read, masked
        self.bodies = {}

    @staticmethod
    def decl(var, text):
        """-> (type, ref marks, the match) of the last declaration of var in text, else None"""
        pat = re.compile(r'(?:^|(?<=[\s(,{;]))(?:const\s+)?(?P<type>(?:\w+::)*\w+)(?:\s*<[^;()<>]*>)?\s*(?:const\s*)?(?P<ref>[&*]*)\s*\b'
                         + re.escape(var) + r'\b\s*(?=[;=,)\[{:(])')
        got = None
        for m in pat.finditer(text):
            if m.group('type') in NOT_TYPES or m.group('type') == var:
                continue
            if not m.group('ref') and not re.match(r'\s', text[m.end('type'):m.end('type') + 1] or 'x') and not text[m.end('type'):].startswith('<'):
                continue
            got = m
        return (got.group('type'), got.group('ref'), got) if got else None

    def body(self, typ):
        name = short(typ)
        if name in self.bodies:
            return self.bodies[name]
        pat = re.compile(r'\b(?:struct|class)\s+' + re.escape(name) + r'\b[^;{]*\{')
        found = None
        for t in self.masked.values():
            m = pat.search(t)
            if m:
                found = t[m.end():CD._match(t, m.end() - 1, '{', '}') - 1]
                break
        if found is None:
            cmd = ['git', 'grep', '-l', '-E', r'\b(struct|class)\s+' + name + r'\b'] + ([self.ref] if self.ref else []) + ['--', 'src']
            r = subprocess.run(cmd, cwd=O.REPO, capture_output=True, text=True, encoding='utf-8', errors='replace')
            for ln in r.stdout.splitlines() if r.returncode == 0 else []:
                rel = ln.split(':', 1)[1] if self.ref and ':' in ln else ln
                try:
                    t = CD._mask(self.read(rel))
                except (OSError, O.OstateError):
                    continue
                m = pat.search(t)
                if m:
                    found = t[m.end():CD._match(t, m.end() - 1, '{', '}') - 1]
                    break
        self.bodies[name] = found
        return found

    def member(self, typ, m):
        b = self.body(typ) if typ else None
        if b is None:
            return None
        mm = None
        for mm in re.finditer(r'(?P<type>(?:\w+::)*\w+)(?:\s*<[^;<>]*>)?\s*[&*]?\s+\b' + re.escape(m)
                              + r'\b\s*(?:\[[^\]]*\]\s*)*(?:\{[^;]*\}|=[^;]*)?\s*;', b):
            if mm.group('type') not in NOT_TYPES:
                return mm.group('type')
        for mm in re.finditer(r'\}\s*' + re.escape(m) + r'\s*(?:\[[^\]]*\]\s*)*;', b):   # `struct Inner { ... } m;` (a nested struct, its body kept)
            d, i = 0, mm.start()
            while i >= 0:
                d += {'}': 1, '{': -1}.get(b[i], 0)
                if d == 0:
                    break
                i -= 1
            hm = re.search(r'\b(?:struct|class)\s+(\w+)\s*$', b[max(0, i - 120):i])
            if i >= 0 and hm:
                self.bodies[hm.group(1)] = b[i + 1:mm.start()]
                return hm.group(1)
        return None


# ------------------------------------------------------------------ the units
class Unit:
    def __init__(self, kind, rel, fn, span, base, lo, hi, at_ln, var=None, owner=None):
        self.kind, self.rel, self.fn, self.span, self.base = kind, rel, fn, span, base
        self.lo, self.hi, self.at_ln, self.var, self.owner = lo, hi, at_ln, var, owner
        self.state = None      # {field: [(value, cond, at, kind, extra)]}
        self.busy = False


class Wide:
    def __init__(self, ref, read, files, cache):
        self.ref, self.read, self.files, self.cache = ref, read, files, cache
        self.problems, self.aside = [], []
        self.units, self.copies = [], {}
        self.game_members = {}   # (struct, member) -> the decoder function
        self.types = None

    def text(self, rel):
        if rel not in self.cache:
            text = self.read(rel)
            self.cache[rel] = (text, CD._mask(text), O.TextSrc(rel, text))
        self.cache[rel][2].wide_rel = rel
        return self.cache[rel]

    # -------------------------------------------------------------- discovery
    def discover(self):
        masked = {}
        for rel in self.files:
            try:
                masked[rel] = self.text(rel)[1]
            except (OSError, O.OstateError) as e:
                self.problems.append(f'{rel}: {e}')
        self.types = Types(self.ref, self.read, masked)
        refs = {}
        for rel, t in masked.items():
            src = self.text(rel)[2]
            blocks = []   # (var, decl start, region end, kind): the by-value DrawStates of the file
            for m in re.finditer(O.START_RE, t):
                se = O.scope_end(t, m.start())
                um = re.compile(r'\bState\s*\(\s*' + re.escape(m.group('var')) + r'\s*\)').search(t, m.end(), se)
                blocks.append((m.group('var'), m.start(), um.start() if um else se, 'block'))
            for m in COPY_RE.finditer(t):
                ln = O.line_of(t, m.start())
                sp = fn_span(t, src, ln, m.start())
                if sp is None:
                    continue
                se = O.scope_end(t, m.start())
                um = re.compile(r'\bState\s*\(\s*' + re.escape(m.group('var')) + r'\s*\)').search(t, m.end(), se)
                hi = um.start() if um else se
                u = Unit('copy', rel, src.name(ln), sp, m.group('src').strip(), m.end(), hi, ln, var=m.group('var'))
                u.decl = m.start()
                self.units.append(u)
                self.copies[(rel, sp, m.group('var'), m.start())] = u
                blocks.append((m.group('var'), m.start(), hi, 'copy'))
            for m in STORE_RE.finditer(t):
                if m.group('op') == '->' and plant() == 'no_arrow':
                    continue
                pos = m.start()
                ln = O.line_of(t, pos)
                bm = BASE_RE.search(t[max(0, pos - 200):pos])
                if not bm:
                    continue
                base = re.sub(r'\s+', '', bm.group(1))
                sp = fn_span(t, src, ln, pos)
                if sp is None:
                    continue
                ps, _ops = parts_of(base)
                first = ps[0][0]
                own = [b for b in blocks if b[0] == first and sp[0] <= b[1] < pos]
                if own and len(ps) == 1:
                    var, start, end, kind = max(own, key=lambda b: b[1])
                    if pos < end:
                        continue   # a `{}` block's store (q ostate's) or a copy block's own (read with it)
                    self.aside.append((rel, ln, f'a store to {var} after the State({var}) that recorded it (line {O.line_of(t, end)}): not that draw\'s state'))
                    continue
                typ, why = self.type_of(ps, t[sp[0]:pos])
                if typ is None:
                    continue   # not resolved: q pairs --coverage keeps it OUTSIDE
                if not is_ds(typ) and plant() != 'any_type':
                    self.aside.append((rel, ln, f'not a DrawState: {why}'))
                    continue
                owner = None
                if len(ps) > 1:
                    pt, _w = self.type_of(ps[:-1], t[sp[0]:pos])
                    owner = (short(pt), ps[-1][0]) if pt else None
                key = (rel, sp, base)
                u = refs.get(key)
                if u is None:
                    body = t.find('{', sp[0], sp[1])
                    u = refs[key] = Unit('ref', rel, src.name(ln), sp, base, body + 1 if body >= 0 else sp[0], sp[1], ln, owner=owner)
                    self.units.append(u)
        for u in self.units:   # the decoders: the game's device words into a DrawState (DrawFixed::state)
            t = masked[u.rel]
            if u.kind == 'ref' and DECODER_RE.search(t[u.span[0]:u.span[1]]):
                if u.owner:
                    self.game_members[u.owner] = u.fn
                u.decoder = True

    def type_of(self, ps, ftext):
        """-> (type, how) of a base [(name, index)] in a function's text up to the store, else (None, why not)"""
        d = Types.decl(ps[0][0], ftext)
        if d is None:
            return None, f'{ps[0][0]}: no declaration in the function'
        typ, refm, _m = d
        if typ == 'auto':
            return None, f'{ps[0][0]} is auto'
        how = f'{ps[0][0]} is {short(typ)}'
        for n, _ix in ps[1:]:
            mt = self.types.member(typ, n)
            if mt is None:
                return None, f'{short(typ)}::{n} not found'
            how += f'.{n} is {short(mt)}'
            typ = mt
        return typ, how

    # -------------------------------------------------------------- the state of a unit
    def entries_of_block(self, rel, start):
        """a `{}` block's final state (qostate.ours_state), as entries"""
        st, _ln = O.ours_state({}, {'file': rel, 'from': start, 'end': None}, self.read)
        out = {}
        for f in O.FIELDS:
            out[f] = [(v, st[f]['when'].get(w), w.split(' ')[0], 'default' if w.endswith('{})') else 'set', {})
                      for v, ws in st[f]['vals'].items() for w in sorted(set(ws))]
        return out

    def resolve(self, rel, sp, expr, pos, depth=0):
        """-> ({field: [entry]}, description) of a DrawState expression at pos in a function"""
        e = O.strip_parens(re.sub(r'\s+', ' ', expr))
        t = self.text(rel)[1]
        if depth > DEPTH:
            return self.named(f'{e} (resolution deeper than {DEPTH})'), e
        if re.fullmatch(r'\w+', e):
            cands = [(k, u) for k, u in self.copies.items() if k[0] == rel and k[1] == sp and k[2] == e and k[3] < pos]
            blocks = [m for m in re.compile(O.START_RE).finditer(t, sp[0], pos) if m.group('var') == e]
            best_copy = max(cands, key=lambda kv: kv[0][3]) if cands else None
            best_block = blocks[-1] if blocks else None
            if best_block and (not best_copy or best_block.start() > best_copy[0][3]):
                return self.entries_of_block(rel, best_block.start()), f'{e}{{}} @{rel}:{O.line_of(t, best_block.start())}'
            if best_copy:
                u = best_copy[1]
                return self.state_of(u, depth + 1), f'{e} (the copy @{rel}:{u.at_ln})'
            d = Types.decl(e, t[sp[0]:pos])
            what = f'{short(d[0])}{d[1]} {e}' if d else e
            fn = self.text(rel)[2].name(O.line_of(t, pos))
            got = A.climb(self, rel, sp, e, fn, depth) if plant() != 'no_climb' else None   # S111 CDARMS: a parameter: what its callers pass
            if got is not None:
                return got
            return self.named(f'{what} in {fn} @{rel}:{O.line_of(t, pos)}: no DrawState of that function (a parameter: its callers\' state, not followed)'), e
        ps, _ops = parts_of(e)
        if len(ps) > 1 and re.fullmatch(r'[\w.\->\[\]\s]+', e):
            pt, _w = self.type_of(ps[:-1], t[sp[0]:pos])
            if pt:
                key = (short(pt), ps[-1][0])
                if key in self.game_members:
                    return ({f: [(O.GAME, None, f'{rel}:{O.line_of(t, pos)}', 'copy', {})] for f in O.FIELDS},
                            f'{key[0]}::{key[1]} (the game\'s device words, decoded by {self.game_members[key]})')
                got, names = self.writers(key, depth)
                return got, f'{key[0]}::{key[1]}' + (f" (written by {'; '.join(names)})" if names else '')
        return self.named(f'{e} (not read)'), e

    def named(self, what):
        return {f: [(f'RULE: copied from {what}', None, None, 'copy', {})] for f in O.FIELDS}

    def writers(self, key, depth):
        """T::m's whole writers `W.m = SRC;` (W declared T in its function) -> (the union of their sources' entries, the writers named)"""
        struct, member = key
        out, names = {f: [] for f in O.FIELDS}, []
        pat = re.compile(r'\b(\w+)\s*(?:\.|->)\s*' + re.escape(member) + r'\s*=(?!=)\s*([^;=]+?)\s*;')
        for rel in self.files:
            try:
                _text, t, src = self.text(rel)
            except (OSError, O.OstateError):
                continue
            for m in pat.finditer(t):
                ln = O.line_of(t, m.start())
                sp = fn_span(t, src, ln, m.start())
                if sp is None:
                    continue
                d = Types.decl(m.group(1), t[sp[0]:m.start()])
                if not d or short(d[0]) != struct:
                    continue
                got, desc = self.resolve(rel, sp, m.group(2), m.start(), depth + 1)
                names.append(f'{rel}:{ln} {src.name(ln)}: {desc}')
                for f in O.FIELDS:
                    out[f] += got.get(f, [])
        if not names:
            return self.named(f'{struct}::{member} (no writer found)'), []
        return out, names

    def state_of(self, u, depth=0):
        if u.state is not None:
            return u.state
        if u.busy:
            return self.named(f'{u.base} (a cycle)')
        u.busy = True
        rel = u.rel
        t = self.text(rel)[1]
        if u.kind == 'copy':
            base, desc = self.resolve(rel, u.span, u.base, u.decl, depth)
            at = f'{rel}:{u.at_ln}'
            st = {f: [(v, c, at, 'copy', {'base': u.base, 'base_src': desc, **({'base_at': a} if a and a != at else {})})
                      for v, c, a, _k, _x in base.get(f, [])] for f in O.FIELDS}
            pre = re.compile(r'(?<![\w.>])' + re.escape(u.var) + r'\s*(?:\.|->)\s*')
        else:
            st = {f: [] for f in O.FIELDS}
            pre = prefix_re(u.base)
        self.walk(u, t, pre, st)
        u.state, u.busy = st, False
        return st

    def walk(self, u, t, pre, st):
        """the unit's own stores over [lo, hi): qostate.ours_state's loop with the base rewritten to `__W.`"""
        ftext = t[u.span[0]:u.span[1]]
        ctx = O.Ctx([t], ftext)
        aliases = set()
        for pos, stmt, conds in O.statements(t, u.lo, u.hi, [], []):
            live, skip = [], False
            for c, taken in conds:
                rc = pre.sub('__W.', c)
                fm = re.fullmatch(r'for \((?:const\s+)?(?:auto|(?:rdr::)?BlendState)\s*&\s*(\w+)\s*:\s*__W\.\s*blend\s*\)', rc)
                if fm:   # a range-for over the four blends: it always runs; its variable stands for blend[0]
                    aliases.add(fm.group(1))
                    continue
                v = O.eval_cond(c, ctx, {})
                if v is None:
                    live.append(c if taken else f'!({c})')
                elif v != taken:
                    skip = True
            if skip:
                continue
            rs = pre.sub('__W.', stmt)
            aliases |= set(re.findall(r'(?:rdr::BlendState|auto)\s*&\s*(\w+)\s*=\s*__W\s*\.\s*blend\s*\[\s*0\s*\]\s*;', rs))
            for part in O._depth_split(rs.rstrip().rstrip(';'), ','):
                paths, expr = O.assign_targets(O.CASE_RE.sub('', part, count=1), '__W', aliases)
                if not paths:
                    continue
                key = part.strip()[:20].replace('__W.', '')
                off = stmt.find(key) if key and key in stmt else 0
                where = f'{u.rel}:{O.line_of(t, pos + max(off, 0))}'
                for path in paths:
                    fields = O.WHOLE_BLEND if path == 'blend[0]' else (O.OURS_FIELD[path],) if path in O.OURS_FIELD else ()
                    zero = path == 'blend[0]' and ZERO_BLEND_RE.fullmatch(expr.strip())
                    vals = {0} if zero else O.eval_val(expr, ctx) if fields else set()
                    for f in fields:
                        fv = vals
                        if f == 'cmask' and path == 'blend[0]' and not zero:
                            fv = {f'RULE: {expr.strip()} (the whole blend state)'}
                        cond = ' && '.join(live) or None
                        if not live and not (u.kind == 'copy' and plant() == 'no_override'):
                            st[f] = []
                        for v in sorted(fv, key=str):
                            st[f].append((v, cond, where, 'set', {}))

    # -------------------------------------------------------------- the facts
    def facts(self):
        out = []
        for u in self.units:
            if getattr(u, 'decoder', False) and plant() != 'no_decoder':
                t = self.text(u.rel)[1]
                for m in STORE_RE.finditer(t, u.span[0], u.span[1]):
                    bm = BASE_RE.search(t[max(0, m.start() - 200):m.start()])
                    if bm and re.sub(r'\s+', '', bm.group(1)) == u.base:
                        self.aside.append((u.rel, O.line_of(t, m.start()),
                                           f'the game\'s own state, decoded: {u.fn} reads the guest device words (Ld32(base, dev + kDev..)) into '
                                           f'{"::".join(u.owner) if u.owner else u.base}; a copy of it is read as the game\'s word'))
                continue
            try:
                st = self.state_of(u)
            except (OSError, O.OstateError, CD.DiffError) as e:
                self.problems.append(f'{u.rel}:{u.at_ln}: {e}')
                continue
            block = f'{u.rel}:{u.at_ln}' if u.kind == 'copy' else f'{u.rel}:{min(int(e[2].rpartition(":")[2]) for f in O.FIELDS for e in st[f] if e[2]) if any(e[2] for f in O.FIELDS for e in st[f]) else u.at_ln}'
            for f in O.FIELDS:
                seen = set()
                for v, c, at, kind, extra in st[f]:
                    k = (str(v), c, at, kind)
                    if k in seen:
                        continue
                    seen.add(k)
                    out.append(O._fact(u.fn, O.FIELD_KEY[f], O.val_of(v), c, at or block, kind, block=block, wide=u.kind,
                                       **({'base': u.base} if u.kind == 'ref' else {}), **extra))
        return out


def wide_files(ref, read):
    """the .h / .cpp / .inc files of WIDE_DIRS (recursive; tests left out) that hold a DrawState store or copy"""
    pat = r'DrawState|(\.|->)\s*(depth_test|depth_write|depth_compare|cull|stencil_test|blend\s*\[)'
    if ref:
        r = subprocess.run(['git', 'grep', '-l', '-E', pat, ref, '--'] + list(WIDE_DIRS), cwd=O.REPO, capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
        if r.returncode not in (0, 1):
            raise O.OstateError(f'git grep at {ref}: {r.stderr.strip()[:200]}')
        rels = [ln.split(':', 1)[1] for ln in r.stdout.splitlines() if ':' in ln]
    else:
        rels = []
        for d in WIDE_DIRS:
            for dp, dn, fn in os.walk(os.path.join(O.REPO, d.replace('/', os.sep))):
                dn[:] = [x for x in dn if not x.startswith('.') and x != '__pycache__']
                for n in fn:
                    rel = os.path.relpath(os.path.join(dp, n), O.REPO).replace(os.sep, '/')
                    if n.endswith(EXTS):
                        try:
                            if re.search(pat, read(rel)):
                                rels.append(rel)
                        except (OSError, O.OstateError):
                            continue
    return sorted(r for r in rels if r.endswith(EXTS) and 'test' not in r.rpartition('/')[2].lower())


# ------------------------------------------------------------------ (c) the bind records an alpha test is read from
ALPHA_SRC_RE = re.compile(r'\b(\w+)\s*(?:\.|->)\s*alpha_test\b')
MAKE_RE = re.compile(r'\s*=\s*(?:std::make_(?:unique|shared)\s*<\s*(?:\w+::)*(\w+)\s*>|new\s+(?:\w+::)*(\w+)\b)')


def decl_type(var, text):
    """Types.decl, with `auto V = std::make_unique<T>()` / `new T` read as T"""
    d = Types.decl(var, text)
    if d and d[0] == 'auto':
        m = MAKE_RE.match(text, d[2].end())
        return m.group(1) or m.group(2) if m else 'auto'
    return d[0] if d else None


def bind_facts(w, prior):
    """S111 OSTATEWIDE (GINHERIT's ALPHAWREST re-find): a block's alpha test read from a record's flag (`b.alpha_test`, b a `Bound` that
    FrontEnd::BindMaterial / BindRule fill) is the state that bind fixes for every later draw of the material. For each type T the
    alpha facts of `prior` name as their source (`X.alpha_test`, X declared T in the fact's function; an InstancedDraw is the caller
    reader's), every writer `V.alpha_test = EXPR` / `V->alpha_test = EXPR` (V declared T, or `auto V = std::make_unique<T>()`) in the wide
    files is a fact of the writer's function (kind `bind`, store T::alpha_test, its if / for conditions, the value read by
    qostate.eval_bool: the material flag, a rule of ours), as is `V->program = ProgramFor(.., .., ALPHA)` when ALPHA is not V's own flag
    (store T::program: the alpha twin bound); the source fact gets `bind` = those writers. -> (facts, the types linked)"""
    types = {}
    for f in prior:
        if f.get('kind') != 'alpha' or f.get('field') != 'alpha_test' or not isinstance(f.get('value'), dict) or 'material' not in f['value']:
            continue
        rel, _, ln = f['at'].rpartition(':')
        if not ln.isdigit():
            continue
        try:
            _text, t, src = w.text(rel)
        except (OSError, O.OstateError):
            continue
        upto = w_off(t, int(ln))
        sp = fn_span(t, src, int(ln), upto)
        if sp is None:
            continue
        for var in set(ALPHA_SRC_RE.findall(str(f['value']['material']))):
            typ = decl_type(var, t[sp[0]:upto])
            if typ and typ not in ('auto', 'InstancedDraw') and not is_ds(typ):
                types.setdefault(short(typ), []).append(f)
    out = {}
    for typ in types:
        pat = re.compile(r'\b(\w+)\s*(?:\.|->)\s*(alpha_test|program)\s*=(?!=)\s*([^;]+?)\s*;')
        for rel in w.files:
            try:
                _text, t, src = w.text(rel)
            except (OSError, O.OstateError):
                continue
            walked = {}
            for m in pat.finditer(t):
                ln = O.line_of(t, m.start())
                sp = fn_span(t, src, ln, m.start())
                if sp is None or short(decl_type(m.group(1), t[sp[0]:m.start()]) or '') != typ:
                    continue
                expr = m.group(3)
                if m.group(2) == 'program':
                    pm = re.fullmatch(r'\w+\s*\((.*)\)', expr.strip(), re.S)
                    args = O._depth_split(pm.group(1), ',') if pm else []
                    if len(args) < 3 or re.search(r'\b' + re.escape(m.group(1)) + r'\s*(?:\.|->)\s*alpha_test\b', args[2]):
                        continue   # the twin follows the record's own flag (its alpha_test store is the fact)
                    expr = args[2].strip()
                if sp not in walked:
                    body = t.find('{', sp[0], sp[1])
                    walked[sp] = O.statements(t, body + 1, sp[1] - 1, [], []) if body >= 0 else []
                cond, spos = None, m.start()
                for pos, stmt, conds in walked[sp]:
                    if pos <= m.start() < pos + len(stmt):
                        cond, spos = O._and(*[c if taken else f'!({c})' for c, taken in conds]), pos
                        break
                ctx = O.Ctx([t], t[sp[0]:sp[1]])
                arm = A.arm_keys(w, rel, sp, spos, ln)   # S111 CDARMS: its if-arm, the comment block above it, that comment's scope words
                for v, kc in O.eval_bool(expr, ctx, set()):
                    out.setdefault(typ, []).append(O._fact(src.name(ln), 'alpha_test', v, O._and(cond, kc), f'{rel}:{ln}', 'bind',
                                                           store=f'{typ}::{m.group(2)}', src=expr.strip(),
                                                           what='the bind record a later draw reads its alpha test from', **arm))
    for typ, fs in types.items():
        link = [f"{b['fn']} @{b['at']}: {O.value_text(b['value'])}" + (f" when {b['cond']}" if b.get('cond') else '') for b in out.get(typ, [])]
        for f in fs:
            f['bind'] = link or [f'{typ}::alpha_test: no writer found']
    return [b for fs in out.values() for b in fs], sorted(types)


def w_off(t, ln):
    """the offset of the end of line ln"""
    i = -1
    for _ in range(ln):
        i = t.find('\n', i + 1)
        if i < 0:
            return len(t)
    return i


def collect(ref, read, files=None, cache=None, prior=()):
    """-> (facts, problems, counts) of the wide units and the bind records; files None = wide_files (a given list is read as it is: the
    selftest); prior = q ostate's block facts (their alpha sources are linked to the bind records, in place)"""
    cache = {} if cache is None else cache
    try:
        rels = [r for r in files if r.endswith(EXTS)] if files is not None else wide_files(ref, read)
    except O.OstateError as e:
        return [], [f'q ostate wide: {e}'], {'wide_units': 0, 'aside': [], 'bind_stores': 0}
    w = Wide(ref, read, rels, cache)
    w.discover()
    facts = w.facts()
    n = len({f['block'] + '|' + f['fn'] for f in facts})
    binds, btypes = bind_facts(w, prior)
    return facts + binds, w.problems, {'wide_units': n, 'aside': sorted(set(w.aside)), 'bind_stores': len(binds), 'bind_types': btypes}
