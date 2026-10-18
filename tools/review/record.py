"""Record review clips + native stills for a batch of (route, arm) jobs (Session 8, task 4).

    py tools\\review\\record.py --batch batch1 --jobs bryan:default,bryan:1440p,bryan:4k
    py tools\\review\\record.py --batch batch1 --jobs caw:default,caw:4k
    py tools\\review\\record.py --capture-test          desktop capture self-test only (no game)

Takes the run lock once for all jobs (tools/harness/runlock.py; exclusive), then per job:
  1. writes the route variant (make_routes.py; entrance waits scaled to the arm's entrance fps), or uses a fixed
     route from tools/review/routes/;
  2. launches it through tools/bench/run_list.ps1 -Exclusive (frozen user data mirrored, timeline on, kill after
     720 s);
  3. in-game capture (default, --capture ingame): passes --wwe2k14_clip_dir=<job folder>, so the route's
     clip_start / clip_stop lines make the game record its own output at the internal resolution
     (src/clip_capture.h: NNN_clip_NAME.mp4 + .json with its frame counts, renamed here to clip_NAME.*); works with
     the display asleep. capture_ok needs a clean run_list line (no 'script FAILED' / crash / triage) and a
     finished clip for every clip_start in the route file; clips that missed > 5% of the game's frames are listed
     in capture_degraded (still OK, flagged on the page);
     Desktop fallback (--capture auto|ddagrab|gdigrab): tails the game log and on "review: clip_start NAME" starts
     an ffmpeg desktop capture of the game's client area (ddagrab -> h264_nvenc, 60 fps), stops it on
     "review: clip_stop NAME";
  4. copies the "screenshot rv_*" stills (native internal resolution) as PNG.
Output (gitignored, game-derived): out/harness/review/<batch>/runs/<route>__<arm>/ with clip_*.mp4, still_*.png,
job.json (run number, run_list line, clip timing, capture latency, ffmpeg stats and CPU).
"""
import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'harness'))
sys.path.insert(0, HERE)
import make_routes  # noqa: E402
import winrect  # noqa: E402

EXE_DIR = os.path.join(REPO, 'out', 'build', 'win-amd64-release')
LOGS = os.path.join(EXE_DIR, 'logs')
RUN_LIST = os.path.join(REPO, 'tools', 'bench', 'run_list.ps1')
REVIEW = os.path.join(REPO, 'out', 'harness', 'review')

ARMS = {
    'default': {'flags': '--wwe2k14_preset_1440p60=false', 'entrance_fps': 30,  # the original look (Session 8: 1440p60 is now the app default)
                'label': 'Default (4x MSAA, 1280x720, 30 fps entrances)'},
    '1440p': {'flags': '--wwe2k14_preset_1440p60=true', 'entrance_fps': 60,
              'label': '1440p preset (no MSAA, 2560x1440, 60 fps entrances)'},
    '4k': {'flags': '--wwe2k14_preset_4k60=true', 'entrance_fps': 60,
           'label': '4K preset (no MSAA, 3840x2160, 60 fps entrances)'},
    # Cheaper in-game capture at the 4K preset (run 395: 4K60 clips cost the heavy Undertaker entrance 57.9 vs
    # 59.9 fps): clips at 30 fps, or at half size (1920x1080) and 60 fps.
    '4k30': {'flags': '--wwe2k14_preset_4k60=true', 'entrance_fps': 60, 'clip_args': '30 1',
             'label': '4K preset, in-game clips at 30 fps (3840x2160)'},
    '4khalf': {'flags': '--wwe2k14_preset_4k60=true', 'entrance_fps': 60, 'clip_args': '60 0.5',
               'label': '4K preset, in-game clips at half size (1920x1080, 60 fps)'},
}
FIXED_ROUTES = {'caw': os.path.join(HERE, 'routes', 'review_caw.txt')}

# Capture configurations, tried in order by the self-test. ddagrab = Desktop Duplication (sees the composed
# desktop, including a Vulkan swapchain), frames stay on the GPU and go straight to NVENC.
ENC = ['-c:v', 'h264_nvenc', '-preset', 'p5', '-tune', 'hq', '-rc', 'vbr', '-cq', '18', '-b:v', '0',
       '-g', '60', '-bf', '0']
