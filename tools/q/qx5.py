"""qx5: text-level answers for OUR code the clang AST index cannot give (S103 QX5; docs\\agents\\q_ours.md). All read this checkout's own files: always current.

    py tools/q/q.py ours cvar NAME            a cvar: every REXCVAR_DEFINE_* (ours AND the SDK's thirdparty\\rexglue-sdk-src), its type / default / category / help, the #if block it
                                              sits in (stream = ON build only, stream-free = OFF build only, Android), every REXCVAR_GET / SET / DECLARE use, every string-literal
                                              use (GetFlagByName("x"), flag_int("x")), and every `--NAME=value` on a command line in tools\\**, routes, .tsv, android\\.
                                              NAME may be a part of a name (lists the cvars containing it). `q ours def|refs NAME` answers the same when the C++ index has
                                              no such symbol (a cvar is a macro, not a symbol).
    (qours.py appends to writes / reads / refs)  the sites inside `#if defined(__ANDROID__)` branches: the ON / OFF index never compiles them (text match).

Not an AST: a text scan with a preprocessor-condition stack (#if / #ifdef / #elif / #else / #endif). A condition it cannot read is printed as written.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CPP = ('.h', '.hpp', '.cpp', '.inl', '.cc', '.inc')
OTHER = ('.py', '.ps1', '.json', '.toml', '.txt', '.cmake', '.cfg', '.bat', '.tsv', '.md', '.java', '.kt', '.xml', '.gradle', '.csv')
SKIP = ('__pycache__', 'node_modules', '.git', 'spv')
MAX_BYTES = 1_500_000
TOPS = ('src', 'tools', 'probes', 'config', 'android')

CMT = re.compile(r'//[^\n]*|/\*.*?\*/|R"([^()\s]{0,16})\(.*?\)\1"|"(?:[^"\\\n]|\\.)*"|(?<![0-9a-fA-F])\'(?:[^\'\\\n]|\\.)*\'', re.S)
CMT_STR = re.compile(r'//[^\n]*|/\*.*?\*/|R"([^()\s]{0,16})\(.*?\)\1"|"(?:[^"\\\n]|\\.)*"|(?<![0-9a-fA-F])\'(?:[^\'\\\n]|\\.)*\'', re.S)


def blank(s):
    return re.sub(r'[^\n]', ' ', s)


def define_lines(lines):
    """-> the set of 0-based line indexes that are a #define or a continuation of one (a macro BODY is not a use / definition site)"""
    out, cont = set(), False
    for i, ln in enumerate(lines):
        if cont or re.match(r'^\s*#\s*define\b', ln):
            out.add(i)
            cont = ln.rstrip().endswith('\\')
        else:
            cont = False
    return out


def mask_comments(text):
    """comments blanked (newlines kept), string and char literals left as they are"""
    def rep(m):
        s = m.group(0)
        return blank(s) if s[0] == '/' else s
    return CMT.sub(rep, text)


def mask_all(text):
    """comments blanked and the INSIDE of string / char literals blanked (quotes kept): braces and parens in text no longer count"""
    def rep(m):
        s = m.group(0)
        if s[0] == '/':
            return blank(s)
        return s[0] + blank(s[1:-1]) + s[-1]
    return CMT_STR.sub(rep, text)


def rel_of(root, p):
    return os.path.relpath(p, root).replace('\\', '/')


def walk_files(base, exts, tops=None):
    tops = tops if tops is not None else ('',)
    for top in tops:
        for dp, dn, fn in os.walk(os.path.join(base, top) if top else base):
            dn[:] = [d for d in dn if d not in SKIP and not d.startswith('.')]
            for f in fn:
                if f.endswith(exts):
                    fp = os.path.join(dp, f)
                    try:
                        if os.path.getsize(fp) > MAX_BYTES:
                            continue
                    except OSError:
                        continue
                    yield fp


