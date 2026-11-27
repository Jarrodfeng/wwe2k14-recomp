"""qsym (S110 QTEXT): `q ours def | refs | callers | callees | writes | reads | fn | body | outline | enclosing | covers` answered by PLAIN TEXT.

There is no index and no compiler. A name is searched as a whole word, case-sensitive, in the C++ files of src / tools / probes / android (one
`git grep -n -w -F --untracked`, the call shape of qx6._git_grep_lines, so it is the same in the main checkout and in any worktree: the files on
disk). Each hit then gets, from the file's own text:
  * code / comment / string   (the file's comments and literals are masked first: qx5.mask_all, block comments included)
  * its enclosing function and namespace / class chain   (a brace scan over the masked text, regex-driven: only { } ; are visited)
  * the #if chain             (qx6._guards)
  * def / write / read / call by line regexes
`Scope::name` keeps a hit when Scope is a whole word on its line, or names an enclosing namespace / class / function qualifier, or the hit's FILE names Scope
somewhere (a member call `obj.name()` never repeats the type); other hits are counted, not shown. That is all the identity there is: a common name is noisy
(`Read`, `Get`); qualify it, or narrow with --in PATH.

JSON shape (kept from the index form): {'symbol', 'matches': [{'qname', 'file', 'line', 'sites': [{'func', 'file', 'lines', 'kinds', 'guards'}], 'n', ...}]}.
"""
import bisect
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

CPP = ('.h', '.hpp', '.cpp', '.cc', '.inl', '.inc')
PATHS = ('src', 'tools', 'probes', 'android')
SKIP = ('__pycache__', 'node_modules', '.git', 'spv', 'spirv')
MAX_BYTES = 1_500_000
IDENT = re.compile(r'^[A-Za-z_]\w*$')
VERBS = ('def', 'refs', 'callers', 'callees', 'writes', 'reads', 'fn', 'body', 'outline', 'enclosing', 'covers')
KEYWORDS = {'return', 'else', 'new', 'delete', 'throw', 'case', 'goto', 'co_return', 'co_await', 'co_yield', 'sizeof', 'if', 'while', 'for', 'switch', 'do',
            'and', 'or', 'not', 'typeid', 'using', 'typedef', 'namespace', 'public', 'private', 'protected', 'catch', 'try', 'default', 'break', 'continue'}
CONTROL = {'if', 'for', 'while', 'switch', 'catch', 'sizeof', 'return', 'defined', 'alignas', 'decltype', 'static_assert', 'alignof', 'noexcept', 'throw'}


class SymError(Exception):
    pass


# ------------------------------------------------------------------------------------------------------------------ search

def _skip_rel(rel):
    return any(x in SKIP for x in rel.split('/'))


def grep(root, tail, files_only=False):
    """-> [(rel, line, text)] of the C++ lines holding `tail` as a whole word (case-sensitive); files_only: -> set of rel paths. git grep, else a walk."""
    args = ['git', '-C', root, 'grep', '-I', '--untracked', '-w', '-F', '-e', tail]
    args.insert(4, '-l' if files_only else '-n')
    args += ['--'] + list(PATHS)
    try:
        p = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
        ok = p.returncode in (0, 1)
    except (OSError, subprocess.SubprocessError):
        ok = False
    out = []
    if ok:
        for ln in p.stdout.splitlines():
            if files_only:
                if ln.endswith(CPP) and not _skip_rel(ln):
                    out.append(ln)
                continue
            m = re.match(r'^(.*?):(\d+):(.*)$', ln)
            if m and m.group(1).endswith(CPP) and not _skip_rel(m.group(1)):
                out.append((m.group(1), int(m.group(2)), m.group(3)))
        return set(out) if files_only else out
    rx = re.compile(r'(?<![\w])' + re.escape(tail) + r'(?![\w])')        # no git here (a fixture folder): walk
    for top in PATHS:
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in SKIP and not d.startswith('.')]
            for f in fn:
                if not f.endswith(CPP):
                    continue
                fp = os.path.join(dp, f)
                rel = os.path.relpath(fp, root).replace('\\', '/')
                try:
                    if os.path.getsize(fp) > MAX_BYTES:
                        continue
                    with open(fp, encoding='utf-8', errors='replace') as fh:
                        for i, ln in enumerate(fh.read().split('\n'), 1):
                            if rx.search(ln):
                                out.append((rel, i, ln))
                                if files_only:
                                    break
                except OSError:
                    pass
    return {r[0] for r in out} if files_only else out


# ------------------------------------------------------------------------------------------------------------------ one file

_STRIP_QUALS = re.compile(r'\s*(?:const|noexcept(?:\s*\([^()]*\))?|override|final|volatile|&&?|->\s*[^()]*|throw\s*\(\s*\))$')
_NAME_AT_END = re.compile(r'((?:[A-Za-z_]\w*\s*::\s*)*(?:~\s*)?[A-Za-z_]\w*|(?:[A-Za-z_]\w*\s*::\s*)*operator\s*(?:\(\s*\)|[^\s\w(]+|new|delete|\w+))\s*$')
_TEMPLATE = re.compile(r'\btemplate\s*<[^<>]*(?:<[^<>]*>[^<>]*)*>')
_TYPE_KW = re.compile(r'\b(class|struct|union)\b')


