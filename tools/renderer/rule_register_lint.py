#!/usr/bin/env python3
"""rule_register_lint (S103 SLV2): a rule header that names a register as a SHADER INPUT must find that register in the shader's constant table.

The SLV review found a rule (the blank plane chain, src\\renderer_adapter\\plane_blank.h) built on c35 of the blur draws as "the weight": the register is uploaded but no shader reads it (the yBlur plane
constants are c33, c36-39 and c40; yBlur.fx.json holds no c35). A rule that depends on a register the shader never reads proves nothing about the picture.

Convention: a header whose rule reads shader constants says so with one or more lines
    // SHADER-INPUT: <effect> c<N> [c<N> ...]
(<effect> = the name of src\\renderer\\shaders\\fx\\<effect>.fx.json). Every named register must lie inside a constant range ({register, count}) of some variant of that effect, vertex or fragment.
A header listed in REQUIRED must carry at least one such line. No game, no compile: a text scan of the tree, under a second.

  py -I tools\\renderer\\rule_register_lint.py [--root DIR]      the tree (exit 1 on a problem)
  py -I tools\\renderer\\rule_register_lint.py --selftest        planted twins: each bad snippet MUST be flagged, the good one must pass (and the pre-SLV2 header text, c35, MUST be flagged)
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
REQUIRED = []   # the rule headers that must carry a SHADER-INPUT line
# S103 00:37: src/renderer_adapter/plane_blank.h (the SHIPPED stream rule, G4b) was taken off REQUIRED when SLV2's rewrite of it was
# reverted: g4b_check on the Bryan capture showed the rewrite skipped 0 of 91 frames where the shipped rule skips 90 with identical
# pictures. The shipped rule reads the uploaded mask c6.w / blur c35.x VALUES as a blank signal (proven per capture by g4b_check), not as
# shader-input registers, so this lint's register claim does not apply to it. Any rewrite of it must keep g4b_check's skip counts.
MARK = re.compile(r'^\s*//\s*SHADER-INPUT:\s*(\w+)\s+((?:c\d+\s*)+)$', re.M)


def registers_of(fx_json_text):
    """Every register covered by a constant range of any variant of the effect."""
    spec = json.loads(fx_json_text)
    covered = set()
    for stage in ('vertex', 'fragment'):
        for v in spec.get(stage, []):
            for c in v.get('constants', []):
                covered.update(range(int(c['register']), int(c['register']) + int(c.get('count', 1))))
    return covered


def check_text(header_text, fx_loader, name='<header>', required=False):
    problems = []
    marks = MARK.findall(header_text)
    if required and not marks:
        problems.append('%s: names no `// SHADER-INPUT: <effect> cN ...` line, and is a rule header that reads shader constants' % name)
    for effect, regs in marks:
        try:
            covered = registers_of(fx_loader(effect))
        except (OSError, ValueError) as e:
            problems.append('%s: effect %s has no readable %s.fx.json (%s)' % (name, effect, effect, e))
            continue
        for r in re.findall(r'c(\d+)', regs):
            if int(r) not in covered:
                problems.append('%s: c%s is named as an input of %s but no constant range of %s.fx.json holds it (the shader never reads that register)' % (name, r, effect, effect))
    return problems


def scan(root):
    def loader(effect):
        with open(os.path.join(root, 'src', 'renderer', 'shaders', 'fx', effect + '.fx.json'), encoding='utf-8') as f:
            return f.read()
    problems, seen = [], 0
    for base, _, files in os.walk(os.path.join(root, 'src')):
        for fn in files:
            if not fn.endswith(('.h', '.cpp')):
                continue
            path = os.path.join(base, fn)
            rel = os.path.relpath(path, root).replace('\\', '/')
            with open(path, encoding='utf-8', errors='replace') as f:
                text = f.read()
            if 'SHADER-INPUT:' in text or rel in REQUIRED:
                seen += 1
                problems += check_text(text, loader, rel, rel in REQUIRED)
    return problems, seen


def selftest(root):
    def loader(effect):
        with open(os.path.join(root, 'src', 'renderer', 'shaders', 'fx', effect + '.fx.json'), encoding='utf-8') as f:
            return f.read()
    good = '// SHADER-INPUT: yBlur c33 c36 c37 c38 c39 c40\n'
    plants = {
        'the pre-SLV2 rule: c35 (the weight) as a blur input': '// SHADER-INPUT: yBlur c33 c35\n',
        'a register no variant holds (c99)': '// SHADER-INPUT: yBlur c99\n',
        'an effect with no fx.json': '// SHADER-INPUT: yNoSuchEffect c0\n',
    }
    bad = 0
    p = check_text(good, loader, 'good')
    print('good snippet -> %s' % ('passes' if not p else 'FLAGGED: %s' % p))
    bad += bool(p)
    for name, text in plants.items():
        p = check_text(text, loader, name)
        print('plant %-52s -> %s' % (name, 'caught' if p else 'NOT CAUGHT'))
        bad += not p
    p = check_text('// a rule header with no marker at all\n', loader, 'unmarked', required=True)
    print('plant %-52s -> %s' % ('a required rule header with no marker', 'caught' if p else 'NOT CAUGHT'))
    bad += not p
    print('SELFTEST %s' % ('FAIL' if bad else 'ok'))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest(a.root)
    problems, seen = scan(a.root)
    print('rule_register_lint: %d header(s) with shader-input markers' % seen)
    for p in problems:
        print('PROBLEM: ' + p)
    print('rule_register_lint: %d problem(s)' % len(problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
