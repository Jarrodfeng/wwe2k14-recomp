r"""S102 lane R18-B2: spv_lint for the lite twins. Every SPIR-V array the lane does not name is word-for-word the base branch's (tools\c1\spv_lint.py's compare), and the lane's own arrays are exactly
the `kFx_yGlareEtc_<permutation>_<lite1|lite2|litep1|litep2|litep3|litem>_frag` twins (no hashes, so the game never picks them), and (review F1) no lite twin has an OpImageSampleImplicitLod inside a conditional region (non-uniform control flow: undefined derivatives). Planted twins: one word of an existing yGlareEtc variant changed must fail;
a new array outside the lane's own names must fail (and each fails as DID NOT APPLY when the edit is a no-op).
  py -I tools\r18\spv_lint_b2.py [--root DIR] [--base REF]"""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'c1'))
import spv_lint  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
OWN = re.compile(r'^kFx_yGlareEtc_[a-z_]+?_lite(?:1|2|p1|p2|p3|m)_frag$')


def implicit_in_conditional(words):
    """Walk the instructions: a structured conditional region opens at OpSelectionMerge / OpLoopMerge and closes at its merge label. Returns the number of OpImageSampleImplicitLod (87)
    inside one, and the number of explicit-lod samples (88) inside one (the legal kind)."""
    stack, bad, explicit, i, pending = [], 0, 0, 5, None
    while i < len(words):
        op, count = words[i] & 0xFFFF, words[i] >> 16
        if count == 0:
            break
        if op == 248:      # OpLabel
            while stack and stack[-1] == words[i + 1]:
                stack.pop()
        elif op == 247:    # OpSelectionMerge: a conditional region only when the header ends in OpBranchConditional (an inlined early return is a switch(true) with one case: uniform)
            pending = words[i + 1]
        elif op == 250 and pending is not None:   # OpBranchConditional
            stack.append(pending)
            pending = None
        elif op in (249, 251):   # OpBranch / OpSwitch: the selection is the inliner's, not data dependent
            pending = None
        elif op == 246:    # OpLoopMerge: spirv-opt wraps an inlined function with an early return (smoothEdges' flat exit) in a one-trip loop: the loop opens no conditional region, the selections inside it do
            pass
        elif op == 87 and stack:
            bad += 1
        elif op == 88 and stack:
            explicit += 1
        i += count
    return bad, explicit


def words_of(arrays_text):
    return [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', arrays_text)]


class OwnRe:
    def __contains__(self, name):
        return bool(OWN.match(name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--base', default='')   # review F4: pinned to tools/r18/BASE (the s102-arc-pt2 commit this lane merged), not the moving branch; retire the pin after the merge
    a = ap.parse_args()
    if not a.base:
        a.base = open(os.path.join(HERE, 'BASE')).read().strip()
    spv_lint.OWN = OwnRe()
    now, base = {}, {}
    for path in sorted(glob.glob(os.path.join(a.root, 'src', 'renderer', 'shaders', 'spv', 'fx_*.h'))):
        rel = 'src/renderer/shaders/spv/' + os.path.basename(path)
        if os.path.basename(path) == 'fx_registry.h':
            continue
        now.update(spv_lint.arrays(open(path, encoding='utf-8', errors='replace').read()))
        g = subprocess.run(['git', '-C', a.root, 'show', '%s:%s' % (a.base, rel)], capture_output=True, text=True, encoding='utf-8', errors='replace')
        if g.returncode == 0:
            base.update(spv_lint.arrays(g.stdout))
    if not base:
        spv_lint.say(False, 'cannot read the base shaders from %s' % a.base)
        return 1
    bad = spv_lint.compare(now, base)
    own = [n for n in now if n in spv_lint.OWN]
    spv_lint.say(not bad, '%d SPIR-V arrays: every one but the lane\'s own %d lite twins is word-for-word the base\'s (%s)' % (len(now), len(own), '; '.join(bad[:3]) or 'clean'))
    spv_lint.say(len(own) >= 60, 'the lane\'s own twins are present (%d)' % len(own))
    sym = 'kFx_yGlareEtc_dof_fog_bloom_frag'
    if sym not in now:
        spv_lint.say(False, 'planted twin DID NOT APPLY (%s is missing)' % sym)
    else:
        words = now[sym]
        planted = dict(now)
        planted[sym] = words[:-1] + ('1' if words[-1] != '1' else '2')
        pb = spv_lint.compare(planted, base)
        spv_lint.say(planted[sym] != words and bool(pb), 'planted twin (one word of yGlareEtc_dof_fog_bloom changed): the lint FAILS it (%s)' % (pb[0] if pb else 'not caught'))
    planted = dict(now)
    planted['kFx_yGlareEtc_dof_fog_bloom_extra_frag'] = '0'
    pb = spv_lint.compare(planted, base)
    spv_lint.say('kFx_yGlareEtc_dof_fog_bloom_extra_frag' not in now and bool(pb), 'planted twin (a new array outside the lane\'s own names): the lint FAILS it (%s)' % (pb[0] if pb else 'not caught'))
    # review F1: no implicit-lod sample in a conditional region of any lite twin; at least one explicit one exists (the lint reads something); a reverted fetch is caught
    total_bad, total_explicit, sample = 0, 0, None
    for name in own:
        w = words_of(now[name]) if isinstance(now[name], str) else None
        if w is None:
            continue
        b, e = implicit_in_conditional(w)
        total_bad += b
        total_explicit += e
        if e and sample is None and name.endswith('_lite2_frag'):
            sample = (name, w)
    spv_lint.say(total_bad == 0 and total_explicit > 0, 'no lite twin samples with implicit derivatives inside a conditional region (%d implicit in conditionals, %d explicit-lod samples there)' % (total_bad, total_explicit))
    if sample is None:
        spv_lint.say(False, 'planted twin (a conditional fetch reverted to texture()) DID NOT APPLY: no lite2 twin has an explicit-lod sample in a conditional')
    else:
        name, w = sample
        stack, i, planted, pending = [], 5, list(w), None
        while i < len(planted):
            op, count = planted[i] & 0xFFFF, planted[i] >> 16
            if op == 248:
                while stack and stack[-1] == planted[i + 1]:
                    stack.pop()
            elif op == 247:
                pending = planted[i + 1]
            elif op == 250 and pending is not None:
                stack.append(pending)
                pending = None
            elif op == 88 and stack:
                planted[i] = (planted[i] & 0xFFFF0000) | 87
                break
            i += count
        b, _ = implicit_in_conditional(planted)
        spv_lint.say(planted != w and b > 0, 'planted twin (one conditional fetch of %s reverted to an implicit-lod sample): the lint FAILS it (%d found)' % (name, b))
    print('spv_lint_b2:', 'FAIL' if spv_lint.FAILS else 'PASS')
    return 1 if spv_lint.FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
