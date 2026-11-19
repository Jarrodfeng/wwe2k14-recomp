"""Score choice_predict.py on the two sealed table sets and check the planted twin (S103 lane UVF).

py -I tools\\harvest\\choice_set_score.py [--tables out\\s103\\decomp] [--plant]

Set 1 (a08 a16 a30 a44 a55) must stay 1178 / 1180; set 2 (a00 a09 a41 a52 a54) must be 1422 / 1422 (the cycled-slot selector rule,
sel_cycled). --plant also runs set 2 with the sel_cycled branch off: the 4 yBG_UVScroll misses must come back (1418 / 1422).
Reads the player's tables locally (names and numbers only; nothing from the game is written).
"""
import argparse, glob, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SETS = {'sealed': ([8, 16, 30, 44, 55], 1178, 1180), 'sealed2': ([0, 9, 41, 52, 54], 1422, 1422)}


def run(args):
    r = subprocess.run([sys.executable, '-I'] + args, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit(2)
    return r.stdout


def score(tdir, name, arenas, plant, tmp):
    meshes = []
    for a in arenas:
        out = os.path.join(tmp, 'p%d%s.json' % (a, 'x' if plant else ''))
        run([os.path.join(HERE, 'choice_predict.py'), str(a), '--out', out] + (['--plant-no-sel-cycled'] if plant else []))
        meshes += json.load(open(out))['meshes']
    pred = os.path.join(tmp, '%s%s.json' % (name, 'x' if plant else ''))
    json.dump({'meshes': meshes}, open(pred, 'w'))
    tables = []
    for a in arenas:
        tables += sorted(glob.glob(os.path.join(tdir, name, 'a%02d__*.json' % a)))
    o = run([os.path.join(HERE, 'choice_score.py'), pred] + tables + ['--show', '0'])
    m = re.search(r'WHOLE\s+(\d+) / (\d+)', o)
    return int(m.group(1)), int(m.group(2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tables', default=os.path.join('out', 's103', 'decomp'))
    ap.add_argument('--plant', action='store_true')
    a = ap.parse_args()
    rc = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (arenas, want, tot) in SETS.items():
            got, n = score(a.tables, name, arenas, False, tmp)
            ok = got >= want and n == tot
            rc |= 0 if ok else 1
            print('%-8s %d / %d (want >= %d / %d) %s' % (name, got, n, want, tot, 'PASS' if ok else 'FAIL'))
        if a.plant:
            got, n = score(a.tables, 'sealed2', SETS['sealed2'][0], True, tmp)
            ok = got == 1418
            rc |= 0 if ok else 1
            print('plant sel_cycled off, sealed2: %d / %d (the 4 misses come back = 1418) %s' % (got, n, 'CAUGHT' if ok else 'NOT CAUGHT'))
    return rc


if __name__ == '__main__':
    sys.exit(main())