# In-game capture (the default, Session 8 phase 2 task t1): the route's clip_start / clip_stop lines record the
# guest output inside the game (src/clip_capture.h) at the internal resolution into the job folder; the game
# starts ffmpeg itself (this path, so it does not depend on the game's PATH).
FFMPEG = shutil.which('ffmpeg') or 'ffmpeg'
# A clip that missed more than this share of the game's frames (no free capture thread, encoder backlog, failed
# captures) is marked degraded in job.json (capture_degraded) and on the review page: its motion is the
# capture's, not the game's (run 395's Undertaker 4K match: 199 of 900 frames dropped by the backlog).
DEGRADED_MISSED = 0.05
# run_list line parts that mean the run did not go as scripted (its failure / triage columns, tools/bench/run_list.ps1)
RUN_FAIL_MARKS = ('script FAILED', '][crash]', 'device lost', 'Failed to await', 'MISMATCH', '[triage]',
                  'STILL RUNNING', 'KILLED', 'ABORT')


def run_line_clean(line):
    """True when run_list's line says the run exited with code 0 and has nothing in its failure columns (a route
    whose wait_file timed out quits with exit code 0 under on_fail quit, with 'script FAILED' in the line)."""
    return bool(line) and 'exit code 0' in line and not any(m in line for m in RUN_FAIL_MARKS)


def route_clip_names(path):
    """NAMEs of the route's clip_start lines (comments stripped), in order: the clips the run must produce."""
    names = []
    with open(path, encoding='utf-8', errors='replace') as rf:
        for line in rf:
            w = line.split('#', 1)[0].split()
            if len(w) >= 2 and w[0].lower() == 'clip_start' and w[1] not in names:
                names.append(w[1])
    return names


def extra_safe(value):
    """-Extra is one string that run_and_triage splits on spaces (Start-Process -ArgumentList), inside a
    pwsh -Command '...' literal: a path with a space or a quote cannot be passed through it."""
    return not any(ch in value for ch in ' "\'`')


def clip_missed(g):
    """(missed, wanted) of an in-game clip's json: frames the clip wanted (capture_requests: one per clip slot; older
    jsons: every game frame) and how many of them it lacks."""
    missed = (g.get('dropped_busy') or 0) + (g.get('dropped_backlog') or 0) + (g.get('capture_failed') or 0)
    return missed, g.get('capture_requests') or g.get('triggers') or 0


def capture_cmd(cfg, rect, out, seconds=None):
    x, y, w, h = rect['x'], rect['y'], rect['w'], rect['h']
    w -= w % 2
    h -= h % 2
    dur = ['-t', str(seconds)] if seconds else []
    if cfg == 'ddagrab':
        src = ['-f', 'lavfi', '-i',
               f'ddagrab=output_idx=0:framerate=60:draw_mouse=0:offset_x={x}:offset_y={y}:video_size={w}x{h}']
        return ['ffmpeg', '-hide_banner'] + (['-nostdin'] if seconds else []) + ['-y'] + src + dur + ENC + [out]
    if cfg == 'gdigrab':
        src = ['-f', 'gdigrab', '-framerate', '60', '-draw_mouse', '0', '-offset_x', str(x), '-offset_y', str(y),
               '-video_size', f'{w}x{h}', '-i', 'desktop']
        return ['ffmpeg', '-hide_banner'] + (['-nostdin'] if seconds else []) + ['-y'] + src + dur + \
            ['-pix_fmt', 'yuv420p'] + ENC + [out]
    raise ValueError(cfg)