def read_text(p):
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def sdk_root(root):
    """thirdparty\\rexglue-sdk-src of this checkout, else of the main checkout (a worktree has no thirdparty\\), else None"""
    p = os.path.join(root, 'thirdparty', 'rexglue-sdk-src')
    if os.path.isdir(p):
        return p
    try:
        import qours
        main = qours._main_checkout(root)
    except Exception:
        main = None
    if main:
        p = os.path.join(main, 'thirdparty', 'rexglue-sdk-src')
        if os.path.isdir(p):
            return p
    return None


# ---------------------------------------------------------------- preprocessor condition stack

PP = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elifdef|elifndef|elif|else|endif)\b(.*)$')


def pp_conds(lines):
    """-> a list, per line (0-based), of the tuple of condition strings that must all hold for the line (else / elif branches carry the negation '!(prev)')"""
    stack, out = [], []
    cont = False
    for ln in lines:
        m = None if cont else PP.match(ln)
        cont = ln.rstrip().endswith('\\')
        if m:
            k, rest = m.group(1), re.sub(r'\s*(//.*|/\*.*)$', '', m.group(2)).strip()
            if k == 'if':
                stack.append({'prev': [], 'cur': rest})
            elif k == 'ifdef':
                stack.append({'prev': [], 'cur': f'defined({rest})'})
            elif k == 'ifndef':
                stack.append({'prev': [], 'cur': f'!defined({rest})'})
            elif k in ('elif', 'elifdef', 'elifndef') and stack:
                top = stack[-1]
                if top['cur'] is not None:
                    top['prev'].append(f'!({top["cur"]})')
                top['cur'] = rest if k == 'elif' else (f'defined({rest})' if k == 'elifdef' else f'!defined({rest})')
            elif k == 'else' and stack:
                top = stack[-1]
                if top['cur'] is not None:
                    top['prev'].append(f'!({top["cur"]})')
                top['cur'] = None
            elif k == 'endif' and stack:
                stack.pop()
        flat = []
        for e in stack:
            flat += e['prev']
            if e['cur'] is not None:
                flat.append(e['cur'])
        out.append(tuple(flat))
    return out


ATOMS = (('stream', re.compile(r'(!?)\s*WWE2K14_STREAM\s*\(\s*\)')),
         ('keep_stream', re.compile(r'(!?)\s*defined\s*\(?\s*WWE2K14_KEEP_STREAM\s*\)?')),
         ('android', re.compile(r'(!?)\s*defined\s*\(?\s*__ANDROID__\s*\)?')),
         ('win', re.compile(r'(!?)\s*defined\s*\(?\s*_WIN32\s*\)?')))


def unparen(c):
    """strip parentheses that wrap the whole condition: '(WWE2K14_STREAM())' -> 'WWE2K14_STREAM()'; '(a) && (b)' stays"""
    c = c.strip()
    while c.startswith('(') and c.endswith(')'):
        depth = 0
        for i, ch in enumerate(c):
            depth += ch == '('
            depth -= ch == ')'
            if depth == 0 and i < len(c) - 1:
                return c
        c = c[1:-1].strip()
    return c


def tags_of(conds):
    """-> a set of 'stream' / 'stream-free' / 'android' / 'non-android' / 'win' / 'non-win' / 'dead' (#if 0) from a line's conditions (only the unambiguous atoms; || chains are skipped)"""
    tags = set()
    for c in conds:
        neg = False
        c = unparen(c)
        m = re.fullmatch(r'!\s*\((.*)\)', c)
        if m:
            neg, c = True, unparen(m.group(1))
        if c in ('0', '(0)') and not neg or c == '1' and neg:
            tags.add('dead')
            continue
        if '||' in c:
            continue
        atoms = [unparen(a) for a in c.split('&&')]
        if neg and len(atoms) > 1:
            continue
        for a in atoms:
            for name, rx in ATOMS:
                mm = rx.fullmatch(a)
                if not mm:
                    continue
                n = bool(mm.group(1)) != neg
                if name == 'stream':
                    tags.add('stream-free' if n else 'stream')
                elif name == 'android':
                    tags.add('non-android' if n else 'android')
                elif name == 'win':
                    tags.add('non-win' if n else 'win')
    return tags


