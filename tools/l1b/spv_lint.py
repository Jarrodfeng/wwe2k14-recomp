r"""S102 lane L1b: after the shader compile every SPIR-V header but this lane's own is word-for-word the tip's (the lane adds two compute shaders (the cubes, the direction table) and the table's planted twin; no effect variant, no registry row changes).
  py -I tools\l1b\spv_lint.py [--root DIR] [--base REF]      (REF defaults to the merge base of HEAD and s102-arc-pt2: the tip this lane branched from)
Compares every file of src\renderer\shaders\spv\ (the working tree) with REF's (git show), line endings normalised. The only differences allowed are the three NEW files rdr_light_cubes_comp.h, rdr_light_dirs_comp.h and
rdr_light_dirs_comp_swap.h. Planted twins (in memory): one word of rdr_present_frag.h changed must FAIL (DID NOT APPLY when the edit is a no-op); a third new header must FAIL; fx_registry.h with
one byte changed must FAIL. Exit 1 on any failure; an unreadable base fails loudly."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
OWN = {'rdr_light_cubes_comp.h', 'rdr_light_dirs_comp.h', 'rdr_light_dirs_comp_swap.h'}
FAILS = 0
DIR = 'src/renderer/shaders/spv'


def say(ok, text):
    global FAILS
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    FAILS += 0 if ok else 1


def norm(text):
    return text.replace('\r\n', '\n')


def compare(now, base):
    bad = []
    for name, text in now.items():
        if name in OWN:
            continue
        if name not in base:
            bad.append('%s is new' % name)
        elif base[name] != text:
            bad.append('%s differs from the base' % name)
    for name in base:
        if name not in now:
            bad.append('%s is gone' % name)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--base', default=None)
    a = ap.parse_args()
    if not a.base:   # the tip this lane branched from: the merge base with s102-arc-pt2 (the branch moves while lanes work)
        mb = subprocess.run(['git', '-C', a.root, 'merge-base', 'HEAD', 's102-arc-pt2'], capture_output=True, text=True)
        a.base = mb.stdout.strip() or 's102-arc-pt2'
    ls = subprocess.run(['git', '-C', a.root, 'ls-tree', '--name-only', a.base, DIR + '/'], capture_output=True, text=True)
    names = [os.path.basename(n) for n in ls.stdout.split() if n.endswith('.h')]
    if ls.returncode != 0 or not names:
        say(False, 'cannot read the base shader headers from %s' % a.base)
        return 1
    base = {}
    for n in names:
        g = subprocess.run(['git', '-C', a.root, 'show', '%s:%s/%s' % (a.base, DIR, n)], capture_output=True, text=True, encoding='utf-8', errors='replace')
        if g.returncode != 0:
            say(False, 'cannot read %s from %s' % (n, a.base))
            return 1
        base[n] = norm(g.stdout)
    now = {}
    folder = os.path.join(a.root, *DIR.split('/'))
    for n in sorted(os.listdir(folder)):
        if n.endswith('.h'):
            now[n] = norm(open(os.path.join(folder, n), encoding='utf-8', errors='replace').read())
    bad = compare(now, base)
    say(not bad, '%d SPIR-V headers: every one but the lane\'s %d new ones is word-for-word the base\'s (%s)' % (len(now), len(OWN & set(now)), '; '.join(bad[:3]) or 'clean'))
    say(OWN <= set(now), 'the lane\'s three compute shader headers are present (%s)' % ', '.join(sorted(OWN & set(now))))
    for n in sorted(OWN & set(now)):
        say(bool(re.search(r'inline constexpr uint32_t kSpv_\w+\[\] = \{', now[n])) and n not in base, '%s is a new header holding one SPIR-V array' % n)
    sym = 'rdr_present_frag.h'
    if sym not in now:
        say(False, 'planted twin DID NOT APPLY (%s is missing)' % sym)
    else:
        planted = dict(now)
        text = now[sym]
        i = text.rindex('0x')
        planted[sym] = text[:i] + '0x00000001' + text[i + 10:] if text[i:i + 10] != '0x00000001' else text[:i] + '0x00000002' + text[i + 10:]
        pb = compare(planted, base)
        say(planted[sym] != text and bool(pb), 'planted twin (one word of %s changed): the lint FAILS it (%s)' % (sym, pb[0] if pb else 'not caught'))
        extra = dict(now)
        extra['rdr_new_extra_frag.h'] = text
        pe = compare(extra, base)
        say(bool(pe), 'planted twin (a third new header): the lint FAILS it (%s)' % (pe[0] if pe else 'not caught'))
        reg = dict(now)
        reg_name = 'fx_registry.h'
        if reg_name in reg:
            reg[reg_name] = reg[reg_name].replace('{', '{ ', 1)
            pr = compare(reg, base)
            say(reg[reg_name] != now[reg_name] and bool(pr), 'planted twin (the registry with one byte changed): the lint FAILS it (%s)' % (pr[0] if pr else 'not caught'))
        else:
            say(False, 'planted registry twin DID NOT APPLY (fx_registry.h is missing)')
    print('spv_lint:', 'FAIL' if FAILS else 'PASS')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
