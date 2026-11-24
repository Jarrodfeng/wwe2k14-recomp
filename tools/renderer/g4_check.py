r"""Lane G4 (S102) offline proof: the dead-work cut (src\renderer_adapter\dead_work.h) replayed by fview on captures, 0 game runs.
  py -I tools\renderer\g4_check.py [--sheets]
Arms (fview --replay, validation on, per-frame picture hashes):
  match   lane86\wm30 (a match: no far cards), card bake cut: pictures byte-equal every frame; the cut equals the census's dead passes; bound draws drop by the cut draws.
  bryan   lane86\bryan (entrance), both switches: byte-equal; pre-light cut equals the census (slots 1-3), the live slot 4 and slot 0 kept; the card bake is kept (far cards read it).
  brock   lane86\brock (Check B, cards), both switches: byte-equal; the cut equals the census.
Planted twins (must be CAUGHT, one of: the picture hashes differ; the cut exceeds the census's dead count):
  cut_live        the first live pre-light pass is cut too.
  ignore_readers  the reader scan finds nothing: every candidate cut.
--sheets: also writes the final frame of each arm on / off side by side, with the pixel difference, to out\s102\sheets\g4\ (KEEP file made).
Counts only. Exit 1 on any failure or when an input cannot be read."""
import datetime
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT')
if not MAIN:
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        MAIN = os.path.dirname(common) if common else ROOT
    except Exception:
        MAIN = ROOT
FVIEW = os.path.join(ROOT, 'out', 'renderer', 'dev', 'fview.exe')
if not os.path.exists(FVIEW):
    FVIEW = os.path.join(MAIN, 'out', 'renderer', 'dev', 'fview.exe')
CAPS = {'match': ('lane86/wm30/capture', 120), 'bryan': ('lane86/bryan/capture', 91), 'brock': ('lane86/brock/capture', 120)}
OUT = os.path.join(MAIN, 'out', 's102', 'g4')
SHEETS = os.path.join(MAIN, 'out', 's102', 'sheets', 'g4')
sys.path.insert(0, HERE)
FAILS = []


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text)
    if not ok:
        FAILS.append(text)


def run(tag, cap, frames, extra, dump=None):
    os.makedirs(OUT, exist_ok=True)
    csv = os.path.join(OUT, tag + '.csv')
    cmd = [FVIEW, '--capture', os.path.join(MAIN, 'out', cap), '--replay', '--frames', str(frames), '--validation', '1', '--hash-frames', csv] + extra
    if dump is not None:
        cmd += ['--dump-frames', str(dump), '--dump-dir', os.path.join(OUT, tag + '_png')]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500)
    text = p.stdout + p.stderr
    if p.returncode != 0 or not os.path.exists(csv):
        print(text[-1500:])
        say(False, '%s: fview exit %d' % (tag, p.returncode))
        return None
    hashes = [l.strip() for l in open(csv) if l.strip() and not l.startswith('format')]
    m = re.search(r'capture: (\d+) frames, (\d+) draws, (\d+) bound', text)
    stats = dict(frames=int(m.group(1)), draws=int(m.group(2)), bound=int(m.group(3))) if m else None
    rep = {}
    for line in text.splitlines():
        m = re.match(r'\[rdr\]\[dead_work\]\s+(crowd card bake|pre-light)\s+candidates (\d+), cut (\d+) \(targets (\d+), clears (\d+), draws (\d+), copies (\d+)\), would cut (\d+) \(draws (\d+), copies (\d+)\), kept live (\d+)', line)
        if m:
            rep[m.group(1)] = dict(cand=int(m.group(2)), cut=int(m.group(3)), targets=int(m.group(4)), clears=int(m.group(5)), draws=int(m.group(6)), copies=int(m.group(7)),
                                   would=int(m.group(8)), would_draws=int(m.group(9)), would_copies=int(m.group(10)), live=int(m.group(11)))
        m = re.search(r'late reads (\d+), stale reads (\d+)', line)
        if m and 'dead_work' in line:
            rep['late'] = int(m.group(1))
            rep['stale'] = int(m.group(2))
    errs = re.search(r'validation errors (\d+)', text)
    return dict(hashes=hashes, stats=stats, rep=rep, errors=int(errs.group(1)) if errs else -1)


def census(cap, frames):
    import g4_census as gc
    import cards_census as cc
    c = cc.Cap(os.path.join(MAIN, 'out', cap))
    if frames < len(c.frames):
        c.frames = c.frames[:frames]
    tot, per_img = gc.census(c, 1)
    return tot