def builds_of(tags, rel=''):
    """the builds a line is compiled in, from its tags: 'ON build only (stream)' / 'OFF build only (stream-free)' / 'ON and OFF', plus '; Android only' / '; not Android'"""
    if 'dead' in tags:
        return 'never (#if 0)'
    if 'stream' in tags and 'stream-free' in tags or 'android' in tags and 'non-android' in tags:
        return 'never'
    parts = []
    if 'stream' in tags:
        parts.append('ON build only (stream)')
    elif 'stream-free' in tags:
        parts.append('OFF build only (stream-free)')
    if 'android' in tags:
        parts.append('Android only')
    elif 'non-android' in tags:
        parts.append('not Android')
    return '; '.join(parts) or 'ON and OFF'


# ---------------------------------------------------------------- the build a FILE is in (S103 QX5 review fix 1)

def _list_of(root, rel):
    out = set()
    for ln in read_text(os.path.join(root, rel)).split('\n'):
        ln = ln.strip()
        if ln.startswith('src/'):
            out.add(ln)
    return out


class FileTags:
    """per file: the stream tag its COMPILATION gives it, never guessed from names. CMakeLists drops every tools/scene/keep_stream_sources.txt file from the OFF build
    (-> ON only) and adds tools/scene/keep_stream_off_sources.txt (-> OFF only); a header takes the tag of every #include site of it (the includer's own file tag plus the
    #if lines around the include; all sites agree -> that tag; one site that is not tagged, or no site found -> none). Cycles count as untagged (the safe side)."""

    def __init__(self, root):
        self.root = root
        self.keep = _list_of(root, 'tools/scene/keep_stream_sources.txt')
        self.off = _list_of(root, 'tools/scene/keep_stream_off_sources.txt')
        self.memo = {}
        self.sites = None
        self.conds = {}

    def _build_sites(self):
        import qtext
        names = qtext.by_name(self.root)
        sites = {}
        for p in qtext.walk(self.root, qtext.CPP):
            text = read_text(p)
            if '#' not in text or 'include' not in text:
                continue
            rel = rel_of(self.root, p)
            lines = None
            for i, ln in enumerate(text.split('\n')):
                m = qtext.INC.match(ln)
                if not m or os.path.basename(m.group(2)) not in names:
                    continue
                res = qtext.resolve_inc(self.root, rel, m.group(1), m.group(2), names)
                if res:
                    sites.setdefault(res.split(' ')[0], []).append((rel, i))
        self.sites = sites

    def _line_tags(self, rel, i):
        if rel not in self.conds:
            self.conds[rel] = pp_conds(mask_comments(read_text(os.path.join(self.root, rel))).split('\n'))
        cs = self.conds[rel]
        return {t for t in tags_of(cs[i]) if t in ('stream', 'stream-free')} if i < len(cs) else set()

    def tags(self, rel):
        if rel in self.memo:
            return self.memo[rel]
        self.memo[rel] = frozenset()          # in progress: a cycle counts as untagged
        if rel in self.keep:
            r = frozenset({'stream'})
        elif rel in self.off:
            r = frozenset({'stream-free'})
        elif rel.endswith(('.h', '.hpp', '.inl', '.inc')):
            if self.sites is None:
                self._build_sites()
            seen = []
            for inc_rel, i in self.sites.get(rel, []):
                t = set(self.tags(inc_rel)) | self._line_tags(inc_rel, i)
                if 'stream' in t and 'stream-free' in t:
                    continue                   # a dead include site
                seen.append(frozenset(t))
            r = seen[0] if seen and all(x == seen[0] for x in seen) else frozenset()
        else:
            r = frozenset()
        self.memo[rel] = r
        return r


def chain_text(conds):
    return ' && '.join(conds) if conds else ''


# ---------------------------------------------------------------- cvar

DEF_RX = re.compile(r'\bREXCVAR_DEFINE_(\w+)\s*\(\s*(\w+)\s*,')
GETTERS = ('GetFlagByName', 'SetFlagByName', 'GetFlagInfo', 'GetFlagSource', 'DeferFlagValue', 'PendingCommandLineValue', 'flag_int', 'flag_bool', 'flag_str',
           'QueryFlag', 'RegisterChangeCallback')


