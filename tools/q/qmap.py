"""q map: the harness knowledge map (Session 89). "Where is X / what does log line Y mean / which frames are scene Z /
how do I call tool T" in one call, for the main session and for builders.

Two layers:
  * GENERATED from the code at query time (never stale; cached by file time in out\\q\\map_gen.json):
      cvar  - every REXCVAR_DEFINE_* in src\\ and the SDK checkout (thirdparty\\rexglue-sdk-src\\src): name, default,
              group, help text, file:line.
      log   - every log call (REXLOG_* / REXGPU_* / REXSYS_* ... ): the joined format string, file:line, the function
              it is in, and the switches (REXCVAR_GET / cvars::) that function reads before it = what turns it on.
      tool  - every script under tools\\ (.py docstring / .ps1 header, first lines): what it is, how it is called.
  * HAND entries in tools\\q\\map\\*.toml ([[entry]]: name, kind, what, words, where, how, source, date): meanings
    the code cannot say (counter -> summary line, hash -> what it draws, scene -> frames, mechanism -> files).
    Every `where` anchor is "path" or "path::text that must appear in it"; `map --check` flags anchors that are gone.

Use:
  py tools\\q\\q.py map WORDS...            every entry containing all the words (case-insensitive), hand entries first
  py tools\\q\\q.py map --line "LOG LINE"   which format string printed this log line (paste it from a run log)
  py tools\\q\\q.py map --kind log WORDS    one kind only (cvar, log, tool, or a hand kind: scene, counter, hash,
                                           mechanism, file, route ...)
  py tools\\q\\q.py map --check             every hand anchor still there? (exit 1 if not); run it when editing a map file
  py tools\\q\\q.py map --stats             entry counts per kind
Add a hand entry when a question took more than one search to answer (docs\\agents\\builder_brief.md).
"""
import json
import os
import re
import sys
import time
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAP_DIR = ROOT / 'tools' / 'q' / 'map'
CACHE = ROOT / 'out' / 'q' / 'map_gen.json'
CODE_ROOTS = [ROOT / 'src', ROOT / 'thirdparty' / 'rexglue-sdk-src' / 'src']
CODE_EXT = {'.h', '.hpp', '.cpp', '.cc', '.inl'}
TOOL_ROOT = ROOT / 'tools'
CACHE_VERSION = f'3-{Path(__file__).stat().st_mtime_ns}'   # an edit of this scanner rebuilds the cache

# fmt::format pieces count too: long summaries are built from them and logged as one string ("dead passes not drawn").
LOG_RE = re.compile(r'\b((?:REX|XE)[A-Z0-9]*_(?:NOISY_)?(?:INFO|WARN|ERROR|DEBUG|TRACE|CRITICAL)|fmt::format)\(')
CVAR_RE = re.compile(r'\bREXCVAR_DEFINE_(\w+)\(\s*(\w+)\s*,')
SWITCH_RE = re.compile(r'REXCVAR_GET\((\w+)\)|\bcvars::(\w+)')


# ------------------------------------------------------------ C parsing (enough for literals and arg lists)

def _skip_string(t, i):
    q = t[i]; i += 1
    while i < len(t):
        if t[i] == '\\': i += 2; continue
        if t[i] == q: return i + 1
        i += 1
    return i


def split_args(t, i):
    """t[i] is just after '('. Returns (list of top-level argument texts, index after ')')."""
    args, depth, start = [], 0, i
    while i < len(t):
        c = t[i]
        if c in '"\'':
            i = _skip_string(t, i); continue
        if t.startswith('//', i):
            j = t.find('\n', i); i = len(t) if j < 0 else j; continue
        if c in '([{': depth += 1
        elif c in ')]}':
            if depth == 0:
                args.append(t[start:i]); return args, i + 1
            depth -= 1
        elif c == ',' and depth == 0:
            args.append(t[start:i]); start = i + 1
        i += 1
    return args, i


def literal(arg):
    """The joined text of a run of C string literals ("a" "b" -> ab), or None if arg is not one."""
    s = arg.strip()
    if not s.startswith('"'):
        m = re.match(r'(?:u8|L|u|U)?"', s)
        if not m: return None
        s = s[m.end() - 1:]
    out, i = [], 0
    while i < len(s):
        if s[i] == '"':
            j = _skip_string(s, i)
            out.append(s[i + 1:j - 1]); i = j
        elif s[i].isspace():
            i += 1
        elif s.startswith('//', i):
            j = s.find('\n', i); i = len(s) if j < 0 else j
        else:
            return None
    return re.sub(r'\\(.)', lambda m: {'n': ' ', 't': ' ', '0': ''}.get(m.group(1), m.group(1)), ''.join(out))


