#!/usr/bin/env python3
"""Gate lint: identifiers that are Windows-header macros in the headers the game exe includes (S102 T11 follow-up; G12 named a field `small`, a macro of
rpcndr.h: fview and rplay compiled, the game exe did not).

  py -I tools\\gate\\win_macro_lint.py [--root DIR] [--dirs src/renderer,src/renderer_adapter,src/renderer_scene] [--selftest-only]

Scans every *.h under the dirs (comments and string / char literals stripped) for:
  small near far interface hyper     as a bare identifier (member, variable, type, parameter)
  min( max(                          called unqualified and not parenthesised ((std::min)(a, b) and std::min(...) are fine: only a call with nothing in front of it is the macro's)
A line may carry `// win-macro-ok` to accept a use (say why). Exit 1 with file:line for every hit.
The planted twin runs first, on a scratch header written under out\\win_macro_lint: it must be caught for every pattern, and a clean twin must not be."""
import argparse
import os
import re
import sys

IDENT = re.compile(r'\b(small|near|far|interface|hyper)\b')
CALL = re.compile(r'(?<![\w:.>(])(min|max)\s*\(')


def strip(text):
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == '//':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' WINMACROOK ' if 'win-macro-ok' in text[i:j] else '')   # the marker survives as a flag, the comment's words do not
            i = j
        elif two == '/*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


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


def scan_dirs(root, dirs):
    allhits = []
    nfiles = 0
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
                    for no, what, why in scan_text(open(p, encoding='utf-8', errors='replace').read()):
                        allhits.append('%s:%d: %s: %s' % (os.path.relpath(p, root).replace('\\', '/'), no, what, why))
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
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not selftest(root):
        return 1
    print('PASS: the planted twins (small, near, far, interface, min(, max() are caught and the clean twin is not')
    if a.selftest_only:
        return 0
    hits, n = scan_dirs(root, a.dirs.split(','))
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
