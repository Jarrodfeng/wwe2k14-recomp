#!/usr/bin/env python3
"""retire_prefix_lint (S103 SMF, BUG2 finding 4): ModelCache::RetireOther(prefix) drops every key that STARTS with the prefix, so a retire of the plain m2 weapon pools must not use the bare "weapon:" prefix
(it also took the weapon:ev / stg / gm / blood pools and their bound copies: every cage, belt and blood pool was rebuilt when the weapon slot set changed).
Rules (src\\renderer_scene\\*.cpp, comments stripped):
  R1  RetireOther("weapon:", ...)  or  RetireOther("weapon:bound:", ...)   a bare weapon prefix: name the family ("weapon:m2:", "weapon:ev", ...)
  R2  a pool name built as  "weapon:" + std::to_string(...)   the plain pools carry their own "weapon:m2:" prefix
  py -I tools\\scene\\retire_prefix_lint.py --root .      scan (exit 1 on an error)
  py -I tools\\scene\\retire_prefix_lint.py --selftest    planted twins: each bad snippet MUST be flagged, the good ones must pass
"""
import argparse
import os
import re
import sys

R1 = re.compile(r'RetireOther\(\s*"weapon:(?:bound:)?"\s*,')
R2 = re.compile(r'"weapon:"\s*\+\s*std::to_string')


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)


def scan_text(path, text):
    errs = []
    for n, line in enumerate(strip_comments(text).splitlines(), 1):
        if R1.search(line): errs.append(f'{path}:{n}: R1 bare "weapon:" retire prefix (it also retires the ev / stg / gm / blood pools): use "weapon:m2:" / "weapon:bound:m2:"')
        if R2.search(line): errs.append(f'{path}:{n}: R2 plain weapon pool named "weapon:" + id: use "weapon:m2:" + id')
    return errs


def selftest():
    ok = True
    good = 'ctx.models.RetireOther("weapon:m2:", "\\x01"); ctx.models.RetireOther("weapon:ev", "\\x01"); auto p = "weapon:m2:" + std::to_string(id);'
    bad1 = 'ctx.models.RetireOther("weapon:", "\\x01");'
    bad2 = 'auto p = "weapon:" + std::to_string(it.id);'
    for name, text, want in (('good', good, 0), ('planted R1', bad1, 1), ('planted R2', bad2, 1)):
        n = len(scan_text('x.cpp', text))
        print(f'{name}: {"PASS" if n == want else "FAIL"} ({n} finding(s), want {want})')
        ok &= n == want
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest: return selftest()
    errs = []
    base = os.path.join(a.root, 'src', 'renderer_scene')
    for f in sorted(os.listdir(base)):
        if f.endswith(('.cpp', '.h')):
            errs += scan_text(f'src/renderer_scene/{f}', open(os.path.join(base, f), encoding='utf-8', errors='replace').read())
    for e in errs: print(e)
    print(f'retire_prefix_lint: {len(errs)} error(s)')
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
