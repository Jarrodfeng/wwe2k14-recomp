#!/usr/bin/env python3
"""cvar_define_once: every REXCVAR_DEFINE_* that sits in a header must be reachable from at most ONE translation unit per build.

Why (S103, 2026-11-18): the stream-free (OFF) player exe did not link: lld-link "duplicate symbol FLAGS_wwe2k14_blackbox_storage_" (and three more). A header
that DEFINES a cvar (blackbox.h, android_lifecycle.h, start_failure_show.h) was included by two translation units of one exe, so the cvar had two definitions.
Rule: a header that defines a cvar is included (transitively) from one TU only; any other TU uses REXCVAR_DECLARE.

How: the unit lists come from the main checkout's build.ninja files (ON = out/build/win-amd64-release, OFF = out/build/win-amd64-release-off; override with
--ninja NAME=PATH); the include graph is a transitive scan of '#include "..."' with ONE bit of preprocessing: '#if WWE2K14_STREAM()' / '#if !WWE2K14_STREAM()'
(and #else / #elif) follow the build's stream value (ON = stream, OFF = no stream); every other condition takes all branches (an over-approximation: a hit may be
a guarded include, then guard it with WWE2K14_STREAM or move the define). Sources: only units under src/ and probes/ (generated code defines no cvars).
Exit 0 = clean (or no build.ninja found: SKIP, said loudly), 1 = a header cvar reachable from two or more TUs of one build.
  py -I tools\\lint\\cvar_define_once.py [--root DIR] [--ninja ON=path --ninja OFF=path]
"""
import argparse
import os
import re
import subprocess
import sys

INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
DEF = re.compile(r'REXCVAR_DEFINE_\w+\(\s*(\w+)')
COND = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)')
STREAM = re.compile(r'^\s*(!?)\s*WWE2K14_STREAM\(\)\s*(//.*|/\*.*)?$')


def main_checkout(root):
    try:
        out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        return os.path.dirname(os.path.abspath(os.path.join(root, out)))
    except Exception:
        return root


def units_from_ninja(path):
    """-> [(unit path relative to the repo, set of -D names)]"""
    units, cur = [], None
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('build '):
            cur = None
            if 'CXX_COMPILER' not in line.split(':', 1)[1][:60]:
                continue
            rhs = line.split(':', 1)[1].replace('$:', ':').split()
            if len(rhs) < 2:
                continue
            m = re.search(r'/wwe2k14-recomp/((?:src|probes)/.*\.(?:cpp|cc|cxx))$', rhs[1].replace(chr(92), '/'))
            if m:
                cur = (m.group(1), set())
                units.append(cur)
        elif cur and line.startswith('  DEFINES = '):
            cur[1].update(re.findall(r'-D(\w+)', line))
    return units


_cache = {}


def lines_of(path):
    if path not in _cache:
        try:
            _cache[path] = open(path, encoding='utf-8', errors='replace').read().split(chr(10))
        except OSError:
            _cache[path] = []
    return _cache[path]


NAMED = re.compile(r'^\s*(!?)\s*defined\s*\(\s*(WWE2K14_\w+)\s*\)\s*(//.*)?$')
DEFINE = re.compile(r'^\s*#\s*define\s+(WWE2K14_\w+)')


def cond_of(kw, rest):
    """Known condition (True = the branch is active) given the macros, or None (unknown: both branches)."""
    return None


def resolve(root, frm, name):
    for base in (os.path.dirname(frm), os.path.join(root, 'src'), root, os.path.join(root, 'probes')):
        p = os.path.normpath(os.path.join(base, name))
        if os.path.isfile(p):
            return p
    return None


def walk(root, unit, stream, macros):
    """Ordered include walk of one TU with a tiny preprocessor (WWE2K14_* macros, WWE2K14_STREAM()). -> {header: [defined cvar names active at the visit]}"""
    macros = set(macros)
    seen, out = set(), {}

    def visit(f):
        if f in seen:
            return
        seen.add(f)
        stack = []   # per open #if: [known, value]
        for line in lines_of(f):
            m = COND.match(line)
            if m:
                kw, rest = m.group(1), m.group(2).strip()
                if kw == 'if':
                    sm = STREAM.match(rest)
                    nm = NAMED.match(rest)
                    if sm:
                        stack.append([True, (sm.group(1) == '') == stream])
                    elif nm:
                        stack.append([True, (nm.group(2) in macros) != (nm.group(1) == '!')])
                    else:
                        stack.append([False, True])
                elif kw in ('ifdef', 'ifndef'):
                    nm = re.match(r'(WWE2K14_\w+)', rest)
                    if nm:
                        stack.append([True, (nm.group(1) in macros) == (kw == 'ifdef')])
                    else:
                        stack.append([False, True])
                elif kw == 'elif':
                    if stack:
                        stack[-1] = [False, True]
                elif kw == 'else':
                    if stack and stack[-1][0]:
                        stack[-1][1] = not stack[-1][1]
                elif kw == 'endif':
                    if stack:
                        stack.pop()
                continue
            if not all(e[1] for e in stack):
                continue
            dm = DEFINE.match(line)
            if dm:
                macros.add(dm.group(1))
                continue
            im = INC.match(line)
            if im:
                p = resolve(root, f, im.group(1))
                if p:
                    visit(p)
            elif not line.lstrip().startswith('//'):
                for d in DEF.findall(line):
                    out.setdefault(f, []).append(d)

    visit(os.path.normpath(os.path.join(root, unit)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=None)
    ap.add_argument('--ninja', action='append', default=[], help='ON=path or OFF=path')
    a = ap.parse_args()
    root = os.path.abspath(a.root or os.path.join(os.path.dirname(__file__), '..', '..'))
    mc = main_checkout(root)
    nin = {'ON': os.path.join(mc, 'out', 'build', 'win-amd64-release', 'build.ninja'),
           'OFF': os.path.join(mc, 'out', 'build', 'win-amd64-release-off', 'build.ninja')}
    for kv in a.ninja:
        k, v = kv.split('=', 1)
        nin[k.upper()] = v
    bad, ran = 0, 0
    for build, path in nin.items():
        if not os.path.isfile(path):
            print(f'cvar_define_once: SKIP {build}: no {path}')
            continue
        ran += 1
        stream = build == 'ON'
        units = units_from_ninja(path)
        owners, defs_of = {}, {}   # header path -> [units]
        for u, dflags in units:
            if not os.path.isfile(os.path.join(root, u)):
                continue
            for f, names in walk(root, u, stream, dflags).items():
                if f.lower().endswith(('.h', '.hpp', '.inl')) and names:
                    owners.setdefault(f, []).append(u)
                    defs_of[f] = names
        for h, us in sorted(owners.items()):
            if len(us) > 1:
                bad += 1
                names = ', '.join(sorted(set(defs_of[h]))[:4])
                print(f'FAIL [{build}] {os.path.relpath(h, root)} defines cvar(s) {names} and is reachable from {len(us)} TUs: {", ".join(us[:6])}')
        print(f'cvar_define_once [{build}]: {len(units)} units, {sum(1 for v in owners.values() if len(v) == 1)} header(s) with cvar defines each in one TU')
    if bad:
        print(f'cvar_define_once: FAIL ({bad}): a header that DEFINES a cvar must reach one TU only; use REXCVAR_DECLARE elsewhere')
        return 1
    print('cvar_define_once: OK' if ran else 'cvar_define_once: SKIP (no build.ninja)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
