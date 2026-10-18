"""Session 29: what a run's input script checked, in one line, plus one contact sheet of its screenshots.

  py tools/harness/run_verdict.py NNN [--exe-dir DIR] [--no-sheet] [--max-tiles 24]

Reads logs/wwe2k14_NNN.log in the exe folder (default out/build/win-amd64-release) and prints one line, e.g.

  checks: script finished; 3 asserts ok, 5 waits met, 1 optional timeout, 2 retries, 2 verdicts PASS; 4 shots, sheet captures/2167__sheet.jpg

A failed script shows its first failure instead ("script FAILED line 23: wait_state cursor 8 not reached ...").
With 2+ screenshots it writes captures/NNN__sheet.jpg: every screenshot as a 480 px tile with its label in the corner,
4 per row (more than --max-tiles: evenly spaced ones, the count is said). Local only: game images, never commit.
run_list.ps1 prints this line under each run's result line (Write-Host: callers that parse run_list's output never
see it). The line never contains "->" (several tools pick run_list lines by that).
"""
import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_EXE_DIR = os.path.join(REPO, 'out', 'build', 'win-amd64-release')

PATTERNS = {
    'assert_ok': re.compile(r'\[wwe2k14\]\[input\] line \d+(?: \([^)]*\))?: assert_(state|text) .* ok \('),
    'wait_state': re.compile(r'\[wwe2k14\]\[input\] line \d+(?: \([^)]*\))?: state .* reached after'),
    'wait_file': re.compile(r"\[wwe2k14\]\[input\] line \d+(?: \([^)]*\))?: (saw '|text '.*' on screen after)"),
    'press_state': re.compile(r'\[wwe2k14\]\[input\] line \d+(?: \([^)]*\))?: .* met \d+ frames after press'),
    'optional': re.compile(r'optional: going on'),
    'retry': re.compile(r'pressing again'),
    'shot': re.compile(r'\[wwe2k14\]\[input\] screenshot saved: (.+?) \(\d+x\d+\)'),
    'failed': re.compile(r'\[wwe2k14\]\[input\] script FAILED(?: at line (\d+)(?: \([^)]*\))?)?: (.*)'),
    'finished': re.compile(r'\[wwe2k14\]\[input\] script finished at frame'),
    'quit': re.compile(r'\[wwe2k14\]\[input\] line \d+(?: \([^)]*\))?: quit \(frame'),
    'replaying': re.compile(r'\[wwe2k14\]\[input\] replaying script'),
    'verdict': re.compile(r'\[wwe2k14\]\[check\] (PASS|FAIL|SKIP|TIMEOUT)\b'),
    'live': re.compile(r'\[wwe2k14\]\[live\] batch \d+: '),  # the start line only (the end line says done)
}


def summarise(lines):
    c = {k: 0 for k in PATTERNS}
    shots, failure, verdicts = [], None, {}
    for line in lines:
        if 'wwe2k14]' not in line:
            continue
        m = PATTERNS['shot'].search(line)
        if m:
            shots.append(m.group(1))
            continue
        m = PATTERNS['failed'].search(line)
        if m:
            c['failed'] += 1
            if failure is None:
                failure = ('line %s: ' % m.group(1) if m.group(1) else '') + m.group(2).strip()
            continue
        m = PATTERNS['verdict'].search(line)
        if m:
            verdicts[m.group(1)] = verdicts.get(m.group(1), 0) + 1
            continue
        for key in ('assert_ok', 'wait_state', 'wait_file', 'press_state', 'optional', 'retry', 'finished', 'quit',
                    'replaying', 'live'):
            if PATTERNS[key].search(line):
                c[key] += 1
                break
    return c, shots, failure, verdicts


def make_sheet(shots, out_path, max_tiles):
    from PIL import Image, ImageDraw
    picked = shots
    if len(shots) > max_tiles:
        step = (len(shots) - 1) / (max_tiles - 1)
        picked = [shots[round(i * step)] for i in range(max_tiles)]
    tiles = []
    for path in picked:
        try:
            im = Image.open(path).convert('RGB')
        except OSError:
            continue
        tw = 480
        th = max(1, round(im.height * tw / im.width))
        im = im.resize((tw, th), Image.LANCZOS)
        label = os.path.splitext(os.path.basename(path))[0]
        label = re.sub(r'^\d+_', '', label)
        d = ImageDraw.Draw(im)
        box = d.textbbox((0, 0), label)
        d.rectangle((0, 0, box[2] + 8, box[3] + 6), fill=(0, 0, 0))
        d.text((4, 2), label, fill=(255, 255, 0))
        tiles.append(im)
    if len(tiles) < 2:
        return 0
    cols = min(4, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    tw = tiles[0].width
    th = max(t.height for t in tiles)
    sheet = Image.new('RGB', (cols * tw + (cols - 1) * 4, rows * th + (rows - 1) * 4), (40, 40, 40))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * (tw + 4), (i // cols) * (th + 4)))
    tmp = out_path + '.part'
    sheet.save(tmp, 'JPEG', quality=85)
    os.replace(tmp, out_path)
    return len(tiles)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', type=int)
    ap.add_argument('--exe-dir', default=DEFAULT_EXE_DIR)
    ap.add_argument('--no-sheet', action='store_true')
    ap.add_argument('--max-tiles', type=int, default=24)
    a = ap.parse_args()
    log = os.path.join(a.exe_dir, 'logs', 'wwe2k14_%03d.log' % a.run)
    if not os.path.exists(log):
        print('checks: no log %s' % log)
        return 0
    with open(log, encoding='utf-8', errors='replace') as f:
        lines = f.readlines()
    c, shots, failure, verdicts = summarise(lines)
    if failure:
        head = 'script FAILED %s' % failure[:200]
    elif c['quit']:
        head = 'script quit'
    elif c['finished']:
        head = 'script finished'
    elif c['replaying'] or c['live']:
        head = 'script did not end'
    else:
        head = 'no input script'
    parts = []
    if c['assert_ok']:
        parts.append('%d asserts ok' % c['assert_ok'])
    waits = c['wait_state'] + c['wait_file'] + c['press_state']
    if waits:
        parts.append('%d waits met' % waits)
    if c['optional']:
        parts.append('%d optional timeout%s' % (c['optional'], '' if c['optional'] == 1 else 's'))
    if c['retry']:
        parts.append('%d retr%s' % (c['retry'], 'y' if c['retry'] == 1 else 'ies'))
    for v in ('PASS', 'FAIL', 'SKIP', 'TIMEOUT'):
        if verdicts.get(v):
            parts.append('%d verdict%s %s' % (verdicts[v], '' if verdicts[v] == 1 else 's', v))
    if c['live']:
        parts.append('%d live batches' % c['live'])
    text = 'checks: %s' % head
    if parts:
        text += '; ' + ', '.join(parts)
    shots = [s for s in shots if os.path.exists(s)]
    if shots:
        text += '; %d shot%s' % (len(shots), '' if len(shots) == 1 else 's')
        if len(shots) == 1:
            text += ', ' + os.path.relpath(shots[0], a.exe_dir)
        elif not a.no_sheet:
            out = os.path.join(a.exe_dir, 'captures', '%03d__sheet.jpg' % a.run)
            try:
                n = make_sheet(shots, out, a.max_tiles)
                if n:
                    text += ', sheet %s%s' % (os.path.relpath(out, a.exe_dir),
                                              ' (%d of %d)' % (n, len(shots)) if n < len(shots) else '')
            except Exception as e:  # never break a run list over a sheet
                text += ' (sheet failed: %s)' % e
    print(text.replace('->', '>'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