def _classify(head, parent):
    """the statement text before a `{` -> (kind, name): kind ns | type | enum | func | init | other. Only at file / namespace / class level."""
    h = ' '.join(head.split())
    if parent in ('func', 'other', 'init'):
        return 'other', ''
    h0 = h
    h = _TEMPLATE.sub(' ', h)
    if h.endswith('extern ""') or h == 'extern':
        return 'ns', ''
    m = re.search(r'\bnamespace\b\s*(?:\[\[[^\]]*\]\]\s*)?((?:[A-Za-z_]\w*\s*::\s*)*[A-Za-z_]\w*)?', h)
    if m and '(' not in h[:m.start()] and '=' not in h[:m.start()]:
        return 'ns', (m.group(1) or '').replace(' ', '')
    m = re.search(r'\benum\b\s*(?:class\b|struct\b)?\s*([A-Za-z_][\w:]*)?', h)
    if m and '(' not in h[:m.start()]:
        return 'enum', m.group(1) or ''
    m = _TYPE_KW.search(h)
    if m and '(' not in h[:m.start()] and '=' not in h[:m.start()]:
        rest = re.split(r'(?<!:):(?!:)', h[m.end():], 1)[0]
        rest = re.sub(r'\[\[[^\]]*\]\]|alignas\s*\([^)]*\)|\bfinal\b', ' ', rest)
        ids = re.findall(r'[A-Za-z_][\w:]*', rest)
        return 'type', (ids[-1] if ids else '')
    s = h
    cut = re.search(r'\)\s*:(?!:)', s)
    if cut:
        tailpart = s[cut.end():]
        if re.search(r'[)}]\s*$', tailpart) or not tailpart.strip():
            s = s[:cut.start() + 1]
        elif re.search(r'[\w>]\s*$', tailpart):
            return 'init', ''
    for _ in range(6):
        s2 = _STRIP_QUALS.sub('', s)
        if s2 == s:
            break
        s = s2
    if not s.endswith(')'):
        return ('init', '') if re.search(r'[\w>]$', s) and '=' not in s else ('other', '')
    depth, lp = 0, -1
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ')':
            depth += 1
        elif s[i] == '(':
            depth -= 1
            if depth == 0:
                lp = i
                break
    if lp < 0:
        return 'other', ''
    pre = s[:lp].rstrip()
    for _ in range(3):
        pre = re.sub(r'<[^<>]*>', '', pre)
    nm = _NAME_AT_END.search(pre)
    if not nm:
        return 'other', ''
    name = re.sub(r'\s+', '', nm.group(1)) if 'operator' not in nm.group(1) else nm.group(1)
    base = name.split('::')[-1]
    if base in CONTROL:
        return 'other', ''
    if re.fullmatch(r'[A-Z][A-Z0-9_]{2,}', base):          # REX_HOOK_RAW(sub_X) { ... }: the macro line is the name
        return 'func', f'{name}({s[lp + 1:-1].strip()[:40]})'
    return 'func', name


_INCLUDER = {}


def _includer(root, rel):
    """(includer rel, 1-based line) of a split part, or None (tools/split/read_unit.includer_of)."""
    key = (root, rel)
    if key not in _INCLUDER:
        _INCLUDER[key] = None
        try:
            split_dir = os.path.join(HERE, '..', 'split')
            if split_dir not in sys.path:
                sys.path.insert(0, split_dir)
            import read_unit
            inc = read_unit.includer_of(os.path.join(root, rel))
            if inc:
                _INCLUDER[key] = (os.path.relpath(str(inc[0]), os.path.realpath(root)).replace('\\', '/'), inc[1])
        except (ImportError, OSError, ValueError):
            pass
    return _INCLUDER[key]


def _part_lines(src):
    """[(line, part rel)] of a split parent's own `// split:` include lines (cmd_outline lists them: the members live there)."""
    out = []
    rx = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"[ \t]*//[ \t]*split:')
    for i, ln in enumerate(src.lines, 1):
        m = rx.match(ln)
        if m:
            out.append((i, os.path.normpath(os.path.join(os.path.dirname(src.rel), m.group(1))).replace('\\', '/')))
    return out


