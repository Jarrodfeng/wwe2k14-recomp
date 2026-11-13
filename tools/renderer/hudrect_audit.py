"""lane 186: the "look at these" sweep for the HUD-rectangle rule (docs\\renderer\\WIDE_OURS.md section 8). Offline, 0 game runs.
Runs rplay --wide 1.3125 --wide-census over every kept capture (out\\captures\\*, out\\cases\\*\\capture) and lists the T_Rects after the HUD marker (VS patched
CAAB5FD3B6EE4944, 'hud' phase) that are NOT full-view but whose box is >= 0.9 of the view wide or tall (strips, letterbox, near-full rects): those are the ones the
squeeze changes most and the first to look at live. The box comes from the census's [wide-box] line (view pixels, the constants' own view size).
usage: py -I tools\\renderer\\hudrect_audit.py [--rplay EXE] [--root DIR]   (read-only; prints a table)"""
import argparse, glob, os, re, subprocess, sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rplay', default=None)
    ap.add_argument('--root', default=None)
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    main_root = os.path.normpath(os.path.join(repo, '..', '..', '..'))
    root = a.root or (main_root if os.path.isdir(os.path.join(main_root, 'out', 'captures')) else repo)
    rplay = os.path.abspath(a.rplay or os.path.join(repo, 'out', 'renderer', 'release', 'rplay.exe'))
    caps = sorted(glob.glob(os.path.join(root, 'out', 'captures', '*', 'images.bin')) + glob.glob(os.path.join(root, 'out', 'cases', '*', 'capture', 'images.bin')))
    tmp = os.path.join(repo, 'out', 'l186_audit.png')
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    total_frames = 0
    hits = {}  # (case, box, view, verdict, wide, tall) -> [frames seen]
    covered = 0
    seen_sig = {}
    for img in caps:
        cap = os.path.dirname(img)
        case = os.path.relpath(cap, root)
        p = subprocess.run([rplay, '--capture', cap, '--frames', '100000', '--scale', '1', '--out', tmp, '--root', root, '--wide', '1.3125', '--wide-census'],
                           capture_output=True, text=True, errors='replace', timeout=900)
        text = p.stdout + p.stderr
        m = re.search(r'\[rplay\] capture: (\d+) frames', text)
        nframes = int(m.group(1)) if m else 0
        if not m:
            print('SKIP (no census) %s' % case, flush=True)
            continue
        covered += 1
        total_frames += nframes
        frame, last, box = 0, -1, None
        for ln in text.splitlines():
            mb = re.match(r'\[wide-box\] draw (\d+) box (\S+) (\S+) (\S+) (\S+) view (\S+) (\S+)', ln)
            if mb:
                d = int(mb.group(1))
                if d < last:
                    frame += 1
                last = d
                box = tuple(float(x) for x in mb.groups()[1:])
                continue
            mw = re.match(r'\[wide\] draw (\d+) vs CAAB5FD3B6EE4944 .* hud (?:post-after )?\s*-> (SQUEEZE|FULL-VIEW|left)', ln)
            if mw and box:
                x0, y0, x1, y1, W, H = box
                if mw.group(2) != 'FULL-VIEW' and (x1 - x0) > 0.5 and (y1 - y0) > 0.5 and ((x1 - x0) >= 0.9 * W or (y1 - y0) >= 0.9 * H):
                    key = (case, tuple(round(v, 1) for v in box), mw.group(2))
                    hits.setdefault(key, []).append(frame)
            if mw:
                box = None
    print('covered %d captures, %d frames' % (covered, total_frames))
    if not hits:
        print('no non-full-view T_Rect after the marker is >= 0.9 wide or tall in any of them')
    for (case, box, verdict), frames in sorted(hits.items()):
        print('%s: box x %.1f..%.1f y %.1f..%.1f (view %.0f x %.0f) %s, %d draws, frames %d..%d' % (case, box[0], box[2], box[1], box[3], box[4], box[5], verdict, len(frames), min(frames), max(frames)))
    try:
        os.remove(tmp)
    except OSError:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
