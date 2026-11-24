#!/usr/bin/env python3
"""S102 lane T1 acceptance test, offline, 0 game runs, no window: constant banks resolved on use (--wwe2k14_rdr_lazy_banks, capture_reader.h BankView).

  py -I tools\\renderer\\lazy_banks_check.py [--root DIR] [--no-build] [--exe FVIEW] [--frames N] [--only bryan|match]

For Bryan's entrance and a match capture (the standard crowd_card_thin captures): the same replay (fview --case ZIP --replay --crowd-ours --validation 1 --hash-frames)
in three arms: OFF (--lazy-banks off: every bank of every frame expanded at parse time, the old reader), ON (--lazy-banks on) and PLANT (ON + --bank-plant: a resolved bank
skips its patches). Our own code against our own code, so equality IS the bar:
  1. picture: ON's presented-surface hash equals OFF's on EVERY frame (byte-equal), on both captures.
  2. counter: the `[fview][time] banks:` line (KB zeroed + copied per frame) is lower with ON than OFF, by the claimed amount (printed; bar: ON <= 60 % of OFF: OFF zeroes then copies every bank into fresh memory, ON copies each bank it reads once into the pool; banks nothing reads cost nothing).
  3. planted twin: PLANT must differ from OFF on at least one frame (the equality check can tell a skipped patch).
  4. the counter itself is read: a missing line fails loudly.
Exit 0 all PASS, 1 any FAIL. A check that cannot read its input fails loudly."""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPS = [
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('match', 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'),
]
BANKS_RE = re.compile(r'banks: lazy (on|off)(?:, blocks (?:on|off))?; (\d+) KB zeroed \+ copied a frame \(([\d.]+) banks built a frame, (\d+) frames\)')


def main_checkout(root):
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return root


def read_hashes(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                a, b = line.split(',')
                out[a if a == 'format' else int(a)] = b
    return out


def run_arm(exe, zip_path, game, work, name, extra, frames):
    hashes = os.path.join(work, name + '.csv')
    if os.path.exists(hashes):
        os.remove(hashes)
    cmd = [exe, '--case', zip_path, '--replay', '--frames', str(frames), '--validation', '1', '--crowd-ours', '--game', game, '--hash-frames', hashes] + extra
    text = ''
    for attempt in range(12):   # the GPU is shared: retry only the logged start-up refusal
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if 'Not enough graphics memory for the frame ring' not in text:
            break
        print('NOTE  arm %s: "Not enough graphics memory for the frame ring", attempt %d of 12' % (name, attempt + 1), flush=True)
        time.sleep(20)
    if not os.path.isfile(hashes):
        print(text[-2500:])
        raise RuntimeError('arm %s wrote no hashes' % name)
    m = BANKS_RE.search(text)
    if not m:
        print(text[-2500:])
        raise RuntimeError('arm %s printed no "banks:" counter line' % name)
    vm = re.search(r'validation errors (\d+)', text)   # S102 G5 (F1 class fix): the csv is written before fview exits 4 on validation errors: the exit code and the count are kept per arm
    return read_hashes(hashes), dict(lazy=m.group(1), kb=int(m.group(2)), banks=float(m.group(3)), frames=int(m.group(4)), rc=p.returncode, val=int(vm.group(1)) if vm else -1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--exe', default='')
    ap.add_argument('--frames', type=int, default=1000)
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    caps = [(n, os.path.join(main_co, z)) for n, z in CAPS if not args.only or n == args.only]
    for need in [game] + [z for _, z in caps]:
        if not os.path.exists(need):
            print('FAIL  input missing: ' + need)
            return 1
    if not args.no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=1800)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            print('FAIL  fview build (exit %d)' % p.returncode)
            return 1
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(prefer=args.exe, root=root, kind='dev')
    work = os.path.join(root, 'out', 'lazy_banks_check')
    os.makedirs(work, exist_ok=True)
    fails = 0

    def line(ok, text):
        nonlocal fails
        print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
        fails += 0 if ok else 1

    for name, z in caps:
        off, c_off = run_arm(exe, z, game, work, name + '_off', ['--lazy-banks', 'off'], args.frames)
        on, c_on = run_arm(exe, z, game, work, name + '_on', ['--lazy-banks', 'on'], args.frames)
        plant, c_pl = run_arm(exe, z, game, work, name + '_plant', ['--lazy-banks', 'on', '--bank-plant'], args.frames)
        n = len(off) - 1
        line(c_off['val'] >= 0 and (c_off['rc'], c_off['val']) == (c_on['rc'], c_on['val']), 'validation, %s: exit code / errors OFF %d / %d, ON %d / %d (must be equal and read)' % (name, c_off['rc'], c_off['val'], c_on['rc'], c_on['val']))
        bad = [k for k in off if off[k] != on.get(k)]
        line(n > 100 and not bad and len(on) == len(off), 'picture, %s: %d frames, lazy banks ON byte-equal with OFF (first differing %s)' % (name, n, bad[:6]))
        line(c_off['lazy'] == 'off' and c_on['lazy'] == 'on' and c_off['frames'] > 100, 'counter read, %s: off %d frames, on %d frames with bank defs' % (name, c_off['frames'], c_on['frames']))
        line(c_on['kb'] * 10 <= c_off['kb'] * 6 and c_off['kb'] > 0,
             'bytes, %s: %d KB zeroed + copied a frame OFF (%.1f banks) -> %d KB ON (%.1f banks), %.1fx less' % (name, c_off['kb'], c_off['banks'], c_on['kb'], c_on['banks'], c_off['kb'] / max(1, c_on['kb'])))
        pbad = [k for k in off if off[k] != plant.get(k)]
        line(len(pbad) > 0, 'planted twin (a resolved bank skips its patches), %s: %d of %d frames differ from OFF (must be > 0)' % (name, len(pbad), n))
    print('RESULT ' + ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