def def_matches(cm, cml=None):
    """REXCVAR_DEFINE_* matches of a masked text with their 1-based line, NOT those inside a #define body (the macro's own `REXCVAR_DEFINE_BOOL(name, ...)` is no cvar)"""
    cml = cml if cml is not None else cm.split('\n')
    skip = define_lines(cml)
    for m in DEF_RX.finditer(cm):
        ln = cm.count('\n', 0, m.start()) + 1
        if ln - 1 not in skip:
            yield m, ln


def split_args(s):
    """top-level comma split of a macro argument text (strings and nested ()/{}/[] honoured)"""
    out, cur, depth, i, n = [], [], 0, 0, len(s)
    while i < n:
        c = s[i]
        if c == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    out.append(''.join(cur).strip())
    return out


def macro_args(text, start):
    """text from the '(' at `start` to its matching ')' -> the inner text (None when unbalanced)"""
    depth, i, n = 0, start, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
        i += 1
    return None


def strlit(s):
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', s)
    return ''.join(parts) if parts else s


def classify_use(line, name, cm_line, rel):
    """-> [(kind, detail)] of one line's mentions of the cvar (a line can hold several: REXCVAR_GET(x) and GetFlagByName("x"))"""
    n = re.escape(name)
    out = []
    m = re.search(r'--' + n + r'(?![\w])(?:=([^\s"' + "'" + r',)]+))?', line)
    if m:
        out.append(('cmdline', m.group(1) or ''))
    if re.search(r'REXCVAR_GET\s*\(\s*' + n + r'\s*\)', cm_line):
        out.append(('get', ''))
    if re.search(r'REXCVAR_SET\s*\(\s*' + n + r'(?!\w)', cm_line):
        out.append(('set', ''))
    if re.search(r'REXCVAR_DECLARE\s*\([^,]*,\s*' + n + r'\s*\)', cm_line):
        out.append(('declare', ''))
    if re.search(r'(?<!-)"' + n + r'"', cm_line):
        out.append(('string', next((g for g in GETTERS if g in cm_line), '')))
    if not out:
        out.append(('name', '') if re.search(r'(?<![\w])' + n + r'(?![\w])', cm_line) else ('comment', ''))
    return out


