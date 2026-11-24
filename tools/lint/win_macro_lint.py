#!/usr/bin/env python3
"""Lint: identifiers that are Windows-header macros in the headers the game exe includes (S102 T11 follow-up; G12 named a field `small`, a macro of
rpcndr.h: fview and rplay compiled, the game exe did not).

  py -I tools\\lint\\win_macro_lint.py [--root DIR] [--dirs src/renderer,src/renderer_adapter,src/renderer_scene] [--selftest-only]

Scans every *.h under the dirs (comments and string / char literals stripped) for:
  small near far interface hyper     as a bare identifier (member, variable, type, parameter)
  min( max(                          called unqualified and not parenthesised ((std::min)(a, b) and std::min(...) are fine: only a call with nothing in front of it is the macro's)
A line may carry `// win-macro-ok` to accept a use (say why). Exit 1 with file:line for every hit.
The planted twin runs first, on a scratch header written under out\\win_macro_lint: it must be caught for every pattern, and a clean twin must not be."""
import argparse
import hashlib
import json
import os
import re
import sys

IDENT = re.compile(r'\b(small|near|far|interface|hyper)\b')
CALL = re.compile(r'(?<![\w:.>(])(min|max)\s*\(')


_TOK = re.compile(r'//[^\n]*|/\*.*?(?:\*/|\Z)|"(?:\\.|[^"\\])*"|' + r"'(?:\\.|[^'\\])*'", re.S)


def _sub(m):
    t = m.group(0)
    if t.startswith('//'):
        return ' WINMACROOK ' if 'win-macro-ok' in t else ''   # the marker survives as a flag, the comment's words do not
    if t.startswith('/*'):
        return re.sub(r'[^\n]', ' ', t)
    return t[0] + t[0]


def strip(text):
    # S103 PS3: one regex pass (the char-by-char loop took 3 min 31 s over 417 headers and timed out in three runs)
    return _TOK.sub(_sub, text)


def scan_text(text):
    hits = []
    for no, line in enumerate(strip(text).split('\n'), 1):
        if 'WINMACROOK' in line:
            continue
        for m in IDENT.finditer(line):
            hits.append((no, m.group(1), 'a Windows macro (rpcndr.h / windef.h)'))
        for m in CALL.finditer(line):
            hits.append((no, m.group(1) + '(', 'min / max called unparenthesised and unqualified: the macro when NOMINMAX is not set'))
    return hits


CACHE_VERSION = 'v2-regex-strip'   # bump when IDENT / CALL / strip change: every cached result is then dropped


def load_cache(path):
    try:
        with open(path, encoding='utf-8') as f:
            d = json.load(f)
        return d.get('hits', {}) if d.get('version') == CACHE_VERSION else {}
    except (OSError, ValueError):
        return {}


def save_cache(path, hits):
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = '%s.%d.tmp' % (path, os.getpid())
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump({'version': CACHE_VERSION, 'hits': hits}, f)
        os.replace(tmp, path)
    except OSError:
        pass   # a cache that cannot be written only costs the next run its speed


def scan_dirs(root, dirs, cache_path=None):
    """-> (hit lines, headers read). A header whose content hash (and this lint's CACHE_VERSION) is in the cache is not scanned again: the result is a pure function of the content."""
    allhits = []
    nfiles = 0
    cache = load_cache(cache_path) if cache_path else {}
    fresh = {}
    for d in dirs:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            print('FAIL: directory %s does not exist' % base)
            return None, 0
        for dp, _, fs in os.walk(base):
            for f in fs:
                if f.endswith('.h'):
                    p = os.path.join(dp, f)
                    nfiles += 1
                    raw = open(p, 'rb').read()
                    key = hashlib.sha1(raw).hexdigest()
                    hits = cache.get(key)
                    if hits is None:
                        hits = scan_text(raw.decode('utf-8', errors='replace'))
                    fresh[key] = hits
                    rel = os.path.relpath(p, root).replace(chr(92), '/')
                    for no, what, why in hits:
                        allhits.append('%s:%d: %s: %s' % (rel, no, what, why))
    if cache_path and fresh != cache:
        save_cache(cache_path, fresh)
    return allhits, nfiles


def selftest(root):
    bad = {'struct A { int small; };': 'small', 'void f(int near, int far);': 'near', 'struct interface;': 'interface', 'auto v = min(a, b);': 'min(', 'int x = max (1, 2);': 'max('}
    for src, what in bad.items():
        if not any(h[1] == what for h in scan_text(src + '\n')):
            print('FAIL: the lint did not catch the planted %r (%s)' % (src, what))
            return False
    clean = '// small near\nint a = (std::min)(1, 2);\nauto b = std::max(1, 2);\nconst char* s = "small far";\nint smaller = 1; /* interface */\nint c = obj.min(1);\nint d = x->max(2);\nint small2;\nint ok_small; int far_away;\n'
    clean += 'int accepted = small;  // win-macro-ok test marker\n'
    got = scan_text(clean)
    if got:
        print('FAIL: the lint flagged clean code: %r' % (got,))
        return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--dirs', default='src/renderer,src/renderer_adapter,src/renderer_scene')
    ap.add_argument('--selftest-only', action='store_true')
    ap.add_argument('--no-cache', action='store_true', help='scan every header (the cache is keyed on content hashes)')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not selftest(root):
        return 1
    print('PASS: the planted twins (small, near, far, interface, min(, max() are caught and the clean twin is not')
    if a.selftest_only:
        return 0
    hits, n = scan_dirs(root, a.dirs.split(','), None if a.no_cache else os.path.join(root, 'out', 'win_macro_lint', 'cache.json'))
    if hits is None:
        return 1
    if n < 50:
        print('FAIL: only %d headers read (a lint that reads nothing proves nothing)' % n)
        return 1
    for h in hits:
        print('FAIL ' + h)
    if hits:
        print('FAIL: %d Windows-macro identifier(s) in %d headers (add `// win-macro-ok` with a reason only if the use is meant)' % (len(hits), n))
        return 1
    print('PASS: %d headers under %s: 0 Windows-macro identifiers' % (n, a.dirs))
    return 0


if __name__ == '__main__':
    sys.exit(main())
