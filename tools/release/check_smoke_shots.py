"""2026-11-18 (docs\\research\\s103\\PBS.md: the stream-free build drew the boot / title / main-menu screens BLACK): the release smoke's verdict on one run log.

  py -I tools\\release\\check_smoke_shots.py RUN_LOG        exit 0 = pass, 1 = a verdict failed (one FAIL line each), 2 = cannot read
  py -I tools\\release\\check_smoke_shots.py --selftest     a planted all-black shot and a planted '2d_rows 0, holes > 0' census must FAIL, a normal one pass

Checks (release.ps1 runs it on the log of every exe it smoke-tests, wwe2k14.exe and wwe2k14_v3.exe):
  1. the route (tools\\input_scripts\\short\\release_smoke.txt) took `screenshot smoke_title` and `screenshot smoke_menu` BEFORE the match; both are in the log
     ("screenshot saved: PATH (WxH)") and neither file is (near) black (99.5th percentile of the luma below 24, or fewer than 0.2 % of the pixels above 24);
  2. the first `[host] frames ...` census of the run (printed every 600 frames and at exit; the first one covers the boot / title / menu frames) does not show
     `2d_rows 0` or `ours 0` together with `holes` > 0 (every 2D row refused = the black menus);
  3. no `[rdr][overlay] ... have no drawer` line (the ImGui dialogs are missing);
  4. the `[host] the scene host is the poster of every present` line is there (the run really was the stream-free build).
The census is cumulative, so check 2 reads the earliest line: a later line would hide a black boot behind a good match.
"""
import re
import sys
import tempfile
from pathlib import Path

SHOTS = ('smoke_title', 'smoke_menu')
SHOT_RE = re.compile(r'\[wwe2k14\]\[input\] screenshot saved: (.+?) \(\d+x\d+\)')
HOST_RE = re.compile(r'\[host\] frames (\d+), ours (\d+),.*? 2d_rows (\d+) drawn \d+ not_drawn \d+ \(.*?\) holes (\d+)')


def black(path):
    from PIL import Image
    im = Image.open(path).convert('L')
    px = sorted(im.tobytes())
    n = len(px)
    p995 = px[min(n - 1, int(n * 0.995))]
    bright = sum(1 for v in px if v > 24) / n
    return (p995 < 24 or bright < 0.002), p995, bright


def verdict(lines):
    fails = []
    shots = {}
    for l in lines:
        m = SHOT_RE.search(l)
        if m:
            for s in SHOTS:
                if s in m.group(1):
                    shots[s] = m.group(1).strip()
    for s in SHOTS:
        if s not in shots:
            fails.append(f'no screenshot {s} in the log (the route must shoot the title and the menu before the match)')
            continue
        p = Path(shots[s])
        if not p.exists():
            fails.append(f'{s}: {p} not found')
            continue
        is_black, p995, bright = black(p)
        if is_black:
            fails.append(f'{s} is (near) black (99.5th percentile luma {p995}, {bright * 100:.2f} % of pixels above 24): {p}')
    first = next((HOST_RE.search(l) for l in lines if '[host] frames' in l and HOST_RE.search(l)), None)
    if first is None:
        fails.append('no [host] census line in the log (not a stream-free run, or the census did not print)')
    else:
        frames, ours, rows, holes = (int(x) for x in first.groups())
        if holes > 0 and (rows == 0 or ours == 0):
            fails.append(f'the first [host] census shows 2d_rows {rows}, ours {ours}, holes {holes}: every 2D row refused (black menus)')
    if any(re.search(r'\[rdr\]\[overlay\].*have no drawer', l) for l in lines):
        fails.append('[rdr][overlay] ... have no drawer: the ImGui dialogs are missing in this build')
    if not any('[host] the scene host is the poster of every present' in l for l in lines):
        fails.append('no "[host] the scene host is the poster of every present" line: this run was not the stream-free build')
    return fails


def selftest():
    from PIL import Image
    d = Path(tempfile.mkdtemp(prefix='smoke_shots_'))
    good, bad = d / 'smoke_title_ok.png', d / 'smoke_menu_black.png'
    Image.new('RGB', (64, 36), (0, 0, 0)).save(bad)
    im = Image.new('RGB', (64, 36))
    im.putdata([((x * 4) % 256, (y * 7) % 256, 120) for y in range(36) for x in range(64)])
    im.save(good)
    good_menu = d / 'smoke_menu_ok.png'
    im.save(good_menu)
    census = '[host] frames 600, ours 590, 2d_only 0, repeat_3d 0 (loading 0), repeat_full 0, no_record 0, no_picture 0, no_size 0, resize_black 0, black_no_3d 0 (mid_match 0), strikes 0, ticks_dropped 0 of 600, 2d_rows %d drawn 5 not_drawn 0 (-) holes %d frames_2d_dropped 0 of 0 particles_not_drawn 0 movie_repeat 0 tween 0 (draws 0, waiting 0, cut 0, cold 0), presented 600'
    poster = '[host] the scene host is the poster of every present (record j with its own tick); not read yet: x'

    def log(menu, rows, holes, extra=()):
        return [f'[wwe2k14][input] screenshot saved: {good} (64x36)', f'[wwe2k14][input] screenshot saved: {menu} (64x36)', poster, census % (rows, holes), *extra]
    cases = [('normal', log(good_menu, 40, 0), True), ('black menu shot', log(bad, 40, 0), False), ('2d_rows 0 with holes', log(good_menu, 0, 7), False),
             ('no drawer line', log(good_menu, 40, 0, ['[rdr][overlay] this build has no command stream: the ImGui dialogs have no drawer']), False),
             ('no poster line', [l for l in log(good_menu, 40, 0) if 'poster' not in l], False),
             ('no menu shot', log(good_menu, 40, 0)[:1] + log(good_menu, 40, 0)[2:], False)]
    bad_n = 0
    for name, lines, want_pass in cases:
        ok = (not verdict(lines)) == want_pass
        print(('ok   ' if ok else 'FAIL ') + f'{name}: {"passes" if want_pass else "is refused"}')
        bad_n += not ok
    print('check_smoke_shots selftest:', 'PASS' if not bad_n else f'FAIL ({bad_n})')
    return 1 if bad_n else 0


def main():
    a = sys.argv[1:]
    if a == ['--selftest']:
        return selftest()
    if len(a) != 1:
        print(__doc__)
        return 2
    try:
        lines = Path(a[0]).read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError as e:
        print('check_smoke_shots: cannot read', e)
        return 2
    fails = verdict(lines)
    for f in fails:
        print('check_smoke_shots: FAIL', f)
    if not fails:
        print('check_smoke_shots: ok (title + menu shots not black, 2D rows drawn, no missing drawer, stream-free poster present)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
