"""Verification helper for the inspector (Session 8 task 15, src/inspector.h): waits for a NEW game log that contains
the inspector's "panel settings drawn" line, puts the game window on top, then grabs one desktop frame of the game's
client area with ffmpeg (ddagrab, then gdigrab). The in-app screenshot holds only the guest image, so this is how the
overlay itself is captured. Output (game imagery) goes only to out/harness/inspector/<preset>_<run>_inspector_*.png.
Launches no game: start it first (background), then the run (tools/input_scripts/inspector_selftest.txt header).

    py tools\\inspector\\snap.py [timeout_s] [delay_s] [preset, default win-amd64-release]
"""
import os
import subprocess
import sys
import time

REPO = r'C:/Users/Jarrod/wwe2k14-recomp'
LOGS = os.path.join(REPO, 'out', 'build', sys.argv[3] if len(sys.argv) > 3 else 'win-amd64-release', 'logs')
OUT = os.path.join(REPO, 'out', 'harness', 'inspector')
sys.path.insert(0, os.path.join(REPO, 'tools', 'review'))
import winrect  # noqa: E402

timeout = float(sys.argv[1]) if len(sys.argv) > 1 else 1800
delay = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
os.makedirs(OUT, exist_ok=True)
before = set(n for n in os.listdir(LOGS) if n.endswith('.log'))
t0 = time.time()
log = None
while time.time() - t0 < timeout:
    for n in sorted(os.listdir(LOGS)):
        if not (n.startswith('wwe2k14_') and n.endswith('.log')) or n in before:
            continue
        try:
            with open(os.path.join(LOGS, n), encoding='utf-8', errors='replace') as f:
                if '[wwe2k14][inspector] panel settings drawn' in f.read():
                    log = n
        except OSError:
            pass
    if log:
        break
    time.sleep(1.0)
if not log:
    print('no inspector log line within timeout')
    sys.exit(1)
num = log[len('wwe2k14_'):-len('.log')]
print('log', log, flush=True)
hwnd, title = winrect.find_window()
if not hwnd:
    print('game window not found')
    sys.exit(1)
print('window', title, 'topmost', winrect.make_topmost(hwnd), flush=True)
time.sleep(delay)
rect = winrect.client_rect(hwnd)
dw, dh = winrect.desktop_size()
x, y = max(0, rect['x']), max(0, rect['y'])
w, h = min(rect['w'], dw - x), min(rect['h'], dh - y)
w -= w % 2
h -= h % 2
print('rect', x, y, w, h, 'desktop', dw, dh, flush=True)
results = []
for cfg in ('ddagrab', 'gdigrab'):
    out = os.path.join(OUT, f'{os.path.basename(os.path.dirname(LOGS))}_{num}_inspector_{cfg}.png')
    if cfg == 'ddagrab':
        cmd = ['ffmpeg', '-hide_banner', '-nostdin', '-y', '-f', 'lavfi', '-i',
               f'ddagrab=output_idx=0:framerate=30:draw_mouse=0:offset_x={x}:offset_y={y}:video_size={w}x{h}',
               '-vf', 'hwdownload,format=bgra', '-frames:v', '1', out]
    else:
        cmd = ['ffmpeg', '-hide_banner', '-nostdin', '-y', '-f', 'gdigrab', '-framerate', '30', '-draw_mouse', '0',
               '-offset_x', str(x), '-offset_y', str(y), '-video_size', f'{w}x{h}', '-i', 'desktop',
               '-frames:v', '1', out]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=40)
        rc, err = r.returncode, r.stderr[-400:]
    except subprocess.TimeoutExpired:
        rc, err = -1, 'timed out (no desktop frames?)'
    stat = ''
    if rc == 0 and os.path.exists(out):
        try:
            from PIL import Image, ImageStat
            im = Image.open(out).convert('RGB')
            st = ImageStat.Stat(im)
            stat = f'size {im.size} mean {[round(m, 1) for m in st.mean]} stddev {[round(s, 1) for s in st.stddev]}'
        except Exception as e:  # noqa: BLE001
            stat = f'stat failed: {e}'
    print(cfg, 'rc', rc, out if rc == 0 else err, stat, flush=True)
    results.append((cfg, rc))
    if rc == 0:
        break
sys.exit(0 if any(rc == 0 for _, rc in results) else 2)