def scan_cvar(root, name):
    """-> {'defs': [...], 'uses': [...]} for the exact cvar NAME over our tree and the SDK's src / include. Build tags = the line's own #if conditions + the FILE's build
    (keep_stream lists, includers of a header: FileTags); a definition that is untagged while another definition of the same name is OFF-only (or ON-only) is the other build's: inferred."""
    wb = re.compile(r'(?<![\w])' + re.escape(name) + r'(?![\w])')
    defs, uses = [], []
    ft = FileTags(root)

    def one(base, p, origin):
        text = read_text(p)
        if name not in text or not wb.search(text):
            return
        rel = rel_of(base, p)
        is_cpp = p.endswith(CPP)
        lines = text.split('\n')
        cm = mask_comments(text) if is_cpp else text
        cml = cm.split('\n')
        conds = pp_conds(cml) if is_cpp else None
        skip = define_lines(cml) if is_cpp else set()
        ftags = set(ft.tags(rel)) if (is_cpp and origin == 'ours') else set()
        if is_cpp:
            for m, ln in def_matches(cm, cml):
                if m.group(2) != name:
                    continue
                op = cm.index('(', m.start())
                inner = macro_args(cm, op) or ''
                a = split_args(inner)
                tg = tags_of(conds[ln - 1]) | ftags
                defs.append({'file': rel, 'line': ln, 'origin': origin, 'type': m.group(1).lower(), 'default': (a[1] if len(a) > 1 else '')[:80],
                             'category': strlit(a[2]) if len(a) > 2 else '', 'help': ' '.join(strlit(' '.join(a[3:])).split())[:600] if len(a) > 3 else '',
                             'cond': chain_text(conds[ln - 1]), 'builds': builds_of(tg), 'tags': sorted(tg)})
        for i, ln_text in enumerate(lines):
            if name not in ln_text or not wb.search(ln_text):
                continue
            if is_cpp and (i in skip or re.search(r'REXCVAR_DEFINE_\w+\s*\(\s*' + re.escape(name) + r'\s*,', cml[i])):
                continue
            tg = (tags_of(conds[i]) | ftags) if is_cpp else set()
            for kind, detail in classify_use(ln_text, name, cml[i] if is_cpp else ln_text, rel):
                if kind == 'comment' and not is_cpp:
                    kind = 'mention'
                uses.append({'file': rel, 'line': i + 1, 'origin': origin, 'kind': kind, 'detail': detail, 'text': ln_text.strip()[:140],
                             'tags': sorted(tg), 'builds': builds_of(tg) if is_cpp else '', 'cond': chain_text(conds[i]) if is_cpp else ''})

    for p in walk_files(root, CPP + OTHER, TOPS):
        one(root, p, 'ours')
    sdk = sdk_root(root)
    if sdk:
        for p in walk_files(sdk, CPP, ('src', 'include')):
            one(sdk, p, 'SDK')
    # (c) one definition is for the OFF build only, another is untagged: the other one is the ON build's (and the reverse), among OUR definitions with the same android tag
    ours = [d for d in defs if d['origin'] == 'ours']
    for d in ours:
        tg = set(d['tags'])
        if tg & {'stream', 'stream-free', 'dead'}:
            continue
        twins = [e for e in ours if e is not d and set(e['tags']) & {'stream', 'stream-free'} and ({'android', 'non-android'} & set(e['tags'])) == ({'android', 'non-android'} & tg)]
        kinds = {t for e in twins for t in e['tags'] if t in ('stream', 'stream-free')}
        if len(kinds) == 1:
            other = 'stream' if kinds == {'stream-free'} else 'stream-free'
            tg.add(other)
            d['tags'] = sorted(tg)
            d['builds'] = builds_of(tg) + ' (inferred)'      # a cvar is defined once per build: the twin definition holds the other build
    return {'defs': defs, 'uses': uses, 'sdk': sdk}


def find_cvar_names(root, part):
    """cvar names containing `part` (case-insensitive), ours and the SDK's"""
    low = part.lower()
    found = {}
    bases = [(root, TOPS, 'ours')]
    sdk = sdk_root(root)
    if sdk:
        bases.append((sdk, ('src', 'include'), 'SDK'))
    for base, tops, origin in bases:
        for p in walk_files(base, CPP, tops):
            text = read_text(p)
            if 'REXCVAR_DEFINE' not in text or low not in text.lower():
                continue
            for m, _ln in def_matches(mask_comments(text)):
                if low in m.group(2).lower():
                    found.setdefault(m.group(2), (origin, rel_of(base, p)))
    return found


def cmd_cvar(root, arg):
    name = arg.strip().lstrip('-').split('=')[0]
    r = scan_cvar(root, name)
    if not r['defs']:
        cands = find_cvar_names(root, name)
        if len(cands) == 1:
            name = next(iter(cands))
            r = scan_cvar(root, name)
            r['guessed'] = name
        elif cands:
            return {'name': arg, 'candidates': {k: f'{v[0]}: {v[1]}' for k, v in sorted(cands.items())}, 'defs': [], 'uses': []}
    r['name'] = name
    return r


def is_cvar_name(root, name):
    """True when NAME is a cvar defined in our tree or the SDK (the fast check q ours def / refs falls back on)"""
    if not re.fullmatch(r'[A-Za-z_]\w*', name):
        return False
    bases = [(root, TOPS)]
    sdk = sdk_root(root)
    if sdk:
        bases.append((sdk, ('src', 'include')))
    for base, tops in bases:
        for p in walk_files(base, CPP, tops):
            text = read_text(p)
            if name in text and any(m.group(2) == name for m, _ln in def_matches(mask_comments(text))):
                return True
    return False