def probe(path):
    try:
        r = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames', '-show_entries',
                            'stream=width,height,nb_read_frames,pix_fmt,codec_name:format=duration', '-of', 'json',
                            path], capture_output=True, text=True, timeout=120)
        j = json.loads(r.stdout or '{}')
        s = (j.get('streams') or [{}])[0]
        return {'w': s.get('width'), 'h': s.get('height'), 'frames': int(s.get('nb_read_frames') or 0),
                'pix_fmt': s.get('pix_fmt'), 'codec': s.get('codec_name'),
                'duration': float((j.get('format') or {}).get('duration') or 0)}
    except Exception as e:  # noqa: BLE001
        return {'error': str(e), 'frames': 0}


def capture_test(outdir):
    """Record 2 s of the desktop with each config; return the first that works (+ results)."""
    os.makedirs(outdir, exist_ok=True)
    info = winrect.window_info()
    dw, dh = info['desktop']
    rect = {'x': 0, 'y': 0, 'w': dw, 'h': dh}
    results = {}
    for cfg in ('ddagrab', 'gdigrab'):
        out = os.path.join(outdir, f'capture_test_{cfg}.mp4')
        t0 = time.time()
        try:
            r = subprocess.run(capture_cmd(cfg, rect, out, seconds=2), capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            # seen at 03:52 (Session 8): ddagrab never delivered a frame (display asleep / locked desktop?);
            # subprocess.run kills our ffmpeg on timeout
            r = subprocess.CompletedProcess([], -1, '', 'timed out after 60 s (no desktop frames?)')
        p = probe(out) if os.path.exists(out) else {'frames': 0}
        if os.path.exists(out):
            os.remove(out)  # it shows whatever is on the desktop (no game runs during the test): don't keep it
        results[cfg] = {'rc': r.returncode, 'wall_s': round(time.time() - t0, 2), 'probe': p,
                        'stderr_tail': (r.stderr or '')[-600:]}
        print(f'capture test {cfg}: rc {r.returncode}, {p}', flush=True)
        if r.returncode == 0 and p.get('frames', 0) >= 60:
            return cfg, results
    return None, results


class Clip:
    def __init__(self, name, cfg, rect, out, log_ts):
        self.name, self.out, self.log_ts = name, out, log_ts
        self.cmd = capture_cmd(cfg, rect, out)
        self.spawn = time.time()
        self.stderr_path = out + '.ffmpeg.txt'
        self.err = open(self.stderr_path, 'w', encoding='utf-8')
        self.p = subprocess.Popen(self.cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=self.err)
        self.stop_log_ts = None
        self.cpu_s = None

    def stop(self, log_ts=None):
        self.stop_log_ts = log_ts
        try:
            import psutil
            pp = psutil.Process(self.p.pid)
            t = pp.cpu_times()
            self.cpu_s = round(t.user + t.system, 2)
        except Exception:  # noqa: BLE001
            pass
        try:
            self.p.stdin.write(b'q')
            self.p.stdin.flush()
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.p.terminate()  # our own ffmpeg only
            self.p.wait(timeout=10)
        self.err.close()

    def info(self):
        tail = ''
        try:
            with open(self.stderr_path, encoding='utf-8', errors='replace') as f:
                tail = f.read()[-800:]
        except OSError:
            pass
        m = re.findall(r'frame=\s*(\d+).*?dup=(\d+)\s+drop=(\d+)', tail)
        d = {'name': self.name, 'file': os.path.basename(self.out), 'spawn_wall': self.spawn,
             'log_start': self.log_ts, 'log_stop': self.stop_log_ts, 'ffmpeg_rc': self.p.returncode,
             'ffmpeg_cpu_s': self.cpu_s, 'ffmpeg_tail': tail[-300:]}
        if self.log_ts:
            d['start_latency_s'] = round(self.spawn - self.log_ts, 3)  # log line -> ffmpeg spawned
        if self.log_ts and self.stop_log_ts:
            d['log_span_s'] = round(self.stop_log_ts - self.log_ts, 3)  # compare with the clip's duration
        if m:
            d['ffmpeg_frames'], d['ffmpeg_dup'], d['ffmpeg_drop'] = (int(v) for v in m[-1])
        return d


LINE_TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')


def line_time(line):
    m = LINE_TS.match(line)
    if not m:
        return None
    return dt.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f').timestamp()


def route_for(route, arm):
    if route in FIXED_ROUTES:
        path = FIXED_ROUTES[route]
    else:
        # optional per-arm in-game clip arguments ("FPS SCALE", e.g. '30 1'); default make_routes.CLIP_ARGS
        ca = ARMS[arm].get('clip_args')
        path = make_routes.write(route, ARMS[arm]['entrance_fps'],
                                 make_routes.default_path(route, ARMS[arm]['entrance_fps'], ca), ca)
    return path, os.path.relpath(path, EXE_DIR)


def run_job(batch, route, arm, cfg):
    # Record into <job>.new and swap it in only when the new recording is OK (or there was none), so a --force
    # re-record or retry that crashes does not destroy a good recording.
    final_dir = os.path.join(REVIEW, batch, 'runs', f'{route}__{arm}')
    jobdir = final_dir + '.new'
    if os.path.isdir(jobdir):
        shutil.rmtree(jobdir)  # a previous unfinished attempt (our own output)
    os.makedirs(jobdir)
    path, rel = route_for(route, arm)
    import hashlib
    with open(path, 'rb') as rf:
        route_sha = hashlib.sha256(rf.read()).hexdigest()
    flags = ARMS[arm]['flags']
    ingame = cfg == 'ingame'
    # In-game clips land straight in the job folder. The run is exclusive (-Exclusive): clips are judged for
    # smoothness and the per-clip game fps is reported, so no other game may run alongside.
    if ingame:
        for what, v in (('ffmpeg path', FFMPEG), ('job folder', jobdir)):
            if not extra_safe(v):
                raise SystemExit(f'{what} {v!r} contains a space or quote, which run_list -Extra cannot pass; put '
                                 'ffmpeg (and the repo) in a folder without spaces, or use --capture auto')
    run_flags = (flags + f' --wwe2k14_clip_dir={jobdir} --wwe2k14_clip_ffmpeg={FFMPEG}').strip() if ingame else flags
    before = set(os.listdir(LOGS))
    cmd = ['pwsh', '-NoProfile', '-Command',
           f"& '{RUN_LIST}' -Scripts @('{rel}') -Extra '{run_flags}' -Exclusive"]
    print(f'[{time.strftime("%H:%M:%S")}] job {route}:{arm}: {rel} {run_flags} (capture {cfg})', flush=True)
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out_lines = []
    reader = threading.Thread(target=lambda: out_lines.extend(p.stdout), daemon=True)
    reader.start()
    log_path, f, clips, active, stills, buf = None, None, [], {}, [], ''
    marks = {}  # in-game mode: clip name -> log times of the route's "review: clip_start/stop" lines
    rect = None
    topmost_done, topmost_try = False, 0.0
    gone_try, gone_since, window_lost = 0.0, None, []
    while True:
        alive = p.poll() is None
        if log_path is None:
            new = [n for n in os.listdir(LOGS) if n.startswith('wwe2k14_') and n.endswith('.log') and n not in before]
            # Other agents' games may run at the same time (shared run-lock mode): our log is the new one whose
            # settings lines name our route file (and, in-game, our job folder), not simply the newest.
            for n in sorted(new, reverse=True):
                try:
                    with open(os.path.join(LOGS, n), encoding='utf-8', errors='replace') as lf:
                        head = lf.read(400000)
                except OSError:
                    continue
                if os.path.basename(path) in head and (not ingame or jobdir in head):
                    log_path = os.path.join(LOGS, n)
                    break
            if log_path:
                f = open(log_path, encoding='utf-8', errors='replace')
                print(f'  log {os.path.basename(log_path)}', flush=True)
        if f and not ingame and not topmost_done and time.time() - topmost_try > 0.5:
            topmost_try = time.time()
            hwnd, _ = winrect.find_window()
            if hwnd:
                topmost_done = winrect.make_topmost(hwnd)
        if f:
            chunk = f.read()
            if chunk:
                buf += chunk
                *lines, buf = buf.split('\n')
                for line in lines:
                    if ingame:  # the game records the clips itself; keep the log times of the route's anchors
                        m = re.search(r'review: clip_(start|stop) (\S+)', line)
                        if m:
                            marks.setdefault(m.group(2), {})['log_' + m.group(1)] = line_time(line)
                            print(f'  {m.group(0)}', flush=True)
                        m = re.search(r'\[clip\] done (\S+): (.*)', line)
                        if m:
                            print(f'  in-game clip {m.group(1)}: {m.group(2)[:240]}', flush=True)
                    m = None if ingame else re.search(r'review: clip_start (\S+)', line)
                    if m and m.group(1) not in active:
                        hwnd, _ = winrect.find_window()
                        if hwnd:
                            winrect.make_topmost(hwnd)  # again, in case another window came up in front
                        if rect is None:
                            wi = winrect.window_info()
                            rect = wi.get('client') or {'x': 0, 'y': 0, 'w': wi['desktop'][0], 'h': wi['desktop'][1]}
                            dw, dh = wi['desktop']
                            # clamp to the desktop (ddagrab can't capture outside the output)
                            rect = {'x': max(0, rect['x']), 'y': max(0, rect['y']),
                                    'w': min(rect['w'], dw - max(0, rect['x'])),
                                    'h': min(rect['h'], dh - max(0, rect['y']))}
                        c = Clip(m.group(1), cfg, rect, os.path.join(jobdir, f'clip_{m.group(1)}.mp4'),
                                 line_time(line))
                        active[m.group(1)] = c
                        clips.append(c)
                        print(f'  clip_start {m.group(1)} (latency {c.spawn - (c.log_ts or c.spawn):.2f} s)',
                              flush=True)
                    m = None if ingame else re.search(r'review: clip_stop (\S+)', line)
                    if m and m.group(1) in active:
                        active.pop(m.group(1)).stop(line_time(line))
                        print(f'  clip_stop {m.group(1)}', flush=True)
                    m = re.search(r'screenshot saved: (.+\\(\d+)_(rv_[^\\]+)\.(?:bmp|png)) \((\d+)x(\d+)\)', line)
                    if m:
                        stills.append({'bmp': m.group(1), 'label': m.group(3), 'w': int(m.group(4)),
                                       'h': int(m.group(5))})
                continue
        # A clip still running when the game window is gone (crash, early quit) would go on recording the desktop
        # until run_list exits: stop it at once and mark it.
        if active and time.time() - gone_try > 0.5:
            gone_try = time.time()
            if winrect.find_window()[0]:
                gone_since = None
            elif gone_since is None:
                gone_since = time.time()
            elif time.time() - gone_since > 1.0:
                for name in list(active):
                    active.pop(name).stop(None)
                    window_lost.append(name)
                    print(f'  game window gone: stopped clip {name}', flush=True)
        if not alive:
            break
        time.sleep(0.02)
    for c in list(active.values()):
        c.stop(None)
    reader.join(timeout=10)
    if f:
        f.close()
    result = [l.rstrip() for l in out_lines if '->' in l and 'wwe2k14_' in l]
    run_no = None
    if log_path:
        run_no = int(re.search(r'wwe2k14_(\d+)\.log', log_path).group(1))
    from PIL import Image
    for s in stills:
        dst = os.path.join(jobdir, f"still_{s['label']}.png")
        try:
            Image.open(s['bmp']).convert('RGB').save(dst, optimize=False, compress_level=3)
            s['png'] = os.path.basename(dst)
        except OSError as e:
            s['error'] = str(e)
    if ingame:
        # The game wrote NNN_clip_NAME.mp4 + .json (its own counts) + .mp4.ffmpeg.txt into the job folder
        # (src/clip_capture.h; NNN = the run's log number, so runs sharing a folder never collide). The job folder
        # belongs to this job alone: drop the prefix, so the files are clip_NAME.* as compose.py expects.
        pre = f'{run_no:03d}_clip_' if run_no is not None else None
        for n in sorted(os.listdir(jobdir)):
            m = re.match(r'(\d+)_clip_(.+)$', n)
            if m and (pre is None or n.startswith(pre)):
                os.replace(os.path.join(jobdir, n), os.path.join(jobdir, 'clip_' + m.group(2)))
        # Every clip the route starts must exist, not only those whose anchor reached the log (a route that failed
        # after its first clip would otherwise pass with the second one missing).
        expected = route_clip_names(path)
        for name in expected:
            marks.setdefault(name, {})
        clip_infos = []
        for name in marks:
            mp4 = os.path.join(jobdir, f'clip_{name}.mp4')
            info = {'name': name, 'file': os.path.basename(mp4), **marks[name]}
            try:
                with open(os.path.join(jobdir, f'clip_{name}.json'), encoding='utf-8') as cj:
                    info['ingame'] = json.load(cj)
            except (OSError, ValueError) as e:
                info['ingame_error'] = str(e)
            info['probe'] = probe(mp4) if os.path.exists(mp4) else {'frames': 0, 'error': 'no file'}
            if info.get('log_start') and info.get('log_stop'):
                info['log_span_s'] = round(info['log_stop'] - info['log_start'], 3)
            missed, trig = clip_missed(info.get('ingame') or {})
            if trig and missed / trig > DEGRADED_MISSED:
                info['degraded'] = (f'missed {missed} of {trig} game frames ({100.0 * missed / trig:.1f}% > '
                                    f'{100 * DEGRADED_MISSED:g}%)')
            clip_infos.append(info)
    else:
        clip_infos = [dict(c.info(), probe=probe(c.out)) for c in clips]
    job = {'batch': batch, 'route': route, 'arm': arm, 'flags': flags, 'run_flags': run_flags,
           'label': ARMS[arm]['label'],
           'route_file': os.path.relpath(path, REPO), 'route_sha256': route_sha, 'run': run_no, 'run_list': result,
           'wall_s': round(time.time() - t0, 1), 'capture': cfg, 'capture_rect': rect,
           'clips': clip_infos, 'stills': stills,
           'game_window_topmost': topmost_done, 'clips_stopped_window_gone': window_lost}
    if ingame:
        # Usable = the run exited cleanly with nothing in run_list's failure columns, every clip_start of the route
        # file produced a clip the game finished (its json written, ffmpeg exit 0, not abandoned) with frames, and
        # stills were saved. A clip that missed many game frames stays usable but is marked degraded (the page
        # flags it; re-record with --force or a cheaper arm such as 4k30).
        job['clips_expected'] = expected
        job['capture_ok'] = bool(result and run_line_clean(result[-1]) and job['clips']
                                 and all(c.get('ingame', {}).get('ffmpeg_rc') == 0 and
                                         not c.get('ingame', {}).get('write_failed') and
                                         not c.get('ingame', {}).get('abandoned') and
                                         c['probe'].get('frames', 0) > 0 for c in job['clips'])
                                 and any('png' in s for s in stills))
        job['capture_degraded'] = [f"{c['name']}: {c['degraded']}" for c in job['clips'] if c.get('degraded')]
    else:
        # Usable = the run exited cleanly, the game window was put in front before any clip (run 186's clips showed
        # the app that was in front instead), every clip has frames, and stills were saved.
        job['capture_ok'] = bool(result and 'exit code 0' in result[-1] and (topmost_done or not clips)
                                 and not window_lost
                                 and all(c['probe'].get('frames', 0) > 0 for c in job['clips'])
                                 and any('png' in s for s in stills))
    with open(os.path.join(jobdir, 'job.json'), 'w', encoding='utf-8') as jf:
        json.dump(job, jf, indent=1)
    if job['capture_ok'] or not job_ok(batch, route, arm):
        if os.path.isdir(final_dir):
            shutil.rmtree(final_dir)
        os.replace(jobdir, final_dir)
    else:
        print(f'  new recording not OK: kept the previous good one; this attempt is in {jobdir}', flush=True)
    print(f'  -> run {run_no}: {result[-1] if result else "(no run_list line)"}', flush=True)
    for c in job['clips']:
        if ingame:
            g = c.get('ingame', {})
            print(f"  clip {c['name']}: {c['probe']}; in-game: {g.get('video_frames')} frames, triggers "
                  f"{g.get('triggers')}, captured {g.get('captured')}, repeated {g.get('repeated')}, dropped_busy "
                  f"{g.get('dropped_busy')}, dropped_backlog {g.get('dropped_backlog')}, ffmpeg rc {g.get('ffmpeg_rc')}",
                  flush=True)
            continue
        print(f"  clip {c['name']}: {c['probe']} latency {c.get('start_latency_s')} cpu {c.get('ffmpeg_cpu_s')}",
              flush=True)
    print(f'  stills: {len([s for s in stills if "png" in s])}', flush=True)
    return job


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--batch', default='batch1')
    ap.add_argument('--jobs', default='', help='route:arm,route:arm,... (routes: bryan taker caw; arms: '
                    + ' '.join(ARMS) + ')')
    ap.add_argument('--capture', choices=['ingame', 'auto', 'ddagrab', 'gdigrab'], default='ingame',
                    help='ingame (default): the game records its own output (src/clip_capture.h); auto / ddagrab / '
                    'gdigrab: desktop capture (fallback; needs the display awake)')
    ap.add_argument('--capture-test', action='store_true')
    ap.add_argument('--force', action='store_true', help='re-record jobs that are already recorded OK')
    a = ap.parse_args()
    jobs = [tuple(j.split(':')) for j in a.jobs.split(',') if j]
    for r, arm in jobs:
        if arm not in ARMS or (r not in make_routes.BASES and r not in FIXED_ROUTES):
            ap.error(f'unknown job {r}:{arm}')
    todo = os.path.join(REVIEW, a.batch, 'todo.txt')
    if not a.capture_test and not a.force and not os.path.exists(todo) and jobs \
            and all(job_ok(a.batch, r, arm) for r, arm in jobs):
        print('all jobs already recorded OK (--force to redo); not queueing for the lock', flush=True)
        return
    from runlock import ensure_lock
    ensure_lock('review ' + a.jobs)  # re-execs this script under the run lock unless we already hold it
    os.makedirs(os.path.join(REVIEW, a.batch), exist_ok=True)
    cfg = a.capture
    if cfg == 'auto' or a.capture_test:
        cfg, results = capture_test(os.path.join(REVIEW, a.batch, 'capture_test'))
        with open(os.path.join(REVIEW, a.batch, 'capture_test', 'result.json'), 'w', encoding='utf-8') as jf:
            json.dump({'chosen': cfg, 'results': results}, jf, indent=1)
        if a.capture_test:
            return
        if not cfg:
            raise SystemExit('no desktop capture method works; see capture_test/result.json')
    # The batch's backlog: <batch>/todo.txt lists more route:arm jobs (one per line or comma separated) that the
    # next record.py run of the batch picks up after its own. Jobs already recorded OK are skipped unless --force.
    todo = os.path.join(REVIEW, a.batch, 'todo.txt')
    done = set()
    while True:
        extra = []
        if os.path.exists(todo):
            with open(todo, encoding='utf-8') as tf:
                extra = [tuple(j.strip().split(':')) for j in tf.read().replace(',', ' ').split() if ':' in j]
        pending = [j for j in jobs + extra if j not in done and len(j) == 2 and j[1] in ARMS
                   and (j[0] in make_routes.BASES or j[0] in FIXED_ROUTES)]
        if not pending:
            break
        r, arm = pending[0]
        done.add((r, arm))
        if not (a.force and (r, arm) in jobs) and job_ok(a.batch, r, arm):  # --force applies to --jobs only
            print(f'job {r}:{arm}: already recorded OK, skipped (--force to redo)', flush=True)
            continue
        run_job(a.batch, r, arm, cfg)


def job_ok(batch, route, arm):
    try:
        with open(os.path.join(REVIEW, batch, 'runs', f'{route}__{arm}', 'job.json'), encoding='utf-8') as jf:
            return bool(json.load(jf).get('capture_ok'))
    except (OSError, ValueError):
        return False


if __name__ == '__main__':
    main()