def scan_code_file(path):
    rel = path.relative_to(ROOT).as_posix()
    try:
        text = path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    lines = text.split('\n')
    starts, pos = [], 0
    for ln in lines:
        starts.append(pos); pos += len(ln) + 1

    def line_of(off):
        lo, hi = 0, len(starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if starts[mid] <= off: lo = mid
            else: hi = mid - 1
        return lo + 1

    def code_of(s):
        s = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', s)   # strings / chars out of the brace count
        return s.split('//', 1)[0]

    def func_of(lineno):
        """(name, first line) of the function around lineno: walk up counting braces; at each opening brace of an
        enclosing block, its header (that line or the ones just above) names a function unless it is a control
        statement, a lambda, a namespace / class / struct, or an initialiser. Review fix: indented member functions
        count, comment lines never do. Not found within 1500 lines -> ('', lineno - 80)."""
        depth = 0
        for k in range(lineno - 2, max(-1, lineno - 1500), -1):
            c = code_of(lines[k])
            st = c.strip()
            if not st or st.startswith(('*', '/*')):
                continue
            depth += c.count('}') - c.count('{')
            if depth >= 0:
                continue
            depth = 0   # an enclosing block opens here
            head = st
            j = k
            while head.startswith('{') or (not re.search(r'\w\s*\(', head) and j > 0 and j > k - 4):
                j -= 1
                prev = code_of(lines[j]).strip()
                head = prev + ' ' + head
            if re.match(r'(?:\}?\s*else\b|if\b|for\b|while\b|switch\b|do\b|try\b|catch\b|case\b|namespace\b|'
                        r'(?:struct|class|enum|union)\b|return\b|extern\b)', head) or re.search(r'\]\s*\(.*\)\s*(?:mutable\s*)?(?:->[^{]*)?\{', head) \
                    or re.search(r'=\s*\{?\s*$', head.split('{')[0]):
                continue
            m = re.search(r'([A-Za-z_~][\w:~]*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*(?:const|override|noexcept|final|\s)*'
                          r'(?:->\s*[\w:<>*& ]+)?\s*(?::[^{]*)?\{', head)
            if m and m.group(1).split('::')[-1] not in ('if', 'for', 'while', 'switch', 'catch', 'REX_HOOK_RAW'):
                return m.group(1), j + 1
            if m:   # a hook body: REX_HOOK_RAW(sub_X) { ... }
                h = re.search(r'REX_HOOK\w*\((\w+)\)', head)
                return (f'hook {h.group(1)}' if h else m.group(1)), j + 1
        return '', max(1, lineno - 80)

    out = []
    for m in CVAR_RE.finditer(text):
        args, _ = split_args(text, m.end() - len(m.group(0)) + m.group(0).index('(') + 1)
        if len(args) < 3: continue
        help_t = literal(args[-1]) or ''
        group = literal(args[-2]) if len(args) >= 4 else ''
        default = args[1].strip() if len(args) >= 3 else ''
        lit = literal(default)
        out.append({'kind': 'cvar', 'name': m.group(2), 'type': m.group(1).lower(), 'default': lit if lit is not None else default,
                    'group': group or '', 'what': ' '.join(help_t.split()), 'at': f'{rel}:{line_of(m.start())}'})
    for m in LOG_RE.finditer(text):
        args, _ = split_args(text, m.end())
        if not args: continue
        fmt = literal(args[0])
        if not fmt or len(fmt) < 4: continue
        if m.group(1) == 'fmt::format' and not re.search(r'[A-Za-z][A-Za-z ,()-]{11,}', fmt): continue   # "{}{} {:.2f}"
        ln = line_of(m.start())
        fn, fstart = func_of(ln)
        body = '\n'.join(lines[fstart - 1:ln])
        sw = []
        for s in SWITCH_RE.finditer(body):
            name = s.group(1) or s.group(2)
            if name in sw: sw.remove(name)
            sw.append(name)
        out.append({'kind': 'log', 'name': ' '.join(fmt.split()), 'level': m.group(1), 'at': f'{rel}:{ln}', 'fn': fn,
                    'switches': list(reversed(sw))[:6]})
    return out


def scan_tool_file(path):
    rel = path.relative_to(ROOT).as_posix()
    try:
        head = path.read_text(encoding='utf-8', errors='replace').split('\n')[:40]
    except OSError:
        return []
    desc = []
    if path.suffix == '.py':
        joined = '\n'.join(head)
        m = re.search(r'^\s*(?:r|u)?("""|\'\'\')(.*?)(?:\1|$)', re.sub(r'^(#![^\n]*\n|#[^\n]*coding[^\n]*\n)', '', joined), re.S | re.M)
        if m:
            desc = [l.strip() for l in m.group(2).strip().split('\n')[:4]]
        else:
            desc = [l.lstrip('#').strip() for l in head if l.startswith('#') and not l.startswith('#!')][:4]
    else:
        body = head
        if body and body[0].startswith('<#'):
            body = [l for l in body[1:] if not l.startswith('#>')]
            desc = [l.strip() for l in body if l.strip()][:4]
        else:
            desc = [l.lstrip('#').strip() for l in body if l.startswith('#')][:4]
    what = ' '.join(d for d in desc if d)
    if not what: return []
    return [{'kind': 'tool', 'name': rel, 'what': what[:400], 'at': rel}]


# ------------------------------------------------------------ the generated layer, cached by file time

def generated():
    try:
        cache = json.loads(CACHE.read_text(encoding='utf-8'))
        if cache.get('v') != CACHE_VERSION: cache = None
    except (OSError, ValueError):
        cache = None
    cache = cache or {'v': CACHE_VERSION, 'files': {}}
    seen, changed = {}, False
    jobs = []
    for root in CODE_ROOTS:
        if root.exists():
            jobs += [(p, scan_code_file) for p in root.rglob('*') if p.suffix in CODE_EXT]
    jobs += [(p, scan_tool_file) for p in TOOL_ROOT.rglob('*') if p.suffix in ('.py', '.ps1')
             and '__pycache__' not in p.parts and 'node_modules' not in p.parts]
    for p, fn in jobs:
        try:
            st = p.stat()
        except OSError:
            continue
        key = p.relative_to(ROOT).as_posix()
        stamp = [st.st_mtime_ns, st.st_size]
        old = cache['files'].get(key)
        if old and old['stamp'] == stamp:
            seen[key] = old
        else:
            seen[key] = {'stamp': stamp, 'entries': fn(p)}; changed = True
    if changed or len(seen) != len(cache['files']):
        cache['files'] = seen
        # (review: several agents may query at once - a per-process temp file, and the cache is optional: a failed
        # write never fails the query.)
        tmp = CACHE.with_suffix(f'.{os.getpid()}.tmp')
        try:
            CACHE.parent.mkdir(parents=True, exist_ok=True)
            tmp.write_text(json.dumps(cache), encoding='utf-8')
            os.replace(tmp, CACHE)
        except OSError:
            try:
                tmp.unlink()
            except OSError:
                pass
    return [e for f in seen.values() for e in f['entries']]


# ------------------------------------------------------------ the hand layer

def hand_entries():
    out, errors = [], []
    for f in sorted(MAP_DIR.glob('*.toml')):
        try:
            data = tomllib.loads(f.read_text(encoding='utf-8'))
        except (OSError, tomllib.TOMLDecodeError) as e:
            errors.append(f'{f.name}: {e}'); continue
        for e in data.get('entry', []):
            e = dict(e)
            e.setdefault('kind', 'note')
            e['file'] = f.name
            if 'name' not in e or 'what' not in e:
                errors.append(f'{f.name}: entry without name / what: {e.get("name")!r}'); continue
            out.append(e)
    return out, errors


def check_anchor(a):
    path, _, needle = a.partition('::')
    p = ROOT / path.replace('\\', '/')
    if not p.exists():
        return f'file gone: {path}'
    if needle:
        try:
            if p.is_dir():
                return f'{path} is a folder; an anchor with text needs a file'
            if needle not in p.read_text(encoding='utf-8', errors='replace'):
                return f'text gone from {path}: {needle!r}'
        except OSError as ex:
            return f'cannot read {path}: {ex}'
    return None


def cmd_check():
    entries, errors = hand_entries()
    bad = 0
    for e in entries:
        for a in e.get('where', []):
            why = check_anchor(a)
            if why:
                bad += 1
                print(f'STALE  {e["file"]}: {e["name"]}: {why}')
    for x in errors:
        bad += 1
        print(f'ERROR  {x}')
    n_anchor = sum(len(e.get('where', [])) for e in entries)
    print(f'map --check: {len(entries)} hand entries, {n_anchor} anchors, {bad} problem(s)')
    return 1 if bad else 0


# ------------------------------------------------------------ search and print

def haystack(e):
    parts = [e.get('name', ''), e.get('what', ''), e.get('kind', ''), ' '.join(e.get('words', [])),
             e.get('how', ''), e.get('fn', ''), e.get('group', ''), ' '.join(e.get('switches', [])),
             ' '.join(e.get('where', [])), e.get('at', '')]
    return '\n'.join(parts).lower()


def fmt_regex(fmt):
    pieces = re.split(r'\{[^{}]*\}', fmt)
    return re.compile('.*?'.join(re.escape(p) for p in pieces), re.S), max((len(p) for p in pieces), default=0)


def clip(text, n, words):
    """text cut to n characters around the first query word it contains (so a long summary shows the matching part)."""
    if len(text) <= n: return text
    low = text.lower()
    at = min((low.find(w) for w in words if w in low), default=0)
    start = max(0, min(at - n // 3, len(text) - n))
    return ('...' if start else '') + text[start:start + n] + ('...' if start + n < len(text) else '')


def show(e, wide=False, words=()):
    k = e['kind']
    if k in ('cvar', 'log', 'tool'):
        e = dict(e)
        for f in ('name', 'what'):
            if f in e and isinstance(e[f], str): e[f] = clip(e[f], 600 if wide else 240, words)
    if k == 'cvar':
        d = e.get('default')
        print(f'[cvar] --{e["name"]}  ({e["type"]}, default {d!s}; {e["group"]})  {e["at"]}')
        print(f'       {e["what"][:600 if wide else 300]}')
    elif k == 'log':
        sw = f'; switches read before it: {", ".join(e["switches"])}' if e.get('switches') else ''
        print(f'[log]  {e["name"][:400 if wide else 220]}')
        print(f'       {e["at"]} in {e.get("fn") or "?"} ({e["level"]}){sw}')
    elif k == 'tool':
        print(f'[tool] {e["name"]}')
        print(f'       {e["what"][:400 if wide else 260]}')
    else:
        print(f'[{k}] {e["name"]}   ({e["file"]}{", " + e["date"] if e.get("date") else ""})')
        print(f'       {e["what"]}')
        if e.get('how'): print(f'       how: {e["how"]}')
        for a in e.get('where', []): print(f'       where: {a}')
        if e.get('source'): print(f'       source: {e["source"]}')


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='q map', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('words', nargs='*')
    ap.add_argument('--line', help='a log line from a run: which format string printed it')
    ap.add_argument('--kind', help='only this kind (cvar, log, tool, scene, counter, hash, mechanism, ...)')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--limit', type=int, default=15)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--wide', action='store_true', help='longer texts')
    a = ap.parse_args(argv)
    if a.check:
        return cmd_check()
    t0 = time.time()
    hand, errors = hand_entries()
    for x in errors: print(f'q map: WARNING {x}', file=sys.stderr)
    gen = generated()
    allx = hand + gen
    if a.stats:
        from collections import Counter
        c = Counter(e['kind'] for e in allx)
        print(', '.join(f'{k} {n}' for k, n in c.most_common()), f'({time.time() - t0:.1f} s)')
        return 0
    if a.kind:
        allx = [e for e in allx if e['kind'] == a.kind]
    if a.line:
        line = a.line.strip()
        line = re.sub(r'^\[[^\]]*\]\s*\[\w+\]\s*\[\w+\]\s*\[t\d+\]\s*', '', line)   # drop the log's time / level / thread
        hits = []
        for e in allx:
            if e['kind'] != 'log': continue
            rx, longest = fmt_regex(e['name'])
            if longest >= 4 and rx.search(line):
                hits.append((longest, e))
        hits.sort(key=lambda h: -h[0])
        res = [e for _, e in hits[:a.limit]]
        if not res: print('q map: no format string matches that line (a line from an old build, or printed by a tool?)')
    else:
        words = [w.lower() for w in a.words]
        if not words:
            ap.print_usage(); return 2
        res = []
        phrase = ' '.join(words)
        for e in allx:
            h = haystack(e)
            if all(w in h for w in words):
                score = (0 if e['kind'] in ('cvar', 'log', 'tool') else -100) - sum(w in e.get('name', '').lower() for w in words) * 10
                if len(words) > 1 and phrase in h: score -= 1000   # the words as one phrase: the exact text wins
                res.append((score, len(e.get('name', '')), e))
        res.sort(key=lambda r: (r[0], r[1]))
        total = len(res)
        res = [e for _, _, e in res[:a.limit]]
        if total > a.limit:
            print(f'({total} matches, showing {a.limit}: add words, --kind, or --limit N)')
        if not res: print('q map: nothing matches all the words (try fewer words; then add a hand entry when you find it)')
    if a.json:
        print(json.dumps(res, indent=1))
    else:
        for e in res: show(e, a.wide, [w.lower() for w in a.words])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
