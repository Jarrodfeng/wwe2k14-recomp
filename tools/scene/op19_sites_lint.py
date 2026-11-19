# Lane L2a (Session 103, landing 2 P0; docs\renderer\LANDING2_PLAN.md section 2): the op-19 site table in src\renderer_scene\quads2d_core.h (kOp19Sites) must equal the call sites of the op-19 recorder
# sub_82C1B350 that `q callers 82C1B350` lists (site and function), no more, no less. A new caller (a different game build, a q index rebuilt from a new map) fails loudly here.
#   py -I tools\scene\op19_sites_lint.py [--root DIR] [--plant drop_one]     exit 0 = equal; 1 = differ / unreadable
#   --plant drop_one: the table read with its first entry removed (the lint must FAIL; exit 0 only when it does)
import argparse, glob, re, subprocess, sys
from pathlib import Path


def table(root):
    src = (Path(root) / 'src' / 'renderer_scene' / 'quads2d_core.h').read_text(encoding='utf-8')
    rows = re.findall(r'\{0x([0-9A-Fa-f]{8})u, 0x([0-9A-Fa-f]{8})u, SiteClass::(\w+)', src)
    return {(int(s, 16), int(f, 16)) for s, f, _ in rows}, len(rows)


def row_sites(root):
    src = (Path(root) / 'src' / 'renderer_scene' / 'quads2d_core.h').read_text(encoding='utf-8')
    return [(int(s, 16), int(f, 16)) for s, f, c in re.findall(r'\{0x([0-9A-Fa-f]{8})u, 0x([0-9A-Fa-f]{8})u, SiteClass::(\w+)', src) if c == 'Row']


def generated_dir(root):
    """generated/default of this checkout, else of the main checkout (a worktree has none): the git common dir's parent."""
    for cand in [Path(root) / 'generated' / 'default']:
        if cand.is_dir():
            return cand
    try:
        common = subprocess.run(['git', '-C', str(root), 'rev-parse', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        cand = (Path(root) / common).resolve().parent / 'generated' / 'default'
        return cand if cand.is_dir() else None
    except Exception:
        return None


def li_r4_before(gen, site, fn):
    """The literal of the `li r4,N` nearest before the instruction at `site` in sub_<fn> (no other call in between), from the PPC comments of the local generated code; None = not found."""
    name = f'DEFINE_REX_FUNC(sub_{fn:08X})'
    for path in sorted(gen.glob('wwe2k14_recomp.*.cpp')):
        text = path.read_text(encoding='utf-8')
        i = text.find(name)
        if i < 0:
            continue
        body = text[i:text.find('\n}\n', i)]
        addr, last = fn, None
        for line in body.splitlines()[1:]:
            s = line.strip()
            lab = re.match(r'loc_([0-9A-F]{8}):', s)
            if lab:
                addr = int(lab.group(1), 16)
                continue
            if not s.startswith('// '):
                continue
            ins = s[3:]
            if addr == site:
                return last if ins.startswith('bl ') else None
            m = re.match(r'li r4,(-?\d+)$', ins)
            if m:
                last = int(m.group(1))
            elif ins.startswith('bl ') or ins.startswith('b '):
                last = None
            addr += 4
        return None
    return None


def callers(root):
    out = subprocess.run([sys.executable, '-I', str(Path(root) / 'tools' / 'q' / 'q.py'), 'callers', '82C1B350'], capture_output=True, text=True, timeout=300).stdout
    got = set()
    head = re.search(r'sub_82C1B350: (\d+) call sites in (\d+) functions', out)
    for line in out.splitlines():
        m = re.match(r'\s+sub_([0-9A-Fa-f]{8})\s+(.*)$', line)
        if not m:
            continue
        fn = int(m.group(1), 16)
        for site in re.findall(r'0x([0-9A-Fa-f]{8}) call', m.group(2)):
            got.add((int(site, 16), fn))
    return got, head


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--plant', default='')
    a = ap.parse_args()
    mine, n = table(a.root)
    if a.plant == 'drop_one':
        mine = set(sorted(mine)[1:])
    elif a.plant:
        print('unknown plant', a.plant)
        return 2
    theirs, head = callers(a.root)
    if not theirs or not head:
        print('FAIL: q callers 82C1B350 gave no call sites (the q index missing?)')
        return 1   # (S103 L2aF: also under --plant: a plant that could not be tried is not CAUGHT)
    ok = mine == theirs and n == len(mine) and int(head.group(1)) == len(theirs)
    # S103 P3c review: the primitive each Row site draws is the literal `li r4,N` before its call (sub_82C1B350's r4): 6 = strip, 13 = quad list; the matrix drawer assumes exactly these two
    gen = generated_dir(a.root)
    if gen is None:
        print('SKIP: no generated\\default (this checkout or the main one): the li r4 primitive check did not run')
    else:
        for s, fn in row_sites(a.root):
            lit = li_r4_before(gen, s, fn)
            if lit not in (6, 13):
                ok = False
                print(f'  Row site 0x{s:08X} in sub_{fn:08X}: `li r4` before the call is {lit} (want 6 or 13)')
            else:
                print(f'  Row site 0x{s:08X}: li r4,{lit}')
    for s, f in sorted(theirs - mine):
        print(f'  missing from kOp19Sites: 0x{s:08X} in sub_{f:08X}')
    for s, f in sorted(mine - theirs):
        print(f'  in kOp19Sites, not a q caller: 0x{s:08X} in sub_{f:08X}')
    print(('PASS' if ok else 'FAIL') + f': kOp19Sites {len(mine)} sites ({n} rows) vs q callers {len(theirs)} sites ({head.group(1)} by its header)')
    if a.plant:
        print('plant drop_one:', 'CAUGHT' if not ok else 'NOT CAUGHT')
        return 0 if not ok else 1
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
