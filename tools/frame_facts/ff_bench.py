r"""S102 lane R21-A: the PC counts for the lane's claim (docs\renderer\FRAME_FACTS.md).  py -I tools\frame_facts\ff_bench.py [--repeats 3] [--root DIR]
rplay --timing on the Bryan (237 frames) and Brock (341 frames) captures WITH scene records (an unpacked case folder of the main checkout's out\cases: the folder with that many
frames and a scene\ folder; read in place, never copied), the player's bundle, arms frame facts off / on interleaved, `repeats` times. Prints the mean load_us / bind_us /
load+bind per frame over the frames after the 30th, per arm, and the on - off difference. 0 game runs, 0 Surface runs. Not a pass / fail check (timing is noisy)."""
import argparse, csv, glob, os, re, statistics, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
from ff_check import BUNDLE, main_checkout  # noqa: E402


def find_capture(main_co, frames):
    best = None
    # S102 TIDY2: fview_unpack_* copies are scratch (the tidy never holds them): real sources first.
    for d in (glob.glob(os.path.join(main_co, 'out', 'captures', '*')) + glob.glob(os.path.join(main_co, 'out', 'cases', 'case_*', 'capture'))
              + glob.glob(os.path.join(main_co, 'out', 'cases', 'fview_unpack_*', 'capture'))):
        fr = os.path.join(d, 'frames')
        if os.path.isdir(fr) and os.path.isdir(os.path.join(d, 'scene')) and len(os.listdir(fr)) == frames:
            if best is None or os.path.getmtime(d) > os.path.getmtime(best):
                best = d
    return best


def means(path, skip=30):
    rows = list(csv.DictReader(open(path)))[skip:]
    drawn = [r for r in rows if float(r['render_us']) > 0] or rows
    load = statistics.mean(float(r['load_us']) for r in drawn)
    bind = statistics.mean(float(r['bind_us']) for r in drawn)
    return load, bind


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--repeats', type=int, default=3)
    ap.add_argument('--exe', default='')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    exe = a.exe or os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(root, 'out', 'frame_facts', 'bench')
    os.makedirs(work, exist_ok=True)
    for name, frames in (('bryan', 237), ('brock', 341)):
        cap = find_capture(main_co, frames)
        if not cap:
            print('FAIL  no unpacked case folder with %d frames and a scene folder under %s' % (frames, main_co))
            return 1
        res = {'off': [], 'on': []}
        for rep in range(a.repeats):
            for arm in (('off', 'on') if rep % 2 == 0 else ('on', 'off')):
                out = os.path.join(work, '%s_%s_%d.csv' % (name, arm, rep))
                cmd = [exe, '--capture', cap, '--game', game, '--frames', str(frames), '--timing', out, '--out', os.path.join(work, 'x.png')] + BUNDLE + ['--thin', 'arena,ring,wrestlers', '--frame-facts', arm]
                p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=1800)
                m = re.search(r'\[rdr\]\[frame_facts\] .*', p.stdout + p.stderr)
                if p.returncode != 0 or not os.path.isfile(out):
                    print((p.stdout + p.stderr)[-1500:])
                    print('FAIL  rplay %s %s exit %d' % (name, arm, p.returncode))
                    return 1
                load, bind = means(out)
                res[arm].append((load, bind))
                print('  %s %s rep %d: load %.1f us, bind %.1f us a frame   %s' % (name, arm, rep, load, bind, m.group(0)[:150] if m else ''), flush=True)
        for key, idx in (('load_us', 0), ('bind_us', 1)):
            off = statistics.mean(r[idx] for r in res['off'])
            on = statistics.mean(r[idx] for r in res['on'])
            moff, mon = min(r[idx] for r in res['off']), min(r[idx] for r in res['on'])   # the machine is shared: the best repeat of each arm is the least disturbed
            print('%s %s: mean off %.1f, on %.1f, on - off %+.1f; best repeat off %.1f, on %.1f, on - off %+.1f us a frame (%d repeats)' % (name, key, off, on, on - off, moff, mon, mon - moff, a.repeats))
        off = statistics.mean(sum(r) for r in res['off'])
        on = statistics.mean(sum(r) for r in res['on'])
        print('%s load + bind: off %.1f, on %.1f, on - off %+.1f us a frame' % (name, off, on, on - off))
    return 0


if __name__ == '__main__':
    sys.exit(main())
