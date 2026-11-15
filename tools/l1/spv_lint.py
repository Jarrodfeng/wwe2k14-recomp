r"""S102 lane L1: after the shader regeneration every variant this lane does not name keeps byte-equal SPIR-V (the character shaders gained a block member and an include under FX_PRELIGHT_ANALYTIC only).
  py -I tools\l1\spv_lint.py [--root DIR] [--base REF]      (REF defaults to s102-arc-pt2: the tip this lane branched from)
Reads src\renderer\shaders\spv\fx_*.h of the working tree and of REF (git show), compares every `kFx_<effect>_<variant>_<stage>` array word for word, and requires that the only symbols that are new or
changed are the lane's own: the analytic twins, `kFx_<effect>_<variant>_pl_frag` and `..._pl_frag_alpha`, of the character effects. Planted twin: one word of kFx_yCh_skin_base_frag changed in a text copy
must FAIL (and fails as DID NOT APPLY when the edit is a no-op). Also: every twin names a base variant that exists, and the base variant's array is unchanged.
Exit 1 on any failure; an unreadable base fails loudly."""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
OWN = re.compile(r'^kFx_yCh_\w+_pl_frag(_alpha)?$')
ARRAY = re.compile(r'inline constexpr uint32_t (kFx_\w+)\[\] = \{(.*?)\};', re.S)
FAILS = 0


def say(ok, text):
    global FAILS
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    FAILS += 0 if ok else 1


def arrays(text):
    return {m.group(1): re.sub(r'\s+', '', m.group(2)) for m in ARRAY.finditer(text.replace('\r\n', '\n'))}


def compare(now, base):
    bad = []
    for name, words in now.items():
        if OWN.match(name):
            continue
        if name not in base:
            bad.append('%s is new' % name)
        elif base[name] != words:
            bad.append('%s differs from the base' % name)
    for name in base:
        if name not in now:
            bad.append('%s is gone' % name)
    return bad


ROW = re.compile(r'^  (\{"([A-Za-z0-9_]+)",.*\}),?$', re.M)
ARR = re.compile(r'^inline constexpr (?:ConstantRange|Sampler|Attribute) (k[CSA]_\w+)\[\] = (\{.*\});$', re.M)


def registry(text):
    """name -> row text (the whole `{"name", ...}` initialiser) and ident -> constant / sampler / attribute array, twins excluded."""
    text = text.replace('\r\n', '\n')
    rows = {m.group(2): m.group(1) for m in ROW.finditer(text) if not m.group(2).endswith('_pl_fragment')}
    arrs = {m.group(1): m.group(2) for m in ARR.finditer(text) if '_pl_fragment' not in m.group(1)}
    return rows, arrs


def compare_registry(now, base):
    """every non-twin registry row and array of the base is unchanged (a registry generated before a lane merge would reset fields the other lane set, e.g. out_alpha)"""
    bad = []
    (nr, na), (br, ba) = now, base
    for name, row in br.items():
        if nr.get(name) != row:
            bad.append('registry row %s %s' % (name, 'is gone' if name not in nr else 'differs'))
    for ident, arr in ba.items():
        if na.get(ident) != arr:
            bad.append('registry array %s %s' % (ident, 'is gone' if ident not in na else 'differs'))
    for name in nr:
        if name not in br:
            bad.append('registry row %s is new' % name)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--base', default='s102-arc-pt2')
    a = ap.parse_args()
    now, base = {}, {}
    for path in sorted(glob.glob(os.path.join(a.root, 'src', 'renderer', 'shaders', 'spv', 'fx_*.h'))):
        rel = 'src/renderer/shaders/spv/' + os.path.basename(path)
        if os.path.basename(path) == 'fx_registry.h':
            continue
        now.update(arrays(open(path, encoding='utf-8', errors='replace').read()))
        g = subprocess.run(['git', '-C', a.root, 'show', '%s:%s' % (a.base, rel)], capture_output=True, text=True, encoding='utf-8', errors='replace')
        if g.returncode == 0:
            base.update(arrays(g.stdout))
    if not base:
        say(False, 'cannot read the base shaders from %s' % a.base)
        return 1
    own = sorted(n for n in now if OWN.match(n))
    bad = compare(now, base)
    say(not bad, '%d SPIR-V arrays: every one but the lane\'s %d analytic twins is word-for-word the base\'s (%s)' % (len(now), len(own), '; '.join(bad[:3]) or 'clean'))
    say(len(own) >= 600, 'the twins are present (%d arrays: a fragment and an alpha-test array for each of the cube-sampling character variants)' % len(own))
    orphans = [n for n in own if not n.endswith('_alpha') and n.replace('_pl_frag', '_frag') not in now]
    say(not orphans, 'every twin has its base variant (%s)' % ('; '.join(orphans[:3]) or 'none missing'))
    sym = 'kFx_yCh_skin_base_frag'
    if sym not in now:
        say(False, 'planted twin DID NOT APPLY (%s is missing)' % sym)
    else:
        words = now[sym]
        planted = dict(now)
        planted[sym] = words[:-1] + ('1' if words[-1] != '1' else '2')
        pb = compare(planted, base)
        say(planted[sym] != words and bool(pb), 'planted twin (one word of %s changed): the lint FAILS it (%s)' % (sym, pb[0] if pb else 'not caught'))
        extra = dict(now)
        extra['kFx_yCh_skin_base_newvariant_frag'] = words
        pe = compare(extra, base)
        say(bool(pe), 'planted twin (a new non-twin variant): the lint FAILS it (%s)' % (pe[0] if pe else 'not caught'))
    # the registry (F8): every non-twin row and array equals the base's
    reg_rel = 'src/renderer/shaders/spv/fx_registry.h'
    rg = subprocess.run(['git', '-C', a.root, 'show', '%s:%s' % (a.base, reg_rel)], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if rg.returncode != 0:
        say(False, 'cannot read the base registry from %s' % a.base)
    else:
        now_reg = registry(open(os.path.join(a.root, reg_rel), encoding='utf-8', errors='replace').read())
        base_reg = registry(rg.stdout)
        rb = compare_registry(now_reg, base_reg)
        say(not rb and len(base_reg[0]) > 1000, 'the registry: %d non-twin rows and %d arrays equal the base\'s (%s)' % (len(base_reg[0]), len(base_reg[1]), '; '.join(rb[:3]) or 'clean'))
        # planted twin: a stale registry (one row's out_alpha reset to 0, as a registry generated before the other lane's merge would)
        victim = next((n for n, r in base_reg[0].items() if re.search(r', 2, \d+\}$', r)), None)
        if victim is None:
            say(False, 'planted stale-registry twin DID NOT APPLY (no row with out_alpha 2 in the base)')
        else:
            stale_rows = dict(now_reg[0])
            stale_rows[victim] = re.sub(r', 2, (\d+)\}$', r', 0, \1}', stale_rows[victim])
            pr = compare_registry((stale_rows, now_reg[1]), base_reg)
            say(stale_rows[victim] != now_reg[0][victim] and bool(pr), 'planted twin (the stale registry: %s out_alpha 2 reset to 0): the lint FAILS it (%s)' % (victim, pr[0] if pr else 'not caught'))
    print('spv_lint:', 'FAIL' if FAILS else 'PASS')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