class Src:
    """one C++ file: raw lines, masked lines (comments / literals blanked), spans of its namespaces / types / functions, #if chains. Everything lazy."""

    def __init__(self, root, rel, text=None):
        """text None: read root/rel from disk; else the file over that text (S111 CODEDIFF: a file at a git ref, q ostate's TextSrc)"""
        self.rel = rel
        self.root = root
        self._seed = None
        if text is not None:
            self.text = text
        else:
            try:
                with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
                    self.text = f.read()
            except OSError:
                self.text = ''
        self.lines = self.text.split('\n')
        self._m = self._spans = self._guards = self._fn_at = self._sc_at = None

    @property
    def masked(self):
        if self._m is None:
            import qx5
            self._m = qx5.mask_all(self.text)
            self._ml = self._m.split('\n')
        return self._m

    @property
    def mlines(self):
        self.masked
        return self._ml

    @property
    def guards(self):
        if self._guards is None:
            import qx6
            self._guards = qx6._guards(self.masked)
        return self._guards

    def guard_text(self, ln):
        g = self.guards
        return ' && '.join(x for x in (g[ln - 1] if ln - 1 < len(g) else []) if x)

    @property
    def seed(self):
        """[(kind, name)] outermost first: the namespaces / types a split PART's text sits in, i.e. its includer's chain at the
        `#include "x/part.inc"  // split: NAME` line (tools/split/read_unit.includer_of; nested parts chain up). [] for any other file
        (an unmarked .inc such as binder_ring_d3.inc is not a part). S111 SPLIT-WIRE review: without it Binder's members in
        binder/*.inc read as free functions and `q ours fn Binder::X` found nothing."""
        if self._seed is None:
            self._seed = []
            inc = _includer(self.root, self.rel) if self.rel.endswith('.inc') else None
            if inc:
                prel, line = inc
                p = Src(self.root, prel)
                p._scan()
                if line < len(p._fn_at) and p._fn_at[line] is None:      # (a part inside a function body: no scope to carry)
                    chain = []
                    i = p._sc_at[line]
                    while i is not None:
                        sp = p._spans[i]
                        if sp['kind'] in ('ns', 'type'):
                            chain.append((sp['kind'], sp['name']))
                        i = sp['pidx']
                    self._seed = p.seed + chain[::-1]
        return self._seed

    def seed_words(self):
        return {w for _k, n in self.seed for w in [n] + n.split('::') if w}

    def _scan(self):
        if self._spans is not None:
            return
        text = self.masked
        # preprocessor lines (with continuations) are not code for the brace scan: blank them (same offsets)
        def blank_pp(m):
            return re.sub(r'[^\n]', ' ', m.group(0))
        code = re.sub(r'(?m)^[ \t]*#(?:[^\n\\]|\\[^\n]|\\\n)*', blank_pp, text)
        nl = [m.start() for m in re.finditer('\n', code)]
        self._nl = nl
        spans = []        # dicts: kind name qual start end parent
        stack = [{'kind': k, 'name': n, 'seed': True, 'head': 0} for k, n in self.seed]   # open spans (dicts) incl. 'other' / 'init'; a part starts inside its parent's scopes
        head = 0

        def line_of(pos):
            return bisect.bisect_right(nl, pos - 1) + 1 if pos > 0 else 1
        for m in re.finditer(r'[{};]', code):
            c, pos = m.group(), m.start()
            if c == ';':
                if not stack or stack[-1]['kind'] in ('ns', 'type', 'enum'):
                    head = pos + 1
            elif c == '{':
                parent = stack[-1]['kind'] if stack else None
                kind, name = _classify(code[head:pos], parent)
                hs = head + (len(code[head:pos]) - len(code[head:pos].lstrip()))
                chain = [s['name'] for s in stack if s['kind'] in ('ns', 'type') and s['name']]
                qual = '::'.join(chain + [name]) if name else ''
                sp = {'kind': kind, 'name': name, 'qual': qual, 'start': line_of(hs), 'open': line_of(pos), 'end': None, 'head': head}
                sp['pidx'] = stack[-1]['idx'] if stack and 'idx' in stack[-1] else None
                if kind in ('ns', 'type', 'enum', 'func'):
                    sp['idx'] = len(spans)
                    spans.append(sp)
                stack.append(sp)
                head = pos + 1
            else:
                if not stack or stack[-1].get('seed'):
                    head = pos + 1
                    continue
                sp = stack.pop()
                sp['end'] = line_of(pos)
                head = sp['head'] if sp['kind'] == 'init' else pos + 1
        total = len(self.lines) + 2
        for sp in stack:
            sp['end'] = total
        self._spans = spans
        fn_at = [None] * (total + 1)
        sc_at = [None] * (total + 1)
        for i, sp in enumerate(spans):
            arr = fn_at if sp['kind'] == 'func' else sc_at
            for ln in range(sp['start'], min(sp['end'], total) + 1):
                arr[ln] = i
        self._fn_at, self._sc_at = fn_at, sc_at

    @property
    def spans(self):
        self._scan()
        return self._spans

    def func_at(self, ln):
        self._scan()
        i = self._fn_at[ln] if ln < len(self._fn_at) else None
        return self._spans[i] if i is not None else None

    def defines_function(self, ln, tail):
        """True when a function body starts with its head on line `ln` (head start .. opening brace) and is named `tail`"""
        self._scan()
        for i in (self._fn_at[ln] if ln < len(self._fn_at) else None, self._fn_at[ln + 1] if ln + 1 < len(self._fn_at) else None):
            if i is None:
                continue
            sp = self._spans[i]
            if sp['start'] <= ln <= sp['open'] and (sp['name'].split('::')[-1].lstrip('~') == tail or sp['name'].startswith(tail + '(')):
                return True
        return False

    def scope_names(self, ln):
        """names of the namespaces / types / function qualifier enclosing line ln (innermost first)"""
        self._scan()
        out = []
        f = self.func_at(ln)
        i = None
        if f is not None:
            out += [f['name']] + f['name'].split('::')
            i = f['pidx']
        else:
            i = self._sc_at[ln] if ln < len(self._sc_at) else None
        while i is not None:
            sp = self._spans[i]
            if sp['name']:
                out += [sp['name']] + sp['name'].split('::')
            i = sp['pidx']
        for _k, n in reversed(self.seed):
            if n:
                out += [n] + n.split('::')
        return out

    def scope_kind(self, ln):
        self._scan()
        if self.func_at(ln) is not None:
            return 'func'
        i = self._sc_at[ln] if ln < len(self._sc_at) else None
        return self._spans[i]['kind'] if i is not None else None

    def label(self, ln):
        """the group name of a site: the enclosing function, else the enclosing type / namespace, else (file scope)"""
        f = self.func_at(ln)
        if f is not None:
            return f['qual'] or f['name']
        self._scan()
        i = self._sc_at[ln] if ln < len(self._sc_at) else None
        while i is not None:
            sp = self._spans[i]
            if sp['kind'] in ('type', 'enum') and sp['qual']:
                return f'(in {sp["qual"]})'
            i = sp['pidx']
        if self.seed and self.seed[-1][0] == 'type':
            return '(in %s)' % '::'.join(n for _k, n in self.seed if n)
        return '(file scope)'


# ------------------------------------------------------------------------------------------------------------------ one hit

def _rx_name(tail):
    return re.compile(r'(?<![\w])' + re.escape(tail) + r'(?![\w])')


WRITE_AFTER = r'\s*(?:\[[^\]]*\]\s*)*(?:=(?!=)|[-+*/|&^%]=|<<=|>>=|\+\+|--|\.\s*(?:store|exchange|fetch_\w+|push_back|emplace\w*|insert|erase|clear|assign|reset|swap|resize|pop_\w+|fill|append)\s*\()'
_PRE_INC = r'(?:\+\+|--)\s*(?:[\w:]+\s*(?:\.|->)\s*)*$'
_TYPE_PART = r'(?:[\w:<>,\*&\s\[\]]*?[\s\*&>])?'


def _first_token_ok(pre):
    t = re.findall(r'[A-Za-z_]\w*', pre)
    return not t or t[0] not in KEYWORDS


