#!/usr/bin/env python3
"""dev_cvar_lint (S103 P4F review F3): a cvar whose description says "DEV ONLY" must only be READ inside an expression that also tests the build is a +dev
build (wwe2k14::IsDevBuild(), or a local `dev` that was set from it). Otherwise a player's release build honours a switch that was meant for tests (the
P4 movie-planes switch did: its Active() read the cvar alone).

  py -I tools\\scene\\dev_cvar_lint.py --root .           scan src\\ (exit 1 on a violation)
  py -I tools\\scene\\dev_cvar_lint.py --selftest         the planted twin: a bad snippet MUST be flagged, a good one must pass

How a read is judged: REXCVAR_GET(name) is fine when the statement it sits in (from the previous ';' '{' '}' or the start of the line two lines up, to the read)
contains `IsDevBuild()` or the word `dev` (a local bool the caller set from IsDevBuild(); `dev &&`, `dev ?`, `if (dev)`), or when the (file, name) pair is in
ALLOW below with its reason. Definitions and REXCVAR_DECLARE lines are not reads.
"""
import argparse
import os
import re
import sys

# (relative path, cvar name) -> why a read without IsDevBuild() in the statement is still right. Keep this short; every entry is a reason.
ALLOW = {
    ('src/renderer_adapter/live.cpp', 'wwe2k14_rdr_2d_ours'): 'the read only picks which log line to print ("ignored: a release build" vs "needs 2d_record"); q2d_on itself is `dev && ...` two lines up',
    ('src/renderer_adapter/live.cpp', 'wwe2k14_rdr_scene_unrequire'): 'the list is parsed, then ApplyUnrequire(want, dev) applies it only when dev (a release build gets mask 0)',
    ('src/renderer_adapter/live.cpp', 'wwe2k14_rdr_scene_flat'): 'the read only picks which log line to print ("IGNORED (release build)" vs the DEV line); the store is `dev && ...` in start_settings.cpp ApplyStartSettings (moved from live.cpp, S103 SPB)',
    ('src/start_settings.cpp', 'wwe2k14_rdr_scene_unrequire'): 'the list is parsed, then ApplyUnrequire(want, dev) applies it only when dev (a release build gets mask 0); moved here from live.cpp (S103 SPB)',
}

DEFINE = re.compile(r'REXCVAR_DEFINE_\w+\s*\(\s*(\w+)')
GET = re.compile(r'REXCVAR_GET\s*\(\s*(\w+)\s*\)')
STR = re.compile(r'"((?:[^"\\]|\\.)*)"')


def definitions(text):
    """name -> True for every cvar whose description (all adjacent string literals of the define) says dev only."""
    out = {}
    for m in DEFINE.finditer(text):
        i = m.end()
        depth = 1
        j = m.start() + text[m.start():].index('(') + 1
        # find the matching close paren, skipping string literals
        k = j
        while k < len(text) and depth:
            c = text[k]
            if c == '"':
                k += 1
                while k < len(text) and text[k] != '"':
                    k += 2 if text[k] == '\\' else 1
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            k += 1
        body = text[j:k]
        desc = ' '.join(s for s in STR.findall(body))
        if re.search(r'dev\s*only', desc, re.IGNORECASE):
            out[m.group(1)] = True
    return out


def statement_before(text, pos):
    """The text of the statement a read at `pos` sits in: back to the previous ';' '{' '}' (but at least the line's two previous lines)."""
    cut = max(text.rfind(';', 0, pos), text.rfind('{', 0, pos), text.rfind('}', 0, pos))
    line_start = pos
    for _ in range(3):
        line_start = text.rfind('\n', 0, line_start)
        if line_start < 0:
            line_start = 0
            break
    return text[max(cut + 1, line_start):pos]


GUARD = re.compile(r'IsDevBuild\s*\(\s*\)|\bdev\b')


def violations(text, names, rel):
    out = []
    for m in GET.finditer(text):
        name = m.group(1)
        if name not in names or (rel, name) in ALLOW:
            continue
        before = statement_before(text, m.start())
        after = text[m.end():m.end() + 0]
        if GUARD.search(before + after):
            continue
        line = text.count('\n', 0, m.start()) + 1
        out.append((rel, line, name))
    return out


def scan(root):
    srcdir = os.path.join(root, 'src')
    files = []
    for d, _, fs in os.walk(srcdir):
        for f in fs:
            if f.endswith(('.h', '.cpp', '.hpp', '.cc')):
                files.append(os.path.join(d, f))
    texts = {}
    names = {}
    for p in files:
        try:
            sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
            from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)
            texts[p] = read_path(p)
        except OSError:
            continue
        names.update(definitions(texts[p]))
    bad = []
    for p, t in texts.items():
        rel = os.path.relpath(p, root).replace('\\', '/')
        bad += violations(t, names, rel)
    return names, bad


BAD_SNIPPET = '''REXCVAR_DEFINE_BOOL(wwe2k14_x, false, "wwe2k14",
                    "S9 (DEV ONLY, not a player setting): a test switch");
inline bool Active() { return REXCVAR_GET(wwe2k14_x); }
'''
GOOD_SNIPPET = '''REXCVAR_DEFINE_BOOL(wwe2k14_x, false, "wwe2k14",
                    "S9 (DEV ONLY, not a player setting): a test switch");
inline bool Active() { return wwe2k14::IsDevBuild() && REXCVAR_GET(wwe2k14_x); }
inline bool Other(bool dev) { return dev && REXCVAR_GET(wwe2k14_x); }
'''
NOT_DEV = '''REXCVAR_DEFINE_BOOL(wwe2k14_y, false, "wwe2k14", "a player setting");
inline bool Active() { return REXCVAR_GET(wwe2k14_y); }
'''


def selftest():
    ok = True
    bad = violations(BAD_SNIPPET, definitions(BAD_SNIPPET), 'bad.h')
    good = violations(GOOD_SNIPPET, definitions(GOOD_SNIPPET), 'good.h')
    other = violations(NOT_DEV, definitions(NOT_DEV), 'other.h')
    for what, got, want in (('planted twin (an unguarded DEV ONLY read)', len(bad), 1), ('guarded reads', len(good), 0), ('a non-dev cvar', len(other), 0)):
        fine = got == want
        print('%s %s: %d violation(s), expected %d' % ('ok  ' if fine else 'FAIL', what, got, want))
        ok = ok and fine
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    names, bad = scan(os.path.abspath(a.root))
    print('dev_cvar_lint: %d DEV ONLY cvar(s): %s' % (len(names), ', '.join(sorted(names))))
    for rel, line, name in bad:
        print('VIOLATION %s:%d reads --%s (DEV ONLY) without IsDevBuild() / dev in the statement' % (rel, line, name))
    print('%d violation(s)' % len(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
