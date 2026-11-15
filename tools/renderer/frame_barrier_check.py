r"""S102 lane D1 round 6 (docs\renderer\DEVICE_LOST.md section 13): wwe2k14_rdr_frame_top_barrier proved offline with the replay player. 0 game runs.
  py -I tools\renderer\frame_barrier_check.py [--root DIR] [--no-window]
Arms (rplay, --validation 1, offscreen present, --role-audit so the barrier is seen in the stream):
  Bryan entrance (out\lane86\bryan\capture, 91 frames): off, 1 (top of every frame), 2 (+ before the present), plant (switch 1, the barrier not recorded).
    off: no barrier line; 1: a "top" barrier in every frame; 2: a "top" and a "present" barrier in every frame; plant: none (must be caught as missing);
    the picture byte-equal across off / 1 / 2, 0 validation messages, exit code 0.
  The loss window (out\captures\brock_losswin_rt, 320 frames, the shipped bundle + --game): off vs 1, byte-equal pictures, 0 validation messages, a barrier in
    every frame. (--no-window skips it; the gate runs it.)
Exit 1 on any failure."""
import argparse, hashlib, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--ring-ours', '--flash-ours', '--props-ours', '--particles-ours', '--ui-ours', '--ui-text-ours', '--post-ours',
          '--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--rim-ours', '--rim-chain-ours', '--small-passes-ours', '--circles-ours', '--caw-ours', '--movie-ours', '--screens-ours']


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def run(exe, capture, frames, extra, tag, tmp):
    png, audit = os.path.join(tmp, tag + '.png'), os.path.join(tmp, tag + '.jsonl')
    cmd = [exe, '--capture', capture, '--frames', str(frames), '--validation', '1', '--present-size', '1280x720', '--role-audit', audit, '--out', png] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500)
    text = p.stdout + p.stderr
    rows = [json.loads(l) for l in open(audit, encoding='utf-8') if l.strip()] if os.path.exists(audit) else []
    frames_seen = {r['f'] for r in rows if r['k'] != 'frame_barrier'}
    bars = [r for r in rows if r['k'] == 'frame_barrier']
    d = {'rc': p.returncode, 'validation': len(re.findall(r'\[rdr\]\[validation\]', text)), 'frames': frames_seen,
         'top': {r['f'] for r in bars if r['w'] == 'top'}, 'present': {r['f'] for r in bars if r['w'] in ('present', 'end')},
         'png': hashlib.sha1(open(png, 'rb').read()).hexdigest() if os.path.exists(png) else None, 'rows': len(rows)}
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--no-window', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    mc = main_checkout(root)
    bryan = os.path.join(mc, 'out', 'lane86', 'bryan', 'capture')
    window = os.path.join(mc, 'out', 'captures', 'brock_losswin_rt')
    game = os.path.join(mc, 'game_data', 'extracted')
    for path in (exe, bryan):
        if not os.path.exists(path):
            say(False, 'missing %s' % path)
            return 1
    with tempfile.TemporaryDirectory(prefix='d1_fb_') as tmp:
        arms = {'off': [], 'b1': ['--frame-top-barrier', '1'], 'b2': ['--frame-top-barrier', '2'], 'plant': ['--frame-top-barrier', '1', '--frame-barrier-plant', '1']}
        r = {k: run(exe, bryan, 91, v, 'bryan_' + k, tmp) for k, v in arms.items()}
        for k, d in r.items():
            say(d['rc'] == 0 and d['validation'] == 0 and d['rows'] > 0, 'Bryan %s: exit code %d, %d validation messages, %d audit lines' % (k, d['rc'], d['validation'], d['rows']))
        say(not r['off']['top'] and not r['off']['present'], 'Bryan off: no barrier recorded (off = the stream as before)')
        say(r['b1']['top'] == r['b1']['frames'] and not r['b1']['present'], 'Bryan 1: a top barrier in every one of %d frames, none before the present' % len(r['b1']['frames']))
        say(r['b2']['top'] == r['b2']['frames'] and r['b2']['present'] == r['b2']['frames'], 'Bryan 2: a top barrier and a present barrier in every one of %d frames' % len(r['b2']['frames']))
        say(r['plant']['top'] != r['plant']['frames'], 'plant (switch on, barrier dropped) is caught: %d of %d frames carry the top barrier' % (len(r['plant']['top']), len(r['plant']['frames'])))
        say(r['off']['png'] is not None and r['off']['png'] == r['b1']['png'] == r['b2']['png'], 'Bryan: the picture is byte-equal off / 1 / 2 (%s)' % (r['off']['png'] or '-')[:12])
        if not a.no_window:
            if not (os.path.isdir(window) and os.path.isdir(game)):
                say(False, 'missing %s or %s' % (window, game))
            else:
                w = {k: run(exe, window, 320, ['--game', game] + BUNDLE + v, 'win_' + k, tmp) for k, v in (('off', []), ('b1', ['--frame-top-barrier', '1']))}
                for k, d in w.items():
                    say(d['rc'] == 0 and d['validation'] == 0, 'loss window %s: exit code %d, %d validation messages' % (k, d['rc'], d['validation']))
                say(w['b1']['top'] == w['b1']['frames'] and len(w['b1']['frames']) >= 300, 'loss window 1: a top barrier in every one of %d frames' % len(w['b1']['frames']))
                say(w['off']['png'] is not None and w['off']['png'] == w['b1']['png'], 'loss window: the picture is byte-equal off / 1 (%s)' % (w['off']['png'] or '-')[:12])
    print('frame_barrier_check: %s (%d failure%s)' % ('PASS' if not FAILS else 'FAIL', len(FAILS), '' if len(FAILS) == 1 else 's'), flush=True)
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