def print_cvar(r, args):
    if r.get('candidates') is not None and not r['defs']:
        print(f'cvar {r["name"]!r}: no exact cvar; {len(r["candidates"])} names contain it:')
        for k, v in list(r['candidates'].items())[:args.limit * 2]:
            print(f'  {k}   ({v})')
        return
    if r.get('guessed'):
        print(f'cvar: no exact {r["name"]!r}; the one name containing it is {r["guessed"]}')
    full = getattr(args, 'all', False)
    uses = r['uses']
    print(f'cvar {r["name"]}: {len(r["defs"])} definition(s), {sum(u["kind"] in ("get", "set", "declare") for u in uses)} REXCVAR_GET/SET/DECLARE, '
          f'{sum(u["kind"] == "string" for u in uses)} by name string, {sum(u["kind"] == "cmdline" for u in uses)} command-line mentions'
          + ('' if r.get('sdk') else '   (SDK source not found: thirdparty\\rexglue-sdk-src missing)'))
    for d in r['defs']:
        print(f'  defined [{d["origin"]}] {d["file"]}:{d["line"]}  REXCVAR_DEFINE_{d["type"].upper()}  default {d["default"]}  category {d["category"]!r}   builds: {d["builds"]}'
              + (f'   #if {d["cond"]}' if d['cond'] else ''))
        if d['help']:
            print(f'      help: {d["help"][:300]}')
    groups = {}
    for u in uses:
        groups.setdefault(u['kind'], []).append(u)
    labels = (('get', 'REXCVAR_GET'), ('set', 'REXCVAR_SET'), ('declare', 'REXCVAR_DECLARE'), ('string', 'by name (a string: GetFlagByName / flag_int ...)'),
              ('name', 'other mentions in code'), ('cmdline', 'on a command line / in a script (--NAME)'), ('mention', 'other mentions (scripts, docs, tables)'), ('comment', 'in comments'))
    for k, label in labels:
        us = groups.get(k)
        if not us:
            continue
        print(f'  {label}: {len(us)}')
        # one group per (file, builds, #if text): a file whose sites sit in different builds / branches (stream, Android ...) prints each apart
        byf = {}
        for u in us:
            byf.setdefault((u['origin'], u['file'], u.get('builds', ''), u.get('cond', '')), []).append(u)
        shown = 0
        for (origin, f, builds, cond), fl in byf.items():
            first = fl[0]
            lns = ','.join(str(x['line']) for x in fl[:(99 if full else 6)]) + ('...' if len(fl) > (99 if full else 6) else '')
            extra = ''
            if k == 'cmdline':
                vals = sorted({x['detail'] for x in fl if x['detail']})
                extra = f'   values: {", ".join(vals[:6])}' if vals else ''
            if k == 'string' and first['detail']:
                extra = f'   via {first["detail"]}'
            tg = f'   [{builds}]' if builds and builds != 'ON and OFF' else ''
            ct = f'   (#if {cond[:70]})' if cond else ''
            print(f'    {"[SDK] " if origin == "SDK" else ""}{f}:{lns}{tg}{ct}{extra}')
            if full or len(fl) == 1:
                for x in (fl if full else fl[:1]):
                    print(f'        {x["line"]}: {x["text"][:110]}')
            shown += 1
            if shown >= (400 if full else args.limit):
                print(f'    ... {len(byf) - shown} more groups (--limit / --all)')
                break


# ---------------------------------------------------------------- inactive (#if __ANDROID__) branches

WRITE_RX = '(?:\\s*(?:=(?!=)|\\+=|-=|\\*=|/=|\\|=|&=|\\^=|<<=|>>=|\\+\\+|--)|\\s*\\.\\s*(?:store|exchange|fetch_\\w+|push_back|emplace\\w*|insert|erase|clear|assign|reset|swap|resize)\\s*\\()'


ANDROID_FILES_VER = 1