def def_kind(ml, tail, ctx, defined_hint=True):
    """the masked line `ml` holds `tail` as a definition / declaration -> (kind, defined) or None. ctx: 'func' | 'type' | 'enum' | 'ns' | None (the scope kind of the line)"""
    n = re.escape(tail)
    if re.match(r'^\s*#\s*define\s+' + n + r'\b', ml):
        return 'macro', True
    m = re.search(r'\b(class|struct|union|enum(?:\s+class|\s+struct)?|namespace|concept)\s+(?:\[\[[^\]]*\]\]\s*|alignas\s*\([^)]*\)\s*|[A-Z_]{3,}\s+)*(?:\w+::)*' + n + r'\b(?!\s*::)', ml)
    if m and ctx != 'func' or (m and not ml.rstrip().endswith(';')):
        kw = m.group(1).split()[0]
        return {'class': 'class', 'struct': 'struct', 'union': 'union', 'enum': 'enum', 'namespace': 'namespace', 'concept': 'concept'}[kw], not ml.rstrip().endswith(';')
    if re.match(r'^\s*(?:template\s*<[^;{]*>\s*)?using\s+' + n + r'\s*=', ml) or re.match(r'^\s*typedef\b[^;]*\b' + n + r'\s*;', ml):
        return 'alias', True
    if ctx == 'enum' and re.match(r'^\s*' + n + r'\s*(?:=[^=,]*)?,?\s*$', ml):
        return 'enumerator', True
    pre_rx = r'^\s*(?:template\s*<[^;{]*>\s*)?(?:(?:inline|static|constexpr|consteval|virtual|explicit|friend|extern|const|constinit|thread_local|volatile|mutable|REX_\w+|__\w+)\s+)*'
    fm = re.match(pre_rx + r'(' + _TYPE_PART + r')((?:\w+\s*::\s*)*~?)' + n + r'\s*\(', ml)
    if fm:
        typ = fm.group(1)
        if _first_token_ok(typ + fm.group(2)) and (typ.strip() or ctx != 'func') and '(' not in typ:
            if typ.strip() or fm.group(2).strip() or ctx in ('type', 'ns', None):
                body_end = ml.rstrip()
                return 'function', not (body_end.endswith(';') or re.search(r'=\s*(?:0|default|delete)\s*;', ml) is not None)
    vm = re.match(pre_rx + r'(' + _TYPE_PART.replace('*?', '+?', 1) + r')' + n + r'\s*(?:=(?!=)|;|\{|\[|,|:\s*\d)', ml)
    if vm and _first_token_ok(vm.group(1)) and '(' not in vm.group(1) and vm.group(1).strip():
        return ('field' if ctx == 'type' else 'variable'), True
    return None


def classify_hit(src, ln, text, rx, tail):
    """-> dict(kind, col) for one git-grep hit. kind: code | comment | string (plus def info when it is a definition line)"""
    ml = src.mlines[ln - 1] if ln - 1 < len(src.mlines) else ''
    m = rx.search(ml)
    if m:
        return {'kind': 'code', 'col': m.start(), 'ml': ml}
    mm = rx.search(text)
    col = mm.start() if mm else 0
    import qx6
    k = qx6._span_kind('.cpp', '', 0, col, text)
    return {'kind': 'string' if (k == 'string' or ('"' in text and k != 'comment')) else 'comment', 'col': col, 'ml': ml}


def site_kind(ml, col, tail):
    """a code hit that is not a definition -> call | ref | write | rw | read"""
    after = ml[col + len(tail):]
    before = ml[:col]
    w = re.match(WRITE_AFTER, after) is not None or re.search(_PRE_INC, before) is not None
    comp = re.match(r'\s*(?:\[[^\]]*\]\s*)*(?:[-+*/|&^%]=|<<=|>>=|\+\+|--)', after) is not None or re.search(_PRE_INC, before) is not None
    if w:
        return 'rw' if comp else 'write'
    a = after.lstrip()
    if a.startswith('(') or re.match(r'<[^;(){}]*>\s*\(', a):
        return 'call'
    return 'read'


# ------------------------------------------------------------------------------------------------------------------ the collector

class Found:
    def __init__(self):
        self.tail = self.scope = self.name = ''
        self.hits = []             # dicts: file line text kind col def (None | (kind, defined)) sk func scope_ok
        self.srcs = {}
        self.not_in_scope = 0
        self.total = 0


def _seed_words(f, root, rel):
    """the scope words a split part inherits from its parent (empty for any other file; no read unless rel is a .inc)"""
    if not rel.endswith('.inc'):
        return set()
    src = f.srcs.get(rel)
    if src is None:
        src = f.srcs[rel] = Src(root, rel)
    return src.seed_words()


def collect(root, name, light=False):
    """every whole-word hit of the name's tail in the C++ files, classified, narrowed by `Scope::`"""
    nm = name.strip().lstrip('$')
    parts = [p for p in re.split(r'::|\.', nm) if p]
    if not parts:
        raise SymError('give a name')
    tail = parts[-1].lstrip('~')
    if not IDENT.match(tail):
        raise SymError(f'{name}: no such symbol in our code (not a C++ identifier: a word, or Scope::name; anything else: q ours text / q ours lit)')
    scope = parts[-2] if len(parts) > 1 and IDENT.match(parts[-2]) else None
    raw = grep(root, tail)
    f = Found()
    f.tail, f.scope, f.name, f.total = tail, scope, nm, len(raw)
    if not raw:
        return f
    scope_files = grep(root, scope, files_only=True) if scope else None
    rx = _rx_name(tail)
    srx = _rx_name(scope) if scope else None
    if light:      # body / callees: only the files where the name is followed by ( are scanned for function bodies; no per-hit classification
        head = re.compile(r'(?<![\w])' + re.escape(tail) + r'\s*\(')
        for rel, ln, text in raw:
            if rel not in f.srcs and head.search(text) and (not scope or rel in scope_files or scope in _seed_words(f, root, rel)):
                f.srcs[rel] = Src(root, rel)
        return f
    for rel, ln, text in raw:
        if scope and rel not in scope_files and not srx.search(text) and scope not in _seed_words(f, root, rel):      # the scope word is nowhere in this file (nor in a part's parent scopes): cannot be in scope
            f.not_in_scope += 1
            continue
        src = f.srcs.get(rel)
        if src is None:
            src = f.srcs[rel] = Src(root, rel)
        if ln - 1 >= len(src.lines):
            continue
        h = classify_hit(src, ln, text, rx, tail)
        h.update(file=rel, line=ln, text=text)
        ctx = src.scope_kind(ln) if h['kind'] == 'code' else None
        if scope:
            if h['kind'] == 'code':
                strong = bool(srx.search(h['ml'])) or scope in src.scope_names(ln)
            else:
                strong = bool(srx.search(text))
            if not strong and rel not in scope_files:
                f.not_in_scope += 1
                continue
            h['weak'] = not strong      # kept because the FILE names the scope word, the line / enclosing scope does not
        h['def'] = def_kind(h['ml'], tail, ctx) if h['kind'] == 'code' else None
        if h['kind'] == 'code' and not h['def'] and src.defines_function(ln, tail):       # a body the brace scan found: its head line IS the definition (one-line wrappers, odd layouts)
            h['def'] = ('function', True)
        h['sk'] = site_kind(h['ml'], h['col'], tail) if (h['kind'] == 'code' and not h['def']) else None
        h['ctx'] = ctx
        f.hits.append(h)
    return f


