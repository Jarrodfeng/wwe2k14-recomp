#!/usr/bin/env python3
r"""S103 RL review F1: no NEW environment reads in player code.

A planted fault (a "twin of the old behaviour") belongs in a struct the rig / fview set (rf::c::Plants, WrestlerPlants, load_detail::*Plant()), never in a
std::getenv("WWE_...") inside src\** that the game exe links: the player's machine can carry any variable, and each read is a hidden switch of the shipped look.

The lint counts every getenv("WWE_NAME") in src\**\*.h / *.cpp / *.cc (src\fview excluded: that is a tool) per file and name, and compares the counts with
tools\scene\getenv_baseline.txt (lines `relative/path|WWE_NAME|count`, today's reads, to BURN DOWN). A read that is not in the baseline, or a count above it, FAILS. A count
below the baseline passes and says so (run --update to shrink the baseline).

  py -I tools\scene\getenv_lint.py --root . [--update] [--selftest]
"""
import os, re, sys, tempfile, shutil

PAT = re.compile(r'getenv\s*\(\s*"(WWE(?:2K14)?_[A-Z0-9_]+)"')
EXT = ('.h', '.cpp', '.cc')


def scan(root):
    counts = {}
    src = os.path.join(root, 'src')
    for dp, dn, fn in os.walk(src):
        rel_dir = os.path.relpath(dp, root).replace('\\', '/')
        if rel_dir == 'src/fview' or rel_dir.startswith('src/fview/'):
            dn[:] = []
            continue
        for f in fn:
            if not f.endswith(EXT):
                continue
            path = os.path.join(dp, f)
            text = open(path, encoding='utf-8', errors='replace').read()
            for m in PAT.finditer(text):
                key = (rel_dir + '/' + f, m.group(1))
                counts[key] = counts.get(key, 0) + 1
    return counts


def load_baseline(path):
    base = {}
    if not os.path.exists(path):
        return base
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        p, v, n = line.split('|')
        base[(p, v)] = int(n)
    return base


def write_baseline(path, counts):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# S103 RL F1: today\'s getenv("WWE_... or WWE2K14_...") reads in src/** (tools\\scene\\getenv_lint.py), to BURN DOWN. Never add a line: put a planted fault in a Plants struct the rig sets.\n')
        for (p, v), n in sorted(counts.items()):
            f.write('%s|%s|%d\n' % (p, v, n))


def check(root, baseline_path, quiet=False):
    counts, base = scan(root), load_baseline(baseline_path)
    new = ['%s reads %s x%d (baseline %d)' % (p, v, n, base.get((p, v), 0)) for (p, v), n in sorted(counts.items()) if n > base.get((p, v), 0)]
    gone = [k for k in base if counts.get(k, 0) < base[k]]
    if not quiet:
        if new:
            print('FAIL  getenv_lint: %d new environment read(s) in player code: %s' % (len(new), '; '.join(new)))
            print('      a planted fault goes in a Plants struct the rig / fview sets (rf::c::Plants, assets::load_detail::*Plant()), not in getenv')
        else:
            print('PASS  getenv_lint: %d reads in %d (file, name) pairs, none new%s' % (sum(counts.values()), len(counts), ('; %d baseline entries shrank: run --update' % len(gone)) if gone else ''))
    return not new


def selftest(root):
    tmp = tempfile.mkdtemp(prefix='getenv_lint_')
    try:
        os.makedirs(os.path.join(tmp, 'src', 'x'))
        os.makedirs(os.path.join(tmp, 'src', 'fview'))
        good = os.path.join(tmp, 'src', 'x', 'a.cpp')
        open(good, 'w').write('int f() { return std::getenv("WWE_OLD") != nullptr; }\n')
        open(os.path.join(tmp, 'src', 'fview', 'b.cpp'), 'w').write('int g() { return std::getenv("WWE_TOOL") != nullptr; }\n')   # the tool dir is not scanned
        bl = os.path.join(tmp, 'base.txt')
        write_baseline(bl, scan(tmp))
        res = []
        res.append(('clean tree passes', check(tmp, bl, True) is True))
        open(good, 'a').write('int h() { return std::getenv("WWE_RDR_NEW_PLANT") != nullptr; }\n')   # the planted twin: a NEW read
        res.append(('a new read FAILS', check(tmp, bl, True) is False))
        open(good, 'w').write('int f() { return std::getenv("WWE_OLD") != nullptr && std::getenv("WWE_OLD") != nullptr; }\n')
        res.append(('a second read of a baselined name FAILS', check(tmp, bl, True) is False))
        open(good, 'w').write('int f() { return 0; }\n')
        res.append(('a burned-down read passes', check(tmp, bl, True) is True))
        # and the real tree: the baseline must hold today's reads (no unlisted one)
        res.append(('the real tree is within its baseline', check(root, os.path.join(root, 'tools', 'scene', 'getenv_baseline.txt'), True) is True))
        for name, ok in res:
            print('%s  selftest: %s' % ('PASS' if ok else 'FAIL', name))
        return all(ok for _, ok in res)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    root = sys.argv[sys.argv.index('--root') + 1] if '--root' in sys.argv else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    root = os.path.normpath(root)
    bl = os.path.join(root, 'tools', 'scene', 'getenv_baseline.txt')
    if '--update' in sys.argv:
        write_baseline(bl, scan(root))
        print('baseline rewritten: %s' % bl)
        return 0
    if '--selftest' in sys.argv:
        return 0 if selftest(root) else 1
    return 0 if check(root, bl) else 1


if __name__ == '__main__':
    sys.exit(main())
