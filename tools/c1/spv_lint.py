r"""S102 lane C1 (review F7): after a shader regeneration every variant this lane does not name keeps byte-equal SPIR-V.
  py -I tools\c1\spv_lint.py [--root DIR] [--base REF]      (REF defaults to s102-arc-pt2: the tip this lane branched from / merged)
Reads src\renderer\shaders\spv\fx_*.h of the working tree and of REF (git show), compares every `kFx_<effect>_<variant>_<stage>` array word for word, and requires that the only symbols that are new or
changed are the lane's own (yPreLight base_layered, base_layered_swap). Planted twin: one word of yPreLight_base_vert changed in a text copy must FAIL (and fails as DID NOT APPLY when the edit is a no-op).
Exit 1 on any failure; an unreadable base fails loudly."""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
OWN = {'kFx_yPreLight_base_layered_vert', 'kFx_yPreLight_base_layered_swap_vert'}
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
        if name in OWN:
            continue
        if name not in base:
            bad.append('%s is new' % name)
        elif base[name] != words:
            bad.append('%s differs from the base' % name)
    for name in base:
        if name not in now:
            bad.append('%s is gone' % name)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--base', default='s102-arc-pt2')
    a = ap.parse_args()
    now, base, total = {}, {}, 0
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
    total = len(now)
    bad = compare(now, base)
    say(not bad, '%d SPIR-V arrays: every one but the lane\'s own %s is word-for-word the base\'s (%s)' % (total, sorted(OWN), '; '.join(bad[:3]) or 'clean'))
    say(OWN <= set(now), 'the lane\'s own variants are present')
    sym = 'kFx_yPreLight_base_vert'
    if sym not in now:
        say(False, 'planted twin DID NOT APPLY (%s is missing)' % sym)
    else:
        words = now[sym]
        planted = dict(now)
        planted[sym] = words[:-1] + ('1' if words[-1] != '1' else '2')
        pb = compare(planted, base)
        say(planted[sym] != words and bool(pb), 'planted twin (one word of yPreLight_base_vert changed): the lint FAILS it (%s)' % (pb[0] if pb else 'not caught'))
    print('spv_lint:', 'FAIL' if FAILS else 'PASS')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