def android_files(root):
    """-> [absolute paths] of the src / tools / probes C++ files that mention __ANDROID__, from a cache keyed by (size, mtime) per file: only changed files are re-read
    (out/harness/index/android_files.json; the cost of the text scan used to be every file, every call)"""
    import json
    cp = os.path.join(root, 'out', 'harness', 'index', 'android_files.json')
    cache = {}
    try:
        with open(cp, encoding='utf-8') as f:
            blob = json.load(f)
        if blob.get('ver') == ANDROID_FILES_VER:
            cache = blob['files']
    except (OSError, ValueError, KeyError):
        cache = {}
    out, new, dirty = [], {}, False
    for p in walk_files(root, CPP, ('src', 'tools', 'probes')):
        rel = rel_of(root, p)
        try:
            st = os.stat(p)
        except OSError:
            continue
        key = [st.st_size, st.st_mtime_ns]
        e = cache.get(rel)
        if e and e[0] == key:
            has = e[1]
        else:
            has = '__ANDROID__' in read_text(p)
            dirty = True
        new[rel] = [key, has]
        if has:
            out.append(p)
    if dirty or len(new) != len(cache):
        tmp = cp + f'.{os.getpid()}.tmp'
        try:
            os.makedirs(os.path.dirname(cp), exist_ok=True)
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump({'ver': ANDROID_FILES_VER, 'files': new}, f)
            os.replace(tmp, cp)
        except OSError:
            try:
                os.remove(tmp)
            except OSError:
                pass
    return out


def inactive_sites(root, name, mode='refs', tag='android'):
    """lines inside `#if defined(__ANDROID__)` (positive) branches that mention NAME -> [{file, line, kind, text, cond, exact}]; the ON / OFF index never compiles them.
    A qualified NAME (Thing::Frob, Thing.Frob) is exact only when its qualifier is on the line (case-insensitive: a `thing.Frob()` receiver counts); else the site is
    'name match only' (any other class's Frob looks the same in a text scan)."""
    parts = [x for x in re.split(r'::|\.', name.strip()) if x]
    tail = parts[-1] if parts else ''
    quals = parts[:-1]
    if not re.fullmatch(r'[A-Za-z_]\w*', tail):
        return []
    wb = re.compile(r'(?<![\w])' + re.escape(tail) + r'(?![\w])')
    wr = re.compile(r'(?<![\w])' + re.escape(tail) + WRITE_RX)
    pre = re.compile(r'(\+\+|--)\s*' + re.escape(tail) + r'(?![\w])')
    qrx = re.compile(r'(?<![\w])' + re.escape(quals[-1]) + r'(?![\w])', re.I) if quals and re.fullmatch(r'[A-Za-z_]\w*', quals[-1]) else None
    out = []
    for p in android_files(root):
        text = read_text(p)
        if tail not in text:
            continue
        cml = mask_comments(text).split('\n')
        lines = text.split('\n')
        conds = pp_conds(cml)
        rel = rel_of(root, p)
        for i, ln in enumerate(cml):
            if not wb.search(ln) or PP.match(ln):
                continue
            tg = tags_of(conds[i])
            if tag not in tg:
                continue
            kind = 'write' if (wr.search(ln) or pre.search(ln)) else 'read'
            if mode == 'writes' and kind != 'write' or mode == 'reads' and kind != 'read':
                continue
            out.append({'file': rel, 'line': i + 1, 'kind': kind, 'text': lines[i].strip()[:130], 'cond': chain_text(conds[i]),
                        'exact': qrx is None or bool(qrx.search(ln))})
    return out


def print_inactive(sites, mode, limit=30):
    if not sites:
        return
    print(f'  inside #if __ANDROID__ branches (text match, NOT in the AST index: the ON / OFF builds never compile them): {len(sites)} {mode if mode != "refs" else "reference"} line(s)')
    byf = {}
    for s in sites:
        byf.setdefault(s['file'], []).append(s)
    for f, fl in list(byf.items())[:limit]:
        for s in fl[:12]:
            print(f'    {f}:{s["line"]}  [{s["kind"]}]  {s["text"][:100]}   (#if {s["cond"][:60]})' + ('' if s.get('exact', True) else '   (name match only: the qualifier is not on the line)'))
        if len(fl) > 12:
            print(f'    ... {len(fl) - 12} more in {f}')
