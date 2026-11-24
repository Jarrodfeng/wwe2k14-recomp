"""Session 29: what a run's input script checked, in one line, plus one contact sheet of its screenshots.

  py tools/harness/run_verdict.py NNN [--exe-dir DIR] [--no-sheet] [--max-tiles 24]
  py tools/harness/run_verdict.py --log PATH\\wwe2k14_NNN.log [...]     (S108 SHOT: the run's identity is its LOG FILE; NNN alone is only a number in a folder)
  py tools/harness/run_verdict.py --selftest [--plant no_commit_check]

Reads logs/wwe2k14_NNN.log in the exe folder (default out/build/win-amd64-release) and prints one line, e.g.

  checks: script finished; 3 asserts ok, 5 waits met, 1 optional timeout, 2 retries, 2 verdicts PASS; 4 shots, sheet captures/2167__sheet.jpg

A failed script shows its first failure instead ("script FAILED line 23: wait_state cursor 8 not reached ...").
With 2+ screenshots it writes captures/NNN__sheet.jpg: every screenshot as a 480 px tile with its label in the corner,
4 per row (more than --max-tiles: evenly spaced ones, the count is said). Local only: game images, never commit.
run_list.ps1 prints this line under each run's result line (Write-Host: callers that parse run_list's output never
see it). The line never contains "->" (several tools pick run_list lines by that).

S108 SHOT (docs/research/s108/SHOT_plan.md sections 5, 7): the sheet is built ONLY from the paths the run's own log names, and it is written in the run's own folder
captures/NNN_pPID/ (pid from "[wwe2k14] run log number: NNN (pid P)") with the log path and the pid printed in its header strip. For each
"[wwe2k14][input] subject shot: SUBJECT N label L" whose record the log says was saved ("scene record saved: PATH (guest frame G, host frame J)") the frame rig
(out/tools/frame_rig.exe --record PATH --subject SUBJECT:N) draws the subject offline; its picture is a tile next to the game-camera shot of that label. A rig whose
commit's src differs from the game exe's (out/tools/frame_rig.stamp vs <exe dir>/stamp/wwe2k14_build_stamp.h; S109 WARM: by src content, rig_fits, so docs / tools
commits between them do not count) is REFUSED unless --allow-stale (then said on the line).
Subject tiles carry their limits: DOF off, screens = rig feed, game-camera items as the game drew them, and the rows missing because the game did not draw the model.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

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
# S108 SHOT: the run's identity and its subject shots (src/input_script.h logs these; the log is the only source)
RUN_ID = re.compile(r'\[wwe2k14\] run log number: (\d+) \(pid (\d+)\)')
SUBJ_SHOT = re.compile(r'\[wwe2k14\]\[input\] subject shot: (\w+) (\d+) label (\S+)')
REC_SAVED = re.compile(r'\[wwe2k14\]\[input\] scene record saved: (.+?) \(guest frame (\d+), host frame (\d+)\)')
LIMITS = 'DOF off, screens = rig feed, game-camera items as the game drew them'


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


def shot_label(path):
    return re.sub(r'^\d+_', '', os.path.splitext(os.path.basename(path))[0])


def make_sheet(shots, out_path, max_tiles, header=None):
    """shots: paths, or (path, label) pairs (a subject tile names itself). header: lines printed in a strip on top (the run's log path and pid)."""
    from PIL import Image, ImageDraw
    picked = shots
    if len(shots) > max_tiles:
        step = (len(shots) - 1) / (max_tiles - 1)
        picked = [shots[round(i * step)] for i in range(max_tiles)]
    tiles = []
    for item in picked:
        path, named = item if isinstance(item, tuple) else (item, None)
        try:
            im = Image.open(path).convert('RGB')
        except OSError:
            continue
        tw = 480
        th = max(1, round(im.height * tw / im.width))
        im = im.resize((tw, th), Image.LANCZOS)
        label = named or shot_label(path)
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
    strip = 0
    if header:
        strip = 14 * len(header) + 6
    sheet = Image.new('RGB', (cols * tw + (cols - 1) * 4, rows * th + (rows - 1) * 4 + strip), (40, 40, 40))
    if header:
        d = ImageDraw.Draw(sheet)
        for i, text in enumerate(header):
            d.text((4, 3 + 14 * i), text, fill=(200, 255, 200))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * (tw + 4), strip + (i // cols) * (th + 4)))
    tmp = out_path + '.part'
    sheet.save(tmp, 'JPEG', quality=85)
    os.replace(tmp, out_path)
    return len(tiles)


def scan_run(lines):
    """The run's identity and subject shots from its log: (pid or None, [(kind, n, label)], [record paths saved, in order])."""
    pid, subjects, saved = None, [], []
    for line in lines:
        if 'wwe2k14]' not in line:
            continue
        m = RUN_ID.search(line)
        if m and pid is None:
            pid = int(m.group(2))
            continue
        m = SUBJ_SHOT.search(line)
        if m:
            subjects.append((m.group(1), int(m.group(2)), m.group(3)))
            continue
        m = REC_SAVED.search(line)
        if m:
            saved.append(m.group(1))
    return pid, subjects, saved


SHOT_FRAME = re.compile(r'\[wwe2k14\]\[input\] screenshot saved: (.+?) \(\d+x\d+\) guest frame (\d+)')


def frame_mismatches(lines):
    """S108 SHOT review: a subject shot's record is the host's next DRAWN frame, its screenshot the next PRESENTED one; they can differ (a held or 30 fps-skipped frame, a request made in a menu).
    The screenshot line's guest frame and the record line's host frame are the same counter (the host's message frame). Returns ['LABEL: not the same frame (...)'] for every label whose two differ."""
    shot, rec = {}, {}
    for line in lines:
        m = SHOT_FRAME.search(line)
        if m:
            shot[shot_label(m.group(1))] = int(m.group(2))
            continue
        m = REC_SAVED.search(line)
        if m:
            rec[os.path.splitext(os.path.basename(m.group(1)))[0]] = int(m.group(3))
    # (a screenshot answered by the idle drain, ServeShotSync, logs guest frame 0 = not known: not compared)
    return ['%s: not the same frame (record host frame %d, screenshot guest frame %d)' % (k, rec[k], shot[k]) for k in rec if shot.get(k) and shot[k] != rec[k]]


def exe_stamp(exe_dir):
    """The game exe's build stamp (12-hex git hash) from <exe dir>/stamp/wwe2k14_build_stamp.h; None when there is none."""
    p = os.path.join(exe_dir, 'stamp', 'wwe2k14_build_stamp.h')
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            m = re.search(r'WWE2K14_BUILD_GIT\s+"([0-9a-fA-F]+)"', f.read())
    except OSError:
        return None
    return m.group(1).lower() if m else None


def rig_stamp(rig):
    """The rig's build stamp (tools/scene/build_rig.ps1 writes frame_rig.stamp: 'git HASH dirty 0|1') next to it; None when there is none."""
    p = os.path.splitext(rig)[0] + '.stamp'
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            m = re.search(r'git\s+([0-9a-fA-F]+)', f.read())
    except OSError:
        return None
    return m.group(1).lower() if m else None


def same_commit(a, b):
    return bool(a and b) and (a.startswith(b) or b.startswith(a))


GIT_REPO = REPO   # where the two stamps' commits are looked up (the selftest points it at its own repository)


def rig_fits(es, rs, plant=None):
    """S109 WARM: -> (fits, why). The rig draws what the exe draws when the code they share is the same: every rig source is under src\\ except its own
    harness (tools\\scene\\frame_rig.cpp; tools\\scene\\build_rig.ps1 $srcs), so the rule is `git diff --name-only <rig commit> <exe commit> -- src` is
    empty (the same commit fits at once, as before). Docs / tools commits between them never refuse a sheet. Either commit unknown here = does not fit.
    The stamp's dirty flag is not part of it (it counts any tracked edit, docs too); tools\\exe\\tip_exe.ps1 builds the rig only from a src-clean checkout."""
    if same_commit(es, rs):
        return True, ''
    if not es or not rs or plant == 'commit_only':
        return False, ''
    for c in (rs, es):
        p = subprocess.run(['git', '-C', GIT_REPO, 'rev-parse', '--verify', '-q', c + '^{commit}'], capture_output=True, text=True, timeout=60)
        if p.returncode != 0:
            return False, 'commit %s is not known in %s' % (c, GIT_REPO)
    p = subprocess.run(['git', '-C', GIT_REPO, 'diff', '--name-only', rs, es, '--', 'src'], capture_output=True, text=True, timeout=60)
    if p.returncode != 0:
        return False, 'git diff %s %s failed: %s' % (rs, es, p.stderr.strip()[:150])
    files = [f for f in p.stdout.splitlines() if f.strip()]
    if files:
        return False, '%d file(s) under src differ between them: %s%s' % (len(files), ', '.join(files[:4]), ' ...' if len(files) > 4 else '')
    return True, 'rig %s and exe %s have the same src' % (rs[:9], es[:9])


def find_game_dir():
    """The player's extracted files: game_data/extracted in this checkout, else in the main checkout above a .claude/worktrees/NAME worktree."""
    d = REPO
    for _ in range(5):
        p = os.path.join(d, 'game_data', 'extracted')
        if os.path.isdir(p):
            return p
        d = os.path.dirname(d)
    return os.path.join(REPO, 'game_data', 'extracted')


def rig_command(rig, args):
    return ([sys.executable] if rig.lower().endswith('.py') else []) + [rig] + args


def run_subject_shots(subjects, saved, exe_dir, run_dir, rig, game, size, allow_stale, plant=None):
    """Draws every subject shot whose record the log says was saved. Returns (refusal or None, [(png, label, note)], [problem text])."""
    if not subjects:
        return None, [], []
    es, rs = exe_stamp(exe_dir), rig_stamp(rig)
    stale = ''
    fits, fwhy = rig_fits(es, rs, plant)
    if plant != 'no_commit_check' and not fits:
        why = 'the rig %s is built from %s, the game exe from %s%s' % (rig, rs or 'an unknown commit (no frame_rig.stamp: rebuild with tools/scene/build_rig.ps1)', es or 'an unknown commit (no stamp/wwe2k14_build_stamp.h)', ('; ' + fwhy) if fwhy else '')
        if not allow_stale:
            return 'subject shots refused: %s; rebuild the rig (tools/scene/build_rig.ps1; the tip runner tools/exe/tip_exe.ps1 keeps it with the tip), or pass --allow-stale' % why, [], []
        stale = ' (STALE rig allowed: %s)' % why
    if not os.path.exists(rig):
        return 'subject shots refused: no rig at %s (pwsh tools/scene/build_rig.ps1)' % rig, [], []
    tiles, problems = [], []
    for kind, n, label in subjects:
        rec = next((p for p in saved if os.path.splitext(os.path.basename(p))[0] == label), None)
        if rec is None:
            problems.append('%s: the log names no saved record' % label)
            continue
        if not os.path.isabs(rec):
            rec = os.path.join(exe_dir, rec)
        if not os.path.exists(rec):
            problems.append('%s: record %s is not on disk' % (label, rec))
            continue
        prefix = os.path.join(run_dir, '%s_subject' % label)
        cmd = rig_command(rig, ['--record', rec, '--game', game, '--size', size, '--subject', '%s:%d' % (kind, n), '--no-family-png', '--prefix', prefix])
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        except (OSError, subprocess.TimeoutExpired) as e:
            problems.append('%s: the rig did not run (%s)' % (label, e))
            continue
        png = prefix + '_whole.png'
        if r.returncode != 0 or not os.path.exists(png):
            tail = (r.stdout + r.stderr).strip().splitlines()[-1:] or ['']
            problems.append('%s: rig exit %d %s' % (label, r.returncode, tail[0][:160]))
            continue
        miss = re.search(r'missing_draw_rows (\d+)', r.stdout)
        note = '%s %d: %s; missing draw rows %s%s' % (kind, n, LIMITS, miss.group(1) if miss else '?', stale)
        tiles.append((png, '%s (subject %s %d)' % (label, kind, n), note))
    return None, tiles, problems


def sheet_header(log, run, pid, notes):
    return ['run %03d pid %s  log %s' % (run, pid if pid is not None else '?', log)] + ['subject ' + n for n in notes]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', type=int, nargs='?')
    ap.add_argument('--log', help='the run\'s log file (its identity); the exe dir is the folder above its logs/')
    ap.add_argument('--exe-dir', default=None)
    ap.add_argument('--no-sheet', action='store_true')
    ap.add_argument('--max-tiles', type=int, default=24)
    ap.add_argument('--rig', default=os.path.join(REPO, 'out', 'tools', 'frame_rig.exe'))
    ap.add_argument('--game', default=None, help='the player\'s extracted files for the rig (default: game_data/extracted)')
    ap.add_argument('--size', default='1280x720', help='the subject pictures\' size')
    ap.add_argument('--allow-stale', action='store_true', help='draw subject shots with a rig built from another commit than the exe (said on the line)')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--plant', default=None, help='selftest only: no_commit_check | commit_only (S109 WARM: the rig judged by commit identity again: the src-fit row must FAIL)')
    a = ap.parse_args()
    if a.selftest:
        return selftest(a.plant)
    if a.log:
        log = os.path.abspath(a.log)
        m = re.match(r'wwe2k14_(\d+)\.log$', os.path.basename(log))
        if not m:
            print('checks: --log must name a wwe2k14_NNN.log, got %s' % log)
            return 0
        run = int(m.group(1))
        exe_dir = a.exe_dir or os.path.dirname(os.path.dirname(log))
    elif a.run is not None:
        run = a.run
        exe_dir = a.exe_dir or DEFAULT_EXE_DIR
        log = os.path.join(exe_dir, 'logs', 'wwe2k14_%03d.log' % run)
    else:
        ap.error('give the run number or --log PATH')
    a.exe_dir = exe_dir
    if not os.path.exists(log):
        print('checks: no log %s' % log)
        return 0
    with open(log, encoding='utf-8', errors='replace') as f:
        lines = f.readlines()
    c, shots, failure, verdicts = summarise(lines)
    pid, subjects, saved = scan_run(lines)
    a.run = run
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
    # S108 SHOT: the subject shots (the rig draws each saved record) go in the run's own folder captures/NNN_pPID/ (the sheet too, when the pid is known)
    run_dir = os.path.join(a.exe_dir, 'captures', '%03d_p%d' % (a.run, pid)) if pid is not None else os.path.join(a.exe_dir, 'captures')
    subj_tiles, notes = [], []
    if subjects:
        text += '; %d subject shot%s' % (len(subjects), '' if len(subjects) == 1 else 's')
        os.makedirs(run_dir, exist_ok=True)
        refusal, subj_tiles, problems = run_subject_shots(subjects, saved, a.exe_dir, run_dir, a.rig, a.game or find_game_dir(), a.size, a.allow_stale, os.environ.get('WWE_RUN_VERDICT_PLANT'))
        if refusal:
            text += ' (%s)' % refusal
        elif problems:
            text += ' (%s)' % '; '.join(problems)
        if subj_tiles:
            text += ', %d drawn by the rig (%s)' % (len(subj_tiles), LIMITS)
        notes = [n for _, _, n in subj_tiles]
        if any('STALE' in n for n in notes):
            text += '; STALE rig used (--allow-stale)'
        for mm in frame_mismatches(lines):   # S108 SHOT review: a subject picture from another frame than the game-camera shot next to it is said, on the line and in the sheet's header
            text += '; ' + mm
            notes.append(mm)
    all_tiles = []   # the game-camera shots in the log's order, each subject picture right after the shot of its label
    for s in shots:
        all_tiles.append(s)
        for png, name, _ in subj_tiles:
            if name.startswith(shot_label(s) + ' ('):
                all_tiles.append((png, name))
    all_tiles += [(png, name) for png, name, _ in subj_tiles if (png, name) not in all_tiles]
    if all_tiles:
        text += '; %d shot%s' % (len(shots), '' if len(shots) == 1 else 's')
        if len(all_tiles) == 1:
            first = all_tiles[0][0] if isinstance(all_tiles[0], tuple) else all_tiles[0]
            text += ', ' + os.path.relpath(first, a.exe_dir)
        elif not a.no_sheet:
            out = os.path.join(run_dir, '%03d__sheet.jpg' % a.run)
            os.makedirs(run_dir, exist_ok=True)   # S108: a run with no subject shot never made its folder (runs 32 and the MENU route runs lost their sheet)
            try:
                n = make_sheet(all_tiles, out, a.max_tiles, sheet_header(log, a.run, pid, notes))
                if n:
                    text += ', sheet %s%s' % (os.path.relpath(out, a.exe_dir),
                                              ' (%d of %d)' % (n, len(all_tiles)) if n < len(all_tiles) else '')
            except Exception as e:  # never break a run list over a sheet
                text += ' (sheet failed: %s)' % e
    print(text.replace('->', '>'))
    return 0


FAKE_RIG = '''import sys, os
a = sys.argv[1:]
prefix = a[a.index('--prefix') + 1]
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rig_called.txt'), 'a').write(' '.join(a) + '\\n')
from PIL import Image
Image.new('RGB', (64, 36), (10, 120, 200)).save(prefix + '_whole.png')
print('subject ' + a[a.index('--subject') + 1] + ' target 0,0,0 radius 1 forward 0,0,1 (eyelids) drawn_rows 1 light_refs 0 change_colour 0 missing_draw_rows 2 pre-pass_holes 0')
'''


def selftest(plant=None):
    """--selftest: the run's identity is its log, the sheet and the subject pictures land in the run's own folder with the log path and pid in the header, a rig built from another commit than the exe is
    refused (--plant no_commit_check: the refusal is off, so check 'refused' must FAIL), and a sheet never takes a path another run's log names."""
    from PIL import Image
    import io
    import contextlib
    fails = []

    def check(name, ok):
        print('%s %s' % ('PASS' if ok else 'FAIL', name))
        if not ok:
            fails.append(name)

    if plant:
        os.environ['WWE_RUN_VERDICT_PLANT'] = plant
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, 'exe')
        rigdir = os.path.join(td, 'rig')
        for d in ('logs', 'captures', 'stamp'):
            os.makedirs(os.path.join(exe, d))
        os.makedirs(rigdir)
        open(os.path.join(exe, 'stamp', 'wwe2k14_build_stamp.h'), 'w').write('#define WWE2K14_BUILD_GIT "aaaaaaaaaaaa"\n#define WWE2K14_BUILD_DIRTY 0\n')
        rig = os.path.join(rigdir, 'frame_rig.py')
        open(rig, 'w').write(FAKE_RIG)
        called = os.path.join(rigdir, 'rig_called.txt')
        run_dir = os.path.join(exe, 'captures', '042_p7777')
        os.makedirs(run_dir)
        for p in (os.path.join(exe, 'captures', '042_main.png'), os.path.join(exe, 'captures', '042_face0.png'), os.path.join(exe, 'captures', '043_other.png')):
            Image.new('RGB', (64, 36), (200, 30, 30)).save(p)
        open(os.path.join(run_dir, 'face0.rsc'), 'wb').write(b'x')

        def write_log(num, lines):
            p = os.path.join(exe, 'logs', 'wwe2k14_%03d.log' % num)
            open(p, 'w').write('\n'.join('[2026-11-22 10:00:00.000] [info] ' + l for l in lines) + '\n')
            return p

        cap = os.path.join(exe, 'captures')
        log42 = write_log(42, [
            '[wwe2k14] run log number: 042 (pid 7777)',
            '[wwe2k14][input] screenshot saved: %s (1280x720)' % os.path.join(cap, '042_main.png'),
            '[wwe2k14][input] subject shot: face 0 label face0',
            '[wwe2k14][input] screenshot saved: %s (1280x720) guest frame 9' % os.path.join(cap, '042_face0.png'),
            '[wwe2k14][input] scene record saved: %s (guest frame 5, host frame 9)' % os.path.join(run_dir, 'face0.rsc'),
            '[wwe2k14][input] subject shot: stage 114 label gone',
        ])
        write_log(43, ['[wwe2k14] run log number: 043 (pid 8888)', '[wwe2k14][input] screenshot saved: %s (1280x720)' % os.path.join(cap, '043_other.png')])
        pid, subjects, saved = scan_run(open(log42).read().splitlines())
        check('log: the pid and both subject shots and the saved record are read', pid == 7777 and subjects == [('face', 0, 'face0'), ('stage', 114, 'gone')] and len(saved) == 1)

        def run_main(args):
            buf = io.StringIO()
            old = sys.argv
            sys.argv = ['run_verdict.py'] + args
            try:
                with contextlib.redirect_stdout(buf):
                    main()
            finally:
                sys.argv = old
            return buf.getvalue().strip()

        base = ['--log', log42, '--rig', rig, '--game', td]
        # 1. a rig built from another commit than the exe is refused: the rig is not run
        open(os.path.join(rigdir, 'frame_rig.stamp'), 'w').write('git bbbbbbbbbbbb dirty 0')
        out = run_main(base)
        check('commit: a rig from another commit is refused', 'refused' in out and not os.path.exists(called))
        # 2. --allow-stale: drawn, and said
        out = run_main(base + ['--allow-stale'])
        check('commit: --allow-stale draws the subject and says STALE', os.path.exists(called) and 'STALE' in out)
        os.remove(called)
        # 3. the same commit: drawn, no refusal
        open(os.path.join(rigdir, 'frame_rig.stamp'), 'w').write('git aaaaaaaaaaaa dirty 0')
        out = run_main(base)
        args_called = open(called).read() if os.path.exists(called) else ''
        check('commit: the same commit runs the rig with --record and --subject face:0', '--subject face:0' in args_called and os.path.join(run_dir, 'face0.rsc') in args_called and 'refused' not in out)
        check('subject: the picture is in the run folder', os.path.exists(os.path.join(run_dir, 'face0_subject_whole.png')))
        check('subject: a shot whose record the log never saved is named, not drawn', 'gone: the log names no saved record' in out)
        sheet = os.path.join(run_dir, '042__sheet.jpg')
        check('sheet: written in the run folder captures/042_p7777/', os.path.exists(sheet) and 'captures%s042_p7777%s042__sheet.jpg' % (os.sep, os.sep) in out)
        with Image.open(sheet) as im:
            sheet_h = im.height
        hdr = sheet_header(log42, 42, 7777, ['face 0: x'])
        check('sheet: a header strip above the tiles (taller than one 480 px tile row), with the log path and the pid', sheet_h > 480 * 36 // 64 + 10 and log42 in hdr[0] and 'pid 7777' in hdr[0])
        with Image.open(sheet) as im:   # (review fix: the tile count is read off the sheet itself: 3 tiles of 480 px in one row = 1448 px; a fourth tile, 043's picture, makes it 1924)
            sheet_w = im.width
        check('sheet: only the paths this log names (3 tiles: the 042 shots and the subject; 043\'s picture exists and is not used)', os.path.exists(os.path.join(cap, '043_other.png')) and 'sheet' in out and sheet_w == 3 * 480 + 2 * 4)
        check('pairing: the subject\'s record and its screenshot are the same host frame (nothing said)', 'not the same frame' not in out)
        # 4. the number alone and --log name the same run; the sheet of one run never takes the other's
        out_log = run_main(base + ['--no-sheet'])
        out2 = run_main(['42', '--exe-dir', exe, '--no-sheet', '--rig', rig, '--game', td])
        check('identity: NNN + --exe-dir reads the same log (the same line as --log)', out2 == out_log and 'subject shot' in out2)
        # 5. a record saved from another frame than the screenshot of its label (the host saves the next DRAWN frame): said on the line and on the tile, never silent
        os.makedirs(os.path.join(exe, 'captures', '044_p9999'))
        Image.new('RGB', (64, 36), (200, 30, 30)).save(os.path.join(cap, '044_face0.png'))
        open(os.path.join(exe, 'captures', '044_p9999', 'face0.rsc'), 'wb').write(b'x')
        log44 = write_log(44, [
            '[wwe2k14] run log number: 044 (pid 9999)',
            '[wwe2k14][input] subject shot: face 0 label face0',
            '[wwe2k14][input] screenshot saved: %s (1280x720) guest frame 12' % os.path.join(cap, '044_face0.png'),
            '[wwe2k14][input] scene record saved: %s (guest frame 20, host frame 30)' % os.path.join(exe, 'captures', '044_p9999', 'face0.rsc'),
        ])
        out44 = run_main(['--log', log44, '--rig', rig, '--game', td])
        check('pairing: a record from another frame than its screenshot is said (record host frame 30, screenshot guest frame 12)', 'face0: not the same frame (record host frame 30, screenshot guest frame 12)' in out44)
        # 6. S109 WARM: the fit is judged by src content: a rig and an exe of different commits with the same src fit; a src change between them does not
        global GIT_REPO
        gr = os.path.join(td, 'git')
        os.makedirs(os.path.join(gr, 'src'))
        os.makedirs(os.path.join(gr, 'docs'))

        def gsh(*a):
            return subprocess.run(['git', '-C', gr, '-c', 'core.hooksPath=' + os.path.join(td, 'nohooks').replace('\\', '/')] + list(a), capture_output=True, text=True, check=True).stdout.strip()
        gsh('init', '-q')
        gsh('config', 'user.email', 'a@b')
        gsh('config', 'user.name', 'a')
        commits = []
        for path, text in (('src/a.cpp', 'v1'), ('docs/n.md', 'docs'), ('src/a.cpp', 'v2')):
            open(os.path.join(gr, path), 'w').write(text)
            gsh('add', '-A')
            gsh('commit', '-q', '-m', path)
            commits.append(gsh('rev-parse', '--short=12', 'HEAD'))
        saved_repo, GIT_REPO = GIT_REPO, gr
        try:
            open(os.path.join(exe, 'stamp', 'wwe2k14_build_stamp.h'), 'w').write('#define WWE2K14_BUILD_GIT "%s"\n#define WWE2K14_BUILD_DIRTY 0\n' % commits[1])
            open(os.path.join(rigdir, 'frame_rig.stamp'), 'w').write('git %s dirty 1' % commits[0])
            if os.path.exists(called):
                os.remove(called)
            out = run_main(base + ['--no-sheet'])
            check('src fit: rig at A, exe at B, only docs between: drawn, no refusal', os.path.exists(called) and 'refused' not in out)
            open(os.path.join(exe, 'stamp', 'wwe2k14_build_stamp.h'), 'w').write('#define WWE2K14_BUILD_GIT "%s"\n#define WWE2K14_BUILD_DIRTY 0\n' % commits[2])
            if os.path.exists(called):
                os.remove(called)
            out = run_main(base + ['--no-sheet'])
            check('src fit: rig at A, exe at C (src/a.cpp changed): refused, the file named, the rig not run', 'refused' in out and 'src/a.cpp' in out and not os.path.exists(called))
            open(os.path.join(rigdir, 'frame_rig.stamp'), 'w').write('git cccccccccccc dirty 0')
            out = run_main(base + ['--no-sheet'])
            check('src fit: a rig commit this repository does not know: refused', 'refused' in out and 'not known' in out)
        finally:
            GIT_REPO = saved_repo
    if fails:
        print('run_verdict selftest FAIL: %s' % '; '.join(fails))
        return 1
    print('run_verdict selftest PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
