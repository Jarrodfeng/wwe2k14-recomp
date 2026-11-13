"""Scene-start first-frame check (S101 lane 271, docs\\renderer\\SKIP_GAME_WORK_MAP.md 19.8 / 19.9). Offline: rplay on a capture with scene records, no game, nothing on screen.

What it proves: the per-arena loads (arena, ring, crowd, flash, props, screens, crowd props) start when the match card names the arena (live.cpp calls each class's Announce
the moment session_log.h publishes the card), so the first frame whose record names the arena binds without waiting for them. rplay runs the loads on the calling thread
when a frame needs them (offline mode), so the time its first record frames take IS the load time the live renderer would have shown as "not ready" frames.

Three arms on the same capture (default: the first 4 frames; frame 0 has no scene record, frames 1-3 are the first ones that name the arena):
  base      no announcement                  frames 1-3 bind = the loads (seconds: props alone is ~3 s)
  announce  --announce-arena A, 6 s later    frames 1-3 bind = the work that is left (tens of ms)
  wrong     --announce-arena B (the plant)   the early load is for another arena: discarded, the right one starts late; frames 1-3 must NOT be fast
Exit 0 when announce stays under --limit-ms (sum of frames 1-3 bind_us), base is at least 5x slower, and the plant is over the limit.

  py -I tools\\renderer\\scene_start_check.py --zip out\\s101\\runs\\RUN\\case_*.zip --game game_data\\extracted [--arena 4] [--limit-ms 150]
  py -I tools\\renderer\\scene_start_check.py --capture DIR --game game_data\\extracted
Needs out\\renderer\\dev\\rplay.exe (pwsh -NoProfile -File tools\\renderer\\build.ps1). The arena id is in the case zip's log ("match card ... arena N") or the run log.
"""
import argparse
import csv
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def extract(zip_path: Path, dest: Path) -> Path:
    with zipfile.ZipFile(zip_path) as z:
        for name in z.namelist():
            if name.startswith('capture/'):
                z.extract(name, dest)
    return dest / 'capture'


def run_arm(rplay: Path, capture: Path, game: Path, out: Path, name: str, extra: list[str], frames: int) -> list[dict]:
    csv_path = out / f'arm_{name}.csv'
    cmd = [str(rplay), '--capture', str(capture), '--frames', str(frames), '--crowd-ours', '--arena-ours', '--ring-ours', '--props-ours', '--flash-ours',
           '--screens-ours', '--game', str(game), '--timing', str(csv_path)] + extra
    log = out / f'arm_{name}.log'
    with open(log, 'w', encoding='utf-8') as f:
        subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=900, check=False)
    with open(csv_path, newline='') as f:
        return list(csv.DictReader(f))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--zip')
    ap.add_argument('--capture')
    ap.add_argument('--game', required=True)
    ap.add_argument('--arena', type=int, default=4, help='the capture\'s arena id (the match card line of its log)')
    ap.add_argument('--wrong-arena', type=int, default=7)
    ap.add_argument('--limit-ms', type=float, default=150.0)
    ap.add_argument('--frames', type=int, default=4)
    ap.add_argument('--out', default=str(REPO / 'out' / 'scene_start_check'))
    ap.add_argument('--rplay', default=str(REPO / 'out' / 'renderer' / 'dev' / 'rplay.exe'))
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    if args.zip:
        capture = extract(Path(args.zip), out / 'cap')
    elif args.capture:
        capture = Path(args.capture)
    else:
        ap.error('--zip or --capture')
    game, rplay = Path(args.game), Path(args.rplay)
    arms = {
        'base': [],
        'announce': ['--announce-arena', str(args.arena), '--announce-wait-ms', '6000'],
        'wrong': ['--announce-arena', str(args.wrong_arena), '--announce-wait-ms', '6000'],
    }
    sums = {}
    for name, extra in arms.items():
        rows = run_arm(rplay, capture, game, out, name, extra, args.frames)
        binds = [float(r['bind_us']) / 1000.0 for r in rows]
        sums[name] = sum(binds[1:4])
        print(f'{name:9s} bind ms by frame: ' + ', '.join(f'{b:8.1f}' for b in binds) + f'   frames 1-3: {sums[name]:9.1f} ms')
    ok = True
    if sums['announce'] > args.limit_ms:
        print(f'FAIL announce: {sums["announce"]:.1f} ms over the limit {args.limit_ms:.0f} ms')
        ok = False
    if sums['base'] < 5 * max(sums['announce'], 1.0):
        print(f'FAIL base: {sums["base"]:.1f} ms is not 5x the announced arm: this capture has no load to hide')
        ok = False
    if sums['wrong'] <= args.limit_ms:
        print(f'FAIL plant: the wrong-arena announcement was not caught ({sums["wrong"]:.1f} ms <= {args.limit_ms:.0f} ms)')
        ok = False
    # the plant's own evidence: in the wrong-arena arm the real arena's upload line comes after the announcement line (the right load started late), and the log says the early load was replaced
    wrong = (out / 'arm_wrong.log').read_text(encoding='utf-8', errors='replace').splitlines()
    said = next((i for i, l in enumerate(wrong) if 'announced arena' in l), -1)
    up = next((i for i, l in enumerate(wrong) if f'[rdr][arena] arena {args.arena}:' in l and 'uploaded' in l), -1)
    if not (said >= 0 and up > said):
        print(f'FAIL plant: the arena {args.arena} upload line ({up}) is not after the announcement line ({said}) in arm_wrong.log')
        ok = False
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