def main():
    sheets = '--sheets' in sys.argv
    if not os.path.exists(FVIEW):
        print('FAIL no fview.exe (tools\\fview\\build.ps1)')
        sys.exit(1)
    runs = {}
    for name, (cap, frames) in CAPS.items():
        if not os.path.isdir(os.path.join(MAIN, 'out', cap)):
            print('FAIL cannot read capture', cap)
            sys.exit(1)
        last = frames - 1
        dump = last if sheets else None
        base = run(name + '_off', cap, frames, [], dump)
        if base is None:
            continue
        on = run(name + '_on', cap, frames, ['--dead-work', '3'], dump)
        if on is None:
            continue
        cen = census(cap, frames)
        runs[name] = (base, on, cen)
        eq = base['hashes'] == on['hashes'] and len(base['hashes']) == frames
        say(eq, '%s: %d frames, picture hashes %s with both cuts on (validation errors %d / %d)' % (name, frames, 'equal' if eq else 'DIFFER', base['errors'], on['errors']))
        say(base['errors'] == 0 and on['errors'] == 0, '%s: validation clean' % name)
        r = on['rep']
        bake_cut = r.get('crowd card bake', {}).get('cut', 0)
        pre_cut = r.get('pre-light', {}).get('would', 0)   # pre-light is census only (review F1): 'cut' must be 0, 'would' is what a cut would take
        say(r.get('pre-light', {}).get('cut', 0) == 0 and r.get('pre-light', {}).get('copies', 0) == 0, '%s: pre-light cuts nothing (census only)' % name)
        # the cut equals the census's dead passes (the first frame of a capture holds CPU uploads: its passes stay, so the census bounds the cut from above, within one frame's passes)
        bake_dead = cen.get('bake_copies_dead', 0)
        pre_dead = cen.get('prelight_copies_dead', 0) // 6
        say(0 <= bake_dead - bake_cut <= 20 and bake_cut <= bake_dead, '%s: card bake cut %d passes, census dead %d (the first frame keeps up to 20)' % (name, bake_cut, bake_dead))
        say(0 <= pre_dead - pre_cut <= 5 and pre_cut <= pre_dead, '%s: pre-light cut %d passes, census dead %d (6 copies a pass)' % (name, pre_cut, pre_dead))
        cut_draws = sum(v['draws'] for k, v in r.items() if isinstance(v, dict))
        say(base['stats']['bound'] - on['stats']['bound'] == cut_draws, '%s: bound draws %d -> %d (cut %d)' % (name, base['stats']['bound'], on['stats']['bound'], cut_draws))
        say(r.get('late', 1) == 0, '%s: late reads %d (a reader before its pass)' % (name, r.get('late', -1)))
        say(r.get('stale', 1) == 0, '%s: stale reads %d (a cut destination read before a kept write)' % (name, r.get('stale', -1)))
        copies = sum(v['copies'] for k, v in r.items() if isinstance(v, dict))
        passes = sum(v['cut'] for k, v in r.items() if isinstance(v, dict))
        print('  %s: cut %d passes, %d copies, %d draws of %d bound' % (name, passes, copies, cut_draws, base['stats']['bound']))
    if 'match' in runs:
        say(runs['match'][1]['rep']['crowd card bake']['cut'] >= 20 * 100, 'match: the card bake is cut in nearly every frame (%d)' % runs['match'][1]['rep']['crowd card bake']['cut'])
    if 'bryan' in runs:
        say(runs['bryan'][1]['rep']['pre-light']['would'] == 3 * (CAPS['bryan'][1]), 'bryan: pre-light slots 1-3 counted dead every frame (%d of %d), none cut' % (runs['bryan'][1]['rep']['pre-light']['would'], 3 * CAPS['bryan'][1]))
    if 'brock' in runs:
        say(runs['brock'][1]['rep']['pre-light']['would'] == 4 * (CAPS['brock'][1]), 'brock: pre-light 4 of 5 slots counted dead every frame (%d of %d)' % (runs['brock'][1]['rep']['pre-light']['would'], 4 * CAPS['brock'][1]))
    if 'bryan' in runs:
        say(runs['bryan'][1]['rep']['crowd card bake']['cut'] == 0, 'bryan: the card bake is kept (far cards read it)')
    # planted twins
    for fault, name, mask in (('cut_live', 'bryan', '2'), ('ignore_readers', 'bryan', '2'), ('ignore_readers', 'brock', '1')):
        if name not in runs:
            continue
        cap, frames = CAPS[name]
        t = run('%s_%s' % (name, fault), cap, frames, ['--dead-work', mask, '--dead-work-fault', fault])
        if t is None:
            continue
        base, on, cen = runs[name]
        differs = t['hashes'] != base['hashes']
        key = 'pre-light' if mask == '2' else 'crowd card bake'
        dead = (cen.get('prelight_copies_dead', 0) // 6) if mask == '2' else cen.get('bake_copies_dead', 0)
        over = t['rep'].get(key, {}).get('cut', 0) > dead
        say(differs or over, 'planted %s on %s: caught (pictures differ: %s; cut %d exceeds the census dead count %d: %s)' % (fault, name, differs, t['rep'].get(key, {}).get('cut', 0), dead, over))
        say(differs, 'planted %s on %s: the picture check alone catches it' % (fault, name))
    # review F1: stale_window (pre-light made real, its readers hidden): the baking frames are cut, the next frames read a cube nobody re-baked
    if 'brock' in runs:
        cap, frames = CAPS['brock']
        t = run('brock_stale_window', cap, frames, ['--dead-work', '2', '--dead-work-fault', 'stale_window'])
        if t is not None:
            differs = t['hashes'] != runs['brock'][0]['hashes']
            say(differs and t['rep'].get('stale', 0) >= 1, 'planted stale_window on brock: the picture differs (%s) and stale_read %d >= 1' % (differs, t['rep'].get('stale', 0)))
    # review F2: phantom_late (a synthetic reader of the first bake destination before its pass in frame 10)
    if 'match' in runs:
        cap, frames = CAPS['match']
        t = run('match_phantom_late', cap, frames, ['--dead-work', '1', '--dead-work-fault', 'phantom_late'])
        if t is not None:
            base_cut = runs['match'][1]['rep']['crowd card bake']['cut']
            got = t['rep'].get('crowd card bake', {}).get('cut', 0)
            want = base_cut - (frames - 10)
            say(t['rep'].get('late', 0) == 1 and got == want, 'planted phantom_late on match: late_read %d == 1, cut %d == %d - %d = %d' % (t['rep'].get('late', 0), got, base_cut, frames - 10, want))
            say(t['hashes'] == runs['match'][0]['hashes'], 'planted phantom_late on match: pictures still equal (fewer cuts only)')
    if sheets:
        make_sheets()
    print('%d failure(s)' % len(FAILS))
    sys.exit(1 if FAILS else 0)


def make_sheets():
    from PIL import Image, ImageChops
    os.makedirs(SHEETS, exist_ok=True)
    rows = []
    for name, (cap, frames) in CAPS.items():
        pair = []
        for arm in ('off', 'on'):
            d = os.path.join(OUT, '%s_%s_png' % (name, arm))
            pngs = sorted(f for f in os.listdir(d) if f.lower().endswith('.png')) if os.path.isdir(d) else []
            if not pngs:
                say(False, '%s %s: no dumped frame' % (name, arm))
                return
            pair.append(Image.open(os.path.join(d, pngs[-1])).convert('RGB'))
        diff = ImageChops.difference(pair[0], pair[1])
        bbox = diff.getbbox()
        npx = sum(1 for px in diff.getdata() if px != (0, 0, 0)) if bbox else 0
        rows.append((name, pair, npx))
    w = 640
    sheet = Image.new('RGB', (w * 2, sum(int(p[0].height * w / p[0].width) + 4 for _, p, _ in rows)), (24, 24, 24))
    y = 0
    for name, pair, npx in rows:
        for k, im in enumerate(pair):
            h = int(im.height * w / im.width)
            sheet.paste(im.resize((w, h)), (k * w, y))
        y += int(pair[0].height * w / pair[0].width) + 4
        print('sheet row %s: left = cuts off, right = cuts on (card bake + pre-light), %d differing pixels' % (name, npx))
    path = os.path.join(SHEETS, 'g4_final_frames_off_on.png')
    sheet.save(path)
    until = (datetime.date.today() + datetime.timedelta(days=7)).isoformat()
    with open(os.path.join(SHEETS, 'KEEP'), 'w') as f:
        f.write('reason: lane G4 final-frame sheet (dead work cut off / on, match + Bryan + Check B), judged by Jarrod\nuntil %s\n' % until)
    print('sheet', path)


if __name__ == '__main__':
    main()