def _group(f, hits, kind_of):
    g = {}
    for h in hits:
        src = f.srcs[h['file']]
        fn = src.label(h['line'])
        e = g.setdefault((fn, h['file'], bool(h.get('weak'))), {'func': fn, 'file': h['file'], 'lines': [], 'kinds': set(), 'guards': set(), 'name_only': bool(h.get('weak'))})
        e['lines'].append(h['line'])
        e['kinds'].add(kind_of(h))
        gt = src.guard_text(h['line'])
        if gt:
            e['guards'].add(gt)
    out = []
    for e in g.values():
        e['lines'] = sorted(set(e['lines']))
        e['kinds'] = sorted(e['kinds'])
        e['guards'] = sorted(e['guards'])
        out.append(e)
    out.sort(key=lambda e: (e['file'], e['lines'][0]))
    return out


def summary(groups):
    return f'{len(groups)} places' + (f' in {len({e["file"] for e in groups})} files' if groups else '')


def _defs(f):
    out = []
    for h in f.hits:
        if h['def']:
            src = f.srcs[h['file']]
            sc = src.scope_names(h['line'])
            chain = []
            i = src._sc_at[h['line']] if h['line'] < len(src._sc_at) else None
            while i is not None:
                sp = src._spans[i]
                if sp['name']:
                    chain.insert(0, sp['name'])
                i = sp['pidx']
            q = h['text'].strip()
            mq = re.search(r'((?:\w+\s*::\s*)+)' + re.escape(f.tail) + r'\b', h['ml'])
            qn = '::'.join(chain + ([mq.group(1).replace(' ', '').rstrip(':').rstrip(':')] if mq else []) + [f.tail])
            out.append({'qname': qn, 'kind': h['def'][0], 'file': h['file'], 'line': h['line'], 'defined': bool(h['def'][1]), 'builds': 'text', 'text': q[:130]})
    return out


def _rest_counts(f, shown_kinds):
    cs = sum(1 for h in f.hits if h['kind'] != 'code')
    decl = sum(1 for h in f.hits if h['def'])
    return {'comment_string_lines': cs, 'definition_lines': decl, 'not_in_scope_lines': f.not_in_scope}


def _is_cvar_define(f):
    """a hit is the first argument of a REXCVAR_DEFINE_*( macro (same line, or the macro opens on the line above)"""
    rx = re.compile(r'REXCVAR_DEFINE_\w+\s*\(\s*' + re.escape(f.tail) + r'(?!\w)')
    for h in f.hits:
        if h['kind'] != 'code':
            continue
        if rx.search(h['text']):
            return True
        src = f.srcs[h['file']]
        if h['line'] >= 2 and re.match(r'\s*' + re.escape(f.tail) + r'\s*,', h['text']) and 'REXCVAR_DEFINE_' in src.lines[h['line'] - 2]:
            return True
    return False


