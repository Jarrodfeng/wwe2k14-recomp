#!/usr/bin/env python3
"""S102 lane T1b acceptance test, offline, 0 game runs, no window: constants by block map (--wwe2k14_rdr_bank_blocks, capture_reader.h BankView::Block / BankRef).

  py -I tools\\renderer\\bank_blocks_check.py [--root DIR] [--no-build] [--exe FVIEW] [--frames N] [--only bryan|match]

For Bryan's entrance and a match capture: the same replay (fview --case ZIP --replay --crowd-ours --validation 1 --hash-frames) in four arms: OFF (--lazy-banks off --bank-blocks off:
the old reader, every bank expanded at parse time), LAZY (--lazy-banks on: T1), BLOCKS (--bank-blocks on) and PLANT (BLOCKS + --bank-block-plant 4: a block map skips its patches).
  1. picture: BLOCKS' presented-surface hash equals OFF's on EVERY frame (byte-equal), on both captures.
  2. counter: KB zeroed + copied per frame (fview's `banks:` line): BLOCKS is below LAZY (the T1 state) and at most 60 % of OFF; the three numbers are printed (the before / after of the claim).
  3. planted twin: PLANT must differ from OFF on at least one frame.
  4. the counter is read: a missing line fails loudly.
Exit 0 all PASS, 1 any FAIL."""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPS = [
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('match', 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'),
]
BANKS_RE = re.compile(r'banks: lazy (on|off), blocks (on|off); (\d+) KB zeroed \+ copied a frame \(([\d.]+) banks built a frame, (\d+) frames\)')


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
    # S102 main (fview lint): fview writes the csv before it exits 4 on validation errors, so the csv alone proves nothing.
    v = re.findall(r'validation errors (\d+)', text)
    errors = int(v[-1]) if v else -1
    if p.returncode not in (0, 4) or errors < 0:
        print(text[-2500:])
        raise RuntimeError('arm %s: fview exit %s, validation count %s (a crash or no count)' % (name, p.returncode, errors))
    if errors > 0 and 'plant' not in name:
        print(text[-2500:])
        raise RuntimeError('arm %s: %d validation errors (exit %s)' % (name, errors, p.returncode))
    m = BANKS_RE.search(text)
    if not m:
        print(text[-2500:])
        raise RuntimeError('arm %s printed no "banks:" counter line' % name)
    return read_hashes(hashes), dict(lazy=m.group(1), blocks=m.group(2), kb=int(m.group(3)), banks=float(m.group(4)), frames=int(m.group(5)))


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
    work = os.path.join(root, 'out', 'bank_blocks_check')
    os.makedirs(work, exist_ok=True)
    fails = 0

    def line(ok, text):
        nonlocal fails
        print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
        fails += 0 if ok else 1

    for name, z in caps:
        off, c_off = run_arm(exe, z, game, work, name + '_off', ['--lazy-banks', 'off', '--bank-blocks', 'off'], args.frames)
        lazy, c_lazy = run_arm(exe, z, game, work, name + '_lazy', ['--lazy-banks', 'on', '--bank-blocks', 'off'], args.frames)
        blk, c_blk = run_arm(exe, z, game, work, name + '_blocks', ['--lazy-banks', 'off', '--bank-blocks', 'on'], args.frames)
        plant, c_pl = run_arm(exe, z, game, work, name + '_plant', ['--bank-blocks', 'on', '--bank-block-plant', '4'], args.frames)
        n = len(off) - 1
        bad = [k for k in off if off[k] != blk.get(k)]
        line(n > 100 and not bad and len(blk) == len(off), 'picture, %s: %d frames, block maps ON byte-equal with OFF (first differing %s)' % (name, n, bad[:6]))
        lbad = [k for k in off if off[k] != lazy.get(k)]
        line(not lbad, 'picture, %s: lazy banks (T1) byte-equal with OFF too (reference arm; first differing %s)' % (name, lbad[:6]))
        line(c_off['blocks'] == 'off' and c_blk['blocks'] == 'on' and c_off['frames'] > 100 and c_blk['frames'] > 100,
             'counter read, %s: off %d frames, blocks %d frames' % (name, c_off['frames'], c_blk['frames']))
        line(c_blk['kb'] < c_lazy['kb'] and c_blk['kb'] * 10 <= c_off['kb'] * 6 and c_off['kb'] > 0,
             'bytes, %s: KB zeroed + copied a frame: OFF %d (%.1f banks) -> lazy %d (%.1f banks) -> blocks %d (%.1f banks assembled), blocks = %.1f %% of lazy, %.1f %% of OFF' % (
                 name, c_off['kb'], c_off['banks'], c_lazy['kb'], c_lazy['banks'], c_blk['kb'], c_blk['banks'], 100.0 * c_blk['kb'] / max(1, c_lazy['kb']), 100.0 * c_blk['kb'] / max(1, c_off['kb'])))
        pbad = [k for k in off if off[k] != plant.get(k)]
        line(len(pbad) > 0, 'planted twin (a block map skips its patches), %s: %d of %d frames differ from OFF (must be > 0)' % (name, len(pbad), n))
    print('RESULT ' + ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
