#!/usr/bin/env python3
r"""S103 lane UPS (review of the shared stage page): lints that keep the one-page-image rule from being undone by the next model somebody registers. 0 game runs.

  py -I tools\scene\ups_lint.py [--root DIR] [--selftest]

Rules (each has a twin in --selftest: a planted violation FAILS, the marked / fixed form passes):
  R1  every `Register(ctx, ...)` call in src\renderer_scene\rf_c_core.cpp (a model of an arena set that can carry the stage_lightmap page) passes the set's page_group as its last argument, or its line
      carries `// no page group: <reason>` (a reason, not an empty marker). Today's marked call: ring_rest (a look-check follow-up).
  R2  more than one page group alive while one arena set is live gives a WARN: live_scene_draw.cpp's TexUpLine prints `WARN: more than one page group alive`.
  R3  AddModelSharing from an owner with page_group != 0 asserts: front_end.cpp's AddModelImpl refuses it and counts it (page_share_violations_), and TexUpLine prints the count.
Exit 0 = all rules hold.
"""
import argparse, os, re, sys

CALL_RE = re.compile(r'(?<![\w:])Register\(ctx\b')


def register_problems(text):
    """problems in a rf_c_core.cpp-shaped source: Register calls with neither page_group nor a reasoned marker"""
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if not CALL_RE.search(line):
            continue
        code, _, comment = line.partition('//')
        m = re.search(r'no page group:\s*(\S.*)$', comment)
        if 'page_group' in code:
            continue
        if m and len(m.group(1).strip()) >= 8:
            continue
        out.append('line %d: Register(...) passes no page_group and carries no `// no page group: <reason>` marker: %s' % (n, line.strip()[:110]))
    return out


def tree_problems(root):
    out = []
    def rd(rel):
        p = os.path.join(root, rel)
        return open(p, encoding='utf8', errors='replace').read() if os.path.exists(p) else None
    core = rd('src/renderer_scene/rf_c_core.cpp')
    if core is None:
        out.append('rf_c_core.cpp missing')
    else:
        calls = [l for l in core.splitlines() if CALL_RE.search(l)]
        if not calls:
            out.append('R1: no Register(ctx, ...) call found in rf_c_core.cpp (the lint reads nothing: update CALL_RE)')
        out += ['R1 ' + p for p in register_problems(core)]
    ls = rd('src/renderer_scene/live_scene_draw.cpp') or ''
    if 'WARN: more than one page group alive' not in ls:
        out.append('R2: live_scene_draw.cpp TexUpLine no longer prints `WARN: more than one page group alive`')
    fe = rd('src/renderer_scene/front_end.cpp') or ''
    if not re.search(r'page_group != 0\)\s*\{[^}]*\+\+page_share_violations_', fe, re.S) or 'return -1;' not in fe:
        out.append('R3: front_end.cpp AddModelImpl no longer refuses + counts AddModelSharing from an owner on a page group')
    if 'page_share_violations()' not in ls:
        out.append('R3: TexUpLine no longer prints the AddModelSharing violation count')
    return out


def selftest():
    bad = 0
    def expect(ok, what):
        nonlocal bad
        print(('PASS' if ok else 'FAIL') + '  selftest: ' + what)
        bad += 0 if ok else 1
    expect(not register_problems('FamilyModel fm = Register(ctx, key, std::move(sub), as->all_models, as->page_group);\n'), 'a call passing page_group is clean')
    expect(bool(register_problems('FamilyModel f = Register(ctx, key, std::move(rest), as->all_models);\n')), 'PLANT: a call with no page_group and no marker FAILS')
    expect(not register_problems('FamilyModel f = Register(ctx, key, std::move(rest), as->all_models);   // no page group: the ring at rest carries no stage page\n'), 'a call with a reasoned marker is clean')
    expect(bool(register_problems('FamilyModel f = Register(ctx, key, std::move(rest), as->all_models);   // no page group:\n')), 'PLANT: an EMPTY marker FAILS')
    expect(bool(register_problems('FamilyModel f = Register(ctx, key, m, all);   // page_group is not passed here\n')), 'PLANT: page_group named only in a comment FAILS')
    expect(not register_problems('FamilyModel Register(BuildCtx& ctx, const std::string& key) {\n'), 'the definition is not a call')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        bad = selftest()
        print('ups_lint selftest: %s' % ('PASS' if not bad else 'FAIL (%d)' % bad))
        return 1 if bad else 0
    probs = tree_problems(args.root)
    for p in probs:
        print('FAIL  ' + p)
    print('ups_lint: %s' % ('PASS' if not probs else 'FAIL (%d)' % len(probs)))
    return 1 if probs else 0


if __name__ == '__main__':
    sys.exit(main())