def sdk_defines_cvar(root, name):
    """True when the ReXGlue SDK source defines a cvar of that name (REXCVAR_DEFINE_*(name ...): one git grep, no tree walk)"""
    if not IDENT.match(name):
        return False
    try:
        import qx5
        sdk = qx5.sdk_root(root)
        if not sdk:
            return False
        p = subprocess.run(['git', '-C', sdk, 'grep', '--no-index', '-n', '-w', '-F', '-e', name, '--', 'src', 'include'], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
    except (OSError, subprocess.SubprocessError, ImportError):
        return False
    rx = re.compile(r'REXCVAR_DEFINE_\w+\s*\(\s*' + re.escape(name) + r'(?!\w)')
    return any(rx.search(ln) for ln in p.stdout.splitlines())


def _no_symbol(f, name):
    raise SymError(f'{name}: no such symbol in our code (no whole-word, case-sensitive text hit in src / tools / probes; a name tail works: presence::g_crowd)')


def answer(verb, name, root, limit=30):
    """-> (result dict, printer name). Raises SymError. `no such symbol` in the message = nothing found (the caller may try the tools index, cvars ...)."""
    if verb in ('outline', 'enclosing', 'covers'):
        return FILE_VERBS[verb](root, name)
    f = collect(root, name, light=verb in ('body', 'callees'))
    if not f.total:
        raise SymError(f'{name}: no such symbol in our code (no whole-word, case-sensitive text hit in src / tools / probes; a name tail works: presence::g_crowd)')
    if verb in ('body', 'callees'):
        return _func_verbs(verb, f, name, [], limit)
    defs = _defs(f)
    if verb in ('def', 'refs', 'writes', 'reads') and not defs and _is_cvar_define(f):
        raise SymError(f'{name}: no such symbol in our code (it is a cvar: REXCVAR_DEFINE_* is a macro, not a C++ symbol)')
    first = defs[0] if defs else None
    base = {'qname': f.name, 'file': first['file'] if first else None, 'line': first['line'] if first else None, 'defs': defs, 'text_search': True}
    if verb == 'def':
        base.update(_rest_counts(f, None))
        if not defs:
            base['note'] = (f'no definition-shaped line for {f.tail} ({len(f.hits)} lines mention it: q ours refs {f.name}, or q ours text {f.tail})')
        return {'symbol': name, 'matches': defs, 'note': base.get('note'), **{k: base[k] for k in ('comment_string_lines', 'not_in_scope_lines')}}, 'def'
    if verb == 'refs':
        hits = [h for h in f.hits if not h['def']]
        sites = _group(f, hits, lambda h: h['sk'] if h['kind'] == 'code' else h['kind'])
    elif verb == 'callers':
        hits = [h for h in f.hits if h['kind'] == 'code' and not h['def']]
        sites = _group(f, hits, lambda h: 'call' if h['sk'] == 'call' else 'ref')
    elif verb in ('writes', 'reads'):
        want = ('write', 'rw') if verb == 'writes' else ('read', 'rw')
        hits = [h for h in f.hits if h['kind'] == 'code' and not h['def'] and h['sk'] in want]
        sites = _group(f, hits, lambda h: h['sk'])
    elif verb in ('fn', 'callees', 'body'):
        return _func_verbs(verb, f, name, defs, limit)
    else:
        raise SymError(f'{verb}: not a text verb')
    m = {**base, 'sites': sites, 'n': len(hits), 'summary': summary(sites), 'mode': verb, **_rest_counts(f, None)}
    return {'symbol': name, 'matches': [m]}, verb


# ------------------------------------------------------------------------------------------------------------------ function verbs

def _func_spans(f):
    """[(src, span)] the function spans named like the tail (and passing the scope filter)"""
    out = []
    for rel, src in f.srcs.items():
        for sp in src.spans:
            if sp['kind'] != 'func':
                continue
            base = sp['name'].split('::')[-1].lstrip('~')
            if base != f.tail and not sp['name'].startswith(f.tail + '('):
                continue
            if f.scope and f.scope not in sp['qual'].split('::') and f.scope not in src.masked:
                continue
            out.append((src, sp))
    out.sort(key=lambda t: (t[0].rel, t[1]['start']))
    return out


def _body_text(src, sp):
    return src.lines[sp['start'] - 1:sp['end']]


def _callee_names(src, sp):
    rx = re.compile(r'(?<![\w])([A-Za-z_][\w:]*)\s*\(')
    seen = {}
    for i in range(sp['open'], sp['end'] + 1):
        ml = src.mlines[i - 1] if i - 1 < len(src.mlines) else ''
        for m in rx.finditer(ml):
            nm = m.group(1)
            if nm.split('::')[-1] in CONTROL or nm in KEYWORDS:
                continue
            e = seen.setdefault(nm, {'callee': nm, 'lines': [], 'n': 0})
            e['n'] += 1
            e['lines'].append(i)
    return sorted(seen.values(), key=lambda e: e['lines'][0])


def _func_verbs(verb, f, name, defs, limit):
    spans = _func_spans(f)
    if not spans:
        raise SymError(f'{name}: found as text ({len(f.hits)} lines, q ours refs {name}) but no function body named {f.tail} was found by the brace scan: '
                       f'not a function defined in a C++ file (q ours def {name}; a python / ps1 name: q py ...)')
    res = []
    for src, sp in spans[:12]:
        e = {'qname': sp['qual'] or sp['name'], 'file': src.rel, 'line': sp['start'], 'endline': sp['end'], 'kind': 'function', 'defined': True, 'text_search': True}
        if verb == 'callees':
            e['callees'] = _callee_names(src, sp)
            e['note'] = 'names followed by ( in the body, by text: not resolved to a symbol'
        elif verb == 'body':
            e['lines_text'] = _body_text(src, sp)
        else:
            callers = collect_callers_of(f, src, sp)
            body = '\n'.join(src.mlines[sp['open'] - 1:sp['end']])
            raw = '\n'.join(src.lines[sp['open'] - 1:sp['end']])
            e.update(callers=callers, n_callers=sum(len(c['lines']) for c in callers),
                     switches=sorted(set(re.findall(r'REXCVAR_(?:GET|SET)\(\s*(\w+)', raw))),
                     logs=[t for t in re.findall(r'"((?:[^"\\\n]|\\.)*)"', raw) if len(t) > 8 and re.search(r'\[\w+\]|%\w|\{\}', t)][:6],
                     calls=[c['callee'] for c in _callee_names(src, sp)][:30],
                     note='text form: calls by name only; no globals / build tags / units (q ours covers FILE for the units that include it)')
        res.append(e)
    r = {'symbol': name, 'matches': res}
    if len(spans) > 12:
        r['more'] = len(spans) - 12
    return r, verb


def collect_callers_of(f, src, sp):
    hits = [h for h in f.hits if h['kind'] == 'code' and not h['def'] and not (h['file'] == src.rel and sp['start'] <= h['line'] <= sp['end'])]
    return _group(f, hits, lambda h: 'call' if h['sk'] == 'call' else 'ref')


# ------------------------------------------------------------------------------------------------------------------ file verbs

def find_file(root, spec):
    rel = spec.replace('\\', '/')
    r = root.replace('\\', '/').rstrip('/') + '/'
    if rel.startswith(r):
        rel = rel[len(r):]
    if os.path.isfile(os.path.join(root, rel)):
        return rel
    try:
        p = subprocess.run(['git', '-C', root, 'ls-files', '-co', '--exclude-standard', '--', *PATHS], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
        files = [x for x in p.stdout.splitlines() if x.endswith(CPP)]
    except (OSError, subprocess.SubprocessError):
        files = []
    hits = [x for x in files if x == rel or x.endswith('/' + rel)]
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        raise SymError(f'{spec}: ambiguous: ' + ', '.join(hits[:6]))
    raise SymError(f'{spec}: no such C++ file under src / tools / probes / android')


def cmd_outline(root, spec):
    rel = find_file(root, spec)
    src = Src(root, rel)
    rows = []
    field_rx = re.compile(r'^\s*(?:(?:static|inline|constexpr|const|mutable|volatile|thread_local|alignas\([^)]*\))\s+)*(?!using\b|typedef\b|friend\b|return\b|template\b|public\b|private\b|protected\b)'
                          r'[\w:<>,\*&\s\[\]]*?[\s\*&>](\w+)\s*(?:\[[^\]]*\]\s*)*(?:=[^;(]*|\{[^}]*\})?;')
    for i, sp in enumerate(src.spans):
        if sp['kind'] in ('type', 'enum', 'func') and sp['name']:
            e = {'kind': sp['kind'], 'name': sp['qual'] or sp['name'], 'start': sp['start'], 'end': sp['end']}
            if sp['kind'] == 'type':
                fl = []
                for ln in range(sp['open'] + 1, min(sp['end'], len(src.lines)) + 1):
                    if src._sc_at[ln] == i and src._fn_at[ln] is None:
                        m = field_rx.match(src.mlines[ln - 1])
                        if m and '(' not in src.mlines[ln - 1].split('=')[0]:
                            fl.append((m.group(1), ln))
                e['fields'] = fl
            rows.append(e)
    rows.sort(key=lambda r: (r['start'], -r['end']))
    return {'file': rel, 'n_lines': len(src.lines), 'rows': rows, 'parts': _part_lines(src)}, 'outline'


def cmd_enclosing(root, spec):
    m = re.match(r'^(.*?):(\d+)$', spec.strip())
    if not m:
        raise SymError('enclosing: FILE:LINE (src/frame_stats.h:170)')
    rel = find_file(root, m.group(1))
    ln = int(m.group(2))
    src = Src(root, rel)
    f = src.func_at(ln)
    prev = nxt = None
    for sp in src.spans:
        if sp['kind'] != 'func':
            continue
        if sp['end'] < ln and (prev is None or sp['end'] > prev['end']):
            prev = sp
        if sp['start'] > ln and (nxt is None or sp['start'] < nxt['start']):
            nxt = sp
    d = lambda sp: {'qname': sp['qual'] or sp['name'], 'start': sp['start'], 'end': sp['end']} if sp else None
    return {'file': rel, 'line': ln, 'function': d(f), 'previous': d(prev), 'next': d(nxt)}, 'enclosing'


def scripts_naming(root, rel):
    pat = re.compile(re.escape(rel).replace('/', r'[/\\]+'))
    out = []
    for dp, dn, fn in os.walk(os.path.join(root, 'tools')):
        dn[:] = [d for d in dn if d != '__pycache__']
        for fl in fn:
            if fl.endswith(('.py', '.ps1', '.txt', '.cmake')):
                p = os.path.join(dp, fl)
                try:
                    with open(p, encoding='utf-8', errors='replace') as fh:
                        if pat.search(fh.read()):
                            out.append(os.path.relpath(p, root).replace('\\', '/'))
                except OSError:
                    pass
    return sorted(out)


def _build_list_files(root):
    out = []
    for top in ('CMakeLists.txt',):
        if os.path.isfile(os.path.join(root, top)):
            out.append(top)
    for dp, dn, fn in os.walk(os.path.join(root, 'cmake')):
        out += [os.path.relpath(os.path.join(dp, x), root).replace('\\', '/') for x in fn if x.endswith(('.cmake', '.txt'))]
    for dp, dn, fn in os.walk(os.path.join(root, 'tools')):
        dn[:] = [d for d in dn if d != '__pycache__']
        out += [os.path.relpath(os.path.join(dp, x), root).replace('\\', '/') for x in fn if x in ('CMakeLists.txt',) or x.endswith('.cmake')]
    return out


def cmd_covers(root, spec):
    """which units include the file (the text include graph of qgx, transitive), where a build list (CMakeLists / *.cmake) names each unit, which scripts name the file"""
    import qgx
    rel = find_file(root, spec)
    try:
        g = qgx.cmd_includes_graph(root, rel, 'transitive')
    except qgx.GxError as e:
        raise SymError(str(e))
    units = [k for v, k in g['rows'] if k.endswith(('.cpp', '.cc'))]
    if rel.endswith(('.cpp', '.cc')):
        units.insert(0, rel)
    lists = {}
    for bl in _build_list_files(root):
        try:
            with open(os.path.join(root, bl), encoding='utf-8', errors='replace') as fh:
                lists[bl] = fh.read().replace('\\', '/')
        except OSError:
            pass
    unit_rows = []
    for u in units:
        named = [bl for bl, t in lists.items() if u in t]
        unit_rows.append({'unit': u, 'listed_in': named, 'class': 'game/src' if u.startswith('src/') else 'tool/rig' if u.startswith('tools/') else 'probe' if u.startswith('probes/') else 'other'})
    return {'file': rel, 'units': unit_rows, 'scripts_naming_it': scripts_naming(root, rel),
            'note': 'the text include graph: a unit that #includes it, directly or through headers; builds (ON / OFF) are not tracked'}, 'covers'


FILE_VERBS = {'outline': cmd_outline, 'enclosing': cmd_enclosing, 'covers': cmd_covers}


# ------------------------------------------------------------------------------------------------------------------ enclosing function for lit / line / str hits

def encl_func(root):
    """encl(rel, line) -> the enclosing function's qualified name or '' (the closure qtext.cmd_line / cmd_lit / qgl.cmd_str take); one scan per file per process"""
    cache = {}

    def encl(rel, ln):
        if not rel.endswith(CPP):
            return ''
        s = cache.get(rel)
        if s is None:
            s = cache[rel] = Src(root, rel)
        sp = s.func_at(ln) if 0 < ln <= len(s.lines) else None
        return (sp['qual'] or sp['name']) if sp else ''
    return encl


# ------------------------------------------------------------------------------------------------------------------ for q find

def find_block(root, x, spec, in_scope):
    """q find's C++ symbol probe: {'matches': [{qname, kind, file, line, defined, builds, refs: {kind: n}, callers: [{func, file, line}]}]} or None when the text has no definition of it"""
    try:
        f = collect(root, x)
    except SymError:
        return None
    defs = _defs(f)
    if not defs:
        return None
    refs = {}
    for h in f.hits:
        if h['def']:
            continue
        k = h['sk'] if h['kind'] == 'code' else h['kind']
        refs[k] = refs.get(k, 0) + 1
    calls = {}
    for h in f.hits:
        if h['kind'] == 'code' and not h['def'] and in_scope(h['file'], spec):
            fn = f.srcs[h['file']].label(h['line'])
            calls.setdefault((fn, h['file']), h['line'])
    callers = [{'func': k[0], 'file': k[1], 'line': v} for k, v in sorted(calls.items(), key=lambda t: (t[0][1], t[1]))]
    out = []
    for d in defs:
        if spec and not (in_scope(d['file'], spec) or callers):
            continue
        e = dict(d)
        e['refs'] = refs
        if d['kind'] in ('function',):
            e['callers'] = callers
        out.append(e)
    return {'matches': out} if out else None


# ------------------------------------------------------------------------------------------------------------------ printing

def _print_sites(groups, args, indent='  '):
    import qcap
    shown, cut = qcap.cap(groups, args.limit, getattr(args, 'all', False))
    for e in shown:
        ls = ','.join(str(x) for x in e['lines'][:6]) + ('...' if len(e['lines']) > 6 else '')
        g = ('  #if ' + '  |  #if '.join(e['guards'][:3]) + (' ...' if len(e['guards']) > 3 else '')) if e.get('guards') else ''
        no = '  (name only: this line does not name the scope; its file does)' if e.get('name_only') else ''
        print(f'{indent}{e["file"]}:{ls}  in {e["func"]}  [{"/".join(e["kinds"])}]{g}{no}')
    if cut:
        import qours
        print(f'{indent}{qcap.trim_line(len(shown), len(groups), "places", qours.again_call(args) or "--all")}')


def _print_rest(m, args):
    bits = []
    if m.get('comment_string_lines') and m.get('mode') != 'refs':
        bits.append(f'{m["comment_string_lines"]} comment / string lines mention it (q ours text {m["qname"].split("::")[-1]})')
    if m.get('not_in_scope_lines'):
        bits.append(f'{m["not_in_scope_lines"]} lines in files that never name the scope word are not shown (drop the scope to see them)')
    if bits:
        print('  (' + '; '.join(bits) + ')')


def print_def(r, args):
    for m in r['matches'][:args.limit]:
        print(f'{m["kind"]:<12} {m["qname"]}  {m["file"]}:{m["line"]}  {"defined" if m["defined"] else "declared only"}   {m["text"]}')
    if not r['matches']:
        print(r.get('note') or 'no definition-shaped line')
    elif len(r['matches']) > args.limit:
        print(f'  ... {len(r["matches"]) - args.limit} more definitions (--limit)')
    if r.get('not_in_scope_lines'):
        print(f'  ({r["not_in_scope_lines"]} lines in files that never name the scope word are not shown)')


def print_sitelist(r, args):
    for m in r['matches']:
        what = {'refs': 'references', 'callers': 'call / reference sites', 'writes': 'writes', 'reads': 'reads'}.get(m.get('mode'), 'sites')
        d = m['defs'][0] if m['defs'] else None
        print(f'{m["qname"]}  ({d["kind"] + " " + d["file"] + ":" + str(d["line"]) if d else "no definition line found"})  {m["n"]} {what}: {m["summary"]}')
        _print_sites(m['sites'], args)
        if not m['sites']:
            print(f'  (no code line is {what[:-1] if what != "references" else "a reference"} by text)')
        _print_rest(m, args)


def print_fn(r, args):
    for m in r['matches']:
        print(f'== {m["kind"]} {m["qname"]}   {m["file"]}:{m["line"]}-{m["endline"]}')
        print(f'  callers ({m["n_callers"]} lines, {summary(m["callers"])}):')
        _print_sites(m['callers'], type('A', (), {'limit': 8, 'all': getattr(args, 'all', False), 'cmd': 'fn', 'arg': args.arg, 'in_path': getattr(args, 'in_path', None)})(), '    ')
        print(f'  calls by name ({len(m["calls"])}): ' + ', '.join(m['calls'][:14]) + (' ...' if len(m['calls']) > 14 else ''))
        print('  switches read:   ' + (', '.join(m['switches']) or '-'))
        for t in m['logs']:
            print(f'    "{t}"')
        print(f'  ({m["note"]})')
    if r.get('more'):
        print(f'  ... {r["more"]} more functions of that name (--in PATH)')


def print_callees(r, args):
    for m in r['matches']:
        print(f'{m["qname"]}  ({m["file"]}:{m["line"]}-{m["endline"]})  {len(m["callees"])} names followed by ( ; {m["note"]}')
        for e in m['callees'][:args.limit * 2]:
            print(f'  {e["callee"]:<48} at {",".join(str(x) for x in e["lines"][:5])}' + (f'  x{e["n"]}' if e['n'] > 1 else ''))


def print_body(r, args):
    for m in r['matches']:
        print(f'== {m["qname"]}  {m["file"]}:{m["line"]}-{m["endline"]}  [text]')
        lines = m['lines_text']
        for i, t in enumerate(lines[:args.limit]):
            print(f'{m["line"] + i}\t{t}')
        if len(lines) > args.limit:
            print(f'  ... {len(lines) - args.limit} more lines (--limit {len(lines)})')
    if r.get('more'):
        print(f'  ... {r["more"]} more functions of that name (--in PATH)')


def print_outline(r, args):
    print(f'== {r["file"]}  ({r["n_lines"]} lines)  [text brace scan]')
    for e in r['rows'][:args.limit]:
        pad = '  ' if e['kind'] == 'func' else ''
        fl = e.get('fields') or []
        fs = ('   fields: ' + ', '.join(f'{n}@{l}' for n, l in fl[:24]) + (f' ... +{len(fl) - 24}' if len(fl) > 24 else '')) if fl else ''
        print(f'  {e["start"]}-{e["end"]}'.ljust(14) + f'{pad}{e["kind"] if e["kind"] != "func" else "fn"} {e["name"]}{fs}')
    if len(r['rows']) > args.limit:
        print(f'  ... {len(r["rows"]) - args.limit} more (--limit {len(r["rows"])})')
    for ln, part in r.get('parts') or []:
        print(f'  {ln}'.ljust(14) + f'part {part}   (its members: q ours outline {part})')


def print_enclosing(r, args):
    f = r['function']
    if f:
        print(f'{r["file"]}:{r["line"]}  in {f["qname"]}  {f["start"]}-{f["end"]}  [text]')
    else:
        print(f'{r["file"]}:{r["line"]}  not inside a function [text]')
        for k in ('previous', 'next'):
            if r[k]:
                print(f'  {k}: {r[k]["qname"]}  {r[k]["start"]}-{r[k]["end"]}')


def print_covers(r, args):
    print(f'{r["file"]}  ({r["note"]})')
    us = r['units']
    print(f'  units that include it ({len(us)}): ' + ', '.join(u['unit'] + (f' [listed in {u["listed_in"][0]}]' if u['listed_in'] else '') for u in us[:10]) + (' ...' if len(us) > 10 else ''))
    if r['scripts_naming_it']:
        print(f'  scripts that name it: {", ".join(r["scripts_naming_it"][:8])}{" ..." if len(r["scripts_naming_it"]) > 8 else ""}')


PRINTERS = {'def': print_def, 'refs': print_sitelist, 'callers': print_sitelist, 'writes': print_sitelist, 'reads': print_sitelist, 'fn': print_fn,
            'callees': print_callees, 'body': print_body, 'outline': print_outline, 'enclosing': print_enclosing, 'covers': print_covers}
