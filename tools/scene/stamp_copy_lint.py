#!/usr/bin/env python3
"""S103 L2dF lint (Opus review of lane L2d; landing 2): four classes of slip that the scene-kind work found.
  A  every OwnedStamp <-> OwnedPassStamp copy copies ALL six fields (pass, decision, why_not, pad, inputs_mask, guest_frame). A copy that drops `pad` loses the scene kind (the Scene3D stamp
     carries it there) and a CAW record reads as match.
  B  every render-side reader of the Scene3D `required_inputs` goes through scene_kind::RequiredFor / StaleBits (the kind's own mask), or carries the marker `lint:required-ok` (a log of the
     registered mask). Render side = src/renderer_adapter, src/renderer_scene, src/renderer_game/fallback_census.h.
  C  `SetSkip2DRows(true` appears only inside live.cpp's Apply2D (the one place the stream's 2D rows are left out).
  D  every call of Apply2D( in live.cpp is guarded in the 3 lines before it by `Apply2DAllowed(` (a flat frame after its verdict) or `.Whole()` (a 3D OURS frame's 2D-whole check): never on a
     frame that is not decided OURS (the menu would lose its UI).
  py -I tools\\scene\\stamp_copy_lint.py [--root DIR] [--selftest]
Exit 0 = pass. --selftest plants one twin per rule and each must FAIL its rule."""
import argparse
import glob
import os
import re
import sys

FIELDS = ('pass', 'decision', 'why_not', 'pad', 'inputs_mask', 'guest_frame')
COPY_RE = re.compile(r'\b\w+\.inputs_mask\s*=\s*\w+\.inputs_mask\b')
REQ_RE = re.compile(r'required_inputs\s*\.\s*load')


def rule_a(name, text):
    bad = []
    lines = text.split('\n')
    for i, ln in enumerate(lines):
        if not COPY_RE.search(ln):
            continue
        win = '\n'.join(lines[max(0, i - 3):i + 4])
        for f in FIELDS:
            if not re.search(r'\b\w+\.' + f + r'\s*=\s*\w+\.' + f + r'\b', win):
                bad.append('A %s:%d: a stamp copy without `.%s`' % (name, i + 1, f))
    return bad


def rule_b(name, text):
    bad = []
    for i, ln in enumerate(text.split('\n')):
        if REQ_RE.search(ln) and 'RequiredFor(' not in ln and 'StaleBits(' not in ln and 'lint:required-ok' not in ln:
            bad.append('B %s:%d: required_inputs read without scene_kind::RequiredFor / StaleBits' % (name, i + 1))
    return bad


def apply2d_body(text):
    m = re.search(r'\bvoid\s+Apply2D\s*\(', text)
    if not m:
        return None
    i = text.index('{', m.end())
    depth = 0
    j = i
    while j < len(text):
        depth += (text[j] == '{') - (text[j] == '}')
        j += 1
        if depth == 0:
            break
    return i, j


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), s, flags=re.S)
    return re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), s)


def rule_c_d(name, text):
    bad = []
    text = strip_comments(text)
    span = apply2d_body(text)
    for m in re.finditer(r'SetSkip2DRows\s*\(\s*true', text):
        if not span or not (span[0] <= m.start() < span[1]):
            bad.append('C %s:%d: SetSkip2DRows(true outside Apply2D' % (name, text.count('\n', 0, m.start()) + 1))
    lines = text.split('\n')
    for i, ln in enumerate(lines):
        if re.search(r'\bApply2D\s*\(', ln) and not re.search(r'\bvoid\s+Apply2D\s*\(', ln) and not ln.lstrip().startswith('//'):
            win = '\n'.join(lines[max(0, i - 3):i + 1])
            if 'Apply2DAllowed(' not in win and '.Whole()' not in win:
                bad.append('D %s:%d: Apply2D( not guarded by Apply2DAllowed( / .Whole() in the 3 lines before' % (name, i + 1))
    return bad


def selftest():
    ok = True
    twins = [
        ('A', rule_a, 'st.pass = s.pass, st.decision = s.decision, st.why_not = s.why_not, st.inputs_mask = s.inputs_mask, st.guest_frame = s.guest_frame;\n',
         'st.pass = s.pass, st.decision = s.decision, st.why_not = s.why_not, st.pad = s.pad, st.inputs_mask = s.inputs_mask, st.guest_frame = s.guest_frame;\n'),
        ('B', rule_b, 'const uint32_t r = rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load(std::memory_order_relaxed);\n',
         'const uint32_t r = scene_kind::RequiredFor(k, rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load(std::memory_order_relaxed));\n'),
        ('C', rule_c_d, 'void Draw() { binder.SetSkip2DRows(true); }\nvoid Apply2D() { binder.SetSkip2DRows(true); }\n',
         'void Draw() { binder.SetSkip2DRows(false); }\nvoid Apply2D() { binder.SetSkip2DRows(true); }\n'),
        ('D', rule_c_d, 'void Apply2D() { binder.SetSkip2DRows(true); }\nvoid F() {\n  if (flat) {\n    x();\n    Apply2D(binder);\n  }\n}\n',
         'void Apply2D() { binder.SetSkip2DRows(true); }\nvoid F() {\n  if (Apply2DAllowed(w)) Apply2D(binder);\n  if (r.Whole()) {\n    Apply2D(binder);\n  }\n}\n'),
    ]
    for rule, fn, planted, clean in twins:
        caught = bool(fn('planted', planted)) and all(b.startswith(rule + ' ') for b in fn('planted', planted))
        passes = not fn('clean', clean)
        print('selftest %s: planted twin %s, clean sample %s' % (rule, 'caught' if caught else 'NOT CAUGHT', 'passes' if passes else 'FAILS'))
        ok = ok and caught and passes
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    root = a.root
    bad = []
    n = 0
    for p in sorted(glob.glob(os.path.join(root, 'src', '**', '*.*'), recursive=True)):
        if not p.endswith(('.h', '.cpp')):
            continue
        rel = os.path.relpath(p, root).replace('\\', '/')
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
        from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)
        text = read_path(p)
        n += 1
        bad += rule_a(rel, text)
        if rel.startswith(('src/renderer_adapter/', 'src/renderer_scene/')) or rel == 'src/renderer_game/fallback_census.h':
            bad += rule_b(rel, text)
        if rel == 'src/renderer_adapter/live.cpp':
            if apply2d_body(text) is None:
                bad.append('C %s: Apply2D not found' % rel)
            bad += rule_c_d(rel, text)
        elif 'SetSkip2DRows(true' in text and rel != 'src/renderer_adapter/binder.h':
            bad += rule_c_d(rel, text)
    for b in bad:
        print('FAIL: ' + b)
    print(('FAIL' if bad else 'PASS') + ': stamp_copy_lint: %d src files, %d problem(s)' % (n, len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
